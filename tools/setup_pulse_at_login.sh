#!/bin/bash
# Run once so PipeWire/Pulse starts at kiosk login (and after reboot).
# Run as the kiosk user (or root with KIOSK_USER=damon).
# Usage: ./tools/setup_pulse_at_login.sh   OR   sudo KIOSK_USER=pi ./tools/setup_pulse_at_login.sh

set -e
KIOSK_USER="${KIOSK_USER:-$(id -un)}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "Setting up Pulse/PipeWire for user: $KIOSK_USER"

# 1. Enable linger so user's systemd runs at boot (needed for autologin kiosk)
if [ "$(id -u)" -eq 0 ]; then
  loginctl enable-linger "$KIOSK_USER" 2>/dev/null || true
  echo "Linger enabled for $KIOSK_USER"
else
  echo "To enable linger (optional), run: sudo loginctl enable-linger $KIOSK_USER"
fi

# 2. Enable PipeWire user services (run as the kiosk user)
if [ "$(id -un)" = "$KIOSK_USER" ]; then
  systemctl --user enable pipewire pipewire-pulse 2>/dev/null && echo "Enabled pipewire pipewire-pulse" || true
  systemctl --user enable wireplumber 2>/dev/null && echo "Enabled wireplumber" || true
  systemctl --user start pipewire pipewire-pulse 2>/dev/null && echo "Started pipewire pipewire-pulse" || true
  systemctl --user start wireplumber 2>/dev/null && echo "Started wireplumber" || true
  sleep 2
  if command -v pactl &>/dev/null && pactl info &>/dev/null; then
    echo "Pulse is running. Reboot or run arrival board; sound should use PipeWire/Pulse."
  else
    echo "Pulse not responding. Install: sudo apt install pipewire pipewire-pulse pulseaudio-utils; then re-run this script."
  fi
else
  echo "Run as $KIOSK_USER to enable user services: su - $KIOSK_USER -c 'systemctl --user enable pipewire pipewire-pulse; systemctl --user start pipewire pipewire-pulse'"
fi
