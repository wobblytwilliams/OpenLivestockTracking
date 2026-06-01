#include "olg_gateway.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "olg_ble.h"
#include "olg_bt.h"
#include "olg_config.h"
#include "olg_log_format.h"
#include "olg_sd.h"
#include "olg_time.h"

#if IS_ENABLED(CONFIG_BT) && IS_ENABLED(CONFIG_OLG_GATEWAY_ENABLE)
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>

#if defined(CONFIG_SOC_FAMILY_NORDIC_NRF)
#include <hal/nrf_ficr.h>
#endif

#define OLG_GW_CMD_PREPARE  1U
#define OLG_GW_CMD_MANIFEST 2U
#define OLG_GW_CMD_STREAM   3U
#define OLG_GW_CMD_DONE     4U

#define OLG_GW_MSG_STATUS   1U
#define OLG_GW_MSG_MANIFEST 2U
#define OLG_GW_MSG_CHUNK    3U
#define OLG_GW_MSG_BLOCK    4U

#define OLG_GW_STATUS_OK    0U
#define OLG_GW_STATUS_EOF   1U
#define OLG_GW_STATUS_ERROR 2U
#define OLG_GW_STATUS_NOT_ELIGIBLE 3U

#define OLG_GW_UPLOAD_ACC BIT(0)
#define OLG_GW_UPLOAD_GPS BIT(1)
#define OLG_GW_UPLOAD_BLE BIT(2)
#define OLG_GW_UPLOAD_ALL (OLG_GW_UPLOAD_ACC | OLG_GW_UPLOAD_GPS | OLG_GW_UPLOAD_BLE)
#define OLG_GW_BLOCK_MSG_HEADER_LEN 16U
#define OLG_GW_MFG_COMPANY_ID 0xffffU
#define OLG_GW_ADV_AGE_NEVER 0xffffU
#define OLG_GW_ADV_AGE_UNKNOWN 0xfffeU
#define OLG_GW_ADV_PAYLOAD_LEN 19U
#define OLG_GW_STATE_MAGIC 0x31535747U
#define OLG_GW_STATE_VERSION 1U

#define OLG_GW_ADV_FLAG_DATA_AVAILABLE BIT(0)
#define OLG_GW_ADV_FLAG_IN_COOLDOWN    BIT(1)
#define OLG_GW_ADV_FLAG_ELIGIBLE       BIT(2)
#define OLG_GW_ADV_FLAG_NEVER          BIT(3)
#define OLG_GW_ADV_FLAG_AGE_UNKNOWN    BIT(4)

#define OLG_GW_ADV_UUID BT_UUID_128_ENCODE(0x8f0a0001, 0x4f4c, 0x4747, 0x4154, 0x455741593031)

static struct bt_uuid_128 gw_svc_uuid =
	BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x8f0a0001, 0x4f4c, 0x4747, 0x4154, 0x455741593031));
static struct bt_uuid_128 gw_info_uuid =
	BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x8f0a0002, 0x4f4c, 0x4747, 0x4154, 0x455741593031));
static struct bt_uuid_128 gw_control_uuid =
	BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x8f0a0003, 0x4f4c, 0x4747, 0x4154, 0x455741593031));
static struct bt_uuid_128 gw_data_uuid =
	BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x8f0a0004, 0x4f4c, 0x4747, 0x4154, 0x455741593031));

static const uint8_t adv_flags[] = { BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR };
static const uint8_t adv_uuid[] = { OLG_GW_ADV_UUID };

struct gw_info_payload {
	uint8_t magic[4];
	uint8_t version;
	uint8_t upload_mask;
	uint16_t chunk_bytes;
	uint64_t logger_id;
	uint32_t gateway_period_ms;
	uint32_t segment_bytes;
	uint8_t status_flags;
	uint8_t reserved;
	uint16_t last_download_age_minutes;
	uint16_t cooldown_remaining_minutes;
} __attribute__((packed));

struct gw_download_state_file {
	uint32_t magic;
	uint16_t version;
	uint16_t len;
	uint8_t ever_downloaded;
	uint8_t reserved[3];
	uint32_t last_success_boot_ms;
	uint64_t last_success_unix_ms;
	uint32_t crc32;
} __attribute__((packed));

struct gw_download_state {
	bool ever_downloaded;
	bool success_this_boot;
	uint32_t last_success_boot_ms;
	uint64_t last_success_unix_ms;
};

