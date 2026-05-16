#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if [ ! -f .venv/bin/activate ]; then
  echo "Missing .venv. Run: bash setup_pi.sh"
  exit 1
fi

bash ensure_bluetooth.sh

. .venv/bin/activate
echo "Bluetooth ready. Starting gateway scanner..."
python -u olg_gateway.py --data-dir GatewayData "$@"
