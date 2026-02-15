#!/bin/bash
# Generate a short mechanical "flip" click for tile animation (optional).
# Requires: sox (sudo apt-get install sox libsox-fmt-all)
# Usage: ./tools/gen_flip_sound.sh [output.wav]

OUT="${1:-$HOME/arrival_board/flip.wav}"
mkdir -p "$(dirname "$OUT")"
if command -v sox >/dev/null 2>&1; then
  # Two quick clicks to suggest a mechanical flap
  sox -n -r 44100 -c 1 "$OUT" synth 0.02 square 600 vol 0.25 : synth 0.03 square 400 vol 0.2
  echo "Wrote $OUT - set SOUND_FLIP=$OUT in arrival_board.env"
else
  echo "Install sox: sudo apt-get install sox libsox-fmt-all"
  exit 1
fi
