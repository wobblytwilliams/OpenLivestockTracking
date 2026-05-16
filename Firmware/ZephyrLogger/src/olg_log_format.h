#pragma once

#include <stdint.h>

#define OLG_LOG_BLOCK_MAGIC      0x31474c4fU
#define OLG_LOG_BLOCK_VERSION    1U
#define OLG_LOG_BLOCK_HEADER_LEN 24U

#define OLG_LOG_REC_ACC 1U
#define OLG_LOG_REC_GPS 2U
#define OLG_LOG_REC_BLE 3U

#define OLG_LOG_ACC_BODY_LEN 18U
#define OLG_LOG_GPS_BODY_LEN 20U
#define OLG_LOG_BLE_BODY_LEN 19U

struct olg_log_block_header {
	uint32_t magic;
	uint16_t version;
	uint16_t header_len;
	uint32_t sequence;
	uint32_t payload_len;
	uint16_t record_count;
	uint16_t reserved;
	uint32_t crc32;
} __attribute__((packed));

struct olg_log_record_prefix {
	uint8_t type;
	uint8_t len;
} __attribute__((packed));

struct olg_log_acc_record {
	uint8_t type;
	uint8_t len;
	uint32_t ms;
	uint64_t unix_ms;
	int16_t x;
	int16_t y;
	int16_t z;
} __attribute__((packed));

struct olg_log_gps_record {
	uint8_t type;
	uint8_t len;
	uint32_t ms;
	uint64_t unix_ms;
	float lat;
	float lon;
} __attribute__((packed));

struct olg_log_ble_record {
	uint8_t type;
	uint8_t len;
	uint32_t ms;
	uint64_t unix_ms;
	uint8_t mac[6];
	int8_t rssi;
} __attribute__((packed));
