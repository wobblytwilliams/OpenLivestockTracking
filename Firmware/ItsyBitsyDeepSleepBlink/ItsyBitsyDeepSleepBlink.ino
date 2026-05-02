#include <Arduino.h>
#include "nrf_gpio.h"

enum TestMode : uint8_t {
  TEST_MODE_PERIODIC_IDLE = 0,
  TEST_MODE_SYSTEM_OFF_FLOOR = 1
};

static constexpr TestMode TEST_MODE = TEST_MODE_SYSTEM_OFF_FLOOR;
static constexpr uint32_t BLINK_ON_MS = 25;
static constexpr uint32_t BLINK_PERIOD_MS = 5000;
static constexpr uint8_t STARTUP_BLINKS = 3;

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

static void parkGpio() {
  for (uint8_t pin = 0; pin < PINS_COUNT; ++pin) {
    if (pin == LED_BUILTIN ||
        pin == PIN_DOTSTAR_DATA ||
        pin == PIN_DOTSTAR_CLOCK ||
        pin == PIN_QSPI_CS ||
        pin == PIN_QSPI_SCK ||
        pin == PIN_QSPI_IO0 ||
        pin == PIN_QSPI_IO1 ||
        pin == PIN_QSPI_IO2 ||
        pin == PIN_QSPI_IO3) {
      continue;
    }

    nrf_gpio_cfg_default(g_ADigitalPinMap[pin]);
  }
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
  NRF_TWIM0->ENABLE = 0;
  NRF_TWIM1->ENABLE = 0;
  NRF_PWM0->ENABLE = 0;
  NRF_PWM1->ENABLE = 0;
  NRF_PWM2->ENABLE = 0;
  NRF_PWM3->ENABLE = 0;
}

static void blinkOnce(uint32_t on_ms) {
  digitalWrite(LED_BUILTIN, HIGH);
  delay(on_ms);
  digitalWrite(LED_BUILTIN, LOW);
}

static void lowPowerDelay(uint32_t ms) {
  const uint32_t start = millis();
  while ((uint32_t)(millis() - start) < ms) {
    waitForEvent();
  }
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  dotstarOff();
  qspiFlashDeepPowerDown();
  parkGpio();
  stopUnusedPeripherals();

  for (uint8_t i = 0; i < STARTUP_BLINKS; ++i) {
    blinkOnce(60);
    delay(180);
  }

  if (TEST_MODE == TEST_MODE_SYSTEM_OFF_FLOOR) {
    digitalWrite(LED_BUILTIN, LOW);
    systemOff(PIN_BUTTON1, LOW);
  }
}

void loop() {
  blinkOnce(BLINK_ON_MS);
  lowPowerDelay(BLINK_PERIOD_MS - BLINK_ON_MS);
}
