cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(open_livestock_zephyr_logger)

target_sources(app PRIVATE
  src/main.c
  src/olg_adxl345.c
  src/olg_ble.c
  src/olg_bt.c
  src/olg_config.c
  src/olg_gateway.c
  src/olg_gps.c
  src/olg_power.c
  src/olg_ring.c
  src/olg_sd.c
  src/olg_storage.c
  src/olg_time.c
)
