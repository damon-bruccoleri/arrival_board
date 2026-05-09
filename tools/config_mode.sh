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
    local current_status=""
    if [ -f "$STATUS_PATH" ]; then
      current_status="$(tr -d '\r\n' < "$STATUS_PATH" 2>/dev/null || true)"
    fi
    case "$current_status" in
      Applying*|Reboot*|Saving\ configuration*|Apply\ failed*)
        break
        ;;
    esac
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
    start_err="$(mktemp /tmp/arrival_board_hotspot_start.XXXXXX)"
    if ! sudo -n /bin/bash "$SCRIPT_DIR/config_network.sh" start-ap 2>"$start_err"; then
      reason="$(tr '\n' ' ' < "$start_err" | sed -E 's/[[:space:]]+/ /g; s/^ //; s/ $//')"
      rm -f "$start_err" 2>/dev/null || true
      if [ -z "$reason" ]; then
        status "Apply failed: hotspot start failed"
      else
        status "Apply failed: hotspot start failed ($reason)"
      fi
      # Keep helper alive briefly so UI can show explicit failure text.
      sleep 10
      exit 1
    fi
    rm -f "$start_err" 2>/dev/null || true
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
