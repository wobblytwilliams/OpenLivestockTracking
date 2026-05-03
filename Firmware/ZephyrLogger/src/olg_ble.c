#include "olg_ble.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "olg_config.h"
#include "olg_ring.h"

#if IS_ENABLED(CONFIG_BT) && IS_ENABLED(CONFIG_OLG_BLE_ENABLE)
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/net_buf.h>
#endif

static atomic_t seen_count;
static atomic_t scan_start_count;
static atomic_t scan_stop_count;
static atomic_t error_count;

#if IS_ENABLED(CONFIG_BT) && IS_ENABLED(CONFIG_OLG_BLE_ENABLE)
static bool bt_enabled;
static bool scanning;
static uint32_t next_scan_start_ms;
static uint32_t scan_stop_ms;

static bool time_reached(uint32_t now_ms, uint32_t target_ms)
{
	return (int32_t)(now_ms - target_ms) >= 0;
}

static uint16_t scan_units_from_ms(uint32_t ms)
{
	uint32_t units = (ms * 1600U + 999U) / 1000U;

	if (units < 1U) {
		units = 1U;
	}
	if (units > UINT16_MAX) {
		units = UINT16_MAX;
	}

	return (uint16_t)units;
}

static uint16_t configured_scan_window_units(void)
{
	const struct olg_config *cfg = olg_config_get();
	uint32_t interval_ms = cfg->ble_scan_interval_ms;
	uint32_t window_ms = cfg->ble_scan_window_ms;

	if (window_ms < 1U) {
		window_ms = 1U;
	}
	if (window_ms > interval_ms) {
		window_ms = interval_ms;
	}

	return scan_units_from_ms(window_ms);
}

static void scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t adv_type,
		    struct net_buf_simple *buf)
{
	ARG_UNUSED(adv_type);
	ARG_UNUSED(buf);

	if (addr == NULL) {
		return;
	}

	(void)olg_ring_push_ble(k_uptime_get_32(), addr->a.val, rssi);
	atomic_inc(&seen_count);
}

static int enable_bt_if_needed(void)
{
	if (bt_enabled) {
		return 0;
	}

	int err = bt_enable(NULL);
	if (err) {
		atomic_inc(&error_count);
		return err;
	}

	bt_enabled = true;
	return 0;
}

static void schedule_next_period(uint32_t now_ms)
{
	const struct olg_config *cfg = olg_config_get();
	uint32_t period_ms = cfg->ble_period_ms;

	if (period_ms < 1U) {
		period_ms = 1U;
	}

	do {
		next_scan_start_ms += period_ms;
	} while (time_reached(now_ms, next_scan_start_ms));
}

static void start_scan(uint32_t now_ms)
{
	const struct olg_config *cfg = olg_config_get();
	uint32_t interval_ms = cfg->ble_scan_interval_ms;

	if (interval_ms < 1U) {
		interval_ms = 1U;
	}

	if (enable_bt_if_needed()) {
		next_scan_start_ms = now_ms + cfg->ble_period_ms;
		return;
	}

	const struct bt_le_scan_param params = {
		.type = BT_LE_SCAN_TYPE_PASSIVE,
		.options = BT_LE_SCAN_OPT_FILTER_DUPLICATE,
		.interval = scan_units_from_ms(interval_ms),
		.window = configured_scan_window_units(),
		.timeout = 0,
		.interval_coded = 0,
		.window_coded = 0,
	};

	int err = bt_le_scan_start(&params, scan_cb);
	if (err && err != -EALREADY) {
		atomic_inc(&error_count);
		next_scan_start_ms = now_ms + cfg->ble_period_ms;
		return;
	}

	scanning = true;
	scan_stop_ms = next_scan_start_ms + cfg->ble_window_ms;
	if (cfg->ble_window_ms < 1U || time_reached(now_ms, scan_stop_ms)) {
		scan_stop_ms = now_ms + 1U;
	}

	atomic_inc(&scan_start_count);
}

static void stop_scan(uint32_t now_ms)
{
	int err = bt_le_scan_stop();

	if (err && err != -EALREADY) {
		atomic_inc(&error_count);
	}

	scanning = false;
	atomic_inc(&scan_stop_count);
	schedule_next_period(now_ms);
}
#endif

int olg_ble_init(void)
{
	atomic_clear(&seen_count);
	atomic_clear(&scan_start_count);
	atomic_clear(&scan_stop_count);
	atomic_clear(&error_count);

#if IS_ENABLED(CONFIG_BT) && IS_ENABLED(CONFIG_OLG_BLE_ENABLE)
	const struct olg_config *cfg = olg_config_get();
	if (!cfg->ble_enabled) {
		return 0;
	}

	next_scan_start_ms = k_uptime_get_32() + cfg->ble_period_ms;
	scan_stop_ms = next_scan_start_ms + cfg->ble_window_ms;

	return enable_bt_if_needed();
#else
	return 0;
#endif
}

void olg_ble_service(uint32_t now_ms)
{
#if IS_ENABLED(CONFIG_BT) && IS_ENABLED(CONFIG_OLG_BLE_ENABLE)
	const struct olg_config *cfg = olg_config_get();
	if (!cfg->ble_enabled) {
		return;
	}

	if (scanning) {
		if (time_reached(now_ms, scan_stop_ms)) {
			stop_scan(now_ms);
		}
		return;
	}

	if (time_reached(now_ms, next_scan_start_ms)) {
		start_scan(now_ms);
	}
#else
	ARG_UNUSED(now_ms);
#endif
}

uint32_t olg_ble_ms_until_transition(uint32_t now_ms)
{
#if IS_ENABLED(CONFIG_BT) && IS_ENABLED(CONFIG_OLG_BLE_ENABLE)
	const struct olg_config *cfg = olg_config_get();
	if (!cfg->ble_enabled) {
		return UINT32_MAX;
	}

	uint32_t target_ms = scanning ? scan_stop_ms : next_scan_start_ms;

	if (time_reached(now_ms, target_ms)) {
		return 1U;
	}

	return target_ms - now_ms;
#else
	ARG_UNUSED(now_ms);
	return UINT32_MAX;
#endif
}

uint32_t olg_ble_seen_count(void)
{
	return (uint32_t)atomic_get(&seen_count);
}

uint32_t olg_ble_scan_start_count(void)
{
	return (uint32_t)atomic_get(&scan_start_count);
}

uint32_t olg_ble_scan_stop_count(void)
{
	return (uint32_t)atomic_get(&scan_stop_count);
}

uint32_t olg_ble_error_count(void)
{
	return (uint32_t)atomic_get(&error_count);
}