struct gw_status {
	bool data_available;
	bool in_cooldown;
	bool transfer_eligible;
	bool never_downloaded;
	bool age_unknown;
	uint16_t last_download_age_minutes;
	uint16_t cooldown_remaining_minutes;
	uint32_t cooldown_remaining_ms;
};

static struct bt_conn *current_conn;
static bool subscribed;
static bool advertising;
static bool connected;
static bool initialized;
static bool cleanup_pending;
static bool transfer_success;
static bool pending_prepare;
static bool pending_manifest;
static bool pending_done;
static bool streaming;
static uint16_t manifest_next_segment;
static uint16_t manifest_active_segment;
static uint16_t stream_segment;
static uint32_t stream_offset;
static bool filtered_block_ready;
static uint16_t filtered_block_len;
static uint16_t filtered_send_pos;
static uint32_t filtered_raw_start;
static uint32_t filtered_raw_end;
static uint32_t next_adv_ms;
static uint32_t adv_stop_ms;
static uint32_t session_deadline_ms;
static uint8_t retry_left;
static uint8_t raw_payload[CONFIG_OLG_SD_BINARY_PAYLOAD_BYTES];
static uint8_t filtered_block[OLG_LOG_BLOCK_HEADER_LEN + CONFIG_OLG_SD_BINARY_PAYLOAD_BYTES];
static struct gw_download_state download_state;

static void gateway_status(uint32_t now_ms, struct gw_status *status);
static uint8_t gateway_status_flags(const struct gw_status *status);
static int load_download_state(void);
static int mark_download_success(uint32_t now_ms);

static bool time_reached(uint32_t now_ms, uint32_t target_ms)
{
	return (int32_t)(now_ms - target_ms) >= 0;
}

static uint64_t logger_id(void)
{
#if defined(CONFIG_SOC_FAMILY_NORDIC_NRF) && NRF_FICR_HAS_DEVICE_ID
	uint64_t lo = nrf_ficr_deviceid_get(NRF_FICR, 0);
	uint64_t hi = nrf_ficr_deviceid_get(NRF_FICR, 1);

	return (hi << 32) | lo;
#else
	return 0;
#endif
}

static uint32_t retry_delay_ms(void)
{
	const struct olg_config *cfg = olg_config_get();
	uint32_t min_ms = cfg->gateway_retry_min_ms;
	uint32_t max_ms = cfg->gateway_retry_max_ms;

	if (max_ms <= min_ms) {
		return min_ms;
	}

	return min_ms + (sys_rand32_get() % (max_ms - min_ms + 1U));
}

static uint8_t upload_mask(void)
{
	const struct olg_config *cfg = olg_config_get();
	uint8_t mask = 0U;

	if (cfg->gateway_upload_acc) {
		mask |= OLG_GW_UPLOAD_ACC;
	}
	if (cfg->gateway_upload_gps) {
		mask |= OLG_GW_UPLOAD_GPS;
	}
	if (cfg->gateway_upload_ble) {
		mask |= OLG_GW_UPLOAD_BLE;
	}

	return mask;
}

static uint16_t bounded_minutes_from_ms(uint64_t ms)
{
	uint64_t minutes = ms / 60000U;

	if (minutes >= OLG_GW_ADV_AGE_UNKNOWN) {
		return OLG_GW_ADV_AGE_UNKNOWN - 1U;
	}

	return (uint16_t)minutes;
}

static uint16_t bounded_ceil_minutes_from_ms(uint32_t ms)
{
	uint64_t minutes = ((uint64_t)ms + 59999U) / 60000U;

	if (minutes >= OLG_GW_ADV_AGE_UNKNOWN) {
		return OLG_GW_ADV_AGE_UNKNOWN - 1U;
	}

	return (uint16_t)minutes;
}

