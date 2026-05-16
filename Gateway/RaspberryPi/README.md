# Raspberry Pi Gateway

The gateway is the heavier side of the system. It scans for loggers, connects
when a logger opens its 20-minute GATT window, downloads only new binary SD log
blocks, and stores validated rows in SQLite + Parquet.

## Fresh Pi Setup

Use these commands from the SSH prompt on the Pi. They clone this branch into a
known folder name, so the later `cd` commands are always the same.

```bash
cd ~
git clone -b gateway-comms https://github.com/wobblytwilliams/OpenAgLivestockTracking.git OpenLivestockGateway
cd ~/OpenLivestockGateway/Gateway/RaspberryPi
bash setup_pi.sh
```

If you have already cloned the repo, update it with:

```bash
cd ~/OpenLivestockGateway
git fetch origin
git switch gateway-comms
git pull --ff-only
cd ~/OpenLivestockGateway/Gateway/RaspberryPi
bash setup_pi.sh
```

## Run The Gateway

Open one SSH tab and run:

```bash
cd ~/OpenLivestockGateway/Gateway/RaspberryPi
bash run_gateway.sh
```

Open a second SSH tab and run:

```bash
cd ~/OpenLivestockGateway/Gateway/RaspberryPi
bash run_dashboard.sh
```

While the Pi is still on your normal Wi-Fi, open the dashboard from a phone or
computer on the same network:

```text
http://raspberrypi.local:8080
```

If name lookup is not working, use the Pi IP address instead:

```text
http://<pi-ip-address>:8080
```

Later, once the Pi hotspot is configured, use the hotspot IP address, for
example:

```text
http://192.168.4.1:8080
```

The dashboard shows gateway heartbeat, logger transfer sessions, downloaded
row counts, storage space, and a `Download CSV ZIP` button for `ACC.CSV`,
`GPS.CSV`, and `BLE.CSV`.

Convert a recovered logger SD card to CSV:

```bash
cd ~/OpenLivestockGateway/Gateway/RaspberryPi
. .venv/bin/activate
python olg_log_convert.py sd-to-csv --input /media/pi/LOGGER --output exported_csv
```

Export gateway Parquet data to CSV:

```bash
cd ~/OpenLivestockGateway/Gateway/RaspberryPi
. .venv/bin/activate
python olg_log_convert.py parquet-to-csv --input GatewayData/parquet --output exported_csv
```
