# ZephyrLogger

Side-by-side Zephyr migration for the OpenLivestockGPS logger. The first milestone is a low-power proof for:

- ADXL345 over I2C at 12.5 Hz
- FIFO watermark interrupt on ItsyBitsy D13 / P0.12
- raw ACC samples into a RAM ring buffer
- BLE observer scan windows, 10 s every 60 s
- first BLE scan starts after the first 60 s, so the initial floor is ACC-only
- GPS and SD available behind an explicit test config

## Build

Install upstream Zephyr, then from this directory:

```powershell
west build -b adafruit_itsybitsy/nrf52840 .
```

ACC-only comparison build:

```powershell
west build -b adafruit_itsybitsy/nrf52840 . -- -DEXTRA_CONF_FILE=acc_only.conf
```

BLE stack-off experiment:

```powershell
west build -b adafruit_itsybitsy/nrf52840 . -- -DEXTRA_CONF_FILE=ble_stack_off.conf
```

Temporary ACC FIFO visual check:

```powershell
west build -b adafruit_itsybitsy/nrf52840 . -- -DEXTRA_CONF_FILE=acc_diag_led.conf
```

This blinks the red LED three times at startup, then pulses it after each FIFO
drain. It also polls the FIFO every 2 s so it can distinguish "ACC is running
but INT1 is not firing" from "ACC is not producing samples". Leave it off for
final current readings.

GPS + SD feature-parity test:

```powershell
west build -b adafruit_itsybitsy/nrf52840 . -- -DEXTRA_CONF_FILE=gps_sd.conf
```

This keeps ACC and BLE enabled, enables GPS UART on D0/D1, enables SD over SPI
with CS on D10/P0.05, mounts the card at startup to create `ACC.CSV`,
`GPS.CSV`, and `BLE.CSV`, then closes/unmounts/deinitializes/releases the SD bus
between flush bursts. The first failsafe flush is set to 3 minutes for power
testing. A solid red LED after startup means the SD startup check failed.

The app disables USB serial/logging for power measurements and explicitly keeps
DotStar SPI, ADC, USB, and onboard QSPI flash out of the test build.
ADXL345 I2C transactions are grouped around FIFO drains and the I2C device is
asked to suspend again between bursts.

## Flash

Double-tap reset to enter `ITSY840BOOT`, then copy:

```text
build/zephyr/zephyr.uf2
```

onto the bootloader drive.

## Power Expectations

- ACC-only should be near the Arduino ACC-only floor, about 2-3 mA on the current dev board.
- ACC+BLE should show a scan-current bump for about 10 s per minute.
- The key test is whether the non-scan phase returns near the ACC-only floor.

## Feature-Parity Stubs

GPS and SD are now real modules, but remain disabled in `prj.conf`. Use
`gps_sd.conf` when the GPS and SD hardware are connected and you want to test the
full logger path.
