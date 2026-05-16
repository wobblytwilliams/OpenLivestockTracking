#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "Installing Raspberry Pi packages..."
sudo apt update
sudo apt install -y git python3-venv python3-pip bluetooth bluez rfkill

if getent group bluetooth >/dev/null 2>&1; then
  sudo usermod -aG bluetooth "$USER" || true
fi

bash ensure_bluetooth.sh || true

echo "Creating Python virtual environment..."
python3 -m venv .venv
. .venv/bin/activate

python -m pip install --upgrade pip
python -m pip install -r requirements.txt

mkdir -p GatewayData

echo
echo "Gateway setup complete."
echo "Run the BLE downloader with: bash run_gateway.sh"
echo "Run the dashboard in another SSH tab with: bash run_dashboard.sh"
echo "If Bluetooth access fails, reboot the Pi once and try again."