static void gateway_status(uint32_t now_ms, struct gw_status *status)
{
	const struct olg_config *cfg = olg_config_get();
	uint64_t age_ms = 0U;
	bool age_known = false;

	memset(status, 0, sizeof(*status));
	status->data_available = (upload_mask() != 0U) && olg_sd_gateway_data_available();

	if (!download_state.ever_downloaded) {
		status->never_downloaded = true;
		status->last_download_age_minutes = OLG_GW_ADV_AGE_NEVER;
	} else if (download_state.success_this_boot) {
		age_ms = (uint32_t)(now_ms - download_state.last_success_boot_ms);
		status->last_download_age_minutes = bounded_minutes_from_ms(age_ms);
		age_known = true;
	} else if (download_state.last_success_unix_ms > 0U && olg_time_valid()) {
		uint64_t now_unix_ms = olg_time_unix_ms_from_uptime(now_ms);

		if (now_unix_ms >= download_state.last_success_unix_ms) {
			age_ms = now_unix_ms - download_state.last_success_unix_ms;
			status->last_download_age_minutes = bounded_minutes_from_ms(age_ms);
			age_known = true;
		}
	}

	if (download_state.ever_downloaded && !age_known) {
		status->age_unknown = true;
		status->last_download_age_minutes = OLG_GW_ADV_AGE_UNKNOWN;
	}

	if (age_known && age_ms < cfg->gateway_download_cooldown_ms) {
		status->in_cooldown = true;
		status->cooldown_remaining_ms =
			(uint32_t)(cfg->gateway_download_cooldown_ms - age_ms);
		status->cooldown_remaining_minutes =
			bounded_ceil_minutes_from_ms(status->cooldown_remaining_ms);
	}

	status->transfer_eligible = status->data_available && !status->in_cooldown;
}

static uint8_t gateway_status_flags(const struct gw_status *status)
{
	uint8_t flags = 0U;

	if (status->data_available) {
		flags |= OLG_GW_ADV_FLAG_DATA_AVAILABLE;
	}
	if (status->in_cooldown) {
		flags |= OLG_GW_ADV_FLAG_IN_COOLDOWN;
	}
	if (status->transfer_eligible) {
		flags |= OLG_GW_ADV_FLAG_ELIGIBLE;
	}
	if (status->never_downloaded) {
		flags |= OLG_GW_ADV_FLAG_NEVER;
	}
	if (status->age_unknown) {
		flags |= OLG_GW_ADV_FLAG_AGE_UNKNOWN;
	}

	return flags;
}

static void put_le64(uint64_t value, uint8_t *buf)
{
	for (uint8_t i = 0; i < 8U; i++) {
		buf[i] = (uint8_t)(value >> (8U * i));
	}
}

static void encode_adv_payload(uint8_t *payload, const struct gw_status *status)
{
	sys_put_le16(OLG_GW_MFG_COMPANY_ID, &payload[0]);
	payload[2] = 'O';
	payload[3] = 'L';
	payload[4] = 'G';
	payload[5] = 'A';
	payload[6] = 1U;
	payload[7] = upload_mask();
	payload[8] = gateway_status_flags(status);
	sys_put_le16(status->last_download_age_minutes, &payload[9]);
	put_le64(logger_id(), &payload[11]);
}

static void schedule_next(uint32_t now_ms, bool success)
{
	const struct olg_config *cfg = olg_config_get();
	struct gw_status status;

	if (!success && retry_left > 0U) {
		retry_left--;
		next_adv_ms = now_ms + retry_delay_ms();
		return;
	}

	retry_left = cfg->gateway_retry_count;
	gateway_status(now_ms, &status);
	if (status.in_cooldown) {
		if (cfg->gateway_cooldown_adv_enabled) {
			next_adv_ms = now_ms + cfg->gateway_cooldown_adv_period_ms;
		} else if (status.cooldown_remaining_ms > 0U) {
			next_adv_ms = now_ms + status.cooldown_remaining_ms;
		} else {
			next_adv_ms = now_ms + cfg->gateway_eligible_adv_period_ms;
		}
		return;
	}

	next_adv_ms = now_ms + cfg->gateway_eligible_adv_period_ms;
}

static ssize_t read_info(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			 void *buf, uint16_t len, uint16_t offset)
{
	ARG_UNUSED(attr);

	const struct olg_config *cfg = olg_config_get();
	struct gw_status status;

	gateway_status(k_uptime_get_32(), &status);

	struct gw_info_payload info = {
		.magic = { 'O', 'L', 'G', 'I' },
		.version = 3,
		.upload_mask = upload_mask(),
		.chunk_bytes = CONFIG_OLG_GATEWAY_CHUNK_BYTES,
		.logger_id = logger_id(),
		.gateway_period_ms = cfg->gateway_eligible_adv_period_ms,
		.segment_bytes = CONFIG_OLG_SD_SEGMENT_BYTES,
		.status_flags = gateway_status_flags(&status),
		.reserved = 0,
		.last_download_age_minutes = status.last_download_age_minutes,
		.cooldown_remaining_minutes = status.cooldown_remaining_minutes,
	};

	return bt_gatt_attr_read(conn, attr, buf, len, offset, &info, sizeof(info));
}

