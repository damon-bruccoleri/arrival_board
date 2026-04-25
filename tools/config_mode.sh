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
  local connected=0
  while true; do
    if iw dev "$IFACE" station dump 2>/dev/null | grep -q '^Station '; then
      if [ "$connected" -ne 1 ]; then
        status "Connected"
        connected=1
      fi
    else
      if [ "$connected" -ne 0 ]; then
        status "Hotspot enabled. Connect to ArrivalBoard"
        connected=0
      fi
    fi
    sleep 1
  done
}

case "${1:-}" in
  start)
    status "Loading bus stop list"
    python3 "$ROOT_DIR/tools/config_portal/portal.py" --refresh-stops >/dev/null 2>&1 || true
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
