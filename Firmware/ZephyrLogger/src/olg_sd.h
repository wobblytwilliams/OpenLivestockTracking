#pragma once

#include <stdint.h>

int olg_sd_init(void);
int olg_sd_startup_mount(void);
int olg_sd_startup_open_log_files(void);
int olg_sd_startup_ensure_log_headers(void);
void olg_sd_mark_startup_failed(void);
void olg_sd_sleep(void);
void olg_sd_service(uint32_t now_ms);
uint32_t olg_sd_ms_until_transition(uint32_t now_ms);
uint32_t olg_sd_write_fail_count(void);
uint32_t olg_sd_bad_event_count(void);
uint32_t olg_sd_written_count(void);
uint8_t olg_sd_startup_failed(void);
