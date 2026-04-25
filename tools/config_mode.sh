#!/usr/bin/env bash
# Start Arrival Board phone configuration mode.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
STATUS_PATH="${CONFIG_STATUS_PATH:-/tmp/arrival_board_config_status}"
IFACE="${CONFIG_WIFI_IFACE:-wlan0}"

status() {
  rm -f "$STATUS_PATH" 2>/dev/null || true
  printf '%s\n' "$*" > "$STATUS_PATH" 2>/dev/null || true
}

monitor_phone_connection() {
  while true; do
    if iw dev "$IFACE" station dump 2>/dev/null | grep -q '^Station '; then
      status "Connected"
      return 0
    fi
    sleep 1
  done
}

case "${1:-}" in
  start)
    status "Starting hotspot"
    sudo -n "$SCRIPT_DIR/config_network.sh" start-ap
    status "Hotspot enabled. Connect to ArrivalBoard"
    monitor_phone_connection &
    cd "$ROOT_DIR"
    exec python3 "$ROOT_DIR/tools/config_portal/portal.py"
    ;;
  *)
    echo "Usage: $0 start" >&2
    exit 2
    ;;
esac
