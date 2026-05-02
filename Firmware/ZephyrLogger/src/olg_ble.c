#include "olg_ble.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

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
	uint32_t window_ms = (CONFIG_OLG_BLE_SCAN_INTERVAL_MS *
			      CONFIG_OLG_BLE_SCAN_DUTY_PERMILLE) /
			     1000U;

	if (window_ms < 1U) {
		window_ms = 1U;
	}
	if (window_ms > CONFIG_OLG_BLE_SCAN_INTERVAL_MS) {
		window_ms = CONFIG_OLG_BLE_SCAN_INTERVAL_MS;
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

static void disable_bt_if_requested(void)
{
	if (!IS_ENABLED(CONFIG_OLG_BLE_DISABLE_STACK_BETWEEN_WINDOWS) || !bt_enabled) {
		return;
	}

	int err = bt_disable();
	if (err) {
		atomic_inc(&error_count);
		return;
	}

	bt_enabled = false;
}

static void schedule_next_period(uint32_t now_ms)
{
	uint32_t period_ms = CONFIG_OLG_BLE_PERIOD_MS;

	if (period_ms < 1U) {
		period_ms = 1U;
	}

	do {
		next_scan_start_ms += period_ms;
	} while (time_reached(now_ms, next_scan_start_ms));
}

static void start_scan(uint32_t now_ms)
{
	uint32_t interval_ms = CONFIG_OLG_BLE_SCAN_INTERVAL_MS;

	if (interval_ms < 1U) {
		interval_ms = 1U;
	}

	if (enable_bt_if_needed()) {
		next_scan_start_ms = now_ms + CONFIG_OLG_BLE_PERIOD_MS;
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
		next_scan_start_ms = now_ms + CONFIG_OLG_BLE_PERIOD_MS;
		return;
	}

	scanning = true;
	scan_stop_ms = next_scan_start_ms + CONFIG_OLG_BLE_WINDOW_MS;
	if (CONFIG_OLG_BLE_WINDOW_MS < 1U || time_reached(now_ms, scan_stop_ms)) {
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
	disable_bt_if_requested();
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
	next_scan_start_ms = k_uptime_get_32() + CONFIG_OLG_BLE_PERIOD_MS;
	scan_stop_ms = next_scan_start_ms + CONFIG_OLG_BLE_WINDOW_MS;

	if (IS_ENABLED(CONFIG_OLG_BLE_DISABLE_STACK_BETWEEN_WINDOWS)) {
		bt_enabled = false;
		return 0;
	}

	return enable_bt_if_needed();
#else
	return 0;
#endif
}

void olg_ble_service(uint32_t now_ms)
{
#if IS_ENABLED(CONFIG_BT) && IS_ENABLED(CONFIG_OLG_BLE_ENABLE)
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
