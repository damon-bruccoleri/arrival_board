#!/usr/bin/env bash
# Convert the flip MP3 to WAV for use with aplay (no mpg123 needed).
# Requires ffmpeg or sox. Run: ./tools/convert_mp3_to_flip_wav.sh

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MP3=""
for p in "${SCRIPT_DIR}/daviddumaisaudio-steampunk-mechanical-gadget-188052.mp3" \
         "${SCRIPT_DIR}/../daviddumaisaudio-steampunk-mechanical-gadget-188052.mp3"; do
  [ -f "$p" ] && MP3="$p" && break
done
WAV="${SCRIPT_DIR}/flip.wav"

if [ -z "$MP3" ]; then
  echo "Error: MP3 not found (daviddumaisaudio-steampunk-mechanical-gadget-188052.mp3)" >&2
  exit 1
fi

if command -v ffmpeg >/dev/null 2>&1; then
  ffmpeg -y -i "$MP3" -acodec pcm_s16le -ar 44100 -ac 1 "$WAV" 2>/dev/null
  echo "Converted $MP3 -> $WAV (ffmpeg)"
elif command -v sox >/dev/null 2>&1; then
  sox "$MP3" -r 44100 -c 1 "$WAV" 2>/dev/null
  echo "Converted $MP3 -> $WAV (sox)"
else
  echo "Error: Install ffmpeg or sox: sudo apt install ffmpeg" >&2
  exit 1
fi