static ssize_t write_control(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			     const void *buf, uint16_t len, uint16_t offset,
			     uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	if (offset != 0U || len < 1U) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	const uint8_t *cmd = buf;

	switch (cmd[0]) {
	case OLG_GW_CMD_PREPARE:
		pending_prepare = true;
		break;
	case OLG_GW_CMD_MANIFEST:
		manifest_next_segment = 0;
		pending_manifest = true;
		break;
	case OLG_GW_CMD_STREAM:
		if (len < 7U) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		stream_segment = sys_get_le16(&cmd[1]);
		stream_offset = sys_get_le32(&cmd[3]);
		streaming = true;
		filtered_block_ready = false;
		filtered_block_len = 0;
		filtered_send_pos = 0;
		break;
	case OLG_GW_CMD_DONE:
		streaming = false;
		pending_done = true;
		break;
	default:
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}

	return len;
}

static void data_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);

	subscribed = (value == BT_GATT_CCC_NOTIFY);
}

BT_GATT_SERVICE_DEFINE(olg_gw_svc,
	BT_GATT_PRIMARY_SERVICE(&gw_svc_uuid.uuid),
	BT_GATT_CHARACTERISTIC(&gw_info_uuid.uuid, BT_GATT_CHRC_READ, BT_GATT_PERM_READ,
			       read_info, NULL, NULL),
	BT_GATT_CHARACTERISTIC(&gw_control_uuid.uuid,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE, NULL, write_control, NULL),
	BT_GATT_CHARACTERISTIC(&gw_data_uuid.uuid, BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(data_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE)
);

static int notify_data(const uint8_t *data, uint16_t len)
{
	if (!connected || !subscribed || current_conn == NULL) {
		return -ENOTCONN;
	}

	return bt_gatt_notify(current_conn, &olg_gw_svc.attrs[6], data, len);
}

static void notify_status(uint8_t status)
{
	uint8_t msg[2] = { OLG_GW_MSG_STATUS, status };

	(void)notify_data(msg, sizeof(msg));
}

static void connected_cb(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		return;
	}

	current_conn = bt_conn_ref(conn);
	connected = true;
	advertising = false;
	transfer_success = false;
	pending_done = false;
	session_deadline_ms = k_uptime_get_32() + olg_config_get()->gateway_session_timeout_ms;
}

static void disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(reason);

	if (current_conn == conn) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}

	connected = false;
	subscribed = false;
	streaming = false;
	filtered_block_ready = false;
	pending_prepare = false;
	pending_manifest = false;
	cleanup_pending = true;
}

static struct bt_conn_cb conn_callbacks = {
	.connected = connected_cb,
	.disconnected = disconnected_cb,
};

static int start_advertising(uint32_t now_ms)
{
	const struct olg_config *cfg = olg_config_get();
	struct gw_status status;
	uint8_t adv_payload[OLG_GW_ADV_PAYLOAD_LEN];
	int err = olg_bt_enable();

	if (err) {
		return err;
	}

	gateway_status(now_ms, &status);
	if (status.in_cooldown && !cfg->gateway_cooldown_adv_enabled) {
		schedule_next(now_ms, true);
		return 0;
	}

	encode_adv_payload(adv_payload, &status);
	olg_ble_pause(true);

	static const struct bt_data ad[] = {
		BT_DATA(BT_DATA_FLAGS, adv_flags, sizeof(adv_flags)),
		BT_DATA(BT_DATA_UUID128_ALL, adv_uuid, sizeof(adv_uuid)),
	};
	const struct bt_data sd[] = {
		BT_DATA(BT_DATA_MANUFACTURER_DATA, adv_payload, sizeof(adv_payload)),
	};
	const struct bt_le_adv_param *param =
		BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN,
				BT_GAP_ADV_FAST_INT_MIN_2,
				BT_GAP_ADV_FAST_INT_MAX_2,
				NULL);

	err = bt_le_adv_start(param, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err && err != -EALREADY) {
		olg_ble_pause(false);
		return err;
	}

	advertising = true;
	adv_stop_ms = now_ms + (status.in_cooldown ? cfg->gateway_cooldown_adv_window_ms :
				 cfg->gateway_eligible_adv_window_ms);
	return 0;
}

