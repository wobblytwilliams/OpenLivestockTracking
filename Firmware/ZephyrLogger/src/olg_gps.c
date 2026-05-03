#include "olg_gps.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "olg_config.h"
#include "olg_ring.h"
#include "olg_time.h"

static atomic_t fix_count;
static atomic_t sentence_count;
static atomic_t error_count;

#if IS_ENABLED(CONFIG_OLG_GPS_ENABLE)
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/pm/device.h>

#define GPS_UART_NODE DT_NODELABEL(uart0)
#define GPS_TX_NODE   DT_ALIAS(olg_gps_tx)

static const struct device *const uart_dev = DEVICE_DT_GET(GPS_UART_NODE);

#if DT_NODE_HAS_STATUS(GPS_TX_NODE, okay)
static const struct gpio_dt_spec gps_tx_gpio = GPIO_DT_SPEC_GET(GPS_TX_NODE, gpios);
#endif

struct gps_state {
	bool rmc_valid;
	bool have_latlon;
	float lat;
	float lon;
	bool have_sats;
	uint8_t sats;
	bool have_hdop;
	uint16_t hdop_centi;
};

static struct gps_state gps_st;
static bool gps_busy_state;
static bool gps_sleep_state;
static uint32_t next_attempt_ms;
static uint32_t attempt_deadline_ms;
static uint32_t last_sleep_req_ms;
static char line_buf[128];
static uint8_t line_idx;

static bool time_reached(uint32_t now_ms, uint32_t target_ms)
{
	return (int32_t)(now_ms - target_ms) >= 0;
}

static uint32_t gps_interval_ms(void)
{
	const struct olg_config *cfg = olg_config_get();

	return MAX(cfg->gps_interval_ms, 1U);
}

static void gps_state_reset(void)
{
	memset(&gps_st, 0, sizeof(gps_st));
	gps_st.hdop_centi = UINT16_MAX;
}

static void resume_uart(void)
{
	if (IS_ENABLED(CONFIG_PM_DEVICE)) {
		(void)pm_device_action_run(uart_dev, PM_DEVICE_ACTION_RESUME);
	}

	const struct uart_config cfg = {
		.baudrate = CONFIG_OLG_GPS_BAUD,
		.parity = UART_CFG_PARITY_NONE,
		.stop_bits = UART_CFG_STOP_BITS_1,
		.data_bits = UART_CFG_DATA_BITS_8,
		.flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
	};

	if (uart_configure(uart_dev, &cfg)) {
		atomic_inc(&error_count);
	}
}

static void suspend_uart(void)
{
	if (IS_ENABLED(CONFIG_PM_DEVICE)) {
		(void)pm_device_action_run(uart_dev, PM_DEVICE_ACTION_SUSPEND);
	}

#if DT_NODE_HAS_STATUS(GPS_TX_NODE, okay)
	if (IS_ENABLED(CONFIG_OLG_GPS_TX_IDLE_HIGH) && gpio_is_ready_dt(&gps_tx_gpio)) {
		(void)gpio_pin_configure_dt(&gps_tx_gpio, GPIO_OUTPUT_ACTIVE);
	}
#endif
}

static void uart_write_u8(uint8_t value)
{
	uart_poll_out(uart_dev, value);
}

static void drain_uart(uint32_t max_bytes)
{
	for (uint32_t i = 0; i < max_bytes; i++) {
		unsigned char c;

		if (uart_poll_in(uart_dev, &c)) {
			break;
		}
	}
}

static void ubx_send(uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t len)
{
	uint8_t ck_a = 0;
	uint8_t ck_b = 0;

	uart_write_u8(0xb5);
	uart_write_u8(0x62);

	uart_write_u8(cls);
	ck_a += cls;
	ck_b += ck_a;

	uart_write_u8(id);
	ck_a += id;
	ck_b += ck_a;

	uint8_t len_lsb = (uint8_t)(len & 0xff);
	uint8_t len_msb = (uint8_t)(len >> 8);

	uart_write_u8(len_lsb);
	ck_a += len_lsb;
	ck_b += ck_a;

	uart_write_u8(len_msb);
	ck_a += len_msb;
	ck_b += ck_a;

	for (uint16_t i = 0; i < len; i++) {
		uart_write_u8(payload[i]);
		ck_a += payload[i];
		ck_b += ck_a;
	}

	uart_write_u8(ck_a);
	uart_write_u8(ck_b);
}

