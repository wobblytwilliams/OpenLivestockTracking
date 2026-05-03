# OpenLivestockGPS

OpenLivestockGPS is store-on-board firmware for livestock loggers. The
logger records movement, GPS position, and nearby Bluetooth devices to
CSV files on a microSD card so the data can be analysed after the deployment.

The repository now keeps two production firmware code bases:

- `Firmware/ZephyrLogger`: the current low-power field-test firmware for the
  Adafruit ItsyBitsy nRF52840. This is the recommended path for new work.
- `Firmware/ArduinoLogger`: the Arduino firmware. It keeps the same
  logging idea in the Arduino ecosystem and is useful as a fallback or for quick
  experiments, or if you're just more familiar in this environment.

## What The Logger Records

The logger combines three signals that are useful in animal science field work:

- Accelerometer data from an ADXL345. This gives body movement and activity
  patterns such as resting, grazing, walking, or handling events.
- GPS attempts from a u-blox SAM-M10Q. These provide position fixes when the
  animal has sky view. If a fix is not available, the logger still records the
  attempt as `0,0` so you can see when GPS was tried.
- Bluetooth scan records. These can be used as rough proximity information when
  tags, phones, gateways, or other BLE devices are nearby.

Data is written to three files:

```text
ACC.CSV  ms,unix_ms,x_g,y_g,z_g
GPS.CSV  ms,unix_ms,lat,lon
BLE.CSV  ms,unix_ms,mac,rssi
```

`ms` is time since boot. `unix_ms` stays `0` until the GPS has supplied a valid
UTC time; after that it is Unix time in milliseconds. This makes early samples
usable even before the first GPS time sync.

## Hardware Setup

The current firmware targets:

- Controller: Adafruit ItsyBitsy nRF52840
- Accelerometer: ADXL345 on I2C, with INT1 connected to D13 / P0.12
- GPS: u-blox SAM-M10Q on UART, using D0 / P0.25 and D1 / P0.24
- Storage: microSD over SPI, with CS on D10 / P0.05

Use a FAT-formatted microSD card. The Zephyr firmware treats the SD card as
required because it stores both the configuration file and the CSV output. If SD
startup fails, the status LED is held on solid so the fault is visible before a
field deployment.

## ZephyrLogger Quick Start

Build the current production firmware from `Firmware/ZephyrLogger`:

```powershell
west build -b adafruit_itsybitsy/nrf52840 .
```

On Windows, Zephyr may behave badly when the app path contains spaces. If the
build fails in a path such as `My Drive`, copy `Firmware/ZephyrLogger` to a short
path such as `C:\Users\<you>\olg-zephyr-app` and build there.

To flash, double-tap reset on the ItsyBitsy so the `ITSY840BOOT` drive appears,
then copy:

```text
build/zephyr/zephyr.uf2
```

onto that drive.

## Field Configuration

On first boot, `ZephyrLogger` creates `CONFIG.TXT` on the SD card if it does not
already exist. Edit this file on a computer, put the card back into the logger,
and reboot to apply changes.

Default `CONFIG.TXT`:

```ini
acc_enabled=true
acc_odr_hz=12.5
acc_range_g=16

ble_enabled=true
ble_period_ms=60000
ble_window_ms=10000
ble_scan_interval_ms=100
ble_scan_window_ms=10

gps_enabled=true
gps_interval_ms=180000
gps_timeout_ms=15000
gps_min_sats=4
gps_min_hdop=2.5
```

The main knobs are:

- `acc_odr_hz`: accelerometer sample rate. Supported values are `12.5`, `25`,
  `50`, and `100`.
- `acc_range_g`: accelerometer range. Supported values are `2`, `4`, `8`, and
  `16`. Use higher range for more violent movement, lower range for finer
  detail.
- `ble_period_ms` and `ble_window_ms`: how often Bluetooth scans run and how
  long each scan lasts.
- `gps_interval_ms` and `gps_timeout_ms`: how often GPS wakes and how long it
  tries for a usable fix.
- `gps_min_sats` and `gps_min_hdop`: quality filters for accepting GPS fixes.

Invalid or missing values fall back to compiled defaults, so a typo should not
stop the logger from starting.

## Choosing A Firmware

Use `ZephyrLogger` for field tests and new development. It is the clean
production path with runtime `CONFIG.TXT` settings and the tested SD low-current
behaviour.

Use `ArduinoLogger` when you specifically want the Arduino toolchain or need a
reference implementation that is easier to inspect in one sketch. It writes the
same CSV style but is configured in code rather than by `CONFIG.TXT`.

## Typical Field Workflow

1. Charge or connect the logger battery.
2. Format the microSD card as FAT/FAT32.
3. Boot once to create `CONFIG.TXT`, then check the file on a computer.
4. Set the sampling and scan intervals for the study.
5. Reboot the logger with the SD card installed.
6. Confirm the status LED is not solid on.
7. Deploy the logger.
8. After recovery, copy `ACC.CSV`, `GPS.CSV`, and `BLE.CSV` from the SD card for
   analysis.