static void stop_advertising(uint32_t now_ms, bool count_as_failure)
{
	int err = bt_le_adv_stop();

	ARG_UNUSED(err);
	advertising = false;
	olg_ble_pause(false);
	schedule_next(now_ms, !count_as_failure);
}

static void service_prepare(void)
{
	struct gw_status status;

	pending_prepare = false;
	gateway_status(k_uptime_get_32(), &status);
	if (!status.transfer_eligible) {
		notify_status(OLG_GW_STATUS_NOT_ELIGIBLE);
		return;
	}

	int err = olg_sd_gateway_prepare();

	notify_status(err ? OLG_GW_STATUS_ERROR : OLG_GW_STATUS_OK);
}

static void service_done(uint32_t now_ms)
{
	pending_done = false;
	streaming = false;
	filtered_block_ready = false;

	int err = mark_download_success(now_ms);

	transfer_success = true;
	cleanup_pending = true;
	notify_status(err ? OLG_GW_STATUS_ERROR : OLG_GW_STATUS_OK);
}

static void service_manifest(void)
{
	int err = olg_sd_gateway_active_segment(&manifest_active_segment);

	if (err) {
		notify_status(OLG_GW_STATUS_ERROR);
		pending_manifest = false;
		return;
	}

	for (uint8_t sent = 0; sent < 4U && manifest_next_segment <= manifest_active_segment;) {
		struct olg_sd_segment_info entry;
		err = olg_sd_gateway_segment_info(manifest_next_segment, &entry);
		if (err == -ENOENT) {
			manifest_next_segment++;
			continue;
		}
		if (err) {
			notify_status(OLG_GW_STATUS_ERROR);
			pending_manifest = false;
			return;
		}

		uint8_t msg[9];

		msg[0] = OLG_GW_MSG_MANIFEST;
		sys_put_le16(entry.index, &msg[1]);
		sys_put_le32(entry.size, &msg[3]);
		msg[7] = entry.active;
		msg[8] = (manifest_next_segment == manifest_active_segment) ? 1U : 0U;
		if (notify_data(msg, sizeof(msg))) {
			return;
		}

		manifest_next_segment++;
		sent++;
	}

	if (manifest_next_segment > manifest_active_segment) {
		pending_manifest = false;
		notify_status(OLG_GW_STATUS_OK);
	}
}

static uint32_t crc32_ieee(const uint8_t *data, size_t len)
{
	uint32_t crc = 0xffffffffU;

	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (uint8_t bit = 0; bit < 8U; bit++) {
			uint32_t mask = 0U - (crc & 1U);
			crc = (crc >> 1) ^ (0xedb88320U & mask);
		}
	}

	return crc ^ 0xffffffffU;
}

static int load_download_state(void)
{
	struct gw_download_state_file file;
	size_t got = 0;
	int err;

	memset(&download_state, 0, sizeof(download_state));
	err = olg_sd_gateway_state_read((uint8_t *)&file, sizeof(file), &got);
	if (err || got != sizeof(file)) {
		return err ? err : -EINVAL;
	}

	uint32_t expected_crc = file.crc32;
	uint32_t actual_crc = crc32_ieee((const uint8_t *)&file, sizeof(file) - sizeof(file.crc32));

	if (file.magic != OLG_GW_STATE_MAGIC || file.version != OLG_GW_STATE_VERSION ||
	    file.len != sizeof(file) || actual_crc != expected_crc) {
		memset(&download_state, 0, sizeof(download_state));
		return -EINVAL;
	}

	download_state.ever_downloaded = file.ever_downloaded != 0U;
	download_state.success_this_boot = false;
	download_state.last_success_boot_ms = file.last_success_boot_ms;
	download_state.last_success_unix_ms = file.last_success_unix_ms;
	return 0;
}

