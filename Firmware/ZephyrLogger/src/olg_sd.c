#include "olg_sd.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "olg_event.h"
#include "olg_gps.h"
#include "olg_log_format.h"
#include "olg_ring.h"
#include "olg_time.h"

static atomic_t write_fail_count;
static atomic_t bad_event_count;
static atomic_t written_count;
static atomic_t startup_failed;

#if IS_ENABLED(CONFIG_OLG_SD_ENABLE)
#include <ff.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/fs/fs.h>
#include <zephyr/pm/device.h>
#include <zephyr/storage/disk_access.h>

#define SD_DISK_NAME "SD"
#define SD_MOUNT_PT  "/" SD_DISK_NAME ":"
#define SD_LOG_DIR   SD_MOUNT_PT "/LOG"

#define SD_SLOT_NODE DT_ALIAS(olg_sd_slot)
#define SD_CS_NODE   DT_ALIAS(olg_sd_cs)
#define SD_SCK_NODE  DT_ALIAS(olg_sd_sck)
#define SD_MOSI_NODE DT_ALIAS(olg_sd_mosi)
#define SD_MISO_NODE DT_ALIAS(olg_sd_miso)

BUILD_ASSERT(sizeof(struct olg_log_block_header) == OLG_LOG_BLOCK_HEADER_LEN);
BUILD_ASSERT(sizeof(struct olg_log_acc_record) == 2U + OLG_LOG_ACC_BODY_LEN);
BUILD_ASSERT(sizeof(struct olg_log_gps_record) == 2U + OLG_LOG_GPS_BODY_LEN);
BUILD_ASSERT(sizeof(struct olg_log_ble_record) == 2U + OLG_LOG_BLE_BODY_LEN);

static FATFS fat_fs;
static struct fs_mount_t mount = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
	.mnt_point = SD_MOUNT_PT,
};

static struct fs_file_t log_file;
static bool mounted;
static bool log_file_open;
static bool flush_requested;
static bool gateway_session;
static uint8_t flush_reason;
static uint16_t active_segment;
static uint32_t active_segment_size;
static uint32_t next_retry_ms;
static uint32_t last_flush_done_ms;
static uint32_t block_sequence;
static bool first_data_flush_done;

static uint8_t raw_snapshot[CONFIG_OLG_SD_BINARY_PAYLOAD_BYTES];
static uint8_t block_payload[CONFIG_OLG_SD_BINARY_PAYLOAD_BYTES];

#if DT_NODE_HAS_STATUS(SD_SLOT_NODE, okay)
static const struct device *const spi_dev = DEVICE_DT_GET(DT_BUS(SD_SLOT_NODE));
#endif

#if DT_NODE_HAS_STATUS(SD_CS_NODE, okay)
static const struct gpio_dt_spec sd_cs = GPIO_DT_SPEC_GET(SD_CS_NODE, gpios);
#endif
#if DT_NODE_HAS_STATUS(SD_SCK_NODE, okay)
static const struct gpio_dt_spec sd_sck = GPIO_DT_SPEC_GET(SD_SCK_NODE, gpios);
#endif
#if DT_NODE_HAS_STATUS(SD_MOSI_NODE, okay)
static const struct gpio_dt_spec sd_mosi = GPIO_DT_SPEC_GET(SD_MOSI_NODE, gpios);
#endif
#if DT_NODE_HAS_STATUS(SD_MISO_NODE, okay)
static const struct gpio_dt_spec sd_miso = GPIO_DT_SPEC_GET(SD_MISO_NODE, gpios);
#endif

enum flush_why {
	FLUSH_NORMAL = 1,
	FLUSH_EMERG = 2,
	FLUSH_FAILSAFE = 3,
};

static bool time_reached(uint32_t now_ms, uint32_t target_ms)
{
	return (int32_t)(now_ms - target_ms) >= 0;
}

static void resume_spi(void)
{
#if DT_NODE_HAS_STATUS(SD_SLOT_NODE, okay)
	if (IS_ENABLED(CONFIG_PM_DEVICE)) {
		(void)pm_device_action_run(spi_dev, PM_DEVICE_ACTION_RESUME);
	}
#endif
}

