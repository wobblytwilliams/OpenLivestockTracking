#!/usr/bin/env bash
set -euo pipefail

run_short() {
  if command -v timeout >/dev/null 2>&1; then
    timeout 8s "$@" || true
  else
    "$@" || true
  fi
}

if command -v systemctl >/dev/null 2>&1; then
  run_short sudo systemctl enable --now bluetooth >/dev/null 2>&1
  run_short sudo systemctl start bluetooth >/dev/null 2>&1
fi

if command -v rfkill >/dev/null 2>&1; then
  run_short sudo rfkill unblock bluetooth
fi

if command -v btmgmt >/dev/null 2>&1; then
  run_short sudo btmgmt power on >/dev/null 2>&1
fi

if command -v bluetoothctl >/dev/null 2>&1; then
  if ! bluetoothctl show 2>/dev/null | grep -q "Powered: yes"; then
    printf "power on\nquit\n" | run_short bluetoothctl >/dev/null 2>&1
  fi

  if ! bluetoothctl show 2>/dev/null | grep -q "Powered: yes"; then
    echo "Bluetooth adapter is not powered on."
    echo "Try: sudo systemctl restart bluetooth && bluetoothctl power on"
    echo "If this is the first setup run, reboot the Pi once and retry."
    exit 1
  fi
fi
