#include "olg_ring.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/ring_buffer.h>

RING_BUF_DECLARE(olg_ring, CONFIG_OLG_RING_BYTES);

static atomic_t ring_drops;
static atomic_t acc_count;
static atomic_t ble_count;
static atomic_t gps_count;

void olg_ring_init(void)
{
	atomic_clear(&ring_drops);
	atomic_clear(&acc_count);
	atomic_clear(&ble_count);
	atomic_clear(&gps_count);
}

static bool push_event(enum olg_event_type type, const void *payload, uint8_t len)
{
	uint8_t header[2] = { (uint8_t)type, len };
	unsigned int key = irq_lock();

	if (ring_buf_space_get(&olg_ring) < (uint32_t)(sizeof(header) + len)) {
		irq_unlock(key);
		atomic_inc(&ring_drops);
		return false;
	}

	(void)ring_buf_put(&olg_ring, header, sizeof(header));
	(void)ring_buf_put(&olg_ring, payload, len);
	irq_unlock(key);

	switch (type) {
	case OLG_EVT_ACC:
		atomic_inc(&acc_count);
		break;
	case OLG_EVT_BLE:
		atomic_inc(&ble_count);
		break;
	case OLG_EVT_GPS:
		atomic_inc(&gps_count);
		break;
	default:
		break;
	}

	return true;
}

bool olg_ring_push_acc(uint32_t ms, int16_t x, int16_t y, int16_t z)
{
	const struct olg_acc_payload payload = {
		.ms = ms,
		.x = x,
		.y = y,
		.z = z,
	};

	return push_event(OLG_EVT_ACC, &payload, sizeof(payload));
}

bool olg_ring_push_ble(uint32_t ms, const uint8_t mac[6], int8_t rssi)
{
	struct olg_ble_payload payload = {
		.ms = ms,
		.rssi = rssi,
	};

	memcpy(payload.mac, mac, sizeof(payload.mac));
	return push_event(OLG_EVT_BLE, &payload, sizeof(payload));
}

bool olg_ring_push_gps(uint32_t ms, float lat, float lon)
{
	const struct olg_gps_payload payload = {
		.ms = ms,
		.lat = lat,
		.lon = lon,
	};

	return push_event(OLG_EVT_GPS, &payload, sizeof(payload));
}

bool olg_ring_peek_event(uint8_t *type, uint8_t *len, uint8_t *payload,
			 uint8_t payload_max)
{
	uint8_t header[2];
	unsigned int key = irq_lock();

	if (ring_buf_size_get(&olg_ring) < sizeof(header) ||
	    ring_buf_peek(&olg_ring, header, sizeof(header)) != sizeof(header)) {
		irq_unlock(key);
		return false;
	}

	if ((uint32_t)(sizeof(header) + header[1]) > ring_buf_size_get(&olg_ring) ||
	    header[1] > payload_max) {
		irq_unlock(key);
		return false;
	}

	uint8_t tmp[sizeof(header) + UINT8_MAX];
	uint32_t got = ring_buf_peek(&olg_ring, tmp, sizeof(header) + header[1]);
	if (got != sizeof(header) + header[1]) {
		irq_unlock(key);
		return false;
	}

	*type = header[0];
	*len = header[1];
	memcpy(payload, tmp + sizeof(header), header[1]);
	irq_unlock(key);

	return true;
}

bool olg_ring_drop_event(uint8_t len)
{
	const uint32_t event_len = 2U + len;
	unsigned int key = irq_lock();

	if (ring_buf_size_get(&olg_ring) < event_len) {
		irq_unlock(key);
		return false;
	}

	(void)ring_buf_get(&olg_ring, NULL, event_len);
	irq_unlock(key);
	return true;
}

uint32_t olg_ring_used(void)
{
	unsigned int key = irq_lock();
	uint32_t used = ring_buf_size_get(&olg_ring);

	irq_unlock(key);
	return used;
}

uint32_t olg_ring_capacity(void)
{
	return CONFIG_OLG_RING_BYTES;
}

uint32_t olg_ring_drops(void)
{
	return (uint32_t)atomic_get(&ring_drops);
}

uint32_t olg_ring_acc_count(void)
{
	return (uint32_t)atomic_get(&acc_count);
}

uint32_t olg_ring_ble_count(void)
{
	return (uint32_t)atomic_get(&ble_count);
}

uint32_t olg_ring_gps_count(void)
{
	return (uint32_t)atomic_get(&gps_count);
}