static void suspend_spi(void)
{
#if DT_NODE_HAS_STATUS(SD_SLOT_NODE, okay)
	if (IS_ENABLED(CONFIG_PM_DEVICE)) {
		(void)pm_device_action_run(spi_dev, PM_DEVICE_ACTION_SUSPEND);
	}
#endif
}

static bool sd_gpio_ready(void)
{
#if DT_NODE_HAS_STATUS(SD_CS_NODE, okay) && DT_NODE_HAS_STATUS(SD_SCK_NODE, okay) && \
	DT_NODE_HAS_STATUS(SD_MOSI_NODE, okay) && DT_NODE_HAS_STATUS(SD_MISO_NODE, okay)
	return gpio_is_ready_dt(&sd_cs) && gpio_is_ready_dt(&sd_sck) &&
	       gpio_is_ready_dt(&sd_mosi) && gpio_is_ready_dt(&sd_miso);
#else
	return false;
#endif
}

static void sd_release_bus_pins(void)
{
	if (!sd_gpio_ready()) {
		return;
	}

	(void)gpio_pin_configure_dt(&sd_cs, GPIO_OUTPUT_INACTIVE);

	if (IS_ENABLED(CONFIG_OLG_SD_RELEASE_SPI_PINS_ON_SLEEP)) {
		(void)gpio_pin_configure_dt(&sd_sck, GPIO_OUTPUT_INACTIVE);
		(void)gpio_pin_configure_dt(&sd_mosi, GPIO_OUTPUT_INACTIVE);
		(void)gpio_pin_configure_dt(&sd_miso, GPIO_INPUT);
	}
}

static uint8_t sd_bitbang_transfer(uint8_t out)
{
	uint8_t in = 0;

	for (uint8_t mask = 0x80; mask != 0; mask >>= 1) {
		(void)gpio_pin_set_dt(&sd_mosi, (out & mask) != 0);
		k_busy_wait(1);
		(void)gpio_pin_set_dt(&sd_sck, 1);
		k_busy_wait(1);
		in <<= 1;
		if (gpio_pin_get_dt(&sd_miso) > 0) {
			in |= 1U;
		}
		(void)gpio_pin_set_dt(&sd_sck, 0);
		k_busy_wait(1);
	}

	return in;
}

static void sd_send_cmd0_idle(void)
{
	if (!IS_ENABLED(CONFIG_OLG_SD_SEND_CMD0_ON_SLEEP) || !sd_gpio_ready()) {
		return;
	}

	(void)gpio_pin_configure_dt(&sd_cs, GPIO_OUTPUT_INACTIVE);
	(void)gpio_pin_configure_dt(&sd_sck, GPIO_OUTPUT_INACTIVE);
	(void)gpio_pin_configure_dt(&sd_mosi, GPIO_OUTPUT_ACTIVE);
	(void)gpio_pin_configure_dt(&sd_miso, GPIO_INPUT);

	for (uint8_t i = 0; i < 10; i++) {
		(void)sd_bitbang_transfer(0xff);
	}

	(void)gpio_pin_set_dt(&sd_cs, 1);
	(void)sd_bitbang_transfer(0x40);
	(void)sd_bitbang_transfer(0x00);
	(void)sd_bitbang_transfer(0x00);
	(void)sd_bitbang_transfer(0x00);
	(void)sd_bitbang_transfer(0x00);
	(void)sd_bitbang_transfer(0x95);

	for (uint8_t i = 0; i < 10; i++) {
		uint8_t response = sd_bitbang_transfer(0xff);

		if ((response & 0x80U) == 0U) {
			break;
		}
	}

	(void)gpio_pin_set_dt(&sd_cs, 0);
	(void)sd_bitbang_transfer(0xff);
}

static uint32_t crc32_ieee(const uint8_t *data, size_t len)
{
	uint32_t crc = 0xffffffffU;

	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (uint8_t bit = 0; bit < 8; bit++) {
			uint32_t mask = 0U - (crc & 1U);

			crc = (crc >> 1) ^ (0xedb88320U & mask);
		}
	}

	return crc ^ 0xffffffffU;
}

static int write_all(struct fs_file_t *file, const void *data, size_t len)
{
	const uint8_t *p = data;

	while (len > 0U) {
		ssize_t written = fs_write(file, p, len);

		if (written < 0) {
			return (int)written;
		}
		if (written == 0) {
			return -EIO;
		}

		p += written;
		len -= (size_t)written;
	}

	return 0;
}

