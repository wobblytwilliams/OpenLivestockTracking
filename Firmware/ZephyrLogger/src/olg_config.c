#include "olg_config.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/sys/util.h>

#if IS_ENABLED(CONFIG_FILE_SYSTEM)
#include <zephyr/fs/fs.h>
#endif

#ifndef CONFIG_OLG_ACC_ODR_MILLIHZ
#define CONFIG_OLG_ACC_ODR_MILLIHZ 12500
#endif
#ifndef CONFIG_OLG_ACC_RANGE_G
#define CONFIG_OLG_ACC_RANGE_G 16
#endif
#ifndef CONFIG_OLG_BLE_PERIOD_MS
#define CONFIG_OLG_BLE_PERIOD_MS 60000
#endif
#ifndef CONFIG_OLG_BLE_WINDOW_MS
#define CONFIG_OLG_BLE_WINDOW_MS 10000
#endif
#ifndef CONFIG_OLG_BLE_SCAN_INTERVAL_MS
#define CONFIG_OLG_BLE_SCAN_INTERVAL_MS 100
#endif
#ifndef CONFIG_OLG_BLE_SCAN_DUTY_PERMILLE
#define CONFIG_OLG_BLE_SCAN_DUTY_PERMILLE 100
#endif
#ifndef CONFIG_OLG_GPS_INTERVAL_MS
#define CONFIG_OLG_GPS_INTERVAL_MS 180000
#endif
#ifndef CONFIG_OLG_GPS_TIMEOUT_MS
#define CONFIG_OLG_GPS_TIMEOUT_MS 15000
#endif
#ifndef CONFIG_OLG_GPS_MIN_SATS
#define CONFIG_OLG_GPS_MIN_SATS 4
#endif
#ifndef CONFIG_OLG_GPS_MIN_HDOP_CENTI
#define CONFIG_OLG_GPS_MIN_HDOP_CENTI 250
#endif
#ifndef CONFIG_OLG_GATEWAY_PERIOD_MS
#define CONFIG_OLG_GATEWAY_PERIOD_MS 120000
#endif
#ifndef CONFIG_OLG_GATEWAY_ADV_WINDOW_MS
#define CONFIG_OLG_GATEWAY_ADV_WINDOW_MS 30000
#endif
#ifndef CONFIG_OLG_GATEWAY_SESSION_TIMEOUT_MS
#define CONFIG_OLG_GATEWAY_SESSION_TIMEOUT_MS 120000
#endif
#ifndef CONFIG_OLG_GATEWAY_RETRY_COUNT
#define CONFIG_OLG_GATEWAY_RETRY_COUNT 2
#endif
#ifndef CONFIG_OLG_GATEWAY_RETRY_MIN_MS
#define CONFIG_OLG_GATEWAY_RETRY_MIN_MS 60000
#endif
#ifndef CONFIG_OLG_GATEWAY_RETRY_MAX_MS
#define CONFIG_OLG_GATEWAY_RETRY_MAX_MS 180000
#endif

static struct olg_config cfg;

static uint32_t clamp_u32(uint32_t value, uint32_t low, uint32_t high)
{
	if (value < low) {
		return low;
	}
	if (value > high) {
		return high;
	}
	return value;
}

static uint32_t default_scan_window_ms(void)
{
	uint32_t interval_ms = MAX(CONFIG_OLG_BLE_SCAN_INTERVAL_MS, 1);
	uint32_t window_ms = (interval_ms * CONFIG_OLG_BLE_SCAN_DUTY_PERMILLE) / 1000U;

	return clamp_u32(window_ms, 1U, interval_ms);
}

static uint32_t normalize_acc_odr(uint32_t millihz)
{
	switch (millihz) {
	case 12500:
	case 25000:
	case 50000:
	case 100000:
		return millihz;
	default:
		return 12500;
	}
}

static uint8_t normalize_acc_range_g(uint32_t range_g)
{
	switch (range_g) {
	case 2:
	case 4:
	case 8:
	case 16:
		return (uint8_t)range_g;
	default:
		return 16;
	}
}

