#pragma once

#include <stdint.h>
#include <stddef.h>

struct olg_sd_segment_info {
	uint16_t index;
	uint32_t size;
	uint8_t active;
};

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

int olg_sd_gateway_begin(void);
int olg_sd_gateway_prepare(void);
int olg_sd_gateway_manifest(struct olg_sd_segment_info *entries, uint8_t max_entries,
			    uint8_t *count_out);
int olg_sd_gateway_active_segment(uint16_t *index_out);
int olg_sd_gateway_segment_info(uint16_t index, struct olg_sd_segment_info *entry);
int olg_sd_gateway_read_segment(uint16_t index, uint32_t offset, uint8_t *buf,
				size_t len, size_t *got_out);
void olg_sd_gateway_end(void);