static int save_download_state(void)
{
	struct gw_download_state_file file = {
		.magic = OLG_GW_STATE_MAGIC,
		.version = OLG_GW_STATE_VERSION,
		.len = sizeof(struct gw_download_state_file),
		.ever_downloaded = download_state.ever_downloaded ? 1U : 0U,
		.reserved = { 0 },
		.last_success_boot_ms = download_state.last_success_boot_ms,
		.last_success_unix_ms = download_state.last_success_unix_ms,
		.crc32 = 0,
	};

	file.crc32 = crc32_ieee((const uint8_t *)&file, sizeof(file) - sizeof(file.crc32));
	return olg_sd_gateway_state_write((const uint8_t *)&file, sizeof(file));
}

static int mark_download_success(uint32_t now_ms)
{
	download_state.ever_downloaded = true;
	download_state.success_this_boot = true;
	download_state.last_success_boot_ms = now_ms;
	download_state.last_success_unix_ms =
		olg_time_valid() ? olg_time_unix_ms_from_uptime(now_ms) : 0U;

	return save_download_state();
}

static bool include_record(uint8_t type, uint8_t len, uint8_t mask)
{
	if (type == OLG_LOG_REC_ACC && len == OLG_LOG_ACC_BODY_LEN) {
		return (mask & OLG_GW_UPLOAD_ACC) != 0U;
	}
	if (type == OLG_LOG_REC_GPS && len == OLG_LOG_GPS_BODY_LEN) {
		return (mask & OLG_GW_UPLOAD_GPS) != 0U;
	}
	if (type == OLG_LOG_REC_BLE && len == OLG_LOG_BLE_BODY_LEN) {
		return (mask & OLG_GW_UPLOAD_BLE) != 0U;
	}

	return false;
}

static int read_exact(uint16_t segment, uint32_t offset, uint8_t *buf, size_t len)
{
	size_t got = 0;
	int err = olg_sd_gateway_read_segment(segment, offset, buf, len, &got);

	if (err) {
		return err;
	}
	if (got != len) {
		return got == 0U ? -ENODATA : -EIO;
	}

	return 0;
}

static int load_next_filtered_block(void)
{
	uint8_t mask = upload_mask();

	if (mask == 0U) {
		return 0;
	}

	for (uint8_t scanned = 0; scanned < 8U; scanned++) {
		uint8_t header_bytes[OLG_LOG_BLOCK_HEADER_LEN];
		size_t got = 0;
		uint32_t raw_start = stream_offset;
		int err = olg_sd_gateway_read_segment(stream_segment, raw_start, header_bytes,
						      sizeof(header_bytes), &got);

		if (err) {
			return err;
		}
		if (got == 0U) {
			return 0;
		}
		if (got != sizeof(header_bytes)) {
			return -EIO;
		}

		uint32_t magic = sys_get_le32(&header_bytes[0]);
		uint16_t version = sys_get_le16(&header_bytes[4]);
		uint16_t header_len = sys_get_le16(&header_bytes[6]);
		uint32_t sequence = sys_get_le32(&header_bytes[8]);
		uint32_t payload_len = sys_get_le32(&header_bytes[12]);
		uint32_t expected_crc = sys_get_le32(&header_bytes[20]);

		if (magic != OLG_LOG_BLOCK_MAGIC || version != OLG_LOG_BLOCK_VERSION ||
		    header_len != OLG_LOG_BLOCK_HEADER_LEN ||
		    payload_len > CONFIG_OLG_SD_BINARY_PAYLOAD_BYTES) {
			return -EINVAL;
		}

		err = read_exact(stream_segment, raw_start + OLG_LOG_BLOCK_HEADER_LEN, raw_payload,
				 payload_len);
		if (err) {
			return err;
		}
		if (crc32_ieee(raw_payload, payload_len) != expected_crc) {
			return -EIO;
		}

		uint32_t raw_end = raw_start + OLG_LOG_BLOCK_HEADER_LEN + payload_len;
		uint32_t pos = 0;
		uint32_t out_pos = OLG_LOG_BLOCK_HEADER_LEN;
		uint16_t record_count = 0;

		while (pos + sizeof(struct olg_log_record_prefix) <= payload_len) {
			const struct olg_log_record_prefix *prefix =
				(const struct olg_log_record_prefix *)&raw_payload[pos];
			uint32_t record_len = sizeof(*prefix) + prefix->len;

			if (record_len < sizeof(*prefix) || pos + record_len > payload_len) {
				return -EINVAL;
			}
			if (include_record(prefix->type, prefix->len, mask)) {
				memcpy(&filtered_block[out_pos], &raw_payload[pos], record_len);
				out_pos += record_len;
				record_count++;
			}
			pos += record_len;
		}
		if (pos != payload_len) {
			return -EINVAL;
		}

		stream_offset = raw_end;
		if (record_count == 0U) {
			filtered_raw_start = raw_start;
			filtered_raw_end = raw_end;
			filtered_block_len = 0;
			filtered_send_pos = 0;
			filtered_block_ready = true;
			return 1;
		}

		uint32_t filtered_payload_len = out_pos - OLG_LOG_BLOCK_HEADER_LEN;
		sys_put_le32(OLG_LOG_BLOCK_MAGIC, &filtered_block[0]);
		sys_put_le16(OLG_LOG_BLOCK_VERSION, &filtered_block[4]);
		sys_put_le16(OLG_LOG_BLOCK_HEADER_LEN, &filtered_block[6]);
		sys_put_le32(sequence, &filtered_block[8]);
		sys_put_le32(filtered_payload_len, &filtered_block[12]);
		sys_put_le16(record_count, &filtered_block[16]);
		sys_put_le16(0, &filtered_block[18]);
		sys_put_le32(crc32_ieee(&filtered_block[OLG_LOG_BLOCK_HEADER_LEN],
					filtered_payload_len),
			     &filtered_block[20]);

		filtered_raw_start = raw_start;
		filtered_raw_end = raw_end;
		filtered_block_len = (uint16_t)out_pos;
		filtered_send_pos = 0;
		filtered_block_ready = true;
		return 1;
	}

	return 1;
}

