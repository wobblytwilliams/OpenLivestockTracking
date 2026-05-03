# ArduinoLogger

Production Arduino firmware for the OpenLivestockGPS logger. This sketch is kept
as the Arduino reference implementation and fallback firmware. For new low-power
field tests, use `Firmware/ZephyrLogger`.

## What It Does

`ArduinoLogger.ino` records the same three data streams as the Zephyr firmware:

```text
ACC.CSV  ms,unix_ms,x_g,y_g,z_g
GPS.CSV  ms,unix_ms,lat,lon
BLE.CSV  ms,unix_ms,mac,rssi
```

The sketch samples the ADXL345 accelerometer through FIFO interrupts, schedules
GPS attempts, scans for Bluetooth devices in windows, buffers events in RAM, and
flushes CSV rows to the SD card.

## Configuration

ArduinoLogger is configured in the `Config` struct near the top of
`ArduinoLogger.ino`. The most useful field settings are:

- `acc_enabled`, `gps_enabled`, and `ble_enabled`
- `acc_odr_hz` and `acc_range_g`
- `ble_period_ms`, `ble_window_ms`, and `ble_scan_interval_ms`
- `gps_interval_ms`, `gps_timeout_ms`, `gps_min_sats`, and `gps_min_hdop`
- `flush_failsafe_ms` and SD low-power settings

Unlike ZephyrLogger, ArduinoLogger does not read `CONFIG.TXT`; changing settings
requires editing the sketch and reflashing.

## Build And Flash

Open `ArduinoLogger.ino` in the Arduino IDE with the Adafruit nRF52 board package
installed for the ItsyBitsy nRF52840. The sketch uses the Arduino `Wire`, `SPI`,
`SdFat`, and Adafruit Bluefruit nRF52 libraries.

Flash with the normal Arduino upload flow, or double-tap reset to enter the UF2
bootloader if your Arduino setup produces a UF2 file.
