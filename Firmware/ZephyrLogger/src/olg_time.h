#pragma once

#include <stdbool.h>
#include <stdint.h>

void olg_time_init(void);
void olg_time_sync(uint32_t local_ms, uint64_t unix_ms);
bool olg_time_valid(void);
uint32_t olg_time_sync_count(void);
uint64_t olg_time_unix_ms_from_uptime(uint32_t local_ms);