static void close_log_file(void)
{
	if (log_file_open) {
		(void)fs_close(&log_file);
		log_file_open = false;
	}
}

static void sd_sleep_internal(void)
{
	close_log_file();

	if (mounted) {
		(void)fs_unmount(&mount);
		mounted = false;
	}

	(void)disk_access_ioctl(SD_DISK_NAME, DISK_IOCTL_CTRL_DEINIT, NULL);
	suspend_spi();
	/* Keep the measured low-current SD state after every write or gateway session. */
	sd_send_cmd0_idle();
	sd_release_bus_pins();
}

static int mount_storage(void)
{
	resume_spi();

	if (!mounted) {
		int err = fs_mount(&mount);

		if (err) {
			return err;
		}
		mounted = true;
	}

	return 0;
}

static void segment_path(uint16_t index, char *buf, size_t len)
{
	(void)snprintf(buf, len, SD_LOG_DIR "/OLG%05u.BIN", index);
}

static int stat_segment(uint16_t index, struct fs_dirent *entry)
{
	char path[32];

	segment_path(index, path, sizeof(path));
	return fs_stat(path, entry);
}

static int find_active_segment(void)
{
	int err = fs_mkdir(SD_LOG_DIR);

	if (err && err != -EEXIST) {
		return err;
	}

	active_segment = 0;
	active_segment_size = 0;

	for (uint32_t i = 0; i <= UINT16_MAX; i++) {
		struct fs_dirent entry;
		int stat_err = stat_segment((uint16_t)i, &entry);

		if (stat_err == -ENOENT) {
			break;
		}
		if (stat_err) {
			return stat_err;
		}

		active_segment = (uint16_t)i;
		active_segment_size = (uint32_t)entry.size;
	}

	if (active_segment_size >= CONFIG_OLG_SD_SEGMENT_BYTES && active_segment < UINT16_MAX) {
		active_segment++;
		active_segment_size = 0;
	}

	return 0;
}

static int ensure_log_storage(void)
{
	int err = mount_storage();

	if (err) {
		return err;
	}

	err = find_active_segment();
	if (err) {
		return err;
	}

	return 0;
}

static int open_active_for_append(void)
{
	if (log_file_open) {
		return 0;
	}

	int err = ensure_log_storage();
	if (err) {
		return err;
	}

	char path[32];
	segment_path(active_segment, path, sizeof(path));
	fs_file_t_init(&log_file);

	err = fs_open(&log_file, path, FS_O_RDWR | FS_O_CREATE | FS_O_APPEND);
	if (err) {
		return err;
	}

	log_file_open = true;
	err = fs_seek(&log_file, 0, FS_SEEK_END);
	if (err) {
		close_log_file();
		return err;
	}

	off_t end = fs_tell(&log_file);
	if (end < 0) {
		close_log_file();
		return (int)end;
	}

	active_segment_size = (uint32_t)end;
	return 0;
}

static int rotate_if_needed(uint32_t block_size)
{
	if (active_segment_size == 0U ||
	    active_segment_size + block_size <= CONFIG_OLG_SD_SEGMENT_BYTES ||
	    active_segment == UINT16_MAX) {
		return 0;
	}

	close_log_file();
	active_segment++;
	active_segment_size = 0;
	return 0;
}

