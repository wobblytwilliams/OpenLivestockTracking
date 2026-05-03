#include "olg_storage.h"

#include <zephyr/sys/util.h>

#include "olg_config.h"
#include "olg_sd.h"

#define OLG_CONFIG_PATH "/SD:/CONFIG.TXT"

int olg_storage_startup(void)
{
	int err = olg_sd_init();

	if (err) {
		return err;
	}

	if (!IS_ENABLED(CONFIG_OLG_SD_ENABLE)) {
		return 0;
	}

	err = olg_sd_startup_mount();
	if (!err) {
		err = olg_config_load_or_create(OLG_CONFIG_PATH);
	}
	if (!err) {
		err = olg_sd_startup_open_log_files();
	}
	if (!err) {
		err = olg_sd_startup_ensure_log_headers();
	}

	olg_sd_sleep();

	if (err) {
		olg_sd_mark_startup_failed();
	}

	return err;
}
