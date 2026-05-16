#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVICE_USER="${SUDO_USER:-$USER}"

if [ "$SERVICE_USER" = "root" ]; then
  echo "Run this as the normal Pi user, not from a root shell."
  echo "Example: bash install_services.sh"
  exit 1
fi

cd "$SCRIPT_DIR"

echo "Preparing gateway Python environment..."
bash setup_pi.sh

echo "Installing dashboard hostname support..."
sudo apt install -y avahi-daemon
sudo systemctl enable --now avahi-daemon

echo "Installing systemd services for user $SERVICE_USER..."

sudo tee /etc/systemd/system/openlivestock-gateway.service >/dev/null <<EOF
[Unit]
Description=OpenLivestock BLE Gateway
After=bluetooth.service network-online.target
Wants=bluetooth.service network-online.target

[Service]
Type=simple
User=$SERVICE_USER
WorkingDirectory=$SCRIPT_DIR
ExecStartPre=+/bin/bash $SCRIPT_DIR/ensure_bluetooth.sh
ExecStart=/bin/bash $SCRIPT_DIR/run_gateway.sh
Restart=always
RestartSec=10
Environment=PYTHONUNBUFFERED=1
Environment=OLG_SKIP_BLUETOOTH_PREFLIGHT=1

[Install]
WantedBy=multi-user.target
EOF

sudo tee /etc/systemd/system/openlivestock-dashboard.service >/dev/null <<EOF
[Unit]
Description=OpenLivestock Gateway Dashboard
After=network.target

[Service]
Type=simple
User=$SERVICE_USER
WorkingDirectory=$SCRIPT_DIR
ExecStart=/bin/bash $SCRIPT_DIR/run_dashboard.sh
Restart=always
RestartSec=10
Environment=PYTHONUNBUFFERED=1

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable --now openlivestock-gateway.service openlivestock-dashboard.service

echo
echo "Services installed."
echo "Gateway status:   systemctl status openlivestock-gateway --no-pager"
echo "Dashboard status: systemctl status openlivestock-dashboard --no-pager"
echo "Gateway logs:     journalctl -u openlivestock-gateway -f"
echo "Dashboard URL:    http://$(hostname).local:8080"