static bool append_record(uint8_t type, uint8_t len, const uint8_t *payload,
			  uint8_t *out, size_t out_max, size_t *out_len)
{
	if (type == OLG_EVT_ACC && len == sizeof(struct olg_acc_payload)) {
		struct olg_acc_payload acc;
		struct olg_log_acc_record rec;

		if (*out_len + sizeof(rec) > out_max) {
			return false;
		}

		memcpy(&acc, payload, sizeof(acc));
		rec.type = OLG_LOG_REC_ACC;
		rec.len = OLG_LOG_ACC_BODY_LEN;
		rec.ms = acc.ms;
		rec.unix_ms = olg_time_unix_ms_from_uptime(acc.ms);
		rec.x = acc.x;
		rec.y = acc.y;
		rec.z = acc.z;
		memcpy(out + *out_len, &rec, sizeof(rec));
		*out_len += sizeof(rec);
		return true;
	}

	if (type == OLG_EVT_GPS && len == sizeof(struct olg_gps_payload)) {
		struct olg_gps_payload gps;
		struct olg_log_gps_record rec;

		if (*out_len + sizeof(rec) > out_max) {
			return false;
		}

		memcpy(&gps, payload, sizeof(gps));
		rec.type = OLG_LOG_REC_GPS;
		rec.len = OLG_LOG_GPS_BODY_LEN;
		rec.ms = gps.ms;
		rec.unix_ms = olg_time_unix_ms_from_uptime(gps.ms);
		rec.lat = gps.lat;
		rec.lon = gps.lon;
		memcpy(out + *out_len, &rec, sizeof(rec));
		*out_len += sizeof(rec);
		return true;
	}

	if (type == OLG_EVT_BLE && len == sizeof(struct olg_ble_payload)) {
		struct olg_ble_payload ble;
		struct olg_log_ble_record rec;

		if (*out_len + sizeof(rec) > out_max) {
			return false;
		}

		memcpy(&ble, payload, sizeof(ble));
		rec.type = OLG_LOG_REC_BLE;
		rec.len = OLG_LOG_BLE_BODY_LEN;
		rec.ms = ble.ms;
		rec.unix_ms = olg_time_unix_ms_from_uptime(ble.ms);
		memcpy(rec.mac, ble.mac, sizeof(rec.mac));
		rec.rssi = ble.rssi;
		memcpy(out + *out_len, &rec, sizeof(rec));
		*out_len += sizeof(rec);
		return true;
	}

	atomic_inc(&bad_event_count);
	return true;
}

static bool build_block_payload(uint32_t max_records, uint32_t *raw_consumed,
				uint16_t *record_count, uint32_t *payload_len)
{
	uint32_t raw_len = olg_ring_peek_raw(raw_snapshot, sizeof(raw_snapshot));
	size_t out_len = 0;
	uint32_t pos = 0;

	*raw_consumed = 0;
	*record_count = 0;
	*payload_len = 0;

	while (pos + 2U <= raw_len && *record_count < max_records) {
		uint8_t type = raw_snapshot[pos];
		uint8_t len = raw_snapshot[pos + 1U];
		uint32_t event_len = 2U + len;
		size_t before = out_len;

		if (pos + event_len > raw_len) {
			break;
		}

		if (!append_record(type, len, raw_snapshot + pos + 2U,
				   block_payload, sizeof(block_payload), &out_len)) {
			break;
		}

		pos += event_len;
		*raw_consumed += event_len;
		if (out_len > before) {
			(*record_count)++;
		}
	}

	*payload_len = (uint32_t)out_len;
	return *raw_consumed > 0U;
}

static void fail_write(void)
{
	atomic_inc(&write_fail_count);
	next_retry_ms = k_uptime_get_32() + CONFIG_OLG_SD_RETRY_BACKOFF_MS;
	flush_requested = false;
	if (!gateway_session) {
		olg_sd_sleep();
	}
}

static bool write_log_block(uint32_t max_records, uint32_t *wrote_records)
{
	uint32_t raw_consumed = 0;
	uint32_t payload_len = 0;
	uint16_t record_count = 0;

	*wrote_records = 0;

	if (olg_ring_used() == 0U) {
		return true;
	}

	if (!build_block_payload(max_records, &raw_consumed, &record_count, &payload_len)) {
		return true;
	}

	if (record_count == 0U) {
		(void)olg_ring_drop_raw(raw_consumed);
		return olg_ring_used() == 0U;
	}

	uint32_t block_size = sizeof(struct olg_log_block_header) + payload_len;
	int err = rotate_if_needed(block_size);

	if (!err) {
		err = open_active_for_append();
	}
	if (err) {
		fail_write();
		return false;
	}

	struct olg_log_block_header header = {
		.magic = OLG_LOG_BLOCK_MAGIC,
		.version = OLG_LOG_BLOCK_VERSION,
		.header_len = OLG_LOG_BLOCK_HEADER_LEN,
		.sequence = block_sequence,
		.payload_len = payload_len,
		.record_count = record_count,
		.reserved = 0,
		.crc32 = crc32_ieee(block_payload, payload_len),
	};

	err = write_all(&log_file, &header, sizeof(header));
	if (!err) {
		err = write_all(&log_file, block_payload, payload_len);
	}
	if (!err && IS_ENABLED(CONFIG_OLG_SD_SYNC_EACH_LINE)) {
		err = fs_sync(&log_file);
	}
	if (err) {
		fail_write();
		return false;
	}

	active_segment_size += block_size;
	block_sequence++;
	*wrote_records = record_count;
	atomic_add(&written_count, record_count);

	if (!olg_ring_drop_raw(raw_consumed)) {
		atomic_inc(&bad_event_count);
	}

	return olg_ring_used() == 0U;
}