static void finalize_config(void)
{
	cfg.acc_enabled = cfg.acc_enabled && IS_ENABLED(CONFIG_OLG_ACC_ENABLE);
	cfg.acc_odr_millihz = normalize_acc_odr(cfg.acc_odr_millihz);
	cfg.acc_range_g = normalize_acc_range_g(cfg.acc_range_g);

	cfg.ble_enabled = cfg.ble_enabled && IS_ENABLED(CONFIG_OLG_BLE_ENABLE);
	cfg.ble_period_ms = MAX(cfg.ble_period_ms, 1U);
	cfg.ble_window_ms = MAX(cfg.ble_window_ms, 1U);
	cfg.ble_scan_interval_ms = MAX(cfg.ble_scan_interval_ms, 1U);
	cfg.ble_scan_window_ms = clamp_u32(cfg.ble_scan_window_ms, 1U,
					   cfg.ble_scan_interval_ms);

	cfg.gps_enabled = cfg.gps_enabled && IS_ENABLED(CONFIG_OLG_GPS_ENABLE);
	cfg.gps_interval_ms = MAX(cfg.gps_interval_ms, 1U);
	cfg.gps_timeout_ms = MAX(cfg.gps_timeout_ms, 1U);
	cfg.gps_min_sats = (uint8_t)clamp_u32(cfg.gps_min_sats, 0U, 32U);

	cfg.gateway_enabled = cfg.gateway_enabled && IS_ENABLED(CONFIG_OLG_GATEWAY_ENABLE);
	cfg.gateway_period_ms = MAX(cfg.gateway_period_ms, 1U);
	cfg.gateway_adv_window_ms = MAX(cfg.gateway_adv_window_ms, 1U);
	cfg.gateway_session_timeout_ms = MAX(cfg.gateway_session_timeout_ms, 1U);
	cfg.gateway_retry_min_ms = MAX(cfg.gateway_retry_min_ms, 1U);
	cfg.gateway_retry_max_ms = MAX(cfg.gateway_retry_max_ms, cfg.gateway_retry_min_ms);
}

void olg_config_init_defaults(void)
{
	cfg.acc_enabled = IS_ENABLED(CONFIG_OLG_ACC_ENABLE);
	cfg.acc_odr_millihz = normalize_acc_odr(CONFIG_OLG_ACC_ODR_MILLIHZ);
	cfg.acc_range_g = normalize_acc_range_g(CONFIG_OLG_ACC_RANGE_G);

	cfg.ble_enabled = IS_ENABLED(CONFIG_OLG_BLE_ENABLE);
	cfg.ble_period_ms = MAX(CONFIG_OLG_BLE_PERIOD_MS, 1);
	cfg.ble_window_ms = MAX(CONFIG_OLG_BLE_WINDOW_MS, 1);
	cfg.ble_scan_interval_ms = MAX(CONFIG_OLG_BLE_SCAN_INTERVAL_MS, 1);
	cfg.ble_scan_window_ms = default_scan_window_ms();

	cfg.gps_enabled = IS_ENABLED(CONFIG_OLG_GPS_ENABLE);
	cfg.gps_interval_ms = MAX(CONFIG_OLG_GPS_INTERVAL_MS, 1);
	cfg.gps_timeout_ms = MAX(CONFIG_OLG_GPS_TIMEOUT_MS, 1);
	cfg.gps_min_sats = (uint8_t)clamp_u32(CONFIG_OLG_GPS_MIN_SATS, 0U, 32U);
	cfg.gps_min_hdop_centi = CONFIG_OLG_GPS_MIN_HDOP_CENTI;

	cfg.gateway_enabled = IS_ENABLED(CONFIG_OLG_GATEWAY_ENABLE);
	cfg.gateway_period_ms = MAX(CONFIG_OLG_GATEWAY_PERIOD_MS, 1);
	cfg.gateway_adv_window_ms = MAX(CONFIG_OLG_GATEWAY_ADV_WINDOW_MS, 1);
	cfg.gateway_session_timeout_ms = MAX(CONFIG_OLG_GATEWAY_SESSION_TIMEOUT_MS, 1);
	cfg.gateway_retry_count = (uint8_t)MIN(CONFIG_OLG_GATEWAY_RETRY_COUNT, 255);
	cfg.gateway_retry_min_ms = MAX(CONFIG_OLG_GATEWAY_RETRY_MIN_MS, 1);
	cfg.gateway_retry_max_ms = MAX(CONFIG_OLG_GATEWAY_RETRY_MAX_MS,
					cfg.gateway_retry_min_ms);
	finalize_config();
}

