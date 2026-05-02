/*
  Logger (ACC + GPS + BLE) with:
  - ACC via ADXL345 FIFO watermark interrupt (DATA READY style) to wake MCU
  - GPS NON-BLOCKING state machine (bounded per-loop time)
  - GPS software standby when not busy (UBX-RXM-PMREQ backup + force, UART RX wake)
  - BLE scan windows (10s every 60s by default), continues during GPS attempts
  - SD flush: threshold-first + emerg + failsafe, but flush is BURSTED (bounded events per loop)
  - Policy: block SD flush while gps_busy==1 except EMERG; FAILSAFE waits for GPS
  - CPU sleep-when-idle via sd_app_evt_wait() (SoftDevice)

  IMPORTANT:
  - Debug printing is gated behind CFG.debug_enable.
    If debug_enable=false, USB Serial is NOT started and status printing is effectively off.

  NOTE:
  - This sketch assumes ADXL345 INT1 is wired to PIN_ADXL_INT1 (default GPIO 13 here).
  - Uses ADXL345 FIFO stream mode + watermark interrupt.
  - Uses AX3-style fifo_watermark=25 for lower MCU wake duty at 12.5 Hz.
*/

#include <Arduino.h>
#include <stdint.h>
#include <Wire.h>
#include <SPI.h>
#include <SdFat.h>
#include <bluefruit.h>
#include "nrf_gpio.h"

#if defined(SOFTDEVICE_PRESENT)
  #include <nrf_soc.h>   // sd_app_evt_wait()
  #include <nrf_sdm.h>   // sd_softdevice_disable()
#endif

#define POWER_TEST_NORMAL            0
#define POWER_TEST_MCU_IDLE_ONLY     1
#define POWER_TEST_GPS_STANDBY_ONLY  2
#define POWER_TEST_LOGGER_NO_GPS     3
#define POWER_TEST_LOGGER_GPS_ASLEEP 4
#define POWER_TEST_GPS_ASLEEP_NO_SD  5
#define POWER_TEST_GPS_ASLEEP_NO_BLE 6
#define POWER_TEST_GPS_ASLEEP_NO_ACC 7
#define POWER_TEST_GPS_CYCLE_NO_SD_NO_ACC 8
#define POWER_TEST_GPS_STANDBY_RELEASE_UART 9
#define POWER_TEST_GPS_GATE_ACC_BLE_SD_3MIN 10

#ifndef POWER_TEST_MODE
  #define POWER_TEST_MODE POWER_TEST_NORMAL
#endif

#ifndef BLE_DISABLE_STACK_BETWEEN_WINDOWS
  #define BLE_DISABLE_STACK_BETWEEN_WINDOWS 0
#endif

// ===============================
// CONFIG
// ===============================
struct Config {
  // Ring
  float    ring_alloc_fraction_of_free_heap = 0.80f;
  float    ring_normal_threshold_fraction   = 0.95f;
  float    ring_emerg_threshold_fraction    = 0.98f;

  // Status / failsafe
  uint32_t status_print_ms   = 1000;
  uint32_t flush_first_ms    = 0;       // disabled for normal low-power logging
  uint32_t flush_failsafe_ms = 900000;  // 15 min since last SUCCESSFUL flush

  // Board power housekeeping
  bool     board_power_cleanup = true;
  bool     usb_disable_when_not_debug = true;

  // SD
  bool     sd_enabled = true;
  bool     sd_close_between_flushes = true;
  bool     sd_send_cmd0_on_sleep = true;
  bool     sd_release_spi_pins_on_sleep = true;
  int8_t   sd_power_en_pin = -1;          // set if the board can switch SD power
  bool     sd_power_en_active_high = true;
  uint8_t  sd_cs_pin = 10;
  uint32_t sd_spi_hz = 12000000;
  uint32_t sd_retry_backoff_ms = 300000;  // avoid tight retry loops after SD failures

  // GPS
  bool     gps_enabled      = true;
  uint32_t gps_interval_ms  = 180000;  // 3 min
  uint32_t gps_timeout_ms   = 15000;   // attempt duration
  uint32_t gps_baud         = 9600;
  uint8_t  gps_min_sats     = 4;
  float    gps_min_hdop     = 2.5f;

  // GPS software standby.
  bool     gps_backup_sleep_enabled = true;
  uint32_t gps_wake_settle_ms       = 250;
  uint32_t gps_sleep_refresh_ms     = 30000;
  bool     gps_release_uart_pins_after_sleep = false;

  // BLE
  bool     ble_enabled          = true;
  bool     ble_log_enabled      = true;
  uint32_t ble_period_ms        = 60000;
  uint32_t ble_window_ms        = 10000;
  bool     ble_dedup_per_window = true;
  uint16_t ble_dedup_max_macs   = 128;
  int8_t   ble_tx_power_dbm     = 0;
  bool     ble_central_only     = true;  // scanner-only; no advertising/peripheral role
  bool     ble_conn_led_enabled = false; // LED blink costs power during scan windows
  bool     ble_disable_stack_between_windows = BLE_DISABLE_STACK_BETWEEN_WINDOWS;

  // BLE scan duty within the scan window
  float    ble_scan_duty        = 0.10f;  // 0.05 to 1.0 is a sensible test range
  uint32_t ble_scan_interval_ms = 100;

  // ACC
  bool     acc_enabled   = true;
  float    acc_odr_hz    = 12.5f;
  uint8_t  acc_range_g   = 16;
  bool     acc_low_power = true;
  uint32_t i2c_hz        = 400000;

  // ADXL345 FIFO / INT
  uint8_t  fifo_watermark = 25;   // 25 samples at 12.5 Hz = wake roughly every 2 seconds
  bool     fifo_int_active_high = true;  // ADXL345 INT is active high by default

  // Loop budgets (keep BLE stable)
  uint32_t gps_poll_time_budget_us   = 1200;
  uint32_t gps_poll_byte_budget      = 96;
  uint32_t sd_flush_max_events_burst = 200;

  // Debug
  bool     debug_enable         = false;
  bool     debug_print_gps_fix  = false;
};

static Config CFG;

// ===============================
// PINS
// ===============================
static const uint8_t PIN_ADXL_INT1 = 13;   // ADXL345 INT1 wired to GPIO13

// ===============================
// SD
// ===============================
SdFat sd;
SdFile fAcc, fGps, fBle;
static bool sd_ok = false;
static bool sd_files_open = false;
static bool sd_startup_failed = false;
static uint32_t sd_write_fail = 0;
static uint32_t sd_next_retry_ms = 0;
static uint32_t ring_bad_events = 0;

// ===============================
// Ring buffer TLV
// ===============================
enum EvType : uint8_t { EVT_ACC = 1, EVT_GPS = 2, EVT_BLE = 3 };
enum FlushWhy : uint8_t { FLUSH_NORMAL = 1, FLUSH_EMERG = 2, FLUSH_FAILSAFE = 3 };

struct Ring {
  uint8_t*  buf = nullptr;
  uint32_t  cap = 0;
  uint32_t  head = 0;
  uint32_t  tail = 0;
  uint32_t  used = 0;
  uint32_t  drops = 0;
  uint32_t  normal_th = 0;
  uint32_t  emerg_th  = 0;
};

static Ring RB;

extern "C" char* sbrk(int);
static uint32_t approx_free_heap_bytes() {
  char stack_dummy;
  char* heap_end = sbrk(0);
  return (uint32_t)(&stack_dummy - heap_end);
}
static inline uint32_t rb_free() { return RB.cap - RB.used; }

static inline void rb_write_u8(uint8_t v) {
  RB.buf[RB.head] = v;
  RB.head = (RB.head + 1) % RB.cap;
  RB.used++;
}
static inline uint8_t rb_read_u8() {
  uint8_t v = RB.buf[RB.tail];
  RB.tail = (RB.tail + 1) % RB.cap;
  RB.used--;
  return v;
}
static inline uint8_t rb_peek_u8(uint32_t offset) {
  return RB.buf[(RB.tail + offset) % RB.cap];
}
static void rb_discard_bytes(uint32_t n) {
  if (n > RB.used) n = RB.used;
  RB.tail = (RB.tail + n) % RB.cap;
  RB.used -= n;
}