static void put_u32_le(uint8_t *dst, uint32_t value)
{
	dst[0] = (uint8_t)(value & 0xff);
	dst[1] = (uint8_t)((value >> 8) & 0xff);
	dst[2] = (uint8_t)((value >> 16) & 0xff);
	dst[3] = (uint8_t)((value >> 24) & 0xff);
}

static void send_cfg_rxm(uint8_t lp_mode)
{
	const uint8_t payload[2] = { 0x00, lp_mode };

	ubx_send(0x06, 0x11, payload, sizeof(payload));
}

static void send_rxm_pmreq(uint32_t duration_ms, uint32_t flags, uint32_t wake_sources)
{
	uint8_t payload[16] = { 0 };

	payload[0] = 0x00;
	put_u32_le(payload + 4, duration_ms);
	put_u32_le(payload + 8, flags);
	put_u32_le(payload + 12, wake_sources);
	ubx_send(0x02, 0x41, payload, sizeof(payload));
}

static void enter_backup_sleep(bool force_refresh)
{
	if (!IS_ENABLED(CONFIG_OLG_GPS_BACKUP_SLEEP_ENABLE) || gps_busy_state) {
		return;
	}
	if (gps_sleep_state && !force_refresh) {
		return;
	}

	resume_uart();
	drain_uart(512);

	/* Backup sleep is the SAM-M10Q low-current state between scheduled fix attempts. */
	send_rxm_pmreq(0, BIT(1) | BIT(2), BIT(3));
	k_msleep(20);
	drain_uart(512);
	suspend_uart();

	last_sleep_req_ms = k_uptime_get_32();
	gps_sleep_state = true;
}

static void wake_for_attempt(void)
{
	bool was_sleeping = gps_sleep_state;

	resume_uart();
	uart_write_u8('\r');
	uart_write_u8('\n');

	if (was_sleeping && CONFIG_OLG_GPS_WAKE_SETTLE_MS > 0) {
		k_msleep(CONFIG_OLG_GPS_WAKE_SETTLE_MS);
	}

	drain_uart(512);
	send_cfg_rxm(0);
	gps_sleep_state = false;
}

static void start_attempt(uint32_t now_ms)
{
	if (gps_busy_state) {
		return;
	}

	wake_for_attempt();
	gps_busy_state = true;
	const struct olg_config *cfg = olg_config_get();
	attempt_deadline_ms = now_ms + MAX(cfg->gps_timeout_ms, 1U);
	gps_state_reset();
	line_idx = 0;
}

static bool parse_2_digits(const char *s, uint8_t *out)
{
	if (!s || !isdigit((unsigned char)s[0]) || !isdigit((unsigned char)s[1])) {
		return false;
	}

	*out = (uint8_t)((s[0] - '0') * 10 + (s[1] - '0'));
	return true;
}

static bool is_leap_year(uint16_t year)
{
	return ((year % 4U) == 0U && (year % 100U) != 0U) || (year % 400U) == 0U;
}

