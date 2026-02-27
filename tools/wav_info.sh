#!/usr/bin/env bash
# Print WAV file info by reading the header only (no sox, no ALSA — won't hang).
# Usage: ./tools/wav_info.sh file.wav [file2.wav ...]

wav_info() {
  local f="$1"
  [ ! -f "$f" ] && echo "Not a file: $f" >&2 && return 1
  [ ! -r "$f" ] && echo "Not readable: $f" >&2 && return 1
  local size; size=$(stat -c%s "$f" 2>/dev/null || stat -f%z "$f" 2>/dev/null)
  [ "$size" -lt 44 ] 2>/dev/null && echo "Too small for WAV: $f" >&2 && return 1

  # Read first 44 bytes as hex (od is POSIX; no sox/ALSA)
  local hex; hex=$(head -c 44 "$f" | od -A n -t x1 -v 2>/dev/null | tr -d ' \n')
  [ ${#hex} -lt 88 ] && echo "Could not read: $f" >&2 && return 1

  # RIFF/WAVE
  [ "${hex:0:8}" != "52494646" ] && echo "Not RIFF: $f" >&2 && return 1
  [ "${hex:16:8}" != "57415645" ] && echo "Not WAV: $f" >&2 && return 1

  # fmt: channels (bytes 22-23 LE), sample rate (24-27 LE), bits (34-35 LE)
  local channels=$((16#${hex:46:2}${hex:44:2}))
  local rate=$((16#${hex:54:2}${hex:52:2}${hex:50:2}${hex:48:2}))
  local bits=$((16#${hex:70:2}${hex:68:2}))

  echo "$f: ${rate} Hz, ${channels} ch, ${bits} bit, ${size} bytes"
}

if [ $# -eq 0 ]; then
  echo "Usage: $0 file.wav [file2.wav ...]" >&2
  echo "  (Safe alternative to sox --info; does not touch ALSA, so it won't hang.)" >&2
  exit 0
fi

for f in "$@"; do
  wav_info "$f" || true
done
