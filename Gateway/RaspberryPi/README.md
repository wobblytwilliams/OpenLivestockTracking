# Raspberry Pi Gateway

The gateway is the heavier side of the system. It scans for loggers, connects
when a logger opens its GATT window, downloads only new binary SD log blocks,
and stores validated rows in SQLite + Parquet. The gateway-comms testing
firmware advertises every 2 minutes by default; longer field deployments can
set `gateway_period_ms=1200000` in `CONFIG.TXT` for a 20-minute interval.

## Fresh Pi Setup

Use these commands from the SSH prompt on the Pi. They clone this branch into a
known folder name, so the later `cd` commands are always the same.

```bash
cd ~
git clone -b gateway-comms https://github.com/wobblytwilliams/OpenLivestockTracking.git OpenLivestockGateway
cd ~/OpenLivestockGateway/Gateway/RaspberryPi
bash setup_pi.sh
```

## Passwordless SSH From Your Computer

Use SSH keys rather than removing the Pi account password. Paste your computer's
public key into the Pi's `authorized_keys` file:

```bash
mkdir -p ~/.ssh
chmod 700 ~/.ssh
cat >> ~/.ssh/authorized_keys <<'EOF'
paste-your-computer-public-key-here
EOF
chmod 600 ~/.ssh/authorized_keys
```

Then connect from the computer with:

```powershell
ssh thom@open-livestock-gateway.local
```

Keep password login enabled until key login has been tested.

If you have already cloned the repo, update it with:

```bash
cd ~/OpenLivestockGateway
git fetch origin
git switch gateway-comms
git pull --ff-only
cd ~/OpenLivestockGateway/Gateway/RaspberryPi
bash setup_pi.sh
```

## Install Boot Services

Once manual gateway and dashboard runs work, install the boot services:

```bash
cd ~/OpenLivestockGateway/Gateway/RaspberryPi
git pull --ff-only
bash install_services.sh
```

Check them with:

```bash
systemctl status openlivestock-gateway --no-pager
systemctl status openlivestock-dashboard --no-pager
journalctl -u openlivestock-gateway -f
```

After a reboot, the gateway and dashboard should start automatically:

```bash
sudo reboot
```

Dashboard address on the normal Wi-Fi:

```text
http://open-livestock-gateway.local:8080
```

Hotspot/WAP setup is a later step after the normal Wi-Fi path is confirmed.

## Run The Gateway

Open one SSH tab and run:

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

If the runner says Bluetooth is not powered on, run:

```bash
sudo systemctl restart bluetooth
bluetoothctl power on
bash run_gateway.sh
```

If this is the first setup after adding the user to the Bluetooth group, reboot
the Pi once and run `bash run_gateway.sh` again.

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
