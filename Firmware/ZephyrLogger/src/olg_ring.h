#ifndef OLG_RING_H
#define OLG_RING_H

#include <stdbool.h>
#include <stdint.h>

#include "olg_event.h"

void olg_ring_init(void);
bool olg_ring_push_acc(uint32_t ms, int16_t x, int16_t y, int16_t z);
bool olg_ring_push_ble(uint32_t ms, const uint8_t mac[6], int8_t rssi);
bool olg_ring_push_gps(uint32_t ms, float lat, float lon);
bool olg_ring_peek_event(uint8_t *type, uint8_t *len, uint8_t *payload,
			 uint8_t payload_max);
bool olg_ring_drop_event(uint8_t len);
uint32_t olg_ring_peek_raw(uint8_t *buf, uint32_t max_len);
bool olg_ring_drop_raw(uint32_t len);
uint32_t olg_ring_used(void);
uint32_t olg_ring_capacity(void);
uint32_t olg_ring_drops(void);
uint32_t olg_ring_acc_count(void);
uint32_t olg_ring_ble_count(void);
uint32_t olg_ring_gps_count(void);

#endif
