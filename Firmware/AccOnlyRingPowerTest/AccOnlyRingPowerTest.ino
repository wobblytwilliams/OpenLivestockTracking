/*
  ACC-only power test for ItsyBitsy nRF52840 + hardwired ADXL345.

  Goal:
  - No BLE, no GPS, no SD, no Serial.
  - ADXL345 on I2C at 12.5 Hz, low-power bit set.
  - FIFO stream mode, watermark 25 samples.
  - INT1 wired to GPIO13 wakes the nRF.
  - Drain samples into RAM ring buffer only. No file flush.

  Expected profile:
  - Mostly system-on sleep.
  - A small wake/current peak roughly every 2 seconds
    (25 samples / 12.5 samples per second).
*/

#include <Arduino.h>
#include <Wire.h>
#include "rtos.h"
#include "nrf_gpio.h"

static const uint8_t PIN_ADXL_INT1 = 13;

static const uint32_t I2C_HZ = 400000;
static const uint8_t ADXL_ODR_12_5HZ_LOW_POWER = 0x10 | 0x07;
static const uint8_t ADXL_RANGE_FULL_RES_16G = 0x08 | 0x03;
static const uint8_t ADXL_FIFO_STREAM_WATERMARK_25 = 0x80 | 25;

static const uint8_t REG_DEVID       = 0x00;
static const uint8_t REG_BW_RATE     = 0x2C;
static const uint8_t REG_POWER_CTL   = 0x2D;
static const uint8_t REG_INT_ENABLE  = 0x2E;
static const uint8_t REG_INT_MAP     = 0x2F;
static const uint8_t REG_INT_SOURCE  = 0x30;
static const uint8_t REG_DATA_FORMAT = 0x31;
static const uint8_t REG_DATAX0      = 0x32;
static const uint8_t REG_FIFO_CTL    = 0x38;
static const uint8_t REG_FIFO_STATUS = 0x39;

static const uint8_t INT_OVERRUN   = 1 << 0;
static const uint8_t INT_WATERMARK = 1 << 1;

struct AccSample {
  uint32_t ms;
  int16_t x;
  int16_t y;
  int16_t z;
};

static const uint16_t RING_SAMPLES = 4096; // about 5.5 minutes at 12.5 Hz
static AccSample ring[RING_SAMPLES];
static volatile uint16_t ring_head = 0;
static volatile uint16_t ring_used = 0;
static volatile uint32_t ring_overwrites = 0;

static volatile bool acc_irq = false;
static TaskHandle_t loop_task_handle = nullptr;
static uint8_t acc_addr = 0x53;
static uint32_t samples_stored = 0;
static uint32_t fifo_drains = 0;
static uint32_t i2c_failures = 0;

static void dotstarWriteByte(uint8_t value) {
  for (uint8_t mask = 0x80; mask; mask >>= 1) {
    digitalWrite(PIN_DOTSTAR_DATA, (value & mask) ? HIGH : LOW);
    digitalWrite(PIN_DOTSTAR_CLOCK, HIGH);
    digitalWrite(PIN_DOTSTAR_CLOCK, LOW);
  }
}

static void dotstarOff() {
  pinMode(PIN_DOTSTAR_DATA, OUTPUT);
  pinMode(PIN_DOTSTAR_CLOCK, OUTPUT);
  digitalWrite(PIN_DOTSTAR_DATA, LOW);
  digitalWrite(PIN_DOTSTAR_CLOCK, LOW);

  for (uint8_t i = 0; i < 4; ++i) dotstarWriteByte(0x00);
  dotstarWriteByte(0xE0);
  dotstarWriteByte(0x00);
  dotstarWriteByte(0x00);
  dotstarWriteByte(0x00);
  dotstarWriteByte(0xFF);
  digitalWrite(PIN_DOTSTAR_DATA, LOW);
  digitalWrite(PIN_DOTSTAR_CLOCK, LOW);
}

