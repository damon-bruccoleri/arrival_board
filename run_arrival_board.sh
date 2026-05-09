#!/bin/bash
set -u
# Hardening: no boot.log writes for normal operation. Only failures are written (see build failure below).
# exec >>"$HOME/arrival_board/boot.log" 2>&1
# set -x

AB_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$AB_ROOT" || exit 1
# Same env as at boot (SDL_VIDEODRIVER=kmsdrm, etc.) so it runs when started manually
if [ -f "$AB_ROOT/arrival_board.env" ]; then
  set -a
  . "$AB_ROOT/arrival_board.env"
  set +a
fi
# Force SDL through PipeWire/Pulse unless explicitly overridden.
# This avoids SDL falling back to direct ALSA on the wrong device.
export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-pulse}"
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
BOOTLOG="${BOOT_LOG_PATH:-$AB_ROOT/boot.log}"
if [ ! -f "$AB_ROOT/tools/asoundrc" ]; then
  echo "$(date -Iseconds): tools/asoundrc missing" >> "$BOOTLOG"
fi
if [ -x "$AB_ROOT/tools/startup_self_check.sh" ]; then
  "$AB_ROOT/tools/startup_self_check.sh" || true
fi

# Unmute Pi HDMI output (try HDMI control first, then Master; often muted after boot).
# 25% ≈ overall ÷4 vs previous 100% default (see audio.c per-stream gains).
amixer -c vc4hdmi0 set HDMI 25% unmute 2>/dev/null || amixer -c vc4hdmi0 set Master 25% unmute 2>/dev/null || true

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
  # PipeWire often marks vc4 HDMI ports "not available" while ALSA still works;
  # forcing output:hdmi-stereo creates the sink so SDL Pulse sees real HDMI.
  while read -r _ card _; do
    case "$card" in *hdmi*)
      pactl set-card-profile "$card" output:hdmi-stereo 2>/dev/null || true ;;
    esac
  done < <(pactl list short cards 2>/dev/null)
  # Prefer HDMI (sink name differs Pi Zero vs Pi 4); avoid auto_null dummy sink.
  hdmi_sink="$(pactl list short sinks 2>/dev/null | awk '/hdmi/ && $2 !~ /auto_null/ { print $2; exit }')"
  if [ -n "$hdmi_sink" ]; then
    pactl set-default-sink "$hdmi_sink" 2>/dev/null || true
  else
    pactl set-default-sink alsa_output.platform-fef00700.hdmi.hdmi-stereo 2>/dev/null || true
  fi
fi

PI_MODEL="$(tr -d '\000' </proc/device-tree/model 2>/dev/null || true)"
IS_PI_ZERO=0
case "$PI_MODEL" in *Zero*) IS_PI_ZERO=1 ;; esac

hdmi_has_1080p() {
  grep -q '^1920x1080$' /sys/class/drm/card0-HDMI-A-1/modes 2>/dev/null
}

# Force KMS to re-read EDID so 1080p modes appear after a service restart.
# Only needed on Pi Zero family; must run while no DRM client holds the display.
ensure_1080p_modes() {
  [ "$IS_PI_ZERO" -eq 1 ] || return 0
  hdmi_has_1080p && return 0

  for attempt in 1 2 3; do
    echo detect | sudo tee /sys/class/drm/card0-HDMI-A-1/status >/dev/null 2>&1 || true
    sleep 2
    if hdmi_has_1080p; then
      return 0
    fi
  done

  # All re-detect attempts failed — reboot is the only reliable recovery.
  local stamp_file="/tmp/arrival_board_1080p_reboot.stamp"
  local now last
  now="$(date +%s)"
  last="$(cat "$stamp_file" 2>/dev/null || echo 0)"
  if [ $((now - last)) -lt 300 ]; then
    echo "$(date -Iseconds): 1080p reboot skipped (cooldown, last $(( now - last ))s ago)" >> "$BOOTLOG"
    return 1
  fi
  echo "$now" > "$stamp_file" 2>/dev/null || true
  echo "$(date -Iseconds): 1080p recovery reboot (modes stuck at low-res after 3 detect attempts)" >> "$BOOTLOG"
  sync
  sudo reboot
  sleep 60
}

while true; do
  # Wait for any lingering previous instance to fully release the DRM device.
  for _ in 1 2 3 4 5; do
    pgrep -x arrival_board >/dev/null 2>&1 || break
    sleep 1
  done

  ensure_1080p_modes

  ./arrival_board
  sleep 2
done
