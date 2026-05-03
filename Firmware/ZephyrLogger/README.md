# ZephyrLogger

Production Zephyr firmware for the OpenLivestockGPS logger on the Adafruit
ItsyBitsy nRF52840. This is the recommended firmware for low-power field tests.
It compiles in ACC, BLE, GPS, and SD support; the SD card `CONFIG.TXT` decides
which subsystems run at boot.

## Build

```powershell
west build -b adafruit_itsybitsy/nrf52840 .
```

On Windows, build from a no-space app path if Zephyr has trouble with a cloud
folder path such as `My Drive`.

GitHub Actions builds the same UF2 from this app and uploads it as
`OpenLivestockGPS-ZephyrLogger-UF2`. The workflow uses `west.yml` in this folder
to pin the CI build to Zephyr `v4.4.0`.

## Runtime Config

At startup the firmware mounts the SD card, creates `CONFIG.TXT` if missing,
loads it, creates CSV headers if needed, then closes/unmounts/deinitializes the
card and releases the SPI pins. A solid red LED after startup means the SD or
config startup path failed.

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

Edit `CONFIG.TXT` on the card and reboot to apply changes. Invalid or missing
values fall back to compiled defaults. `acc_odr_hz` supports `12.5`, `25`, `50`,
and `100`; `acc_range_g` supports `2`, `4`, `8`, and `16`.

Set `acc_enabled`, `ble_enabled`, or `gps_enabled` to `false` to skip that
subsystem for a deployment.

## Output Files

```text
ACC.CSV  ms,unix_ms,x_g,y_g,z_g
GPS.CSV  ms,unix_ms,lat,lon
BLE.CSV  ms,unix_ms,mac,rssi
```

`ms` is time since boot. `unix_ms` remains `0` until GPS provides a valid UTC
sync. GPS attempts that time out write `0,0` so missed fixes are visible in the
data.

## Behavior Notes

- `unix_ms` remains `0` until GPS provides a valid UTC sync.
- GPS timeout rows write `0,0` so attempts are visible even without a fix.
- ACC uses ADXL345 FIFO stream mode and INT1 level-active handling; production
  builds do not poll as a fallback. The FIFO watermark is an internal firmware
  setting, while `acc_range_g` controls the field measurement range.
- SD writes are bursty: files are opened only for startup/header checks and ring
  flushes, then the card is put back into the tested low-current state.
- The first failsafe data flush is after 3 minutes; recurring failsafe flushes
  are every 15 minutes.

## Flash By UF2

Double-tap reset to enter `ITSY840BOOT`, then copy:

```text
build/zephyr/zephyr.uf2
```

onto the bootloader drive.
