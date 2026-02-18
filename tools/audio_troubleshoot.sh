#!/usr/bin/env bash
# Troubleshoot HDMI audio on Raspberry Pi for arrival_board.
# Run: ./tools/audio_troubleshoot.sh

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR/.."
BASE="$PWD"

echo "=== Arrival Board Audio Troubleshooter ==="
echo ""

# 1. Check sound files exist
echo "1. Sound files:"
for f in tools/flip.wav tools/Seaport_Steampunk_Final_Mix.wav; do
  if [ -f "$f" ]; then
    echo "   OK: $f"
  else
    echo "   MISSING: $f"
  fi
done
echo ""

# 2. List ALSA devices
echo "2. ALSA playback devices (aplay -L):"
aplay -L 2>/dev/null || echo "   aplay -L failed"
echo ""

# 3. Test aplay with flip.wav
if [ -f tools/flip.wav ]; then
  echo "3. Testing aplay with tools/flip.wav:"
  for dev in "default" "plughw:CARD=vc4hdmi0,DEV=0" "hdmi:CARD=vc4hdmi0,DEV=0" "plughw:0,0"; do
    printf "   %-35s ... " "$dev"
    if aplay -q -D "$dev" tools/flip.wav 2>/dev/null; then
      echo "OK (did you hear a sound?)"
    else
      echo "failed"
      aplay -D "$dev" tools/flip.wav 2>&1 | head -3 || true
    fi
  done
else
  echo "3. Skipping aplay test (tools/flip.wav not found). Run: make tools/flip.wav"
fi
echo ""

# 4. Check /boot config
echo "4. HDMI config in /boot (if present):"
for f in /boot/firmware/config.txt /boot/config.txt; do
  if [ -f "$f" ]; then
    echo "   $f:"
    grep -E "hdmi_drive|hdmi_force|hdmi_group" "$f" 2>/dev/null || echo "   (no hdmi audio settings)"
    break
  fi
done
echo ""

# 5. Suggestions
echo "5. Common fixes for Pi HDMI audio:"
echo "   - Add to /boot/firmware/config.txt (or /boot/config.txt):"
echo "       hdmi_drive=2"
echo "       hdmi_force_hotplug=1"
echo "       hdmi_force_edid_audio=1"
echo "   - Pi 4: use HDMI0 (left micro-HDMI port) for audio"
echo "   - In arrival_board.env, try: APLAY_DEVICE=plughw:CARD=vc4hdmi0,DEV=0"
echo "   - Run 'aplay -L' and pick a device from the list"
echo ""
