#ifndef OLG_EVENT_H
#define OLG_EVENT_H

#include <stdint.h>

enum olg_event_type {
	OLG_EVT_ACC = 1,
	OLG_EVT_GPS = 2,
	OLG_EVT_BLE = 3,
};

struct olg_acc_payload {
	uint32_t ms;
	int16_t x;
	int16_t y;
	int16_t z;
} __attribute__((packed));

struct olg_ble_payload {
	uint32_t ms;
	uint8_t mac[6];
	int8_t rssi;
} __attribute__((packed));

struct olg_gps_payload {
	uint32_t ms;
	float lat;
	float lon;
} __attribute__((packed));

#endif