// TLV: [type:1][len:1][payload:len]
static bool rb_push_event(EvType type, const uint8_t* payload, uint8_t len) {
  const uint32_t need = 2 + len;
  if (!RB.buf || RB.cap < need) return false;
  if (rb_free() < need) { RB.drops++; return false; }
  rb_write_u8((uint8_t)type);
  rb_write_u8(len);
  for (uint8_t i = 0; i < len; i++) rb_write_u8(payload[i]);
  return true;
}

static bool rb_peek_event(uint8_t& out_type, uint8_t& out_len, uint8_t* out_payload, uint8_t out_payload_max) {
  if (RB.used < 2) return false;
  out_type = rb_peek_u8(0);
  out_len  = rb_peek_u8(1);
  if ((uint32_t)(2 + out_len) > RB.used) return false;
  if (out_len > out_payload_max) return false;
  for (uint8_t i = 0; i < out_len; i++) out_payload[i] = rb_peek_u8(2 + i);
  return true;
}

// ===============================
// Schedulers
// ===============================
static bool in_window(uint32_t now_ms, uint32_t period_ms, uint32_t window_ms) {
  if (period_ms == 0) return false;
  return ((now_ms % period_ms) < window_ms);
}

// ===============================
// ADXL345 (I2C) + FIFO watermark interrupt
// ===============================
static bool acc_present = false;
static uint8_t acc_addr = 0x53;
static uint32_t acc_ok = 0;
static uint32_t acc_fail = 0;

static volatile uint32_t irq_isr = 0;
static volatile uint32_t irq_handled = 0;
static volatile uint8_t  last_int_src = 0;
static volatile uint32_t last_drained = 0;

static bool i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}
static bool i2c_read_regs(uint8_t addr, uint8_t reg, uint8_t* dst, uint8_t n) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  uint8_t got = Wire.requestFrom(addr, n);
  if (got != n) return false;
  for (uint8_t i = 0; i < n; i++) dst[i] = Wire.read();
  return true;
}

static uint8_t adxl_bw_rate_for_odr(float hz) {
  if (hz <= 0.10f) return 0x00;
  if (hz <= 0.20f) return 0x01;
  if (hz <= 0.39f) return 0x02;
  if (hz <= 0.78f) return 0x03;
  if (hz <= 1.56f) return 0x04;
  if (hz <= 3.13f) return 0x05;
  if (hz <= 6.25f) return 0x06;
  if (hz <= 12.5f) return 0x07;
  if (hz <= 25.0f) return 0x08;
  if (hz <= 50.0f) return 0x09;
  if (hz <= 100.0f) return 0x0A;
  if (hz <= 200.0f) return 0x0B;
  if (hz <= 400.0f) return 0x0C;
  if (hz <= 800.0f) return 0x0D;
  if (hz <= 1600.0f) return 0x0E;
  return 0x0F;
}
static uint8_t adxl_dataformat_for_range(uint8_t g) {
  uint8_t r = (g <= 2) ? 0 : (g <= 4) ? 1 : (g <= 8) ? 2 : 3;
  return (1 << 3) | (r & 0x03); // FULL_RES + range
}

// ADXL345 register map
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

// INT bits
static const uint8_t INT_WATERMARK   = 1 << 1; // Watermark
static const uint8_t INT_OVERRUN     = 1 << 0; // Overrun

static bool acc_find_addr() {
  uint8_t devid = 0;
  if (i2c_read_regs(0x53, REG_DEVID, &devid, 1) && devid == 0xE5) { acc_addr = 0x53; return true; }
  if (i2c_read_regs(0x1D, REG_DEVID, &devid, 1) && devid == 0xE5) { acc_addr = 0x1D; return true; }
  return false;
}

static bool acc_init_fifo_int() {
  if (!acc_find_addr()) { acc_present = false; return false; }
  acc_present = true;

  // Standby
  i2c_write_reg(acc_addr, REG_POWER_CTL, 0x00);

  // ODR + range
  uint8_t bw_rate = adxl_bw_rate_for_odr(CFG.acc_odr_hz);
  if (CFG.acc_low_power && bw_rate >= 0x07 && bw_rate <= 0x0C) bw_rate |= 0x10;
  i2c_write_reg(acc_addr, REG_BW_RATE, bw_rate);
  i2c_write_reg(acc_addr, REG_DATA_FORMAT, adxl_dataformat_for_range(CFG.acc_range_g));

  // FIFO stream mode + watermark. ADXL345 uses the lower 5 bits as the sample count.
  uint8_t wm = CFG.fifo_watermark;
  if (wm < 1) wm = 1;
  if (wm > 31) wm = 31; // keep safe margin below full FIFO
  uint8_t fifo_ctl = (0x02 << 6) | (wm & 0x1F); // stream mode=2
  i2c_write_reg(acc_addr, REG_FIFO_CTL, fifo_ctl);

  // Route watermark interrupt to INT1 (INT_MAP bit=0 -> INT1)
  // INT_MAP bit position for watermark is bit1; leave it 0 => INT1
  i2c_write_reg(acc_addr, REG_INT_MAP, 0x00);

  // Enable watermark + overrun interrupt
  i2c_write_reg(acc_addr, REG_INT_ENABLE, (uint8_t)(INT_WATERMARK | INT_OVERRUN));

  // Measurement mode
  i2c_write_reg(acc_addr, REG_POWER_CTL, 0x08);

  // Clear any pending interrupt by reading INT_SOURCE
  uint8_t src = 0;
  (void)i2c_read_regs(acc_addr, REG_INT_SOURCE, &src, 1);

  return true;
}

static uint8_t acc_read_int_source() {
  uint8_t src = 0;
  (void)i2c_read_regs(acc_addr, REG_INT_SOURCE, &src, 1);
  return src;
}

static uint8_t acc_fifo_entries() {
  uint8_t st = 0;
  if (!i2c_read_regs(acc_addr, REG_FIFO_STATUS, &st, 1)) return 0;
  return (uint8_t)(st & 0x3F);
}

static bool acc_read_raw(int16_t& x, int16_t& y, int16_t& z) {
  uint8_t b[6];
  if (!i2c_read_regs(acc_addr, REG_DATAX0, b, 6)) return false;
  x = (int16_t)((b[1] << 8) | b[0]);
  y = (int16_t)((b[3] << 8) | b[2]);
  z = (int16_t)((b[5] << 8) | b[4]);
  return true;
}

// ACC payload: uint32 ms + raw int16 x/y/z.
// Convert to g only when writing CSV so the wake path stays short.
static void acc_push_sample(uint32_t ms, int16_t x, int16_t y, int16_t z) {
  uint8_t p[10];
  memcpy(p + 0,  &ms, 4);
  memcpy(p + 4,  &x,  2);
  memcpy(p + 6,  &y,  2);
  memcpy(p + 8,  &z,  2);
  rb_push_event(EVT_ACC, p, sizeof(p));
}

static void acc_drain_fifo_to_ring(uint32_t ms_now) {
  if (!acc_present) return;

  uint8_t src = acc_read_int_source();
  last_int_src = src;

  uint8_t n = acc_fifo_entries();
  uint32_t drained = 0;

  const uint32_t sample_period_ms = (CFG.acc_odr_hz > 0.0f) ? (uint32_t)((1000.0f / CFG.acc_odr_hz) + 0.5f) : 0;

  for (uint8_t i = 0; i < n; i++) {
    int16_t rx, ry, rz;
    if (!acc_read_raw(rx, ry, rz)) { acc_fail++; break; }

    acc_ok++;
    uint32_t sample_ms = ms_now;
    if (sample_period_ms > 0) {
      uint32_t sample_age_ms = (uint32_t)(n - 1 - i) * sample_period_ms;
      sample_ms = (ms_now >= sample_age_ms) ? (ms_now - sample_age_ms) : 0;
    }
    acc_push_sample(sample_ms, rx, ry, rz);
    drained++;
  }

  last_drained = drained;
}

static volatile bool acc_irq_flag = false;

static void status_led_set(bool on);

static void status_led_init() {
#ifdef LED_BUILTIN
  pinMode(LED_BUILTIN, OUTPUT);
  status_led_set(false);
#endif
}

static void status_led_set(bool on) {
#ifdef LED_BUILTIN
  #ifdef LED_STATE_ON
    digitalWrite(LED_BUILTIN, on ? LED_STATE_ON : !LED_STATE_ON);
  #else
    digitalWrite(LED_BUILTIN, on ? HIGH : LOW);
  #endif
#else
  (void)on;
#endif
}