static bool gps_datetime_to_unix_ms(const char *utc_time, const char *utc_date,
				    uint64_t *out_unix_ms)
{
	uint8_t hour;
	uint8_t minute;
	uint8_t second;
	uint8_t day;
	uint8_t month;
	uint8_t yy;

	if (!parse_2_digits(utc_time + 0, &hour) ||
	    !parse_2_digits(utc_time + 2, &minute) ||
	    !parse_2_digits(utc_time + 4, &second) ||
	    !parse_2_digits(utc_date + 0, &day) ||
	    !parse_2_digits(utc_date + 2, &month) ||
	    !parse_2_digits(utc_date + 4, &yy)) {
		return false;
	}

	uint16_t millis_part = 0;
	if (utc_time[6] == '.') {
		uint16_t scale = 100;

		for (const char *p = utc_time + 7; *p && scale > 0; p++) {
			if (!isdigit((unsigned char)*p)) {
				break;
			}
			millis_part += (uint16_t)(*p - '0') * scale;
			scale /= 10;
		}
	}

	uint16_t year = yy >= 80 ? (uint16_t)(1900U + yy) : (uint16_t)(2000U + yy);
	static const uint8_t days_in_month[] = {
		31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
	};

	if (hour > 23 || minute > 59 || second > 59 || month < 1 || month > 12) {
		return false;
	}

	uint8_t max_day = days_in_month[month - 1U];
	if (month == 2 && is_leap_year(year)) {
		max_day = 29;
	}
	if (day < 1 || day > max_day) {
		return false;
	}

	uint32_t days = 0;
	for (uint16_t y = 1970; y < year; y++) {
		days += is_leap_year(y) ? 366U : 365U;
	}
	for (uint8_t m = 1; m < month; m++) {
		days += days_in_month[m - 1U];
		if (m == 2 && is_leap_year(year)) {
			days++;
		}
	}
	days += (uint32_t)(day - 1U);

	uint64_t seconds = (uint64_t)days * 86400ULL +
			   (uint32_t)hour * 3600UL +
			   (uint32_t)minute * 60UL + second;

	*out_unix_ms = seconds * 1000ULL + millis_part;
	return true;
}

static int nmea_hex_value(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	return -1;
}

static bool nmea_checksum_ok(const char *sentence)
{
	if (!sentence || sentence[0] != '$') {
		return false;
	}

	const char *star = strchr(sentence, '*');
	if (!star || !star[1] || !star[2]) {
		return false;
	}

	int hi = nmea_hex_value(star[1]);
	int lo = nmea_hex_value(star[2]);
	if (hi < 0 || lo < 0) {
		return false;
	}

	uint8_t calc = 0;
	for (const char *p = sentence + 1; p < star; p++) {
		calc ^= (uint8_t)*p;
	}

	return calc == (uint8_t)((hi << 4) | lo);
}

static bool nmea_degmin_to_dec(const char *s, bool is_lon, float *out)
{
	if (!s || !*s) {
		return false;
	}

	const char *dot = strchr(s, '.');
	size_t len = dot ? (size_t)(dot - s) : strlen(s);
	size_t deg_digits = is_lon ? 3U : 2U;

	if (len < deg_digits + 2U) {
		return false;
	}

	char deg_buf[4] = { 0 };
	memcpy(deg_buf, s, deg_digits);

	int deg = (int)strtol(deg_buf, NULL, 10);
	float minutes = strtof(s + deg_digits, NULL);

	*out = (float)deg + (minutes / 60.0f);
	return true;
}

static uint16_t parse_centi_float(const char *s, uint16_t fallback)
{
	if (!s || !*s) {
		return fallback;
	}

	float value = strtof(s, NULL);
	if (value < 0.0f) {
		value = 0.0f;
	}
	if (value > 655.34f) {
		return UINT16_MAX;
	}

	return (uint16_t)(value * 100.0f + 0.5f);
}

static int split_csv(char *line, char *fields[], int max_fields)
{
	int count = 0;

	fields[count++] = line;
	for (char *p = line; *p && count < max_fields; p++) {
		if (*p == ',') {
			*p = '\0';
			fields[count++] = p + 1;
		}
	}

	return count;
}