const struct olg_config *olg_config_get(void)
{
	return &cfg;
}

#if IS_ENABLED(CONFIG_FILE_SYSTEM)
static char *trim(char *text)
{
	while (*text && isspace((unsigned char)*text)) {
		text++;
	}

	char *end = text + strlen(text);
	while (end > text && isspace((unsigned char)end[-1])) {
		*--end = '\0';
	}

	return text;
}

static bool str_eq_ci(const char *a, const char *b)
{
	while (*a && *b) {
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
			return false;
		}
		a++;
		b++;
	}

	return *a == '\0' && *b == '\0';
}

static bool parse_bool(const char *text, bool *out)
{
	if (str_eq_ci(text, "true") || str_eq_ci(text, "yes") || strcmp(text, "1") == 0) {
		*out = true;
		return true;
	}
	if (str_eq_ci(text, "false") || str_eq_ci(text, "no") || strcmp(text, "0") == 0) {
		*out = false;
		return true;
	}

	return false;
}

static bool parse_u32(const char *text, uint32_t *out)
{
	char *end = NULL;
	errno = 0;
	unsigned long value = strtoul(text, &end, 10);

	if (errno || end == text || *trim(end) != '\0' || value > UINT32_MAX) {
		return false;
	}

	*out = (uint32_t)value;
	return true;
}

static bool parse_decimal_scaled(const char *text, uint32_t scale, uint32_t *out)
{
	char *end = NULL;
	errno = 0;
	unsigned long whole = strtoul(text, &end, 10);

	if (errno || end == text || whole > UINT32_MAX / scale) {
		return false;
	}

	uint32_t value = (uint32_t)whole * scale;
	if (*end == '.') {
		end++;
		uint32_t frac_scale = scale / 10U;

		while (*end && isdigit((unsigned char)*end) && frac_scale > 0U) {
			value += (uint32_t)(*end - '0') * frac_scale;
			frac_scale /= 10U;
			end++;
		}
		while (*end && isdigit((unsigned char)*end)) {
			end++;
		}
	}

	if (*trim(end) != '\0') {
		return false;
	}

	*out = value;
	return true;
}

static bool parse_acc_odr(const char *text, uint32_t *out_millihz)
{
	uint32_t millihz = 0;

	if (!parse_decimal_scaled(text, 1000U, &millihz)) {
		return false;
	}

	switch (millihz) {
	case 12500:
	case 25000:
	case 50000:
	case 100000:
		*out_millihz = millihz;
		return true;
	default:
		return false;
	}
}

static bool parse_acc_range_g(const char *text, uint32_t *out_range_g)
{
	uint32_t range_g = 0;

	if (!parse_u32(text, &range_g)) {
		return false;
	}

	switch (range_g) {
	case 2:
	case 4:
	case 8:
	case 16:
		*out_range_g = range_g;
		return true;
	default:
		return false;
	}
}