static bool flush_ring_burst(uint32_t *wrote_events_out)
{
	uint32_t wrote_total = 0;

	*wrote_events_out = 0;

	if (CONFIG_OLG_SD_RETRY_BACKOFF_MS > 0 &&
	    !time_reached(k_uptime_get_32(), next_retry_ms)) {
		return false;
	}

	while (olg_ring_used() > 0U && wrote_total < CONFIG_OLG_SD_FLUSH_MAX_EVENTS_BURST) {
		uint32_t wrote = 0;
		bool done = write_log_block(CONFIG_OLG_SD_FLUSH_MAX_EVENTS_BURST - wrote_total,
					    &wrote);

		wrote_total += wrote;
		if (done || wrote == 0U) {
			break;
		}
	}

	*wrote_events_out = wrote_total;

	bool done = olg_ring_used() == 0U;
	if (done || IS_ENABLED(CONFIG_OLG_SD_CLOSE_BETWEEN_FLUSHES)) {
		if (gateway_session) {
			close_log_file();
		} else {
			olg_sd_sleep();
		}
	}

	return done;
}

static void request_flush(enum flush_why why)
{
	if (why == FLUSH_EMERG) {
		flush_requested = true;
		flush_reason = FLUSH_EMERG;
		return;
	}

	if (flush_requested && flush_reason == FLUSH_EMERG) {
		return;
	}

	flush_requested = true;
	flush_reason = why;
}

static uint32_t normal_threshold(void)
{
	return (olg_ring_capacity() * CONFIG_OLG_RING_NORMAL_THRESHOLD_PERCENT) / 100U;
}

static uint32_t emerg_threshold(void)
{
	return (olg_ring_capacity() * CONFIG_OLG_RING_EMERG_THRESHOLD_PERCENT) / 100U;
}
#endif

void olg_sd_mark_startup_failed(void)
{
	atomic_set(&startup_failed, 1);
}

int olg_sd_startup_mount(void)
{
#if IS_ENABLED(CONFIG_OLG_SD_ENABLE)
	int err = mount_storage();

	if (err) {
		olg_sd_mark_startup_failed();
	}

	return err;
#else
	return 0;
#endif
}

int olg_sd_startup_open_log_files(void)
{
#if IS_ENABLED(CONFIG_OLG_SD_ENABLE)
	int err = ensure_log_storage();
	if (!err) {
		err = open_active_for_append();
	}
	close_log_file();

	if (err) {
		olg_sd_mark_startup_failed();
	}

	return err;
#else
	return 0;
#endif
}

int olg_sd_startup_ensure_log_headers(void)
{
	return 0;
}

void olg_sd_sleep(void)
{
#if IS_ENABLED(CONFIG_OLG_SD_ENABLE)
	sd_sleep_internal();
#endif
}

int olg_sd_init(void)
{
	atomic_clear(&write_fail_count);
	atomic_clear(&bad_event_count);
	atomic_clear(&written_count);
	atomic_clear(&startup_failed);

#if IS_ENABLED(CONFIG_OLG_SD_ENABLE)
#if DT_NODE_HAS_STATUS(SD_SLOT_NODE, okay)
	if (!device_is_ready(spi_dev)) {
		atomic_set(&startup_failed, 1);
		return -ENODEV;
	}
#else
	atomic_set(&startup_failed, 1);
	return -ENODEV;
#endif

	mounted = false;
	log_file_open = false;
	flush_requested = false;
	gateway_session = false;
	flush_reason = FLUSH_NORMAL;
	active_segment = 0;
	active_segment_size = 0;
	next_retry_ms = 0;
	last_flush_done_ms = k_uptime_get_32();
	block_sequence = 0;
	first_data_flush_done = false;

	return 0;
#else
	return 0;
#endif
}

