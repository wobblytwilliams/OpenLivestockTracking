#pragma once

#include <stdbool.h>
#include <stdint.h>

struct olg_config {
	bool acc_enabled;
	uint32_t acc_odr_millihz;
	uint8_t acc_range_g;

	bool ble_enabled;
	uint32_t ble_period_ms;
	uint32_t ble_window_ms;
	uint32_t ble_scan_interval_ms;
	uint32_t ble_scan_window_ms;

	bool gps_enabled;
	uint32_t gps_interval_ms;
	uint32_t gps_timeout_ms;
	uint8_t gps_min_sats;
	uint16_t gps_min_hdop_centi;

	bool gateway_enabled;
	uint32_t gateway_period_ms;
	uint32_t gateway_adv_window_ms;
	uint32_t gateway_session_timeout_ms;
	uint8_t gateway_retry_count;
	uint32_t gateway_retry_min_ms;
	uint32_t gateway_retry_max_ms;
};

void olg_config_init_defaults(void);
const struct olg_config *olg_config_get(void);
int olg_config_load_or_create(const char *path);
