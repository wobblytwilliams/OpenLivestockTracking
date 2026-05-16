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
#include "olg_sd.h"

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

#define OLG_GW_STATUS_OK    0U
#define OLG_GW_STATUS_EOF   1U
#define OLG_GW_STATUS_ERROR 2U

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
	uint8_t reserved;
	uint16_t chunk_bytes;
	uint64_t logger_id;
	uint32_t gateway_period_ms;
	uint32_t segment_bytes;
} __attribute__((packed));

static struct bt_conn *current_conn;
static bool subscribed;
static bool advertising;
static bool connected;
static bool initialized;
static bool cleanup_pending;
static bool transfer_success;
static bool pending_prepare;
static bool pending_manifest;
static bool streaming;
static uint16_t manifest_next_segment;
static uint16_t manifest_active_segment;
static uint16_t stream_segment;
static uint32_t stream_offset;
static uint32_t next_adv_ms;
static uint32_t adv_stop_ms;
static uint32_t session_deadline_ms;
static uint8_t retry_left;

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

static void schedule_next(uint32_t now_ms, bool success)
{
	const struct olg_config *cfg = olg_config_get();

	if (!success && retry_left > 0U) {
		retry_left--;
		next_adv_ms = now_ms + retry_delay_ms();
		return;
	}

	retry_left = cfg->gateway_retry_count;
	next_adv_ms = now_ms + cfg->gateway_period_ms;
}

static ssize_t read_info(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			 void *buf, uint16_t len, uint16_t offset)
{
	ARG_UNUSED(attr);

	const struct olg_config *cfg = olg_config_get();
	struct gw_info_payload info = {
		.magic = { 'O', 'L', 'G', 'I' },
		.version = 1,
		.reserved = 0,
		.chunk_bytes = CONFIG_OLG_GATEWAY_CHUNK_BYTES,
		.logger_id = logger_id(),
		.gateway_period_ms = cfg->gateway_period_ms,
		.segment_bytes = CONFIG_OLG_SD_SEGMENT_BYTES,
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
		break;
	case OLG_GW_CMD_DONE:
		transfer_success = true;
		streaming = false;
		cleanup_pending = true;
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
	int err = olg_bt_enable();

	if (err) {
		return err;
	}

	olg_ble_pause(true);

	static const struct bt_data ad[] = {
		BT_DATA(BT_DATA_FLAGS, adv_flags, sizeof(adv_flags)),
		BT_DATA(BT_DATA_UUID128_ALL, adv_uuid, sizeof(adv_uuid)),
	};
	static const struct bt_data sd[] = {
		BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
			sizeof(CONFIG_BT_DEVICE_NAME) - 1),
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
	adv_stop_ms = now_ms + cfg->gateway_adv_window_ms;
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
	pending_prepare = false;
	int err = olg_sd_gateway_prepare();

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

static void service_stream(void)
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

	retry_left = cfg->gateway_retry_count;
	/* Open a short first window soon after boot so bench setup can confirm gateway comms. */
	next_adv_ms = k_uptime_get_32() + MIN(cfg->gateway_period_ms, 10000U);
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

	if (pending_prepare || pending_manifest || streaming || cleanup_pending) {
		return 1U;
	}

	uint32_t target = next_adv_ms;
	if (advertising) {
		target = adv_stop_ms;
	} else if (connected) {
		target = session_deadline_ms;
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