static void service_raw_stream(void)
{
	uint8_t msg[8 + CONFIG_OLG_GATEWAY_CHUNK_BYTES];
	size_t got = 0;
	uint16_t mtu = current_conn ? bt_gatt_get_mtu(current_conn) : 0U;
	size_t max_value = (mtu > 3U) ? (size_t)mtu - 3U : 0U;

	if (max_value <= 8U) {
		streaming = false;
		notify_status(OLG_GW_STATUS_ERROR);
		return;
	}

	size_t max_payload = MIN((size_t)CONFIG_OLG_GATEWAY_CHUNK_BYTES,
				 MIN(sizeof(msg) - 8U, max_value - 8U));
	int err = olg_sd_gateway_read_segment(stream_segment, stream_offset,
					      &msg[8], max_payload, &got);

	if (err) {
		streaming = false;
		notify_status(OLG_GW_STATUS_ERROR);
		return;
	}

	if (got == 0U) {
		streaming = false;
		notify_status(OLG_GW_STATUS_EOF);
		return;
	}

	msg[0] = OLG_GW_MSG_CHUNK;
	sys_put_le16(stream_segment, &msg[1]);
	sys_put_le32(stream_offset, &msg[3]);
	msg[7] = (uint8_t)got;

	err = notify_data(msg, 8U + (uint16_t)got);
	if (err == 0) {
		stream_offset += (uint32_t)got;
	}
}

static void service_filtered_stream(void)
{
	uint8_t msg[OLG_GW_BLOCK_MSG_HEADER_LEN + CONFIG_OLG_GATEWAY_CHUNK_BYTES];
	uint16_t mtu = current_conn ? bt_gatt_get_mtu(current_conn) : 0U;
	size_t max_value = (mtu > 3U) ? (size_t)mtu - 3U : 0U;

	if (max_value <= OLG_GW_BLOCK_MSG_HEADER_LEN) {
		streaming = false;
		notify_status(OLG_GW_STATUS_ERROR);
		return;
	}

	if (!filtered_block_ready) {
		int loaded = load_next_filtered_block();

		if (loaded < 0) {
			streaming = false;
			notify_status(OLG_GW_STATUS_ERROR);
			return;
		}
		if (loaded == 0) {
			streaming = false;
			notify_status(OLG_GW_STATUS_EOF);
			return;
		}
		if (!filtered_block_ready) {
			return;
		}
	}

	size_t max_payload = MIN((size_t)CONFIG_OLG_GATEWAY_CHUNK_BYTES,
				 max_value - OLG_GW_BLOCK_MSG_HEADER_LEN);
	size_t remaining = filtered_block_len - filtered_send_pos;
	size_t chunk_len = MIN(remaining, max_payload);

	msg[0] = OLG_GW_MSG_BLOCK;
	sys_put_le16(stream_segment, &msg[1]);
	sys_put_le32(filtered_raw_start, &msg[3]);
	sys_put_le32(filtered_raw_end, &msg[7]);
	sys_put_le16(filtered_send_pos, &msg[11]);
	sys_put_le16(filtered_block_len, &msg[13]);
	msg[15] = (uint8_t)chunk_len;
	memcpy(&msg[OLG_GW_BLOCK_MSG_HEADER_LEN], &filtered_block[filtered_send_pos],
	       chunk_len);

	int err = notify_data(msg, OLG_GW_BLOCK_MSG_HEADER_LEN + (uint16_t)chunk_len);
	if (err == 0) {
		filtered_send_pos += (uint16_t)chunk_len;
		if (filtered_send_pos >= filtered_block_len) {
			filtered_block_ready = false;
		}
	}
}

