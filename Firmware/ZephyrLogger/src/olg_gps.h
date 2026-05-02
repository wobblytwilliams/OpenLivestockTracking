#pragma once

#include <stdbool.h>
#include <stdint.h>

int olg_gps_init(void);
void olg_gps_service(uint32_t now_ms);
uint32_t olg_gps_ms_until_transition(uint32_t now_ms);
bool olg_gps_busy(void);
bool olg_gps_sleeping(void);
uint32_t olg_gps_fix_count(void);
uint32_t olg_gps_sentence_count(void);
uint32_t olg_gps_error_count(void);