// ===============================
// Board power housekeeping
// ===============================
static void dotstar_write_byte(uint8_t value) {
#if defined(PIN_DOTSTAR_DATA) && defined(PIN_DOTSTAR_CLOCK)
  for (uint8_t mask = 0x80; mask; mask >>= 1) {
    digitalWrite(PIN_DOTSTAR_DATA, (value & mask) ? HIGH : LOW);
    digitalWrite(PIN_DOTSTAR_CLOCK, HIGH);
    digitalWrite(PIN_DOTSTAR_CLOCK, LOW);
  }
#else
  (void)value;
#endif
}

static void dotstar_off() {
#if defined(PIN_DOTSTAR_DATA) && defined(PIN_DOTSTAR_CLOCK)
  pinMode(PIN_DOTSTAR_DATA, OUTPUT);
  pinMode(PIN_DOTSTAR_CLOCK, OUTPUT);
  digitalWrite(PIN_DOTSTAR_DATA, LOW);
  digitalWrite(PIN_DOTSTAR_CLOCK, LOW);

  for (uint8_t i = 0; i < 4; i++) dotstar_write_byte(0x00);
  dotstar_write_byte(0xE0);
  dotstar_write_byte(0x00);
  dotstar_write_byte(0x00);
  dotstar_write_byte(0x00);
  dotstar_write_byte(0xFF);

  digitalWrite(PIN_DOTSTAR_DATA, LOW);
  digitalWrite(PIN_DOTSTAR_CLOCK, LOW);
#endif
}

static void qspi_flash_deep_power_down() {
#if defined(PIN_QSPI_CS) && defined(PIN_QSPI_SCK) && defined(PIN_QSPI_IO0)
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

#if defined(PIN_QSPI_IO1)
  nrf_gpio_cfg_default(g_ADigitalPinMap[PIN_QSPI_IO1]);
#endif
#if defined(PIN_QSPI_IO2)
  nrf_gpio_cfg_default(g_ADigitalPinMap[PIN_QSPI_IO2]);
#endif
#if defined(PIN_QSPI_IO3)
  nrf_gpio_cfg_default(g_ADigitalPinMap[PIN_QSPI_IO3]);
#endif
#endif
}

static void board_power_cleanup() {
  if (!CFG.board_power_cleanup) return;

#ifdef NRF_POWER
  NRF_POWER->DCDCEN = POWER_DCDCEN_DCDCEN_Enabled;
#ifdef POWER_DCDCEN0_DCDCEN_Enabled
  NRF_POWER->DCDCEN0 = POWER_DCDCEN0_DCDCEN_Enabled;
#endif
#endif

  dotstar_off();
  qspi_flash_deep_power_down();

#ifdef NRF_SAADC
  NRF_SAADC->TASKS_STOP = 1;
  NRF_SAADC->ENABLE = 0;
#endif

#ifdef NRF_USBD
  if (!CFG.debug_enable && CFG.usb_disable_when_not_debug) {
    NRF_USBD->USBPULLUP = 0;
    NRF_USBD->ENABLE = 0;
    NVIC_DisableIRQ(USBD_IRQn);
  }
#endif
}

static void adxl_int1_isr() {
  irq_isr++;
  acc_irq_flag = true;
}

// ===============================
// BLE (scan + dedup)
// ===============================
static bool ble_scanning = false;
static bool ble_stack_started = false;
static bool softdevice_active = false;
static volatile bool ble_resume_allowed = false;
static volatile uint32_t ble_resume_suppressed = 0;
static volatile uint32_t ble_resume_fail = 0;

struct BleQueued {
  uint32_t ms;
  uint8_t mac[6];
  int8_t rssi;
};

static const uint8_t BLE_QUEUE_CAP = 64;
static BleQueued ble_queue[BLE_QUEUE_CAP];
static volatile uint8_t ble_q_head = 0;
static volatile uint8_t ble_q_tail = 0;
static volatile uint32_t ble_q_drops = 0;

static bool ble_queue_pop(BleQueued& out);

struct Mac6 { uint8_t b[6]; };
static Mac6* dedup_macs = nullptr;
static uint16_t dedup_count = 0;
static uint16_t dedup_cap = 0;
static uint32_t dedup_window_id = 0;

static bool mac_equal(const uint8_t* a, const uint8_t* b) {
  for (int i = 0; i < 6; i++) if (a[i] != b[i]) return false;
  return true;
}
static void dedup_reset(uint32_t window_id) {
  dedup_window_id = window_id;
  dedup_count = 0;
}
static bool dedup_seen_or_add(const uint8_t mac[6]) {
  if (!CFG.ble_dedup_per_window) return false;
  for (uint16_t i = 0; i < dedup_count; i++) {
    if (mac_equal(dedup_macs[i].b, mac)) return true;
  }
  if (dedup_count < dedup_cap) {
    memcpy(dedup_macs[dedup_count].b, mac, 6);
    dedup_count++;
  }
  return false;
}

static bool ble_queue_push(uint32_t ms, const uint8_t mac[6], int8_t rssi) {
  uint8_t head = ble_q_head;
  uint8_t next = (uint8_t)((head + 1) % BLE_QUEUE_CAP);
  if (next == ble_q_tail) {
    ble_q_drops++;
    return false;
  }

  ble_queue[head].ms = ms;
  memcpy(ble_queue[head].mac, mac, 6);
  ble_queue[head].rssi = rssi;
  ble_q_head = next;
  return true;
}

static bool ble_queue_pop(BleQueued& out) {
  uint8_t tail = ble_q_tail;
  if (tail == ble_q_head) return false;

  out = ble_queue[tail];
  ble_q_tail = (uint8_t)((tail + 1) % BLE_QUEUE_CAP);
  return true;
}

static uint8_t ble_queue_used() {
  uint8_t head = ble_q_head;
  uint8_t tail = ble_q_tail;
  return (head >= tail) ? (head - tail) : (BLE_QUEUE_CAP + head - tail);
}

// BLE payload: uint32 ms + 6 mac + int8 rssi
static void ble_push(uint32_t ms, const uint8_t mac[6], int8_t rssi) {
  uint8_t p[11];
  memcpy(p + 0, &ms, 4);
  memcpy(p + 4, mac, 6);
  memcpy(p + 10, &rssi, 1);
  rb_push_event(EVT_BLE, p, sizeof(p));
}

static void scan_callback(ble_gap_evt_adv_report_t* report) {
  uint8_t mac[6];
  memcpy(mac, report->peer_addr.addr, 6);
  int8_t rssi = report->rssi;

  uint32_t now = millis();
  (void)ble_queue_push(now, mac, rssi);

  if (ble_resume_allowed && in_window(now, CFG.ble_period_ms, CFG.ble_window_ms)) {
    if (!Bluefruit.Scanner.resume()) ble_resume_fail++;
  } else {
    ble_resume_suppressed++;
  }
}

static void ble_process_queue() {
  BleQueued ev;
  uint8_t processed = 0;
  while (processed < BLE_QUEUE_CAP && ble_queue_pop(ev)) {
    if (CFG.ble_dedup_per_window && CFG.ble_period_ms > 0) {
      uint32_t window_id = ev.ms / CFG.ble_period_ms;
      if (window_id != dedup_window_id) dedup_reset(window_id);
    }
    if (!dedup_seen_or_add(ev.mac)) ble_push(ev.ms, ev.mac, ev.rssi);
    processed++;
  }
}

// Convert ms to 0.625 ms units (scanner API units)
static uint16_t ms_to_scan_units_0p625(uint32_t ms) {
  uint32_t units = (ms * 1600UL + 999UL) / 1000UL; // ceil
  if (units < 4) units = 4;
  if (units > 0xFFFF) units = 0xFFFF;
  return (uint16_t)units;
}

static bool ble_begin_stack() {
  if (ble_stack_started) return true;

  Bluefruit.autoConnLed(CFG.ble_conn_led_enabled && !sd_startup_failed);
  bool ok = false;
  if (CFG.ble_central_only) {
    Bluefruit.configCentralBandwidth(BANDWIDTH_LOW);
    ok = Bluefruit.begin(0, 1);
  } else {
    ok = Bluefruit.begin();
  }
  if (!ok) return false;

  softdevice_active = true;
  ble_stack_started = true;
  Bluefruit.autoConnLed(CFG.ble_conn_led_enabled && !sd_startup_failed);
  Bluefruit.setTxPower(CFG.ble_tx_power_dbm);
  Bluefruit.setName("Logger");
  status_led_set(sd_startup_failed);
  return true;
}

