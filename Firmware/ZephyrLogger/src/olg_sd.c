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

#define SD_DISK_NAME  "SD"
#define SD_MOUNT_PT   "/" SD_DISK_NAME ":"
#define SD_SLOT_NODE  DT_ALIAS(olg_sd_slot)
#define SD_CS_NODE    DT_ALIAS(olg_sd_cs)
#define SD_SCK_NODE   DT_ALIAS(olg_sd_sck)
#define SD_MOSI_NODE  DT_ALIAS(olg_sd_mosi)
#define SD_MISO_NODE  DT_ALIAS(olg_sd_miso)

static FATFS fat_fs;
static struct fs_mount_t mount = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
	.mnt_point = SD_MOUNT_PT,
};

static struct fs_file_t f_acc;
static struct fs_file_t f_gps;
static struct fs_file_t f_ble;

static bool mounted;
static bool files_open;
static bool acc_file_open;
static bool gps_file_open;
static bool ble_file_open;
static bool flush_requested;
static uint8_t flush_reason;
static uint32_t next_retry_ms;
static uint32_t last_flush_done_ms;
static bool first_data_flush_done;

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

static int write_all(struct fs_file_t *file, const char *data, size_t len)
{
	ssize_t written = fs_write(file, data, len);

	if (written < 0) {
		return (int)written;
	}
	if ((size_t)written != len) {
		return -EIO;
	}

	return 0;
}

static int write_cstr(struct fs_file_t *file, const char *text)
{
	return write_all(file, text, strlen(text));
}

static int write_u32(struct fs_file_t *file, uint32_t value)
{
	char buf[11];
	int len = snprintf(buf, sizeof(buf), "%u", value);

	if (len < 0 || len >= (int)sizeof(buf)) {
		return -EINVAL;
	}

	return write_all(file, buf, (size_t)len);
}

static int write_i32(struct fs_file_t *file, int32_t value)
{
	char buf[12];
	int len = snprintf(buf, sizeof(buf), "%d", value);

	if (len < 0 || len >= (int)sizeof(buf)) {
		return -EINVAL;
	}

	return write_all(file, buf, (size_t)len);
}

static int write_u64(struct fs_file_t *file, uint64_t value)
{
	char buf[21];
	char *p = buf + sizeof(buf);

	*--p = '\0';
	do {
		*--p = (char)('0' + (value % 10U));
		value /= 10U;
	} while (value > 0U);

	return write_cstr(file, p);
}

static int write_fixed6_scaled(struct fs_file_t *file, int64_t scaled)
{
	int err = 0;

	if (scaled < 0) {
		err = write_cstr(file, "-");
		if (err) {
			return err;
		}
		scaled = -scaled;
	}

	uint64_t whole = (uint64_t)scaled / 1000000ULL;
	uint32_t frac = (uint32_t)((uint64_t)scaled % 1000000ULL);
	char buf[28];
	int len = snprintf(buf, sizeof(buf), "%llu.%06u",
			   (unsigned long long)whole, frac);

	if (len < 0 || len >= (int)sizeof(buf)) {
		return -EINVAL;
	}

	return write_all(file, buf, (size_t)len);
}

static int write_float6(struct fs_file_t *file, float value)
{
	double scaled = (double)value * 1000000.0;
	int64_t fixed = scaled >= 0.0 ? (int64_t)(scaled + 0.5) : (int64_t)(scaled - 0.5);

	return write_fixed6_scaled(file, fixed);
}

static int write_mac(struct fs_file_t *file, const uint8_t mac[6])
{
	char buf[18];
	int len = snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
			   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

	if (len < 0 || len >= (int)sizeof(buf)) {
		return -EINVAL;
	}

	return write_all(file, buf, (size_t)len);
}

static void close_files(void)
{
	if (acc_file_open) {
		(void)fs_close(&f_acc);
		acc_file_open = false;
	}
	if (gps_file_open) {
		(void)fs_close(&f_gps);
		gps_file_open = false;
	}
	if (ble_file_open) {
		(void)fs_close(&f_ble);
		ble_file_open = false;
	}

	files_open = false;
}

static void sd_sleep(void)
{
	close_files();

	if (mounted) {
		(void)fs_unmount(&mount);
		mounted = false;
	}

	(void)disk_access_ioctl(SD_DISK_NAME, DISK_IOCTL_CTRL_DEINIT, NULL);
	suspend_spi();
	sd_send_cmd0_idle();
	sd_release_bus_pins();
}