static void apply_key_value(const char *key, const char *value)
{
	bool b = false;
	uint32_t u = 0;

	if (strcmp(key, "acc_enabled") == 0 && parse_bool(value, &b)) {
		cfg.acc_enabled = b;
	} else if (strcmp(key, "acc_odr_hz") == 0 && parse_acc_odr(value, &u)) {
		cfg.acc_odr_millihz = u;
	} else if (strcmp(key, "acc_range_g") == 0 && parse_acc_range_g(value, &u)) {
		cfg.acc_range_g = (uint8_t)u;
	} else if (strcmp(key, "ble_enabled") == 0 && parse_bool(value, &b)) {
		cfg.ble_enabled = b;
	} else if (strcmp(key, "ble_period_ms") == 0 && parse_u32(value, &u) && u > 0U) {
		cfg.ble_period_ms = u;
	} else if (strcmp(key, "ble_window_ms") == 0 && parse_u32(value, &u) && u > 0U) {
		cfg.ble_window_ms = u;
	} else if (strcmp(key, "ble_scan_interval_ms") == 0 && parse_u32(value, &u) && u > 0U) {
		cfg.ble_scan_interval_ms = u;
	} else if (strcmp(key, "ble_scan_window_ms") == 0 && parse_u32(value, &u) && u > 0U) {
		cfg.ble_scan_window_ms = u;
	} else if (strcmp(key, "gps_enabled") == 0 && parse_bool(value, &b)) {
		cfg.gps_enabled = b;
	} else if (strcmp(key, "gps_interval_ms") == 0 && parse_u32(value, &u) && u > 0U) {
		cfg.gps_interval_ms = u;
	} else if (strcmp(key, "gps_timeout_ms") == 0 && parse_u32(value, &u) && u > 0U) {
		cfg.gps_timeout_ms = u;
	} else if (strcmp(key, "gps_min_sats") == 0 && parse_u32(value, &u)) {
		cfg.gps_min_sats = (uint8_t)clamp_u32(u, 0U, 32U);
	} else if (strcmp(key, "gps_min_hdop") == 0 && parse_decimal_scaled(value, 100U, &u)) {
		cfg.gps_min_hdop_centi = (uint16_t)MIN(u, UINT16_MAX);
	} else if (strcmp(key, "gateway_enabled") == 0 && parse_bool(value, &b)) {
		cfg.gateway_enabled = b;
	} else if (strcmp(key, "gateway_period_ms") == 0 && parse_u32(value, &u) && u > 0U) {
		cfg.gateway_period_ms = u;
	} else if (strcmp(key, "gateway_adv_window_ms") == 0 && parse_u32(value, &u) && u > 0U) {
		cfg.gateway_adv_window_ms = u;
	} else if (strcmp(key, "gateway_session_timeout_ms") == 0 && parse_u32(value, &u) && u > 0U) {
		cfg.gateway_session_timeout_ms = u;
	} else if (strcmp(key, "gateway_retry_count") == 0 && parse_u32(value, &u)) {
		cfg.gateway_retry_count = (uint8_t)MIN(u, 255U);
	} else if (strcmp(key, "gateway_retry_min_ms") == 0 && parse_u32(value, &u) && u > 0U) {
		cfg.gateway_retry_min_ms = u;
	} else if (strcmp(key, "gateway_retry_max_ms") == 0 && parse_u32(value, &u) && u > 0U) {
		cfg.gateway_retry_max_ms = u;
	}
}

static void parse_config_text(char *text)
{
	char *line = text;

	while (line && *line) {
		char *next = strpbrk(line, "\r\n");
		if (next) {
			*next++ = '\0';
			while (*next == '\r' || *next == '\n') {
				*next++ = '\0';
			}
		}

		char *body = trim(line);
		if (*body != '\0' && *body != '#' && *body != ';') {
			char *equals = strchr(body, '=');
			if (equals) {
				*equals = '\0';
				char *key = trim(body);
				char *value = trim(equals + 1);
				apply_key_value(key, value);
			}
		}

		line = next;
	}

	finalize_config();
}

