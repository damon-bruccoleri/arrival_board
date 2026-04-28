#!/usr/bin/env bash
# verify_aplay_device.sh — Play a short WAV on an ALSA device (test APLAY_DEVICE).
#
# Usage:
#   aplay -L | sed -n '1,40p'    # list devices
#   bash tools/verify_aplay_device.sh 'plughw:CARD=vc4hdmi0,DEV=0'
#
# Uses Front_Center.wav if present, else $REPO/flip.wav.

set -euo pipefail
DEVICE="${1:?Usage: $0 ALSA_device   e.g. plughw:CARD=vc4hdmi0,DEV=0}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CAND=("/usr/share/sounds/alsa/Front_Center.wav" "$ROOT/flip.wav")
WAV=""
for f in "${CAND[@]}"; do
  if [[ -f "$f" ]]; then WAV="$f"; break; fi
done
if [[ -z "$WAV" ]]; then
  echo "No test WAV found (install alsa-utils or add flip.wav)." >&2
  exit 1
fi
echo "Playing: $WAV"
echo "Device:  $DEVICE"
aplay -q -D "$DEVICE" "$WAV"
echo "OK"
