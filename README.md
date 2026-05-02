# OpenAgLivestockTracking
Arduino based livestock tracking sensor firmware for store-on-board research

## Build in VS Code

The Arduino sketch lives at `Firmware/PrototypeLogger_V1/PrototypeLogger_V1.ino`.

Press `Ctrl+Shift+B` in VS Code and choose `Arduino: Compile`. The task builds for
`adafruit:nrf52:feather52840` using Arduino CLI.

If you need to reinstall the board core or libraries, run the VS Code task
`Arduino: Setup Toolchain`.

CSV logs include both device `ms` and GPS-derived `unix_ms`. `unix_ms` is `0`
until a valid GPS RMC sentence has synced UTC time.