static void ble_shutdown_stack() {
  if (!ble_stack_started) return;

  ble_resume_allowed = false;
  if (ble_scanning) {
    Bluefruit.Scanner.stop();
    ble_scanning = false;
  }

#if defined(SOFTDEVICE_PRESENT)
  uint8_t sd_enabled = 0;
  (void)sd_softdevice_is_enabled(&sd_enabled);
  if (sd_enabled) {
    (void)sd_softdevice_disable();
  }
  NVIC_DisableIRQ(SD_EVT_IRQn);
#endif

  ble_stack_started = false;
  softdevice_active = false;
}

static void ble_start_scan() {
  if (!ble_begin_stack()) return;
  if (ble_scanning) return;

  Bluefruit.Scanner.setRxCallback(scan_callback);
  Bluefruit.Scanner.restartOnDisconnect(false);
  Bluefruit.Scanner.filterRssi(-100);

  float duty = CFG.ble_scan_duty;
  if (duty < 0.01f) duty = 0.01f;
  if (duty > 1.00f) duty = 1.00f;

  uint32_t interval_ms = CFG.ble_scan_interval_ms;
  if (interval_ms < 20) interval_ms = 20;

  uint32_t window_ms = (uint32_t)((float)interval_ms * duty);
  if (window_ms < 2) window_ms = 2;
  if (window_ms > interval_ms) window_ms = interval_ms;

  uint16_t interval_units = ms_to_scan_units_0p625(interval_ms);
  uint16_t window_units   = ms_to_scan_units_0p625(window_ms);
  if (window_units > interval_units) window_units = interval_units;

  Bluefruit.Scanner.setInterval(interval_units, window_units);
  Bluefruit.Scanner.useActiveScan(false);
  ble_resume_allowed = true;
  if (Bluefruit.Scanner.start(0)) {
    ble_scanning = true;
  } else {
    ble_resume_allowed = false;
    ble_scanning = false;
  }
}

static void ble_stop_scan() {
  ble_resume_allowed = false;
  if (!ble_stack_started && !ble_scanning) return;

  ble_scanning = false;
  (void)Bluefruit.Scanner.stop();
}

// ===============================
// GPS non-blocking (bounded) + backup sleep (robust)
// ===============================
#define GPS_SERIAL Serial1

struct GpsState {
  bool    rmc_valid = false;
  bool    have_latlon = false;
  float   lat = 0.0f;
  float   lon = 0.0f;
  bool    have_sats = false;
  uint8_t sats = 0;
  bool    have_hdop = false;
  float   hdop = 99.9f;

  void reset() {
    rmc_valid = false;
    have_latlon = false;
    lat = lon = 0.0f;
    have_sats = false;
    sats = 0;
    have_hdop = false;
    hdop = 99.9f;
  }
};

static bool gps_busy = false;
static bool gps_sleeping = false;
static GpsState gps_st;
static uint32_t gps_attempt_start_ms = 0;
static uint32_t gps_attempt_deadline_ms = 0;
static uint32_t gps_last_sleep_req_ms = 0;

static char gps_line[128];
static uint8_t gps_line_idx = 0;

struct UtcClock {
  bool valid = false;
  uint32_t sync_local_ms = 0;
  uint64_t sync_unix_ms = 0;
  uint32_t sync_count = 0;
};

static UtcClock UTC;

static bool parse_2_digits(const char* s, uint8_t& out) {
  if (!s || !isDigit(s[0]) || !isDigit(s[1])) return false;
  out = (uint8_t)((s[0] - '0') * 10 + (s[1] - '0'));
  return true;
}

static bool is_leap_year(uint16_t year) {
  return ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
}