void olg_sd_service(uint32_t now_ms)
{
#if IS_ENABLED(CONFIG_OLG_SD_ENABLE)
	if (gateway_session) {
		return;
	}

	bool retry_waiting = CONFIG_OLG_SD_RETRY_BACKOFF_MS > 0 &&
			     !time_reached(now_ms, next_retry_ms);

	if (!retry_waiting) {
		uint32_t used = olg_ring_used();
		uint32_t emerg = emerg_threshold();
		uint32_t normal = normal_threshold();

		if (emerg > 0U && used >= emerg) {
			request_flush(FLUSH_EMERG);
		} else if (normal > 0U && used >= normal) {
			request_flush(FLUSH_NORMAL);
		}
	}

	if (!retry_waiting && !first_data_flush_done && CONFIG_OLG_SD_FLUSH_FIRST_MS > 0 &&
	    olg_ring_used() > 0U &&
	    time_reached(now_ms, last_flush_done_ms + CONFIG_OLG_SD_FLUSH_FIRST_MS)) {
		request_flush(FLUSH_FAILSAFE);
	}

	if (!retry_waiting && CONFIG_OLG_SD_FLUSH_FAILSAFE_MS > 0 &&
	    olg_ring_used() > 0U &&
	    time_reached(now_ms, last_flush_done_ms + CONFIG_OLG_SD_FLUSH_FAILSAFE_MS)) {
		request_flush(FLUSH_FAILSAFE);
	}

	if (!flush_requested) {
		return;
	}

	if (IS_ENABLED(CONFIG_OLG_SD_BLOCK_FLUSH_DURING_GPS) &&
	    olg_gps_busy() && flush_reason != FLUSH_EMERG) {
		return;
	}

	uint32_t wrote = 0;
	bool done = flush_ring_burst(&wrote);

	if (done || (wrote > 0U && olg_ring_used() < normal_threshold())) {
		last_flush_done_ms = k_uptime_get_32();
		first_data_flush_done = true;
	}

	if (done) {
		flush_requested = false;
	}
#else
	ARG_UNUSED(now_ms);
#endif
}

uint32_t olg_sd_ms_until_transition(uint32_t now_ms)
{
#if IS_ENABLED(CONFIG_OLG_SD_ENABLE)
	if (gateway_session) {
		return UINT32_MAX;
	}
	if (flush_requested) {
		return 1U;
	}

	if (CONFIG_OLG_SD_RETRY_BACKOFF_MS > 0 && !time_reached(now_ms, next_retry_ms)) {
		return MAX(next_retry_ms - now_ms, 1U);
	}

	uint32_t wait_ms = UINT32_MAX;

	if (!first_data_flush_done && CONFIG_OLG_SD_FLUSH_FIRST_MS > 0) {
		uint32_t target = last_flush_done_ms + CONFIG_OLG_SD_FLUSH_FIRST_MS;

		wait_ms = time_reached(now_ms, target) ? 1U : target - now_ms;
	}

	if (CONFIG_OLG_SD_FLUSH_FAILSAFE_MS > 0) {
		uint32_t target = last_flush_done_ms + CONFIG_OLG_SD_FLUSH_FAILSAFE_MS;
		uint32_t failsafe_wait = time_reached(now_ms, target) ? 1U : target - now_ms;

		wait_ms = MIN(wait_ms, failsafe_wait);
	}

	return wait_ms;
#else
	ARG_UNUSED(now_ms);
	return UINT32_MAX;
#endif
}

int olg_sd_gateway_begin(void)
{
#if IS_ENABLED(CONFIG_OLG_SD_ENABLE)
	gateway_session = true;
	int err = ensure_log_storage();

	if (err) {
		gateway_session = false;
		fail_write();
	}

	return err;
#else
	return -ENOTSUP;
#endif
}

int olg_sd_gateway_prepare(void)
{
#if IS_ENABLED(CONFIG_OLG_SD_ENABLE)
	int err = olg_sd_gateway_begin();
	if (err) {
		return err;
	}

	while (olg_ring_used() > 0U) {
		uint32_t wrote = 0;
		bool done = write_log_block(CONFIG_OLG_SD_FLUSH_MAX_EVENTS_BURST, &wrote);

		if (!done && wrote == 0U) {
			return -EIO;
		}
	}

	close_log_file();
	last_flush_done_ms = k_uptime_get_32();
	first_data_flush_done = true;
	flush_requested = false;
	return 0;
#else
	return -ENOTSUP;
#endif
}

