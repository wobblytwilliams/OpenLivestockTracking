#include "olg_power.h"

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#define GPIO0_NODE DT_NODELABEL(gpio0)
#define GPIO1_NODE DT_NODELABEL(gpio1)
#define STATUS_LED_NODE DT_ALIAS(led0)

/* ItsyBitsy board extras. */
#define PIN_DOTSTAR_DATA  8U  /* P0.08 / D8 */
#define PIN_DOTSTAR_CLK   9U  /* P1.09 / D6 */
#define PIN_QSPI_SCK      19U /* P0.19 */
#define PIN_QSPI_CS       23U /* P0.23 */
#define PIN_QSPI_IO0      21U /* P0.21 */
#define PIN_QSPI_IO1      22U /* P0.22 */
#define PIN_QSPI_IO2      0U  /* P1.00 */
#define PIN_QSPI_IO3      17U /* P0.17 */

static const struct device *const gpio0 = DEVICE_DT_GET(GPIO0_NODE);
static const struct device *const gpio1 = DEVICE_DT_GET(GPIO1_NODE);
static const struct gpio_dt_spec status_led = GPIO_DT_SPEC_GET(STATUS_LED_NODE, gpios);
static struct k_work_delayable status_led_off_work;

static int configure_output(const struct device *port, gpio_pin_t pin, int value)
{
	return gpio_pin_configure(port, pin, value ? GPIO_OUTPUT_ACTIVE : GPIO_OUTPUT_INACTIVE);
}

static void pulse_dotstar_bit(uint8_t bit)
{
	gpio_pin_set(gpio0, PIN_DOTSTAR_DATA, bit ? 1 : 0);
	gpio_pin_set(gpio1, PIN_DOTSTAR_CLK, 1);
	gpio_pin_set(gpio1, PIN_DOTSTAR_CLK, 0);
}

static void dotstar_write_byte(uint8_t value)
{
	for (int bit = 7; bit >= 0; bit--) {
		pulse_dotstar_bit((value >> bit) & 0x01U);
	}
}

static void dotstar_off(void)
{
	if (!device_is_ready(gpio0) || !device_is_ready(gpio1)) {
		return;
	}

	(void)configure_output(gpio0, PIN_DOTSTAR_DATA, 0);
	(void)configure_output(gpio1, PIN_DOTSTAR_CLK, 0);

	for (uint8_t i = 0; i < 4; i++) {
		dotstar_write_byte(0x00);
	}

	dotstar_write_byte(0xe0);
	dotstar_write_byte(0x00);
	dotstar_write_byte(0x00);
	dotstar_write_byte(0x00);

	for (uint8_t i = 0; i < 4; i++) {
		dotstar_write_byte(0xff);
	}

	(void)configure_output(gpio0, PIN_DOTSTAR_DATA, 0);
	(void)configure_output(gpio1, PIN_DOTSTAR_CLK, 0);
}

static void qspi_write_bit(uint8_t bit)
{
	gpio_pin_set(gpio0, PIN_QSPI_IO0, bit ? 1 : 0);
	gpio_pin_set(gpio0, PIN_QSPI_SCK, 1);
	gpio_pin_set(gpio0, PIN_QSPI_SCK, 0);
}

static void qspi_write_byte(uint8_t value)
{
	for (int bit = 7; bit >= 0; bit--) {
		qspi_write_bit((value >> bit) & 0x01U);
	}
}

static void qspi_flash_deep_power_down(void)
{
	if (!device_is_ready(gpio0) || !device_is_ready(gpio1)) {
		return;
	}

	(void)configure_output(gpio0, PIN_QSPI_CS, 1);
	(void)configure_output(gpio0, PIN_QSPI_SCK, 0);
	(void)configure_output(gpio0, PIN_QSPI_IO0, 0);
	(void)gpio_pin_configure(gpio0, PIN_QSPI_IO1, GPIO_INPUT);
	(void)gpio_pin_configure(gpio1, PIN_QSPI_IO2, GPIO_INPUT);
	(void)gpio_pin_configure(gpio0, PIN_QSPI_IO3, GPIO_INPUT);

	k_busy_wait(2);
	gpio_pin_set(gpio0, PIN_QSPI_CS, 0);
	qspi_write_byte(0xb9);
	gpio_pin_set(gpio0, PIN_QSPI_CS, 1);
	k_busy_wait(5);

	(void)gpio_pin_configure(gpio0, PIN_QSPI_IO1, GPIO_DISCONNECTED);
	(void)gpio_pin_configure(gpio1, PIN_QSPI_IO2, GPIO_DISCONNECTED);
	(void)gpio_pin_configure(gpio0, PIN_QSPI_IO3, GPIO_DISCONNECTED);
	(void)configure_output(gpio0, PIN_QSPI_IO0, 0);
	(void)configure_output(gpio0, PIN_QSPI_SCK, 0);
	(void)configure_output(gpio0, PIN_QSPI_CS, 1);
}

static void status_led_off(struct k_work *work)
{
	ARG_UNUSED(work);

	olg_power_status_ok();
}

static void status_led_startup_blink(void)
{
	if (!IS_ENABLED(CONFIG_OLG_STATUS_LED) || !gpio_is_ready_dt(&status_led)) {
		return;
	}

	for (uint8_t i = 0; i < 3; i++) {
		(void)gpio_pin_set_dt(&status_led, 1);
		k_msleep(100);
		(void)gpio_pin_set_dt(&status_led, 0);
		k_msleep(100);
	}
}

int olg_power_init(void)
{
	if (!device_is_ready(gpio0) || !device_is_ready(gpio1)) {
		return -ENODEV;
	}

	k_work_init_delayable(&status_led_off_work, status_led_off);

	if (IS_ENABLED(CONFIG_OLG_STATUS_LED)) {
		(void)gpio_pin_configure_dt(&status_led, GPIO_OUTPUT_INACTIVE);
		status_led_startup_blink();
	}

	dotstar_off();
	qspi_flash_deep_power_down();

	return 0;
}

void olg_power_idle(uint32_t ms)
{
	if (ms < 1U) {
		ms = 1U;
	}

	k_sleep(K_MSEC(ms));
}

void olg_power_pulse_status_led(uint32_t ms)
{
	if (!IS_ENABLED(CONFIG_OLG_STATUS_LED) || !gpio_is_ready_dt(&status_led)) {
		return;
	}

	if (ms < 1U) {
		ms = 1U;
	}

	(void)gpio_pin_set_dt(&status_led, 1);
	(void)k_work_reschedule(&status_led_off_work, K_MSEC(ms));
}

void olg_power_status_ok(void)
{
	if (IS_ENABLED(CONFIG_OLG_STATUS_LED) && gpio_is_ready_dt(&status_led)) {
		(void)gpio_pin_set_dt(&status_led, 0);
	}
}

void olg_power_status_fault(void)
{
	if (IS_ENABLED(CONFIG_OLG_STATUS_LED) && gpio_is_ready_dt(&status_led)) {
		(void)gpio_pin_configure_dt(&status_led, GPIO_OUTPUT_ACTIVE);
	}
}