static bool gps_datetime_to_unix_ms(const char* utc_time, const char* utc_date, uint64_t& out_unix_ms) {
  if (!utc_time || !utc_date) return false;
  uint8_t hour = 0, minute = 0, second = 0;
  uint8_t day = 0, month = 0, yy = 0;
  if (!parse_2_digits(utc_time + 0, hour)) return false;
  if (!parse_2_digits(utc_time + 2, minute)) return false;
  if (!parse_2_digits(utc_time + 4, second)) return false;
  if (!parse_2_digits(utc_date + 0, day)) return false;
  if (!parse_2_digits(utc_date + 2, month)) return false;
  if (!parse_2_digits(utc_date + 4, yy)) return false;

  uint16_t millis_part = 0;
  if (utc_time[6] == '.') {
    uint16_t scale = 100;
    for (const char* p = utc_time + 7; *p && scale > 0 && isDigit(*p); p++) {
      millis_part += (uint16_t)(*p - '0') * scale;
      scale /= 10;
    }
  }

  uint16_t year = (yy >= 80) ? (uint16_t)(1900 + yy) : (uint16_t)(2000 + yy);
  static const uint8_t days_in_month[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (hour > 23 || minute > 59 || second > 59) return false;
  if (month < 1 || month > 12) return false;

  uint8_t max_day = days_in_month[month - 1];
  if (month == 2 && is_leap_year(year)) max_day = 29;
  if (day < 1 || day > max_day) return false;

  uint32_t days = 0;
  for (uint16_t y = 1970; y < year; y++) days += is_leap_year(y) ? 366UL : 365UL;
  for (uint8_t m = 1; m < month; m++) {
    days += days_in_month[m - 1];
    if (m == 2 && is_leap_year(year)) days++;
  }
  days += (uint32_t)(day - 1);

  uint64_t seconds = (uint64_t)days * 86400ULL + (uint32_t)hour * 3600UL + (uint32_t)minute * 60UL + second;
  out_unix_ms = seconds * 1000ULL + millis_part;
  return true;
}

static void utc_sync(uint32_t local_ms, uint64_t unix_ms) {
  UTC.valid = true;
  UTC.sync_local_ms = local_ms;
  UTC.sync_unix_ms = unix_ms;
  UTC.sync_count++;
}

static uint64_t utc_ms_from_millis(uint32_t local_ms) {
  if (!UTC.valid) return 0;

  int32_t delta = (int32_t)(local_ms - UTC.sync_local_ms);
  if (delta < 0) {
    uint32_t back = (uint32_t)(-delta);
    return (UTC.sync_unix_ms > back) ? (UTC.sync_unix_ms - back) : 0;
  }
  return UTC.sync_unix_ms + (uint32_t)delta;
}

static int nmea_hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

static bool nmea_checksum_ok(const char* sentence) {
  if (!sentence || sentence[0] != '$') return false;
  const char* star = strchr(sentence, '*');
  if (!star || !star[1] || !star[2]) return false;

  int hi = nmea_hex_value(star[1]);
  int lo = nmea_hex_value(star[2]);
  if (hi < 0 || lo < 0) return false;

  uint8_t calc = 0;
  for (const char* p = sentence + 1; p < star; p++) calc ^= (uint8_t)(*p);
  return calc == (uint8_t)((hi << 4) | lo);
}

static bool nmea_degmin_to_dec(const char* s, bool is_lon, float& out) {
  if (!s || !*s) return false;
  const char* dot = strchr(s, '.');
  int len = dot ? (int)(dot - s) : (int)strlen(s);
  int deg_digits = is_lon ? 3 : 2;
  if (len < deg_digits + 2) return false;

  char deg_buf[4] = {0};
  for (int i = 0; i < deg_digits; i++) deg_buf[i] = s[i];
  int deg = atoi(deg_buf);
  float minutes = atof(s + deg_digits);
  out = (float)deg + (minutes / 60.0f);
  return true;
}

static int split_csv(char* line, char* fields[], int max_fields) {
  int n = 0;
  fields[n++] = line;
  for (char* p = line; *p && n < max_fields; p++) {
    if (*p == ',') { *p = '\0'; fields[n++] = p + 1; }
  }
  return n;
}

// GPS payload: uint32 ms + float lat + float lon
static void gps_push(uint32_t ms, float lat, float lon) {
  uint8_t p[12];
  memcpy(p + 0, &ms, 4);
  memcpy(p + 4, &lat, 4);
  memcpy(p + 8, &lon, 4);
  rb_push_event(EVT_GPS, p, sizeof(p));
}

static void gps_process_sentence(char* line, uint32_t sentence_ms) {
  if (!nmea_checksum_ok(line)) return;
  if (line[0] == '$') line++;
  char* star = strchr(line, '*');
  if (star) *star = '\0';
  if (strlen(line) < 5) return;

  char* fields[20] = {0};
  int nf = split_csv(line, fields, 20);
  if (nf < 1) return;

  const char* msg = fields[0];

  if (strcmp(msg, "GPGGA") == 0 || strcmp(msg, "GNGGA") == 0) {
    if (nf > 7 && fields[7] && *fields[7]) { gps_st.sats = (uint8_t)atoi(fields[7]); gps_st.have_sats = true; }
    if (nf > 8 && fields[8] && *fields[8]) { gps_st.hdop = atof(fields[8]); gps_st.have_hdop = true; }

    if (nf > 5 && fields[2] && *fields[2] && fields[4] && *fields[4]) {
      float lat = 0.0f, lon = 0.0f;
      if (nmea_degmin_to_dec(fields[2], false, lat) && nmea_degmin_to_dec(fields[4], true, lon)) {
        if (fields[3] && (*fields[3] == 'S' || *fields[3] == 's')) lat = -lat;
        if (fields[5] && (*fields[5] == 'W' || *fields[5] == 'w')) lon = -lon;
        gps_st.lat = lat; gps_st.lon = lon; gps_st.have_latlon = true;
      }
    }
    return;
  }

  if (strcmp(msg, "GPRMC") == 0 || strcmp(msg, "GNRMC") == 0) {
    if (nf > 2 && fields[2] && *fields[2]) gps_st.rmc_valid = (fields[2][0] == 'A');

    if (gps_st.rmc_valid && nf > 9 && fields[1] && *fields[1] && fields[9] && *fields[9]) {
      uint64_t unix_ms = 0;
      if (gps_datetime_to_unix_ms(fields[1], fields[9], unix_ms)) utc_sync(sentence_ms, unix_ms);
    }

    if (nf > 6 && fields[3] && *fields[3] && fields[5] && *fields[5]) {
      float lat = 0.0f, lon = 0.0f;
      if (nmea_degmin_to_dec(fields[3], false, lat) && nmea_degmin_to_dec(fields[5], true, lon)) {
        if (fields[4] && (*fields[4] == 'S' || *fields[4] == 's')) lat = -lat;
        if (fields[6] && (*fields[6] == 'W' || *fields[6] == 'w')) lon = -lon;
        gps_st.lat = lat; gps_st.lon = lon; gps_st.have_latlon = true;
      }
    }
    return;
  }
}

// ---- UBX helpers ----
static void gps_ubx_send(uint8_t cls, uint8_t id, const uint8_t* payload, uint16_t len) {
  uint8_t ckA = 0, ckB = 0;
  auto upd = [&](uint8_t b) { ckA = (uint8_t)(ckA + b); ckB = (uint8_t)(ckB + ckA); };

  GPS_SERIAL.write((uint8_t)0xB5);
  GPS_SERIAL.write((uint8_t)0x62);

  GPS_SERIAL.write(cls); upd(cls);
  GPS_SERIAL.write(id);  upd(id);

  uint8_t lsb = (uint8_t)(len & 0xFF);
  uint8_t msb = (uint8_t)(len >> 8);
  GPS_SERIAL.write(lsb); upd(lsb);
  GPS_SERIAL.write(msb); upd(msb);

  for (uint16_t i = 0; i < len; i++) { GPS_SERIAL.write(payload[i]); upd(payload[i]); }

  GPS_SERIAL.write(ckA);
  GPS_SERIAL.write(ckB);
}

static void gps_send_cfg_rxm(uint8_t lpMode) {
  uint8_t p[2] = { 0x00, lpMode };
  gps_ubx_send(0x06, 0x11, p, sizeof(p));
}

static void put_u32_le(uint8_t* dst, uint32_t v) {
  dst[0] = (uint8_t)(v & 0xFF);
  dst[1] = (uint8_t)((v >> 8) & 0xFF);
  dst[2] = (uint8_t)((v >> 16) & 0xFF);
  dst[3] = (uint8_t)((v >> 24) & 0xFF);
}

static const uint32_t GPS_PMREQ_FLAG_BACKUP      = (1UL << 1);
static const uint32_t GPS_PMREQ_FLAG_FORCE       = (1UL << 2);
static const uint32_t GPS_PMREQ_WAKE_UART_RX     = (1UL << 3);

static void gps_send_rxm_pmreq(uint32_t durationMs, uint32_t flags, uint32_t wakeupSources) {
  uint8_t p[16] = {0};
  p[0] = 0x00; // version
  put_u32_le(p + 4, durationMs);
  put_u32_le(p + 8, flags);
  put_u32_le(p + 12, wakeupSources);
  gps_ubx_send(0x02, 0x41, p, sizeof(p));
}

static void gps_drain_uart(uint32_t maxBytes) {
  uint32_t n = 0;
  while (GPS_SERIAL.available() > 0 && n < maxBytes) { (void)GPS_SERIAL.read(); n++; }
}

static void gps_hold_rx_wake_idle() {
#if defined(PIN_SERIAL1_TX)
  pinMode(PIN_SERIAL1_TX, OUTPUT);
  digitalWrite(PIN_SERIAL1_TX, HIGH);
#endif
}

static void gps_release_uart_pins() {
#if defined(PIN_SERIAL1_TX)
  pinMode(PIN_SERIAL1_TX, INPUT);
#endif
#if defined(PIN_SERIAL1_RX)
  pinMode(PIN_SERIAL1_RX, INPUT);
#endif
}

// SAM-M10Q software standby: use backup + force for minimum power, then stop UART locally.
static void gps_enter_backup_sleep(bool forceRefresh = false) {
  if (!CFG.gps_enabled) return;
  if (!CFG.gps_backup_sleep_enabled) return;
  if (gps_busy) return;
  if (gps_sleeping && !forceRefresh) return;

  if (forceRefresh) GPS_SERIAL.begin(CFG.gps_baud);
  gps_drain_uart(512);

  gps_send_rxm_pmreq(0,
                     GPS_PMREQ_FLAG_BACKUP | GPS_PMREQ_FLAG_FORCE,
                     GPS_PMREQ_WAKE_UART_RX);
  GPS_SERIAL.flush();
  delay(20);

  gps_drain_uart(512);

  // Important: stop UART so RX doesn't keep MCU awake
  GPS_SERIAL.end();
  if (CFG.gps_release_uart_pins_after_sleep) gps_release_uart_pins();
  else gps_hold_rx_wake_idle();

  gps_last_sleep_req_ms = millis();
  gps_sleeping = true;
}

static void gps_maintain_backup_sleep(uint32_t now) {
  if (!CFG.gps_enabled) return;
  if (!CFG.gps_backup_sleep_enabled) return;
  if (gps_busy) return;
  if (!gps_sleeping) {
    gps_enter_backup_sleep();
    return;
  }
  if (CFG.gps_sleep_refresh_ms == 0) return;
  if ((uint32_t)(now - gps_last_sleep_req_ms) >= CFG.gps_sleep_refresh_ms) {
    gps_enter_backup_sleep(true);
  }
}

static void gps_wake_for_attempt() {
  if (!CFG.gps_enabled) return;

  bool was_sleeping = gps_sleeping;
  GPS_SERIAL.begin(CFG.gps_baud);
  GPS_SERIAL.write('\r'); GPS_SERIAL.write('\n');
  GPS_SERIAL.flush();

  if (was_sleeping && CFG.gps_wake_settle_ms > 0) delay(CFG.gps_wake_settle_ms);
  gps_drain_uart(512);

  gps_send_cfg_rxm(0); // continuous during attempt
  gps_sleeping = false;
}

static void gps_start_attempt() {
  if (!CFG.gps_enabled) return;
  if (gps_busy) return;

  gps_wake_for_attempt();

  gps_busy = true;
  gps_attempt_start_ms = millis();
  gps_attempt_deadline_ms = gps_attempt_start_ms + CFG.gps_timeout_ms;

  gps_st.reset();
  gps_line_idx = 0;
}

// time-bounded poll (keeps BLE stable)
static void gps_poll_bounded() {
  if (!gps_busy) return;

  uint32_t now = millis();
  if ((int32_t)(now - gps_attempt_deadline_ms) >= 0) {
    gps_busy = false;
    gps_line_idx = 0;
    gps_enter_backup_sleep();
    return;
  }

  uint32_t start_us = micros();
  uint32_t bytes = 0;

  while (GPS_SERIAL.available() > 0) {
    if ((uint32_t)(micros() - start_us) >= CFG.gps_poll_time_budget_us) break;
    if (bytes >= CFG.gps_poll_byte_budget) break;

    char c = (char)GPS_SERIAL.read();
    bytes++;

    if (c == '\r') continue;

    if (c == '\n') {
      if (gps_line_idx > 0) {
        gps_line[gps_line_idx] = '\0';
        uint32_t sentence_ms = millis();

        char tmp[128];
        strncpy(tmp, gps_line, sizeof(tmp));
        tmp[sizeof(tmp) - 1] = '\0';

        gps_process_sentence(tmp, sentence_ms);

        bool sats_ok = gps_st.have_sats && (gps_st.sats >= CFG.gps_min_sats);
        bool hdop_ok = gps_st.have_hdop && (gps_st.hdop <= CFG.gps_min_hdop);

        if (gps_st.rmc_valid && gps_st.have_latlon && sats_ok && hdop_ok) {
          gps_push(sentence_ms, gps_st.lat, gps_st.lon);

          if (CFG.debug_enable && CFG.debug_print_gps_fix) {
            Serial.print("GPS: lat=");
            Serial.print(gps_st.lat, 6);
            Serial.print(" lon=");
            Serial.println(gps_st.lon, 6);
          }

          gps_busy = false;
          gps_line_idx = 0;
          gps_enter_backup_sleep();
          return;
        }
      }
      gps_line_idx = 0;
    } else {
      if (gps_line_idx < sizeof(gps_line) - 1) gps_line[gps_line_idx++] = c;
      else gps_line_idx = 0;
    }
  }
}

// ===============================
// Flush control (BURSTED)
// ===============================
static bool flush_requested = false;
static FlushWhy flush_reason = FLUSH_NORMAL;
static uint32_t t_last_flush_done = 0;

static const char* flushWhyStr(FlushWhy w) {
  switch (w) {
    case FLUSH_NORMAL:   return "NORMAL";
    case FLUSH_EMERG:    return "EMERG";
    case FLUSH_FAILSAFE: return "FAILSAFE";
    default:             return "?";
  }
}

static void request_flush(FlushWhy why) {
  if (why == FLUSH_EMERG) { flush_requested = true; flush_reason = FLUSH_EMERG; return; }
  if (flush_requested && flush_reason == FLUSH_EMERG) return;
  flush_requested = true;
  flush_reason = why;
}

static size_t sd_print_u64(SdFile& file, uint64_t v) {
  char buf[21];
  char* p = buf + sizeof(buf);
  *--p = '\0';
  do {
    *--p = (char)('0' + (v % 10));
    v /= 10;
  } while (v > 0);
  return file.print(p);
}

static void sd_rollback_line(SdFile& file, uint32_t pos) {
  file.clearWriteError();
  (void)file.truncate(pos);
  (void)file.seekSet(pos);
  file.clearWriteError();
}

static bool sd_commit_line(SdFile& file, uint32_t pos) {
  if (file.getWriteError()) {
    sd_rollback_line(file, pos);
    return false;
  }
  if (!file.sync() || file.getWriteError()) {
    sd_rollback_line(file, pos);
    return false;
  }
  return true;
}

static bool sd_write_header_if_empty(SdFile& file, const char* header) {
  if (file.fileSize() != 0) return true;
  uint32_t pos = file.curPosition();
  file.clearWriteError();
  file.println(header);
  return sd_commit_line(file, pos);
}

static bool ensure_headers() {
  if (!sd_write_header_if_empty(fAcc, "ms,unix_ms,x_g,y_g,z_g")) return false;
  if (!sd_write_header_if_empty(fGps, "ms,unix_ms,lat,lon")) return false;
  if (!sd_write_header_if_empty(fBle, "ms,unix_ms,mac,rssi")) return false;
  return true;
}

static bool write_ring_event_to_sd(uint8_t type, uint8_t len, const uint8_t* payload, bool& wrote_event) {
  wrote_event = false;

  if (type == EVT_ACC && (len == 10 || len == 16)) {
    uint32_t ms;
    float x, y, z;
    memcpy(&ms, payload + 0, 4);

    if (len == 10) {
      int16_t rx, ry, rz;
      memcpy(&rx, payload + 4, 2);
      memcpy(&ry, payload + 6, 2);
      memcpy(&rz, payload + 8, 2);
      const float scale_g = 0.0039f;
      x = rx * scale_g;
      y = ry * scale_g;
      z = rz * scale_g;
    } else {
      memcpy(&x, payload + 4, 4);
      memcpy(&y, payload + 8, 4);
      memcpy(&z, payload + 12,4);
    }

    uint32_t pos = fAcc.curPosition();
    fAcc.clearWriteError();
    fAcc.print(ms); fAcc.print(",");
    sd_print_u64(fAcc, utc_ms_from_millis(ms)); fAcc.print(",");
    fAcc.print(x, 6); fAcc.print(",");
    fAcc.print(y, 6); fAcc.print(",");
    fAcc.println(z, 6);

    wrote_event = true;
    return sd_commit_line(fAcc, pos);
  }

  if (type == EVT_GPS && len == 12) {
    uint32_t ms; float lat, lon;
    memcpy(&ms,  payload + 0, 4);
    memcpy(&lat, payload + 4, 4);
    memcpy(&lon, payload + 8, 4);

    uint32_t pos = fGps.curPosition();
    fGps.clearWriteError();
    fGps.print(ms); fGps.print(",");
    sd_print_u64(fGps, utc_ms_from_millis(ms)); fGps.print(",");
    fGps.print(lat, 6); fGps.print(",");
    fGps.println(lon, 6);

    wrote_event = true;
    return sd_commit_line(fGps, pos);
  }

  if (type == EVT_BLE && len == 11) {
    uint32_t ms; int8_t rssi;
    uint8_t mac[6];
    memcpy(&ms, payload + 0, 4);
    memcpy(mac, payload + 4, 6);
    memcpy(&rssi, payload + 10, 1);

    char s[18];
    snprintf(s, sizeof(s), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    uint32_t pos = fBle.curPosition();
    fBle.clearWriteError();
    fBle.print(ms); fBle.print(",");
    sd_print_u64(fBle, utc_ms_from_millis(ms)); fBle.print(",");
    fBle.print(s);  fBle.print(",");
    fBle.println((int)rssi);

    wrote_event = true;
    return sd_commit_line(fBle, pos);
  }

  ring_bad_events++;
  return true;
}

// Returns: done?
static bool flush_ring_burst(uint32_t& wrote_events_out) {
  wrote_events_out = 0;
  if (!sd_ok) return false;
  if (CFG.sd_retry_backoff_ms > 0 && (int32_t)(millis() - sd_next_retry_ms) < 0) return false;
  if (RB.used == 0) return true;

  if (!sd_init_open_files()) {
    sd_write_fail++;
    sd_sleep();
    return false;
  }

  if (!ensure_headers()) {
    sd_write_fail++;
    sd_sleep();
    return false;
  }

  uint8_t type = 0, len = 0;
  uint8_t payload[32];

  for (uint32_t i = 0; i < CFG.sd_flush_max_events_burst; i++) {
    if (!rb_peek_event(type, len, payload, sizeof(payload))) {
      if (RB.used >= 2) {
        uint8_t bad_len = rb_peek_u8(1);
        rb_discard_bytes((uint32_t)2 + bad_len);
        ring_bad_events++;
      }
      break;
    }

    bool wrote_event = false;
    if (!write_ring_event_to_sd(type, len, payload, wrote_event)) {
      sd_write_fail++;
      sd_sleep();
      return false;
    }

    rb_discard_bytes((uint32_t)2 + len);
    if (wrote_event) wrote_events_out++;
  }

  bool done = (RB.used == 0);
  if (done || CFG.sd_close_between_flushes) sd_sleep();
  return done;
}

// ===============================
// Setup helpers
// ===============================
static void sd_close_files() {
  fAcc.close();
  fGps.close();
  fBle.close();
  sd_files_open = false;
}

static bool sd_init_open_files() {
  if (!CFG.sd_enabled) return false;
  if (sd_files_open) return true;

  if (CFG.sd_power_en_pin >= 0) {
    pinMode((uint8_t)CFG.sd_power_en_pin, OUTPUT);
    digitalWrite((uint8_t)CFG.sd_power_en_pin, CFG.sd_power_en_active_high ? HIGH : LOW);
    delay(5);
  }

  SdSpiConfig cfg(CFG.sd_cs_pin, DEDICATED_SPI, CFG.sd_spi_hz);
  if (!sd.begin(cfg)) return false;

  if (!fAcc.open("ACC.CSV", O_RDWR | O_CREAT | O_AT_END)) { sd_close_files(); return false; }
  if (!fGps.open("GPS.CSV", O_RDWR | O_CREAT | O_AT_END)) { sd_close_files(); return false; }
  if (!fBle.open("BLE.CSV", O_RDWR | O_CREAT | O_AT_END)) { sd_close_files(); return false; }
  sd_files_open = true;
  return true;
}

static void sd_release_bus_pins() {
  pinMode(CFG.sd_cs_pin, OUTPUT);
  digitalWrite(CFG.sd_cs_pin, HIGH);

  if (!CFG.sd_release_spi_pins_on_sleep) return;

#if defined(PIN_SPI_SCK)
  pinMode(PIN_SPI_SCK, OUTPUT);
  digitalWrite(PIN_SPI_SCK, LOW);
#endif
#if defined(PIN_SPI_MOSI)
  pinMode(PIN_SPI_MOSI, OUTPUT);
  digitalWrite(PIN_SPI_MOSI, LOW);
#endif
#if defined(PIN_SPI_MISO)
  pinMode(PIN_SPI_MISO, INPUT);
#endif
}

static void sd_send_cmd0_idle() {
  if (!CFG.sd_send_cmd0_on_sleep) return;

  sd_release_bus_pins();

  SPI.begin();
  SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));

  for (uint8_t i = 0; i < 10; i++) SPI.transfer(0xFF);

  digitalWrite(CFG.sd_cs_pin, LOW);
  SPI.transfer(0x40); // CMD0
  SPI.transfer(0x00);
  SPI.transfer(0x00);
  SPI.transfer(0x00);
  SPI.transfer(0x00);
  SPI.transfer(0x95); // valid CRC for CMD0

  for (uint8_t i = 0; i < 10; i++) {
    uint8_t r = SPI.transfer(0xFF);
    if ((r & 0x80) == 0) break;
  }

  digitalWrite(CFG.sd_cs_pin, HIGH);
  SPI.transfer(0xFF);
  SPI.endTransaction();
  SPI.end();
}