static int write_config_defaults(struct fs_file_t *file)
{
	char buf[640];
	char hdop[16];
	uint16_t hdop_whole = cfg.gps_min_hdop_centi / 100U;
	uint16_t hdop_frac = cfg.gps_min_hdop_centi % 100U;

	if (hdop_frac == 0U) {
		(void)snprintf(hdop, sizeof(hdop), "%u", hdop_whole);
	} else if ((hdop_frac % 10U) == 0U) {
		(void)snprintf(hdop, sizeof(hdop), "%u.%u", hdop_whole, hdop_frac / 10U);
	} else {
		(void)snprintf(hdop, sizeof(hdop), "%u.%02u", hdop_whole, hdop_frac);
	}

	int len = snprintf(buf, sizeof(buf),
			   "acc_enabled=%s\n"
			   "acc_odr_hz=%s\n"
			   "acc_range_g=%u\n\n"
			   "ble_enabled=%s\n"
			   "ble_period_ms=%u\n"
			   "ble_window_ms=%u\n"
			   "ble_scan_interval_ms=%u\n"
			   "ble_scan_window_ms=%u\n\n"
			   "gps_enabled=%s\n"
			   "gps_interval_ms=%u\n"
			   "gps_timeout_ms=%u\n"
			   "gps_min_sats=%u\n"
			   "gps_min_hdop=%s\n\n"
			   "gateway_enabled=%s\n"
			   "gateway_period_ms=%u\n"
			   "gateway_adv_window_ms=%u\n"
			   "gateway_session_timeout_ms=%u\n"
			   "gateway_retry_count=%u\n"
			   "gateway_retry_min_ms=%u\n"
			   "gateway_retry_max_ms=%u\n",
			   cfg.acc_enabled ? "true" : "false",
			   cfg.acc_odr_millihz == 12500U ? "12.5" :
			   cfg.acc_odr_millihz == 25000U ? "25" :
			   cfg.acc_odr_millihz == 50000U ? "50" : "100",
			   cfg.acc_range_g,
			   cfg.ble_enabled ? "true" : "false",
			   cfg.ble_period_ms,
			   cfg.ble_window_ms,
			   cfg.ble_scan_interval_ms,
			   cfg.ble_scan_window_ms,
			   cfg.gps_enabled ? "true" : "false",
			   cfg.gps_interval_ms,
			   cfg.gps_timeout_ms,
			   cfg.gps_min_sats,
			   hdop,
			   cfg.gateway_enabled ? "true" : "false",
			   cfg.gateway_period_ms,
			   cfg.gateway_adv_window_ms,
			   cfg.gateway_session_timeout_ms,
			   cfg.gateway_retry_count,
			   cfg.gateway_retry_min_ms,
			   cfg.gateway_retry_max_ms);

	if (len < 0 || len >= (int)sizeof(buf)) {
		return -EINVAL;
	}

	ssize_t written = fs_write(file, buf, (size_t)len);
	if (written < 0) {
		return (int)written;
	}
	if (written != len) {
		return -EIO;
	}

	return fs_sync(file);
}

static int create_default_file(const char *path)
{
	struct fs_file_t file;
	fs_file_t_init(&file);

	int err = fs_open(&file, path, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
	if (err) {
		return err;
	}

	err = write_config_defaults(&file);
	int close_err = fs_close(&file);

	return err ? err : close_err;
}

int olg_config_load_or_create(const char *path)
{
	struct fs_file_t file;
	fs_file_t_init(&file);

	int err = fs_open(&file, path, FS_O_READ);
	if (err == -ENOENT || err == -ENODEV || err == -EINVAL) {
		return create_default_file(path);
	}
	if (err) {
		return err;
	}

	char buf[1024];
	ssize_t got = fs_read(&file, buf, sizeof(buf) - 1U);
	int close_err = fs_close(&file);
	if (got < 0) {
		return (int)got;
	}
	if (close_err) {
		return close_err;
	}

	buf[got] = '\0';
	parse_config_text(buf);
	return 0;
}
#else
int olg_config_load_or_create(const char *path)
{
	ARG_UNUSED(path);

	return 0;
}
#endif
