#include "olg_adxl345.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/atomic.h>

#include "olg_config.h"
#include "olg_ring.h"

#define ADXL_DEVID_VALUE 0xe5

#define REG_DEVID       0x00
#define REG_BW_RATE     0x2c
#define REG_POWER_CTL   0x2d
#define REG_INT_ENABLE  0x2e
#define REG_INT_MAP     0x2f
#define REG_INT_SOURCE  0x30
#define REG_DATA_FORMAT 0x31
#define REG_DATAX0      0x32
#define REG_FIFO_CTL    0x38
#define REG_FIFO_STATUS 0x39

#define INT_OVERRUN     BIT(0)
#define INT_WATERMARK   BIT(1)

#define DATA_FORMAT_FULL_RES BIT(3)
#define DATA_FORMAT_RANGE_2G  0x00
#define DATA_FORMAT_RANGE_4G  0x01
#define DATA_FORMAT_RANGE_8G  0x02
#define DATA_FORMAT_RANGE_16G 0x03
#define POWER_CTL_MEASURE    BIT(3)
#define FIFO_STREAM_MODE     0x80

#define ADXL_INT_NODE DT_ALIAS(olg_adxl_int)

static const struct device *const i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c0));
static const struct gpio_dt_spec int_gpio = GPIO_DT_SPEC_GET(ADXL_INT_NODE, gpios);
static struct gpio_callback int_cb;
static struct k_work drain_work;

static uint8_t adxl_addr;
static atomic_t sample_count;
static atomic_t drain_count;
static atomic_t i2c_failures;

static int configure_fifo_interrupt(gpio_flags_t flags)
{
	return gpio_pin_interrupt_configure_dt(&int_gpio, flags);
}

static void resume_i2c(void)
{
	if (IS_ENABLED(CONFIG_PM_DEVICE)) {
		(void)pm_device_action_run(i2c_dev, PM_DEVICE_ACTION_RESUME);
	}
}

static void suspend_i2c(void)
{
	if (IS_ENABLED(CONFIG_PM_DEVICE)) {
		(void)pm_device_action_run(i2c_dev, PM_DEVICE_ACTION_SUSPEND);
	}
}

static int write_reg(uint8_t reg, uint8_t value)
{
	uint8_t data[2] = { reg, value };
	int err = i2c_write(i2c_dev, data, sizeof(data), adxl_addr);

	if (err) {
		atomic_inc(&i2c_failures);
	}

	return err;
}

static int read_regs(uint8_t reg, uint8_t *dst, size_t len)
{
	int err = i2c_write_read(i2c_dev, adxl_addr, &reg, 1, dst, len);

	if (err) {
		atomic_inc(&i2c_failures);
	}

	return err;
}

static int read_reg_at(uint8_t addr, uint8_t reg, uint8_t *value)
{
	int err = i2c_write_read(i2c_dev, addr, &reg, 1, value, 1);

	if (err) {
		atomic_inc(&i2c_failures);
	}

	return err;
}

static uint8_t bw_rate_for_millihz(uint32_t mhz)
{
	if (mhz <= 100) {
		return 0x00;
	}
	if (mhz <= 200) {
		return 0x01;
	}
	if (mhz <= 390) {
		return 0x02;
	}
	if (mhz <= 780) {
		return 0x03;
	}
	if (mhz <= 1560) {
		return 0x04;
	}
	if (mhz <= 3130) {
		return 0x05;
	}
	if (mhz <= 6250) {
		return 0x06;
	}
	if (mhz <= 12500) {
		return 0x07;
	}
	if (mhz <= 25000) {
		return 0x08;
	}
	if (mhz <= 50000) {
		return 0x09;
	}
	if (mhz <= 100000) {
		return 0x0a;
	}
	if (mhz <= 200000) {
		return 0x0b;
	}
	if (mhz <= 400000) {
		return 0x0c;
	}
	if (mhz <= 800000) {
		return 0x0d;
	}
	if (mhz <= 1600000) {
		return 0x0e;
	}

	return 0x0f;
}

static uint8_t data_format_range_bits(uint8_t range_g)
{
	switch (range_g) {
	case 2:
		return DATA_FORMAT_RANGE_2G;
	case 4:
		return DATA_FORMAT_RANGE_4G;
	case 8:
		return DATA_FORMAT_RANGE_8G;
	case 16:
	default:
		return DATA_FORMAT_RANGE_16G;
	}
}

static uint8_t fifo_watermark(void)
{
	if (CONFIG_OLG_ACC_FIFO_WATERMARK < 1) {
		return 1;
	}
	if (CONFIG_OLG_ACC_FIFO_WATERMARK > 31) {
		return 31;
	}

	return CONFIG_OLG_ACC_FIFO_WATERMARK;
}

static int find_adxl(void)
{
	uint8_t devid = 0;

	if (!read_reg_at(CONFIG_OLG_ACC_I2C_ADDR_PRIMARY, REG_DEVID, &devid) &&
	    devid == ADXL_DEVID_VALUE) {
		adxl_addr = CONFIG_OLG_ACC_I2C_ADDR_PRIMARY;
		return 0;
	}

	if (!read_reg_at(CONFIG_OLG_ACC_I2C_ADDR_SECONDARY, REG_DEVID, &devid) &&
	    devid == ADXL_DEVID_VALUE) {
		adxl_addr = CONFIG_OLG_ACC_I2C_ADDR_SECONDARY;
		return 0;
	}

	return -ENODEV;
}