static void sd_sleep() {
  sd_close_files();

  sd.end();
  sd_send_cmd0_idle();
  SPI.end();

  sd_release_bus_pins();

  if (CFG.sd_power_en_pin >= 0) {
    digitalWrite((uint8_t)CFG.sd_power_en_pin, CFG.sd_power_en_active_high ? LOW : HIGH);
  }
}

static bool sd_startup_check() {
  if (!CFG.sd_enabled) return false;

  bool ok = sd_init_open_files();
  if (ok) ok = ensure_headers();
  sd_sleep();
  return ok;
}

static void ring_init_dynamic() {
  uint32_t free_heap = approx_free_heap_bytes();
  uint32_t target = (uint32_t)(free_heap * CFG.ring_alloc_fraction_of_free_heap);

  const uint32_t MIN_RING = 8UL * 1024UL;
  const uint32_t MAX_RING = 120UL * 1024UL;
  if (target < MIN_RING) target = MIN_RING;
  if (target > MAX_RING) target = MAX_RING;

  RB.buf = (uint8_t*)malloc(target);
  if (!RB.buf) { RB.cap = 0; return; }

  RB.cap = target;
  RB.head = RB.tail = RB.used = 0;
  RB.normal_th = (uint32_t)(RB.cap * CFG.ring_normal_threshold_fraction);
  RB.emerg_th  = (uint32_t)(RB.cap * CFG.ring_emerg_threshold_fraction);
}

