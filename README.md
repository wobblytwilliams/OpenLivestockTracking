# OpenLivestockTracking

[![Build ZephyrLogger UF2](https://github.com/wobblytwilliams/OpenLivestockTracking/actions/workflows/zephyrlogger-uf2.yml/badge.svg)](https://github.com/wobblytwilliams/OpenLivestockTracking/actions/workflows/zephyrlogger-uf2.yml)

OpenLivestockGPS is store-on-board firmware for livestock loggers. The logger
records movement, GPS position, and nearby Bluetooth devices to a microSD card,
then turns those records into familiar CSV files for analysis.

Maintainer: Dr Thomas Williams

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

ZephyrLogger stores field data as compact binary log segments:

```text
LOG/OLG00000.BIN
LOG/OLG00001.BIN
...
```

Binary storage is used because it is faster to write, smaller on the SD card,
and safer after long deployments. Each block has a checksum, so if power is lost
mid-write the converter can recover up to the last valid block.

For analysis, the SD converter or Raspberry Pi gateway exports the same three
CSV tables scientists expect:

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
required because it stores both `CONFIG.TXT` and the binary log segments. If SD
startup fails, the status LED is held on solid so the fault is visible before a
field deployment.

## Install The ZephyrLogger UF2

The easiest way to install the logger is to download the prebuilt UF2 file.
You do not need Zephyr, Arduino, GitHub Actions, or any local build tools for
this path.

1. Download
   [`zephyr.uf2`](https://github.com/wobblytwilliams/OpenLivestockTracking/releases/download/zephyrlogger-latest/zephyr.uf2).
2. Double-tap reset on the ItsyBitsy so the `ITSY840BOOT` drive appears.
3. Copy `zephyr.uf2` onto `ITSY840BOOT`.

After the copy finishes, the board reboots into the new firmware. On first boot,
the firmware creates `CONFIG.TXT` and the `LOG` folder on the microSD card.

The latest build files are also listed on the
[ZephyrLogger Latest UF2 release page](https://github.com/wobblytwilliams/OpenLivestockTracking/releases/tag/zephyrlogger-latest).
GitHub Actions updates that release after successful builds from `main`.

## Build The Firmware Locally

Most users should install the prebuilt UF2 above. Build locally when you are
editing firmware code, changing compiled defaults, or checking a branch before
field testing.

### ZephyrLogger

Build the current production firmware from `Firmware/ZephyrLogger`:

```powershell
west build -b adafruit_itsybitsy/nrf52840 .
```

On Windows, Zephyr may behave badly when the app path contains spaces. If the
build fails in a path such as `My Drive`, copy `Firmware/ZephyrLogger` to a short
path such as `C:\Users\<you>\olg-zephyr-app` and build there.

The local build creates:

```text
build/zephyr/zephyr.uf2
```

To flash it, double-tap reset on the ItsyBitsy so the `ITSY840BOOT` drive
appears, then copy `build/zephyr/zephyr.uf2` onto that drive.

### ArduinoLogger

`Firmware/ArduinoLogger/ArduinoLogger.ino` is configured in code through the
`Config` struct near the top of the sketch. It records directly to CSV and does
not read `CONFIG.TXT`; changing settings requires editing the sketch and
reflashing.

The most useful ArduinoLogger fields are:

- `acc_enabled`, `gps_enabled`, and `ble_enabled`
- `acc_odr_hz` and `acc_range_g`
- `ble_period_ms`, `ble_window_ms`, and `ble_scan_interval_ms`
- `gps_interval_ms`, `gps_timeout_ms`, `gps_min_sats`, and `gps_min_hdop`
- `flush_failsafe_ms` and SD low-power settings

Open `ArduinoLogger.ino` in the Arduino IDE with the Adafruit nRF52 board
package installed for the ItsyBitsy nRF52840. The sketch uses the Arduino
`Wire`, `SPI`, `SdFat`, and Adafruit Bluefruit nRF52 libraries.

Flash with the normal Arduino upload flow, or double-tap reset to enter the UF2
bootloader if your Arduino setup produces a UF2 file.

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

gateway_enabled=true
gateway_period_ms=1200000
gateway_adv_window_ms=30000
gateway_session_timeout_ms=120000
gateway_retry_count=2
gateway_retry_min_ms=60000
gateway_retry_max_ms=180000
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
- `gateway_period_ms`: how often the logger briefly advertises for a Raspberry
  Pi gateway. The default is 20 minutes.
- `gateway_adv_window_ms`: how long the logger is available for a gateway
  connection during each period.

Invalid or missing values fall back to compiled defaults, so a typo should not
stop the logger from starting.

Set `acc_enabled`, `ble_enabled`, or `gps_enabled` to `false` to skip that
subsystem for a deployment.

Set `gateway_enabled=false` if the deployment will use SD-card recovery only.

## Getting CSV Data Back

There are two supported paths back to CSV.

For a recovered SD card, install the Raspberry Pi tools on a computer and run:

```bash
python Gateway/RaspberryPi/olg_log_convert.py sd-to-csv --input /path/to/SD --output exported_csv
```

This reads `LOG/OLG*.BIN` and creates:

```text
exported_csv/ACC.CSV
exported_csv/GPS.CSV
exported_csv/BLE.CSV
```

For near-real-time downloads, run a Raspberry Pi gateway. From an SSH session on
the Pi, clone this branch into a fixed folder name:

```bash
cd ~
git clone -b gateway-comms https://github.com/wobblytwilliams/OpenLivestockTracking.git OpenLivestockGateway
cd ~/OpenLivestockGateway/Gateway/RaspberryPi
bash setup_pi.sh
```

The Pi scans continuously. The logger only advertises briefly every 20 minutes,
so the power cost mostly sits on the Pi. The gateway stores transfer state in
SQLite and validated rows in Parquet.

Start the downloader in one SSH tab:

```bash
cd ~/OpenLivestockGateway/Gateway/RaspberryPi
bash run_gateway.sh
```

You should see repeated scanning messages. If no logger is in its gateway window
yet, this is normal:

```text
Bluetooth ready. Starting gateway scanner...
2026-05-17 10:00:00 Gateway running. Data directory: ...
2026-05-17 10:00:00 Scanning for OpenLivestock loggers for 30 seconds...
2026-05-17 10:00:30 No logger found. Continuing to scan.
```

If it reports that Bluetooth is not powered on, run:

```bash
sudo systemctl restart bluetooth
bluetoothctl power on
bash run_gateway.sh
```

After the first setup, a reboot can also be needed so the Pi applies the
Bluetooth group membership.

Start the dashboard in another SSH tab:

```bash
cd ~/OpenLivestockGateway/Gateway/RaspberryPi
bash run_dashboard.sh
```

Then open the dashboard from a phone or computer on the same network:

```text
http://raspberrypi.local:8080
```

If that name does not resolve, use the Pi IP address:

```text
http://<pi-ip-address>:8080
```

Once the Pi hotspot is configured, the hotspot address will usually be:

```text
http://192.168.4.1:8080
```

The dashboard shows gateway heartbeat, recent transfer sessions, logger status,
row counts, storage space, and a `Download CSV ZIP` button.

To export the gateway store back to CSV from the command line:

```bash
cd ~/OpenLivestockGateway/Gateway/RaspberryPi
. .venv/bin/activate
python olg_log_convert.py parquet-to-csv --input GatewayData/parquet --output exported_csv
```

## Choosing A Firmware

Use `ZephyrLogger` for new development. It is the clean production path with
runtime `CONFIG.TXT` settings, binary SD logging, gateway offload, and tested
lower-current behaviour.

Use `ArduinoLogger` if you want to tinker and you are confident with the Arduino
toolchain. It writes CSV directly but is configured in code rather than by
`CONFIG.TXT`. Power is significantly higher because of SD card latching and the
difficulty of getting the Arduino stack into deep sleep.

## License

This project is licensed under the BSD 3-Clause License. You may use, modify,
and redistribute it, but you must retain the copyright and license notice so Dr
Thomas Williams is acknowledged. See [LICENSE](LICENSE).

## Typical Field Workflow

1. Charge or connect the logger battery.
2. Format the microSD card as FAT/FAT32.
3. Boot once to create `CONFIG.TXT`, then check the file on a computer.
4. Set the sampling and scan intervals for the study.
5. Reboot the logger with the SD card installed.
6. Confirm the status LED is not solid on.
7. Deploy the logger.
8. After recovery, convert the SD `LOG/OLG*.BIN` files to `ACC.CSV`, `GPS.CSV`,
   and `BLE.CSV`, or export the Raspberry Pi gateway Parquet store to CSV.
