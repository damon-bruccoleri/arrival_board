#!/usr/bin/env bash
# install_autostart.sh — systemd user unit for Arrival Board at boot (Lite / kiosk).
# Idempotent. Run from repo: bash tools/install_autostart.sh
# Requires: loginctl enable-linger for user (done here with sudo).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SERVICE_DIR="${HOME}/.config/systemd/user"
SERVICE_FILE="${SERVICE_DIR}/arrival-board.service"

if [ ! -f "${REPO_ROOT}/run_arrival_board.sh" ]; then
  echo "ERROR: run_arrival_board.sh not found under ${REPO_ROOT}"
  exit 1
fi

mkdir -p "$SERVICE_DIR"

cat > "$SERVICE_FILE" <<UNIT
[Unit]
Description=Arrival Board kiosk
After=pipewire-pulse.service

[Service]
WorkingDirectory=${REPO_ROOT}
ExecStart=${REPO_ROOT}/run_arrival_board.sh
Restart=always
RestartSec=3

[Install]
WantedBy=default.target
UNIT

chmod +x "${REPO_ROOT}/run_arrival_board.sh" 2>/dev/null || true
chmod +x "${REPO_ROOT}/tools/config_mode.sh" \
         "${REPO_ROOT}/tools/config_network.sh" \
         "${REPO_ROOT}/tools/config_portal/portal.py" 2>/dev/null || true

systemctl --user daemon-reload
systemctl --user enable arrival-board.service

sudo loginctl enable-linger "$(id -un)"

printf '\nInstalled %s\n' "$SERVICE_FILE"
echo "  Linger enabled for $(id -un) (user services run at boot without login)."
echo "  Verify: systemctl --user status arrival-board.service"
echo "  Start now: systemctl --user start arrival-board.service"
echo "  Or reboot. Stop any manual run_arrival_board.sh first to avoid duplicates."
