#pragma once

#include <stdbool.h>
#include <stdint.h>

int olg_ble_init(void);
void olg_ble_service(uint32_t now_ms);
void olg_ble_pause(bool pause);
uint32_t olg_ble_ms_until_transition(uint32_t now_ms);
uint32_t olg_ble_seen_count(void);
uint32_t olg_ble_scan_start_count(void);
uint32_t olg_ble_scan_stop_count(void);
uint32_t olg_ble_error_count(void);