static void service_stream(void)
{
	if (upload_mask() == OLG_GW_UPLOAD_ALL) {
		service_raw_stream();
	} else {
		service_filtered_stream();
	}
}
#endif

int olg_gateway_init(void)
{
#if IS_ENABLED(CONFIG_BT) && IS_ENABLED(CONFIG_OLG_GATEWAY_ENABLE)
	const struct olg_config *cfg = olg_config_get();

	if (!cfg->gateway_enabled) {
		return 0;
	}

	if (!initialized) {
		bt_conn_cb_register(&conn_callbacks);
		initialized = true;
	}

	(void)load_download_state();
	retry_left = cfg->gateway_retry_count;
	/* Open a short first window soon after boot so bench setup can confirm gateway comms. */
	next_adv_ms = k_uptime_get_32() + MIN(cfg->gateway_eligible_adv_period_ms, 10000U);
	return 0;
#else
	return 0;
#endif
}

void olg_gateway_service(uint32_t now_ms)
{
#if IS_ENABLED(CONFIG_BT) && IS_ENABLED(CONFIG_OLG_GATEWAY_ENABLE)
	const struct olg_config *cfg = olg_config_get();

	if (!cfg->gateway_enabled) {
		return;
	}

	if (pending_done) {
		service_done(now_ms);
	}

	if (cleanup_pending && !connected) {
		cleanup_pending = false;
		olg_sd_gateway_end();
		olg_ble_pause(false);
		schedule_next(now_ms, transfer_success);
		transfer_success = false;
	}

	if (connected && time_reached(now_ms, session_deadline_ms)) {
		(void)bt_conn_disconnect(current_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		return;
	}

	if (pending_prepare) {
		service_prepare();
	}
	if (pending_manifest) {
		service_manifest();
	}
	if (streaming) {
		service_stream();
	}

	if (connected) {
		return;
	}

	if (advertising) {
		if (time_reached(now_ms, adv_stop_ms)) {
			stop_advertising(now_ms, false);
		}
		return;
	}

	if (time_reached(now_ms, next_adv_ms)) {
		if (start_advertising(now_ms)) {
			schedule_next(now_ms, false);
		}
	}
#else
	ARG_UNUSED(now_ms);
#endif
}

uint32_t olg_gateway_ms_until_transition(uint32_t now_ms)
{
#if IS_ENABLED(CONFIG_BT) && IS_ENABLED(CONFIG_OLG_GATEWAY_ENABLE)
	const struct olg_config *cfg = olg_config_get();
	if (!cfg->gateway_enabled) {
		return UINT32_MAX;
	}

	if (pending_prepare || pending_manifest || pending_done || streaming || cleanup_pending) {
		return 1U;
	}

	uint32_t target = next_adv_ms;
	if (advertising) {
		target = adv_stop_ms;
	} else if (connected) {
		/* A GATT write can arrive while the main loop is asleep; poll quickly during sessions. */
		return 10U;
	}

	return time_reached(now_ms, target) ? 1U : target - now_ms;
#else
	ARG_UNUSED(now_ms);
	return UINT32_MAX;
#endif
}

bool olg_gateway_radio_active(void)
{
#if IS_ENABLED(CONFIG_BT) && IS_ENABLED(CONFIG_OLG_GATEWAY_ENABLE)
	return advertising || connected;
#else
	return false;
#endif
}