static void dedup_init() {
  if (!CFG.ble_dedup_per_window || CFG.ble_dedup_max_macs == 0) return;
  dedup_cap = CFG.ble_dedup_max_macs;
  dedup_macs = (Mac6*)malloc(sizeof(Mac6) * dedup_cap);
  if (!dedup_macs) dedup_cap = 0;
}

// ===============================
// Runtime
// ===============================
static uint32_t t_last_status = 0;
static uint32_t t_last_gps_trigger = 0;
static bool first_data_flush_done = false;

static void sleep_when_idle();

static void apply_power_test_config() {
#if POWER_TEST_MODE == POWER_TEST_LOGGER_NO_GPS || \
    POWER_TEST_MODE == POWER_TEST_LOGGER_GPS_ASLEEP || \
    POWER_TEST_MODE == POWER_TEST_GPS_ASLEEP_NO_SD || \
    POWER_TEST_MODE == POWER_TEST_GPS_ASLEEP_NO_BLE || \
    POWER_TEST_MODE == POWER_TEST_GPS_ASLEEP_NO_ACC
  CFG.gps_enabled = false;
#endif

#if POWER_TEST_MODE == POWER_TEST_GPS_ASLEEP_NO_SD
  CFG.sd_enabled = false;
#endif

#if POWER_TEST_MODE == POWER_TEST_GPS_CYCLE_NO_SD_NO_ACC
  CFG.sd_enabled = false;
  CFG.acc_enabled = false;
  CFG.gps_interval_ms = 30000;
#endif

#if POWER_TEST_MODE == POWER_TEST_GPS_ASLEEP_NO_BLE
  CFG.ble_enabled = false;
  CFG.ble_log_enabled = false;
#endif

#if POWER_TEST_MODE == POWER_TEST_GPS_ASLEEP_NO_ACC
  CFG.acc_enabled = false;
#endif

#if POWER_TEST_MODE == POWER_TEST_GPS_STANDBY_RELEASE_UART
  CFG.gps_release_uart_pins_after_sleep = true;
  CFG.gps_sleep_refresh_ms = 0;
#endif

#if POWER_TEST_MODE == POWER_TEST_GPS_GATE_ACC_BLE_SD_3MIN
  CFG.gps_enabled = false;
  CFG.gps_release_uart_pins_after_sleep = true;
  CFG.gps_sleep_refresh_ms = 0;
  CFG.flush_first_ms = 180000;
#endif
}

static void power_test_idle_loop() {
  sleep_when_idle();
}

static void power_test_gps_standby_setup() {
  GPS_SERIAL.begin(CFG.gps_baud);
  delay(250);
  gps_enter_backup_sleep();
}

static void print_status() {
  if (!CFG.debug_enable) return;

  Serial.print("[STATUS] ms=");
  Serial.print(millis());
  Serial.print(" sd=");
  Serial.print(sd_ok ? "OK" : "NO");
  Serial.print(" scan=");
  Serial.print(ble_scanning ? "ON" : "OFF");
  Serial.print(" gps_busy=");
  Serial.print(gps_busy ? 1 : 0);
  Serial.print(" gps_sleep=");
  Serial.print(gps_sleeping ? 1 : 0);
  Serial.print(" utc=");
  Serial.print(UTC.valid ? 1 : 0);
  Serial.print(" utc_syncs=");
  Serial.print(UTC.sync_count);
  Serial.print(" acc_present=");
  Serial.print(acc_present ? 1 : 0);
  if (acc_present) {
    Serial.print(" acc_addr=0x");
    Serial.print(acc_addr, HEX);
  }
  Serial.print(" acc_ok=");
  Serial.print(acc_ok);
  Serial.print(" acc_fail=");
  Serial.print(acc_fail);
  Serial.print(" irq_isr=");
  Serial.print(irq_isr);
  Serial.print(" irq_handled=");
  Serial.print(irq_handled);
  Serial.print(" last_src=0x");
  Serial.print(last_int_src, HEX);
  Serial.print(" drained=");
  Serial.print(last_drained);
  Serial.print(" ring=");
  Serial.print(RB.used);
  Serial.print("/");
  Serial.print(RB.cap);
  Serial.print(" ble_q=");
  Serial.print(ble_queue_used());
  Serial.print(" ble_q_drops=");
  Serial.print(ble_q_drops);
  Serial.print(" sd_fail=");
  Serial.print(sd_write_fail);
  Serial.print(" bad_ev=");
  Serial.print(ring_bad_events);
  Serial.print(" drops=");
  Serial.println(RB.drops);
}

