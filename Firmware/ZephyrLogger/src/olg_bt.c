#include "olg_bt.h"

#include <errno.h>
#include <stdbool.h>

#include <zephyr/sys/util.h>

#if IS_ENABLED(CONFIG_BT)
#include <zephyr/bluetooth/bluetooth.h>
#endif

static bool bt_ready;

int olg_bt_enable(void)
{
#if IS_ENABLED(CONFIG_BT)
	if (bt_ready) {
		return 0;
	}

	int err = bt_enable(NULL);
	if (err && err != -EALREADY) {
		return err;
	}

	bt_ready = true;
	return 0;
#else
	return 0;
#endif
}

bool olg_bt_enabled(void)
{
	return bt_ready;
}
