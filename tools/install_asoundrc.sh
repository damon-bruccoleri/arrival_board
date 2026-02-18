#!/usr/bin/env bash
# Install ALSA config so background music and flip sound can play at same time.
# Copies tools/asoundrc to ~/.asoundrc

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SCRIPT_DIR/asoundrc"
DST="${HOME}/.asoundrc"

if [ ! -f "$SRC" ]; then
  echo "Error: $SRC not found" >&2
  exit 1
fi

cp "$SRC" "$DST"
echo "Installed $DST"