static int open_one(struct fs_file_t *file, const char *path)
{
	fs_file_t_init(file);

	int err = fs_open(file, path, FS_O_RDWR | FS_O_CREATE | FS_O_APPEND);
	if (err) {
		return err;
	}

	return fs_seek(file, 0, FS_SEEK_END);
}

static int open_files(void)
{
	if (files_open) {
		return 0;
	}

	resume_spi();

	int err = 0;
	if (!mounted) {
		err = fs_mount(&mount);
		if (err) {
			return err;
		}
		mounted = true;
	}

	err = open_one(&f_acc, SD_MOUNT_PT "/ACC.CSV");
	if (err) {
		goto fail;
	}
	acc_file_open = true;

	err = open_one(&f_gps, SD_MOUNT_PT "/GPS.CSV");
	if (err) {
		goto fail;
	}
	gps_file_open = true;

	err = open_one(&f_ble, SD_MOUNT_PT "/BLE.CSV");
	if (err) {
		goto fail;
	}
	ble_file_open = true;

	files_open = true;
	return 0;

fail:
	close_files();
	sd_sleep();
	return err;
}

static int write_header_if_empty(struct fs_file_t *file, const char *header)
{
	off_t end = fs_tell(file);

	if (end < 0) {
		return (int)end;
	}
	if (end > 0) {
		return 0;
	}

	int err = write_cstr(file, header);
	if (!err) {
		err = write_cstr(file, "\n");
	}
	if (!err) {
		err = fs_sync(file);
	}

	return err;
}

static int ensure_headers(void)
{
	int err = write_header_if_empty(&f_acc, "ms,unix_ms,x_g,y_g,z_g");

	if (!err) {
		err = write_header_if_empty(&f_gps, "ms,unix_ms,lat,lon");
	}
	if (!err) {
		err = write_header_if_empty(&f_ble, "ms,unix_ms,mac,rssi");
	}

	return err;
}

static int commit_line(struct fs_file_t *file, off_t pos)
{
	int err = 0;

	if (IS_ENABLED(CONFIG_OLG_SD_SYNC_EACH_LINE)) {
		err = fs_sync(file);
	}

	if (err) {
		(void)fs_truncate(file, pos);
		(void)fs_seek(file, pos, FS_SEEK_SET);
	}

	return err;
}

static int write_acc_event(const uint8_t *payload, uint8_t len)
{
	if (len != sizeof(struct olg_acc_payload)) {
		return -EINVAL;
	}

	struct olg_acc_payload acc;
	memcpy(&acc, payload, sizeof(acc));

	off_t pos = fs_tell(&f_acc);
	if (pos < 0) {
		return (int)pos;
	}

	int err = write_u32(&f_acc, acc.ms);
	if (!err) {
		err = write_cstr(&f_acc, ",");
	}
	if (!err) {
		err = write_u64(&f_acc, olg_time_unix_ms_from_uptime(acc.ms));
	}
	if (!err) {
		err = write_cstr(&f_acc, ",");
	}
	if (!err) {
		err = write_fixed6_scaled(&f_acc, (int64_t)acc.x * 3900LL);
	}
	if (!err) {
		err = write_cstr(&f_acc, ",");
	}
	if (!err) {
		err = write_fixed6_scaled(&f_acc, (int64_t)acc.y * 3900LL);
	}
	if (!err) {
		err = write_cstr(&f_acc, ",");
	}
	if (!err) {
		err = write_fixed6_scaled(&f_acc, (int64_t)acc.z * 3900LL);
	}
	if (!err) {
		err = write_cstr(&f_acc, "\n");
	}
	if (!err) {
		err = commit_line(&f_acc, pos);
	}

	return err;
}

static int write_gps_event(const uint8_t *payload, uint8_t len)
{
	if (len != sizeof(struct olg_gps_payload)) {
		return -EINVAL;
	}

	struct olg_gps_payload gps;
	memcpy(&gps, payload, sizeof(gps));

	off_t pos = fs_tell(&f_gps);
	if (pos < 0) {
		return (int)pos;
	}

	int err = write_u32(&f_gps, gps.ms);
	if (!err) {
		err = write_cstr(&f_gps, ",");
	}
	if (!err) {
		err = write_u64(&f_gps, olg_time_unix_ms_from_uptime(gps.ms));
	}
	if (!err) {
		err = write_cstr(&f_gps, ",");
	}
	if (!err) {
		err = write_float6(&f_gps, gps.lat);
	}
	if (!err) {
		err = write_cstr(&f_gps, ",");
	}
	if (!err) {
		err = write_float6(&f_gps, gps.lon);
	}
	if (!err) {
		err = write_cstr(&f_gps, "\n");
	}
	if (!err) {
		err = commit_line(&f_gps, pos);
	}

	return err;
}