static void process_sentence(char *line, uint32_t sentence_ms)
{
	if (!nmea_checksum_ok(line)) {
		return;
	}

	if (line[0] == '$') {
		line++;
	}

	char *star = strchr(line, '*');
	if (star) {
		*star = '\0';
	}
	if (strlen(line) < 5) {
		return;
	}

	char *fields[20] = { 0 };
	int nf = split_csv(line, fields, ARRAY_SIZE(fields));
	if (nf < 1) {
		return;
	}

	const char *msg = fields[0];

	if (strcmp(msg, "GPGGA") == 0 || strcmp(msg, "GNGGA") == 0) {
		if (nf > 7 && fields[7] && *fields[7]) {
			gps_st.sats = (uint8_t)strtoul(fields[7], NULL, 10);
			gps_st.have_sats = true;
		}
		if (nf > 8 && fields[8] && *fields[8]) {
			gps_st.hdop_centi = parse_centi_float(fields[8], UINT16_MAX);
			gps_st.have_hdop = true;
		}

		if (nf > 5 && fields[2] && *fields[2] && fields[4] && *fields[4]) {
			float lat;
			float lon;

			if (nmea_degmin_to_dec(fields[2], false, &lat) &&
			    nmea_degmin_to_dec(fields[4], true, &lon)) {
				if (fields[3] && (*fields[3] == 'S' || *fields[3] == 's')) {
					lat = -lat;
				}
				if (fields[5] && (*fields[5] == 'W' || *fields[5] == 'w')) {
					lon = -lon;
				}
				gps_st.lat = lat;
				gps_st.lon = lon;
				gps_st.have_latlon = true;
			}
		}
		return;
	}

	if (strcmp(msg, "GPRMC") == 0 || strcmp(msg, "GNRMC") == 0) {
		if (nf > 2 && fields[2] && *fields[2]) {
			gps_st.rmc_valid = fields[2][0] == 'A';
		}

		if (gps_st.rmc_valid && nf > 9 && fields[1] && *fields[1] &&
		    fields[9] && *fields[9]) {
			uint64_t unix_ms = 0;

			if (gps_datetime_to_unix_ms(fields[1], fields[9], &unix_ms)) {
				olg_time_sync(sentence_ms, unix_ms);
			}
		}

		if (nf > 6 && fields[3] && *fields[3] && fields[5] && *fields[5]) {
			float lat;
			float lon;

			if (nmea_degmin_to_dec(fields[3], false, &lat) &&
			    nmea_degmin_to_dec(fields[5], true, &lon)) {
				if (fields[4] && (*fields[4] == 'S' || *fields[4] == 's')) {
					lat = -lat;
				}
				if (fields[6] && (*fields[6] == 'W' || *fields[6] == 'w')) {
					lon = -lon;
				}
				gps_st.lat = lat;
				gps_st.lon = lon;
				gps_st.have_latlon = true;
			}
		}
	}
}

static bool fix_ready(void)
{
	const struct olg_config *cfg = olg_config_get();

	return gps_st.rmc_valid && gps_st.have_latlon && gps_st.have_sats &&
	       gps_st.sats >= cfg->gps_min_sats && gps_st.have_hdop &&
	       gps_st.hdop_centi <= cfg->gps_min_hdop_centi;
}

static void push_timeout_location(uint32_t now_ms)
{
	if (!olg_ring_push_gps(now_ms, 0.0f, 0.0f)) {
		atomic_inc(&error_count);
	}
}

static void finish_attempt(uint32_t now_ms, bool timed_out)
{
	if (timed_out) {
		push_timeout_location(now_ms);
	}

	gps_busy_state = false;
	line_idx = 0;
	next_attempt_ms = now_ms + gps_interval_ms();
	enter_backup_sleep(true);
}

static void poll_bounded(uint32_t now_ms)
{
	if (!gps_busy_state) {
		return;
	}

	if (time_reached(now_ms, attempt_deadline_ms)) {
		finish_attempt(now_ms, true);
		return;
	}

	for (uint32_t bytes = 0; bytes < CONFIG_OLG_GPS_POLL_BYTE_BUDGET; bytes++) {
		unsigned char raw;

		if (uart_poll_in(uart_dev, &raw)) {
			break;
		}

		char c = (char)raw;
		if (c == '\r') {
			continue;
		}

		if (c == '\n') {
			if (line_idx > 0) {
				line_buf[line_idx] = '\0';
				uint32_t sentence_ms = k_uptime_get_32();
				char tmp[sizeof(line_buf)];

				memcpy(tmp, line_buf, line_idx + 1U);
				process_sentence(tmp, sentence_ms);
				atomic_inc(&sentence_count);

				if (fix_ready()) {
					(void)olg_ring_push_gps(sentence_ms, gps_st.lat, gps_st.lon);
					atomic_inc(&fix_count);
					finish_attempt(sentence_ms, false);
					return;
				}
			}
			line_idx = 0;
			continue;
		}

		if (line_idx < sizeof(line_buf) - 1U) {
			line_buf[line_idx++] = c;
		} else {
			line_idx = 0;
			atomic_inc(&error_count);
		}
	}
}
#endif

