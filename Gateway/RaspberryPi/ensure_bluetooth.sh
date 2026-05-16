#!/usr/bin/env bash
set -euo pipefail

if command -v systemctl >/dev/null 2>&1; then
  sudo systemctl enable --now bluetooth >/dev/null 2>&1 || sudo systemctl start bluetooth || true
fi

if command -v rfkill >/dev/null 2>&1; then
  sudo rfkill unblock bluetooth || true
fi

if command -v btmgmt >/dev/null 2>&1; then
  sudo btmgmt power on >/dev/null 2>&1 || true
fi

if command -v bluetoothctl >/dev/null 2>&1; then
  if ! bluetoothctl show 2>/dev/null | grep -q "Powered: yes"; then
    printf "power on\nquit\n" | bluetoothctl >/dev/null 2>&1 || true
  fi

  if ! bluetoothctl show 2>/dev/null | grep -q "Powered: yes"; then
    echo "Bluetooth adapter is not powered on."
    echo "Try: sudo systemctl restart bluetooth && bluetoothctl power on"
    echo "If this is the first setup run, reboot the Pi once and retry."
    exit 1
  fi
fi
