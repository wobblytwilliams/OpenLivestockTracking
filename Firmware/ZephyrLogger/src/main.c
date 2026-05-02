#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "olg_adxl345.h"
#include "olg_ble.h"
#include "olg_gps.h"
#include "olg_power.h"
#include "olg_ring.h"
#include "olg_sd.h"
#include "olg_time.h"

static uint32_t min_u32(uint32_t a, uint32_t b)
{
	return a < b ? a : b;
}

static uint32_t bounded_idle_ms(uint32_t now_ms)
{
	uint32_t idle_ms = CONFIG_OLG_MAIN_IDLE_MAX_MS;

	if (idle_ms < 1U) {
		idle_ms = 1U;
	}

	idle_ms = min_u32(idle_ms, olg_ble_ms_until_transition(now_ms));
	idle_ms = min_u32(idle_ms, olg_gps_ms_until_transition(now_ms));
	idle_ms = min_u32(idle_ms, olg_sd_ms_until_transition(now_ms));

	if (idle_ms < 1U || idle_ms == UINT32_MAX) {
		idle_ms = 1U;
	}

	return idle_ms;
}

int main(void)
{
	bool init_failed = false;

	olg_time_init();
	olg_ring_init();

	if (olg_power_init()) {
		init_failed = true;
	}
	if (olg_adxl345_init()) {
		init_failed = true;
	}
	if (olg_ble_init()) {
		init_failed = true;
	}
	if (olg_gps_init()) {
		init_failed = true;
	}
	if (olg_sd_init()) {
		init_failed = true;
	}

	if (init_failed) {
		olg_power_status_fault();
	} else {
		olg_power_status_ok();
	}

	while (true) {
		uint32_t now_ms = k_uptime_get_32();

		olg_ble_service(now_ms);
		olg_gps_service(now_ms);
		olg_sd_service(now_ms);

		olg_power_idle(bounded_idle_ms(k_uptime_get_32()));
	}

	return 0;
}
