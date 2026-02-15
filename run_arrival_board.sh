#!/bin/sh
set -x
cd "$HOME/arrival_board" || exit 1

# Load env file and export vars to children
if [ -f "$HOME/arrival_board/arrival_board.env" ]; then
  set -a
  . "$HOME/arrival_board/arrival_board.env"
  set +a
fi

echo "---- run_arrival_board.sh start $(date) ----"
# don't dump the key value; just show whether it's set
[ -n "${MTA_KEY:-}" ] && echo "MTA_KEY=set" || echo "MTA_KEY=NOT_SET"
echo "STOP_ID=${STOP_ID:-NOT_SET}  POLL_SECONDS=${POLL_SECONDS:-NOT_SET}"
echo "SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-NOT_SET}  SDL_RENDER_DRIVER=${SDL_RENDER_DRIVER:-NOT_SET}"
echo "FONT_PATH=${FONT_PATH:-NOT_SET}"

# Warm up HDMI audio so first beep isn't clipped
if [ -n "${APLAY_DEVICE:-}" ] && [ -n "${SOUND_NEW:-}" ] && [ -f "${SOUND_NEW:-}" ]; then
  sleep 1
  aplay -q -D "$APLAY_DEVICE" "$SOUND_NEW" >/dev/null 2>&1 || true
fi

# Keep trying to start arrival_board; log exit code every time.
# This keeps X alive and prevents chromium from stealing the screen.
while :; do
  echo "---- starting arrival_board $(date) ----"
  ./arrival_board
  rc=$?
  echo "---- arrival_board exited rc=$rc at $(date) ----"
  # short backoff so we don't tight-loop
  sleep 2
done
