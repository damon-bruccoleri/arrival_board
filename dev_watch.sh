#!/bin/bash
# Watch source files; on change, rebuild and restart the app.
# Run this on the Pi (e.g. from console) so the app can use the display.
# Requires: inotify-tools (sudo apt-get install inotify-tools)

set -e
cd "$(dirname "$0")"

if ! command -v inotifywait >/dev/null 2>&1; then
  echo "Install inotify-tools: sudo apt-get install inotify-tools"
  exit 1
fi

if [ -f "$HOME/arrival_board/arrival_board.env" ]; then
  set -a
  . "$HOME/arrival_board/arrival_board.env"
  set +a
fi

build_and_run() {
  echo "--- build ---"
  if make -j USE_SDL_IMAGE=1 2>/dev/null || make -j; then
    pkill -f arrival_board 2>/dev/null || true
    sleep 1
    echo "--- run ---"
    ./run_arrival_board.sh &
    echo $!
    return 0
  fi
  echo 0
  return 1
}

PID=$(build_and_run)
[ "$PID" = "0" ] && exit 1

while true; do
  inotifywait -q -e close_write -r --include '\.(c|h)$' --include 'Makefile$' . 2>/dev/null || true
  echo "Change detected, rebuilding..."
  kill $PID 2>/dev/null || true
  wait $PID 2>/dev/null || true
  NEW_PID=$(build_and_run)
  [ "$NEW_PID" != "0" ] && PID=$NEW_PID
done