static void qspiFlashDeepPowerDown() {
  pinMode(PIN_QSPI_CS, OUTPUT);
  pinMode(PIN_QSPI_SCK, OUTPUT);
  pinMode(PIN_QSPI_IO0, OUTPUT);
  digitalWrite(PIN_QSPI_CS, HIGH);
  digitalWrite(PIN_QSPI_SCK, LOW);
  digitalWrite(PIN_QSPI_IO0, LOW);
  delayMicroseconds(2);

  digitalWrite(PIN_QSPI_CS, LOW);
  delayMicroseconds(1);

  const uint8_t command = 0xB9;
  for (uint8_t mask = 0x80; mask; mask >>= 1) {
    digitalWrite(PIN_QSPI_IO0, (command & mask) ? HIGH : LOW);
    digitalWrite(PIN_QSPI_SCK, HIGH);
    delayMicroseconds(1);
    digitalWrite(PIN_QSPI_SCK, LOW);
    delayMicroseconds(1);
  }

  digitalWrite(PIN_QSPI_CS, HIGH);
  delayMicroseconds(5);
  digitalWrite(PIN_QSPI_IO0, LOW);
  nrf_gpio_cfg_default(g_ADigitalPinMap[PIN_QSPI_IO1]);
  nrf_gpio_cfg_default(g_ADigitalPinMap[PIN_QSPI_IO2]);
  nrf_gpio_cfg_default(g_ADigitalPinMap[PIN_QSPI_IO3]);
}

static void stopUnusedPeripherals() {
  NRF_POWER->DCDCEN = POWER_DCDCEN_DCDCEN_Enabled;
#ifdef POWER_DCDCEN0_DCDCEN_Enabled
  NRF_POWER->DCDCEN0 = POWER_DCDCEN0_DCDCEN_Enabled;
#endif

#ifdef NRF_USBD
  NRF_USBD->USBPULLUP = 0;
  NRF_USBD->ENABLE = 0;
  NVIC_DisableIRQ(USBD_IRQn);
#endif

  NRF_UARTE0->TASKS_STOPRX = 1;
  NRF_UARTE0->TASKS_STOPTX = 1;
  NRF_UARTE0->ENABLE = 0;

  NRF_SAADC->TASKS_STOP = 1;
  NRF_SAADC->ENABLE = 0;

  NRF_SPIM0->ENABLE = 0;
  NRF_SPIM1->ENABLE = 0;
  NRF_SPIM2->ENABLE = 0;
  NRF_PWM0->ENABLE = 0;
  NRF_PWM1->ENABLE = 0;
  NRF_PWM2->ENABLE = 0;
  NRF_PWM3->ENABLE = 0;
}

static void parkUnusedGpio() {
  for (uint8_t pin = 0; pin < PINS_COUNT; ++pin) {
    if (pin == LED_BUILTIN ||
        pin == PIN_DOTSTAR_DATA ||
        pin == PIN_DOTSTAR_CLOCK ||
        pin == PIN_QSPI_CS ||
        pin == PIN_QSPI_SCK ||
        pin == PIN_QSPI_IO0 ||
        pin == PIN_QSPI_IO1 ||
        pin == PIN_QSPI_IO2 ||
        pin == PIN_QSPI_IO3 ||
        pin == PIN_WIRE_SDA ||
        pin == PIN_WIRE_SCL ||
        pin == PIN_ADXL_INT1) {
      continue;
    }

    nrf_gpio_cfg_default(g_ADigitalPinMap[pin]);
  }
}

static bool i2cWriteReg(uint8_t addr, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

static bool i2cReadRegs(uint8_t addr, uint8_t reg, uint8_t* dst, uint8_t n) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;

  uint8_t got = Wire.requestFrom(addr, n);
  if (got != n) return false;

  for (uint8_t i = 0; i < n; ++i) dst[i] = Wire.read();
  return true;
}

static bool findAdxl() {
  uint8_t devid = 0;
  if (i2cReadRegs(0x53, REG_DEVID, &devid, 1) && devid == 0xE5) {
    acc_addr = 0x53;
    return true;
  }
  if (i2cReadRegs(0x1D, REG_DEVID, &devid, 1) && devid == 0xE5) {
    acc_addr = 0x1D;
    return true;
  }
  return false;
}

