#include "olg_time.h"

#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>

struct time_state {
	bool valid;
	uint32_t sync_local_ms;
	uint64_t sync_unix_ms;
	uint32_t sync_count;
};

static struct time_state state;
static struct k_spinlock lock;

void olg_time_init(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	state.valid = false;
	state.sync_local_ms = 0;
	state.sync_unix_ms = 0;
	state.sync_count = 0;
	k_spin_unlock(&lock, key);
}

void olg_time_sync(uint32_t local_ms, uint64_t unix_ms)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	state.valid = true;
	state.sync_local_ms = local_ms;
	state.sync_unix_ms = unix_ms;
	state.sync_count++;
	k_spin_unlock(&lock, key);
}

bool olg_time_valid(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool valid = state.valid;

	k_spin_unlock(&lock, key);
	return valid;
}

uint32_t olg_time_sync_count(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	uint32_t count = state.sync_count;

	k_spin_unlock(&lock, key);
	return count;
}

uint64_t olg_time_unix_ms_from_uptime(uint32_t local_ms)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool valid = state.valid;
	uint32_t sync_local_ms = state.sync_local_ms;
	uint64_t sync_unix_ms = state.sync_unix_ms;

	k_spin_unlock(&lock, key);

	if (!valid) {
		return 0;
	}

	int32_t delta = (int32_t)(local_ms - sync_local_ms);
	if (delta < 0) {
		uint32_t back = (uint32_t)(-delta);

		return sync_unix_ms > back ? sync_unix_ms - back : 0;
	}

	return sync_unix_ms + (uint32_t)delta;
}