// CPU sleep primitive
static void sleep_when_idle() {
  (void)softdevice_active;
  waitForEvent();
}

// ===============================
// Setup / Loop
// ===============================
void setup() {
  status_led_init();

#if POWER_TEST_MODE == POWER_TEST_GPS_GATE_ACC_BLE_SD_3MIN
  CFG.gps_release_uart_pins_after_sleep = true;
  CFG.gps_sleep_refresh_ms = 0;
  power_test_gps_standby_setup();
#endif

#if POWER_TEST_MODE == POWER_TEST_LOGGER_GPS_ASLEEP || \
    POWER_TEST_MODE == POWER_TEST_GPS_ASLEEP_NO_SD || \
    POWER_TEST_MODE == POWER_TEST_GPS_ASLEEP_NO_BLE || \
    POWER_TEST_MODE == POWER_TEST_GPS_ASLEEP_NO_ACC
  power_test_gps_standby_setup();
#endif

  apply_power_test_config();

  if (CFG.debug_enable) {
    Serial.begin(115200);
    delay(100);
  }

  board_power_cleanup();

#if POWER_TEST_MODE == POWER_TEST_MCU_IDLE_ONLY
  return;
#elif POWER_TEST_MODE == POWER_TEST_GPS_STANDBY_ONLY || POWER_TEST_MODE == POWER_TEST_GPS_STANDBY_RELEASE_UART
  power_test_gps_standby_setup();
  return;
#endif

  // I2C
  Wire.begin();
  Wire.setClock(CFG.i2c_hz);

  ring_init_dynamic();
  dedup_init();

  // SD
  sd_ok = CFG.sd_enabled;
  bool sd_startup_ok = sd_ok && sd_startup_check();
  sd_startup_failed = sd_ok && !sd_startup_ok;
  status_led_set(sd_startup_failed);
  if (CFG.debug_enable && sd_ok && !sd_startup_ok) Serial.println("SD startup check failed");

  // ACC
  if (CFG.acc_enabled) {
    acc_init_fifo_int();
    if (acc_present) {
      pinMode(PIN_ADXL_INT1, INPUT);   // important: no pull unless you have a reason
      attachInterrupt(digitalPinToInterrupt(PIN_ADXL_INT1), adxl_int1_isr,
                      CFG.fifo_int_active_high ? RISING : FALLING);
    }
  }

  // BLE
  if (CFG.ble_enabled) {
    if (!CFG.ble_disable_stack_between_windows) {
      (void)ble_begin_stack();
    }
  }

  // GPS
  if (CFG.gps_enabled) {
    GPS_SERIAL.begin(CFG.gps_baud);
    delay(250);               // allow module to boot
    gps_enter_backup_sleep(); // software standby; ends UART
  }

  uint32_t now = millis();
  t_last_status = now;
  t_last_gps_trigger = now;
  t_last_flush_done = now;

  if (CFG.debug_enable) {
    Serial.print("Ring cap="); Serial.print(RB.cap);
    Serial.print(" normal_th="); Serial.print(RB.normal_th);
    Serial.print(" emerg_th="); Serial.println(RB.emerg_th);

    Serial.print("ACC fifo_watermark="); Serial.println(CFG.fifo_watermark);
    Serial.print("BLE scan interval_ms="); Serial.print(CFG.ble_scan_interval_ms);
    Serial.print(" duty="); Serial.println(CFG.ble_scan_duty, 3);
  }
}

void loop() {
#if POWER_TEST_MODE == POWER_TEST_MCU_IDLE_ONLY || \
    POWER_TEST_MODE == POWER_TEST_GPS_STANDBY_ONLY || \
    POWER_TEST_MODE == POWER_TEST_GPS_STANDBY_RELEASE_UART
  power_test_idle_loop();
  return;
#endif

  uint32_t now = millis();

  // 1) BLE scheduling FIRST
  bool ble_window = (CFG.ble_enabled && CFG.ble_log_enabled && in_window(now, CFG.ble_period_ms, CFG.ble_window_ms));
  if (ble_window) {
    uint32_t window_id = (CFG.ble_period_ms > 0) ? (now / CFG.ble_period_ms) : 0;
    if (window_id != dedup_window_id) dedup_reset(window_id);
    if (!ble_scanning) ble_start_scan();
  } else {
    static uint32_t t_last_ble_force_stop = 0;
    if (ble_scanning || (ble_stack_started && (uint32_t)(now - t_last_ble_force_stop) >= 250)) {
      t_last_ble_force_stop = now;
      ble_stop_scan();
    }
    if (CFG.ble_disable_stack_between_windows) ble_shutdown_stack();
  }
  if (CFG.ble_enabled) ble_process_queue();
  if (sd_startup_failed) status_led_set(true);

  // 2) GPS schedule + bounded poll
  if (CFG.gps_enabled) {
    if (!gps_busy) {
      if ((uint32_t)(now - t_last_gps_trigger) >= CFG.gps_interval_ms) {
        t_last_gps_trigger = now;
        gps_start_attempt();
      } else {
        gps_maintain_backup_sleep(now); // ensure low power whenever idle
      }
    }
    gps_poll_bounded();
  }

  // 3) ACC: drain FIFO when interrupt flagged (keep handler short)
  if (CFG.acc_enabled && acc_present && acc_irq_flag) {
    acc_irq_flag = false;
    irq_handled++;
    acc_drain_fifo_to_ring(now);
  }

  // 4) Flush requests
  bool sd_retry_waiting = (CFG.sd_retry_backoff_ms > 0 && (int32_t)(now - sd_next_retry_ms) < 0);
  if (!sd_retry_waiting) {
    if (RB.used >= RB.emerg_th && RB.emerg_th > 0) request_flush(FLUSH_EMERG);
    else if (RB.used >= RB.normal_th && RB.normal_th > 0) request_flush(FLUSH_NORMAL);
  }

  // FAILSAFE since last successful flush done
  if (!sd_retry_waiting && !first_data_flush_done && CFG.flush_first_ms > 0) {
    if (RB.used > 0 && (uint32_t)(now - t_last_flush_done) >= CFG.flush_first_ms) {
      request_flush(FLUSH_FAILSAFE);
    }
  }

  if (!sd_retry_waiting && CFG.flush_failsafe_ms > 0) {
    if ((uint32_t)(now - t_last_flush_done) >= CFG.flush_failsafe_ms) {
      if (RB.used > 0) request_flush(FLUSH_FAILSAFE);
    }
  }

  // 5) Flush servicing with policy + bursting
  if (flush_requested) {
    bool allow = true;
    if (gps_busy && flush_reason != FLUSH_EMERG) allow = false;

    if (allow) {
      uint32_t wrote = 0;
      uint32_t sd_fails_before = sd_write_fail;
      bool done = flush_ring_burst(wrote);
      bool sd_failed = (sd_write_fail != sd_fails_before);

      if (done || (wrote > 0 && RB.used < RB.normal_th)) {
        t_last_flush_done = millis();
        first_data_flush_done = true;
      }

      if (sd_failed) {
        sd_next_retry_ms = millis() + CFG.sd_retry_backoff_ms;
        flush_requested = false;
      }

      if (CFG.debug_enable && wrote > 0 && (done || flush_reason == FLUSH_EMERG)) {
        Serial.print("[FLUSH] why=");
        Serial.print(flushWhyStr(flush_reason));
        Serial.print(" wrote=");
        Serial.print(wrote);
        Serial.print(" ring=");
        Serial.print(RB.used);
        Serial.print("/");
        Serial.print(RB.cap);
        Serial.print(" drops=");
        Serial.println(RB.drops);
      }

      if (done) flush_requested = false;
    }
  }

  // 6) Status (debug only)
  if (CFG.debug_enable && CFG.status_print_ms > 0) {
    if ((uint32_t)(now - t_last_status) >= CFG.status_print_ms) {
      t_last_status = now;
      print_status();
    }
  }

  // 7) Sleep when idle
  sleep_when_idle();
}