static int write_ble_event(const uint8_t *payload, uint8_t len)
{
	if (len != sizeof(struct olg_ble_payload)) {
		return -EINVAL;
	}

	struct olg_ble_payload ble;
	memcpy(&ble, payload, sizeof(ble));

	off_t pos = fs_tell(&f_ble);
	if (pos < 0) {
		return (int)pos;
	}

	int err = write_u32(&f_ble, ble.ms);
	if (!err) {
		err = write_cstr(&f_ble, ",");
	}
	if (!err) {
		err = write_u64(&f_ble, olg_time_unix_ms_from_uptime(ble.ms));
	}
	if (!err) {
		err = write_cstr(&f_ble, ",");
	}
	if (!err) {
		err = write_mac(&f_ble, ble.mac);
	}
	if (!err) {
		err = write_cstr(&f_ble, ",");
	}
	if (!err) {
		err = write_i32(&f_ble, ble.rssi);
	}
	if (!err) {
		err = write_cstr(&f_ble, "\n");
	}
	if (!err) {
		err = commit_line(&f_ble, pos);
	}

	return err;
}

static int write_ring_event(uint8_t type, uint8_t len, const uint8_t *payload,
			    bool *wrote_event)
{
	*wrote_event = false;

	switch (type) {
	case OLG_EVT_ACC:
		*wrote_event = true;
		return write_acc_event(payload, len);
	case OLG_EVT_GPS:
		*wrote_event = true;
		return write_gps_event(payload, len);
	case OLG_EVT_BLE:
		*wrote_event = true;
		return write_ble_event(payload, len);
	default:
		atomic_inc(&bad_event_count);
		return 0;
	}
}

static void fail_write(void)
{
	atomic_inc(&write_fail_count);
	next_retry_ms = k_uptime_get_32() + CONFIG_OLG_SD_RETRY_BACKOFF_MS;
	flush_requested = false;
	sd_sleep();
}

static bool flush_ring_burst(uint32_t *wrote_events_out)
{
	*wrote_events_out = 0;

	if (CONFIG_OLG_SD_RETRY_BACKOFF_MS > 0 &&
	    !time_reached(k_uptime_get_32(), next_retry_ms)) {
		return false;
	}
	if (olg_ring_used() == 0U) {
		return true;
	}

	int err = open_files();
	if (err) {
		fail_write();
		return false;
	}

	err = ensure_headers();
	if (err) {
		fail_write();
		return false;
	}

	for (uint32_t i = 0; i < CONFIG_OLG_SD_FLUSH_MAX_EVENTS_BURST; i++) {
		uint8_t type = 0;
		uint8_t len = 0;
		uint8_t payload[32];

		if (!olg_ring_peek_event(&type, &len, payload, sizeof(payload))) {
			break;
		}

		bool wrote_event = false;
		err = write_ring_event(type, len, payload, &wrote_event);
		if (err) {
			fail_write();
			return false;
		}

		if (!olg_ring_drop_event(len)) {
			atomic_inc(&bad_event_count);
			break;
		}
		if (wrote_event) {
			(*wrote_events_out)++;
			atomic_inc(&written_count);
		}
	}

	bool done = olg_ring_used() == 0U;
	if (done || IS_ENABLED(CONFIG_OLG_SD_CLOSE_BETWEEN_FLUSHES)) {
		sd_sleep();
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

static int startup_check(void)
{
	int err = open_files();

	if (!err) {
		err = ensure_headers();
	}
	sd_sleep();
	return err;
}
#endif

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
	files_open = false;
	acc_file_open = false;
	gps_file_open = false;
	ble_file_open = false;
	flush_requested = false;
	flush_reason = FLUSH_NORMAL;
	next_retry_ms = 0;
	last_flush_done_ms = k_uptime_get_32();
	first_data_flush_done = false;
	sd_release_bus_pins();

	if (IS_ENABLED(CONFIG_OLG_SD_STARTUP_CHECK)) {
		int err = startup_check();

		if (err) {
			atomic_set(&startup_failed, 1);
			return err;
		}
	}

	return 0;
#else
	return 0;
#endif
}

void olg_sd_service(uint32_t now_ms)
{
#if IS_ENABLED(CONFIG_OLG_SD_ENABLE)
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
