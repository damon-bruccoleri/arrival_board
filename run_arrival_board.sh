#!/bin/bash
set -u
# Hardening: no boot.log writes for normal operation. Only failures are written (see build failure below).
# exec >>"$HOME/arrival_board/boot.log" 2>&1
# set -x

cd "$HOME/arrival_board" || exit 1
# Same env as at boot (SDL_VIDEODRIVER=kmsdrm, etc.) so it runs when started manually
if [ -f "$HOME/arrival_board/arrival_board.env" ]; then
  set -a
  . "$HOME/arrival_board/arrival_board.env"
  set +a
fi
ulimit -n "${ULIMIT_NOFILE:-16384}" || true

# operate on controlling tty (tty1) without hardcoding redirections that can hang
setterm -blank 0 -powerdown 0 -powersave off || true
printf "\033[?25l" || true

# Normal operation diagnostics commented out to reduce disk writes.
# echo "=== run_arrival_board.sh ==="
# date
# echo "USER=$(id -un) UID=$(id -u) ULIMIT_NOFILE=$(ulimit -n)"
# echo "ENV: SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-<unset>} SDL_RENDER_DRIVER=${SDL_RENDER_DRIVER:-<unset>} DISPLAY=${DISPLAY:-<unset>}"
# echo "CFG: STOP_ID=${STOP_ID:-<unset>} ROUTE_FILTER=${ROUTE_FILTER:-} POLL_SECONDS=${POLL_SECONDS:-10}"

# If tools/asoundrc is missing, log to boot.log (no copying; user must install ALSA config if needed).
BOOTLOG="${BOOT_LOG_PATH:-$HOME/arrival_board/boot.log}"
if [ ! -f "$HOME/arrival_board/tools/asoundrc" ]; then
  echo "$(date -Iseconds): tools/asoundrc missing" >> "$BOOTLOG"
fi

# Unmute Pi HDMI output (try HDMI control first, then Master; often muted after boot)
amixer -c vc4hdmi0 set HDMI 100% unmute 2>/dev/null || amixer -c vc4hdmi0 set Master 100% unmute 2>/dev/null || true

# Start PipeWire or Pulse so kiosk has sound and music+flip mix. Verify with pactl before using.
ensure_runtime_dir() {
  if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
    export XDG_RUNTIME_DIR=/run/user/$(id -u)
    [ -d "$XDG_RUNTIME_DIR" ] || return 1
  fi
  return 0
}
pulse_ok() {
  command -v pactl &>/dev/null && pactl info &>/dev/null
}
start_sound_server() {
  ensure_runtime_dir || true
  if pulse_ok; then return 0; fi
  if systemctl --user is-active pipewire &>/dev/null; then sleep 2; pulse_ok && return 0; fi
  if command -v pulseaudio &>/dev/null && pulseaudio --check 2>/dev/null; then return 0; fi
  if systemctl --user start pipewire pipewire-pulse 2>/dev/null; then
    for _ in 1 2 3 4 5; do sleep 1; pulse_ok && return 0; done
  fi
  if command -v pipewire &>/dev/null; then
    pipewire &>/dev/null &
    sleep 1
    (command -v pipewire-pulse &>/dev/null && pipewire-pulse &>/dev/null &) || true
    for _ in 1 2 3 4 5; do sleep 1; pulse_ok && return 0; done
  fi
  if command -v pulseaudio &>/dev/null; then
    pulseaudio --start 2>/dev/null
    for _ in 1 2 3 4 5; do sleep 1; pulse_ok && return 0; done
  fi
  return 1
}
# Leave APLAY_DEVICE unset to use Pulse/PipeWire (music+flip mix; kiosk owns device). Set in arrival_board.env for ALSA.
if [ -n "${APLAY_DEVICE:-}" ]; then
  # echo "Using APLAY_DEVICE=$APLAY_DEVICE (ALSA)"
  :
else
  # echo "APLAY_DEVICE unset (using Pulse/PipeWire)"
  start_sound_server || true
  # Prefer HDMI so music/ferry/flip play to the display (Pi default is often headphone jack).
  pactl set-default-sink alsa_output.platform-fef00700.hdmi.hdmi-stereo 2>/dev/null || true
fi

while true; do
  ./arrival_board
  sleep 2
done