static uint8_t fifo_entries(void)
{
	uint8_t status = 0;

	if (read_regs(REG_FIFO_STATUS, &status, 1)) {
		return 0;
	}

	return status & 0x3f;
}

static int read_raw_sample(int16_t *x, int16_t *y, int16_t *z)
{
	uint8_t raw[6];
	int err = read_regs(REG_DATAX0, raw, sizeof(raw));

	if (err) {
		return err;
	}

	*x = (int16_t)((raw[1] << 8) | raw[0]);
	*y = (int16_t)((raw[3] << 8) | raw[2]);
	*z = (int16_t)((raw[5] << 8) | raw[4]);
	return 0;
}

static void drain_fifo(struct k_work *work)
{
	ARG_UNUSED(work);

	resume_i2c();

	uint8_t src = 0;
	(void)read_regs(REG_INT_SOURCE, &src, 1);

	uint8_t entries = fifo_entries();
	if (entries > 32) {
		entries = 32;
	}

	const uint32_t now = k_uptime_get_32();
	const struct olg_config *cfg = olg_config_get();
	const uint32_t sample_period_ms =
		cfg->acc_odr_millihz > 0U ? (1000000U / cfg->acc_odr_millihz) : 0U;

	for (uint8_t i = 0; i < entries; i++) {
		int16_t x;
		int16_t y;
		int16_t z;

		if (read_raw_sample(&x, &y, &z)) {
			break;
		}

		uint32_t sample_ms = now;
		if (sample_period_ms > 0) {
			uint32_t age_ms = (uint32_t)(entries - 1U - i) * sample_period_ms;
			sample_ms = now >= age_ms ? now - age_ms : 0;
		}

		(void)olg_ring_push_acc(sample_ms, x, y, z);
		atomic_inc(&sample_count);
	}

	(void)read_regs(REG_INT_SOURCE, &src, 1);
	atomic_inc(&drain_count);
	suspend_i2c();
	(void)configure_fifo_interrupt(GPIO_INT_LEVEL_ACTIVE);
}

static void gpio_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	(void)configure_fifo_interrupt(GPIO_INT_DISABLE);
	(void)k_work_submit(&drain_work);
}

int olg_adxl345_init(void)
{
	if (!IS_ENABLED(CONFIG_OLG_ACC_ENABLE)) {
		return 0;
	}

	const struct olg_config *cfg = olg_config_get();
	if (!cfg->acc_enabled) {
		return 0;
	}

	if (!device_is_ready(i2c_dev) || !gpio_is_ready_dt(&int_gpio)) {
		return -ENODEV;
	}

	resume_i2c();

	int err = find_adxl();
	if (err) {
		suspend_i2c();
		return err;
	}

	k_work_init(&drain_work, drain_fifo);

	uint8_t bw_rate = bw_rate_for_millihz(cfg->acc_odr_millihz);
	if (bw_rate >= 0x07 && bw_rate <= 0x0c) {
		bw_rate |= BIT(4);
	}

	uint8_t watermark = fifo_watermark();

	err = write_reg(REG_POWER_CTL, 0x00);
	err |= write_reg(REG_BW_RATE, bw_rate);
	err |= write_reg(REG_DATA_FORMAT, DATA_FORMAT_FULL_RES |
					      data_format_range_bits(cfg->acc_range_g));
	err |= write_reg(REG_FIFO_CTL, FIFO_STREAM_MODE | watermark);
	err |= write_reg(REG_INT_MAP, 0x00);
	err |= write_reg(REG_INT_ENABLE, INT_WATERMARK | INT_OVERRUN);
	err |= write_reg(REG_POWER_CTL, POWER_CTL_MEASURE);
	if (err) {
		suspend_i2c();
		return err;
	}

	uint8_t src = 0;
	(void)read_regs(REG_INT_SOURCE, &src, 1);

	err = gpio_pin_configure_dt(&int_gpio, GPIO_INPUT);
	if (err) {
		suspend_i2c();
		return err;
	}

	gpio_init_callback(&int_cb, gpio_isr, BIT(int_gpio.pin));
	err = gpio_add_callback(int_gpio.port, &int_cb);
	if (err) {
		suspend_i2c();
		return err;
	}

	err = configure_fifo_interrupt(GPIO_INT_LEVEL_ACTIVE);
	if (!err) {
		(void)k_work_submit(&drain_work);
	}

	suspend_i2c();
	return err;
}

uint32_t olg_adxl345_sample_count(void)
{
	return (uint32_t)atomic_get(&sample_count);
}

uint32_t olg_adxl345_drain_count(void)
{
	return (uint32_t)atomic_get(&drain_count);
}

uint32_t olg_adxl345_i2c_failures(void)
{
	return (uint32_t)atomic_get(&i2c_failures);
}