int olg_gps_init(void)
{
	atomic_clear(&fix_count);
	atomic_clear(&sentence_count);
	atomic_clear(&error_count);

#if IS_ENABLED(CONFIG_OLG_GPS_ENABLE)
	if (!device_is_ready(uart_dev)) {
		return -ENODEV;
	}

#if DT_NODE_HAS_STATUS(GPS_TX_NODE, okay)
	if (IS_ENABLED(CONFIG_OLG_GPS_TX_IDLE_HIGH) && !gpio_is_ready_dt(&gps_tx_gpio)) {
		return -ENODEV;
	}
#endif

	gps_state_reset();
	gps_busy_state = false;
	gps_sleep_state = false;
	line_idx = 0;

	const struct olg_config *cfg = olg_config_get();
	resume_uart();
	k_msleep(CONFIG_OLG_GPS_WAKE_SETTLE_MS);
	enter_backup_sleep(true);

	if (!cfg->gps_enabled) {
		return 0;
	}

	uint32_t now = k_uptime_get_32();
	next_attempt_ms = now + gps_interval_ms();
	attempt_deadline_ms = now;
	return 0;
#else
	return 0;
#endif
}

void olg_gps_service(uint32_t now_ms)
{
#if IS_ENABLED(CONFIG_OLG_GPS_ENABLE)
	const struct olg_config *cfg = olg_config_get();
	if (!cfg->gps_enabled) {
		return;
	}

	if (gps_busy_state) {
		poll_bounded(now_ms);
		return;
	}

	if (time_reached(now_ms, next_attempt_ms)) {
		start_attempt(now_ms);
		return;
	}

	if (IS_ENABLED(CONFIG_OLG_GPS_BACKUP_SLEEP_ENABLE)) {
		if (!gps_sleep_state) {
			enter_backup_sleep(false);
		} else if (CONFIG_OLG_GPS_SLEEP_REFRESH_MS > 0 &&
			   time_reached(now_ms,
					last_sleep_req_ms + CONFIG_OLG_GPS_SLEEP_REFRESH_MS)) {
			enter_backup_sleep(true);
		}
	}
#else
	ARG_UNUSED(now_ms);
#endif
}

uint32_t olg_gps_ms_until_transition(uint32_t now_ms)
{
#if IS_ENABLED(CONFIG_OLG_GPS_ENABLE)
	const struct olg_config *cfg = olg_config_get();
	if (!cfg->gps_enabled) {
		return UINT32_MAX;
	}

	if (gps_busy_state) {
		return 1U;
	}

	uint32_t wait_ms = time_reached(now_ms, next_attempt_ms) ? 1U : next_attempt_ms - now_ms;

	if (IS_ENABLED(CONFIG_OLG_GPS_BACKUP_SLEEP_ENABLE) &&
	    gps_sleep_state && CONFIG_OLG_GPS_SLEEP_REFRESH_MS > 0) {
		uint32_t refresh_ms = last_sleep_req_ms + CONFIG_OLG_GPS_SLEEP_REFRESH_MS;
		uint32_t refresh_wait = time_reached(now_ms, refresh_ms) ? 1U : refresh_ms - now_ms;

		wait_ms = MIN(wait_ms, refresh_wait);
	}

	return MAX(wait_ms, 1U);
#else
	ARG_UNUSED(now_ms);
	return UINT32_MAX;
#endif
}

bool olg_gps_busy(void)
{
#if IS_ENABLED(CONFIG_OLG_GPS_ENABLE)
	const struct olg_config *cfg = olg_config_get();
	if (!cfg->gps_enabled) {
		return false;
	}

	return gps_busy_state;
#else
	return false;
#endif
}

bool olg_gps_sleeping(void)
{
#if IS_ENABLED(CONFIG_OLG_GPS_ENABLE)
	const struct olg_config *cfg = olg_config_get();
	if (!cfg->gps_enabled) {
		return gps_sleep_state;
	}

	return gps_sleep_state;
#else
	return false;
#endif
}

uint32_t olg_gps_fix_count(void)
{
	return (uint32_t)atomic_get(&fix_count);
}

uint32_t olg_gps_sentence_count(void)
{
	return (uint32_t)atomic_get(&sentence_count);
}

uint32_t olg_gps_error_count(void)
{
	return (uint32_t)atomic_get(&error_count);
}