int olg_sd_gateway_manifest(struct olg_sd_segment_info *entries, uint8_t max_entries,
			    uint8_t *count_out)
{
#if IS_ENABLED(CONFIG_OLG_SD_ENABLE)
	if (entries == NULL || count_out == NULL) {
		return -EINVAL;
	}

	int err = olg_sd_gateway_begin();
	if (err) {
		return err;
	}

	uint8_t count = 0;
	for (uint32_t i = 0; i <= active_segment && count < max_entries; i++) {
		struct fs_dirent entry;
		int stat_err = stat_segment((uint16_t)i, &entry);

		if (stat_err == -ENOENT) {
			continue;
		}
		if (stat_err) {
			return stat_err;
		}

		entries[count].index = (uint16_t)i;
		entries[count].size = (uint32_t)entry.size;
		entries[count].active = (i == active_segment) ? 1U : 0U;
		count++;
	}

	*count_out = count;
	return 0;
#else
	ARG_UNUSED(entries);
	ARG_UNUSED(max_entries);
	ARG_UNUSED(count_out);
	return -ENOTSUP;
#endif
}

int olg_sd_gateway_active_segment(uint16_t *index_out)
{
#if IS_ENABLED(CONFIG_OLG_SD_ENABLE)
	if (index_out == NULL) {
		return -EINVAL;
	}

	int err = olg_sd_gateway_begin();
	if (err) {
		return err;
	}

	*index_out = active_segment;
	return 0;
#else
	ARG_UNUSED(index_out);
	return -ENOTSUP;
#endif
}

int olg_sd_gateway_segment_info(uint16_t index, struct olg_sd_segment_info *entry)
{
#if IS_ENABLED(CONFIG_OLG_SD_ENABLE)
	if (entry == NULL) {
		return -EINVAL;
	}

	int err = olg_sd_gateway_begin();
	if (err) {
		return err;
	}

	struct fs_dirent dirent;
	err = stat_segment(index, &dirent);
	if (err) {
		return err;
	}

	entry->index = index;
	entry->size = (uint32_t)dirent.size;
	entry->active = (index == active_segment) ? 1U : 0U;
	return 0;
#else
	ARG_UNUSED(index);
	ARG_UNUSED(entry);
	return -ENOTSUP;
#endif
}

int olg_sd_gateway_read_segment(uint16_t index, uint32_t offset, uint8_t *buf,
				size_t len, size_t *got_out)
{
#if IS_ENABLED(CONFIG_OLG_SD_ENABLE)
	if (buf == NULL || got_out == NULL) {
		return -EINVAL;
	}

	int err = olg_sd_gateway_begin();
	if (err) {
		return err;
	}

	char path[32];
	struct fs_file_t file;

	segment_path(index, path, sizeof(path));
	fs_file_t_init(&file);

	err = fs_open(&file, path, FS_O_READ);
	if (err) {
		return err;
	}

	err = fs_seek(&file, (off_t)offset, FS_SEEK_SET);
	if (err) {
		(void)fs_close(&file);
		return err;
	}

	ssize_t got = fs_read(&file, buf, len);
	int close_err = fs_close(&file);

	if (got < 0) {
		return (int)got;
	}
	if (close_err) {
		return close_err;
	}

	*got_out = (size_t)got;
	return 0;
#else
	ARG_UNUSED(index);
	ARG_UNUSED(offset);
	ARG_UNUSED(buf);
	ARG_UNUSED(len);
	ARG_UNUSED(got_out);
	return -ENOTSUP;
#endif
}

void olg_sd_gateway_end(void)
{
#if IS_ENABLED(CONFIG_OLG_SD_ENABLE)
	gateway_session = false;
	olg_sd_sleep();
#endif
}

uint32_t olg_sd_write_fail_count(void)
{
	return (uint32_t)atomic_get(&write_fail_count);
}

uint32_t olg_sd_bad_event_count(void)
{
	return (uint32_t)atomic_get(&bad_event_count);
}

uint32_t olg_sd_written_count(void)
{
	return (uint32_t)atomic_get(&written_count);
}

uint8_t olg_sd_startup_failed(void)
{
	return (uint8_t)atomic_get(&startup_failed);
}