static bool initAdxl() {
  if (!findAdxl()) return false;

  if (!i2cWriteReg(acc_addr, REG_POWER_CTL, 0x00)) return false;
  if (!i2cWriteReg(acc_addr, REG_BW_RATE, ADXL_ODR_12_5HZ_LOW_POWER)) return false;
  if (!i2cWriteReg(acc_addr, REG_DATA_FORMAT, ADXL_RANGE_FULL_RES_16G)) return false;
  if (!i2cWriteReg(acc_addr, REG_FIFO_CTL, ADXL_FIFO_STREAM_WATERMARK_25)) return false;

  // Route watermark/overrun to INT1, then enable them.
  if (!i2cWriteReg(acc_addr, REG_INT_MAP, 0x00)) return false;
  if (!i2cWriteReg(acc_addr, REG_INT_ENABLE, INT_WATERMARK | INT_OVERRUN)) return false;
  if (!i2cWriteReg(acc_addr, REG_POWER_CTL, 0x08)) return false;

  uint8_t src = 0;
  (void)i2cReadRegs(acc_addr, REG_INT_SOURCE, &src, 1);
  return true;
}

static uint8_t fifoEntries() {
  uint8_t status = 0;
  if (!i2cReadRegs(acc_addr, REG_FIFO_STATUS, &status, 1)) {
    i2c_failures++;
    return 0;
  }
  return status & 0x3F;
}

static bool readRawSample(int16_t& x, int16_t& y, int16_t& z) {
  uint8_t b[6] = {0};
  if (!i2cReadRegs(acc_addr, REG_DATAX0, b, sizeof(b))) {
    i2c_failures++;
    return false;
  }

  x = (int16_t)((b[1] << 8) | b[0]);
  y = (int16_t)((b[3] << 8) | b[2]);
  z = (int16_t)((b[5] << 8) | b[4]);
  return true;
}

static void pushSample(uint32_t ms, int16_t x, int16_t y, int16_t z) {
  ring[ring_head] = { ms, x, y, z };
  ring_head = (uint16_t)((ring_head + 1) % RING_SAMPLES);
  if (ring_used < RING_SAMPLES) {
    ring_used++;
  } else {
    ring_overwrites++;
  }
  samples_stored++;
}

static void drainFifoToRing() {
  uint8_t src = 0;
  (void)i2cReadRegs(acc_addr, REG_INT_SOURCE, &src, 1);

  uint8_t n = fifoEntries();
  if (n > 32) n = 32;

  const uint32_t now = millis();
  for (uint8_t i = 0; i < n; ++i) {
    int16_t x, y, z;
    if (!readRawSample(x, y, z)) break;

    uint32_t age_ms = (uint32_t)((uint32_t)(n - 1 - i) * 80U);
    uint32_t sample_ms = (now >= age_ms) ? (now - age_ms) : 0;
    pushSample(sample_ms, x, y, z);
  }

  fifo_drains++;
  (void)src;
}

static void adxlInt1Isr() {
  acc_irq = true;

  if (loop_task_handle) {
    BaseType_t task_woken = pdFALSE;
    vTaskNotifyGiveFromISR(loop_task_handle, &task_woken);
    portYIELD_FROM_ISR(task_woken);
  }
}

static void blink(uint8_t count, uint16_t on_ms, uint16_t off_ms) {
  for (uint8_t i = 0; i < count; ++i) {
    digitalWrite(LED_BUILTIN, HIGH);
    delay(on_ms);
    digitalWrite(LED_BUILTIN, LOW);
    delay(off_ms);
  }
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  dotstarOff();
  qspiFlashDeepPowerDown();
  parkUnusedGpio();

  Wire.begin();
  Wire.setClock(I2C_HZ);

  bool ok = initAdxl();
  if (!ok) {
    // Failure indicator. This intentionally changes current so it is obvious.
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH);
    while (true) waitForEvent();
  }

  loop_task_handle = xTaskGetCurrentTaskHandle();

  stopUnusedPeripherals();

  // One short blink means the ACC was found and the measurement phase starts.
  blink(1, 20, 100);

  pinMode(PIN_ADXL_INT1, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_ADXL_INT1), adxlInt1Isr, RISING);
}

void loop() {
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

  if (acc_irq) {
    acc_irq = false;
    drainFifoToRing();
  }
}
