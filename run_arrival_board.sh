#!/bin/bash
set -u
exec >>"$HOME/arrival_board/boot.log" 2>&1
set -x

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

echo "=== run_arrival_board.sh ==="
date
echo "USER=$(id -un) UID=$(id -u) ULIMIT_NOFILE=$(ulimit -n)"
echo "ENV: SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-<unset>} SDL_RENDER_DRIVER=${SDL_RENDER_DRIVER:-<unset>} DISPLAY=${DISPLAY:-<unset>}"
echo "CFG: STOP_ID=${STOP_ID:-<unset>} ROUTE_FILTER=${ROUTE_FILTER:-} POLL_SECONDS=${POLL_SECONDS:-10}"

while true; do
  echo "--- build ---"
  make clean || true
  # Prefer build with background image (Steampunk bus). Install: sudo apt-get install libsdl2-image-dev
  if ! make -j USE_SDL_IMAGE=1; then
    if ! make -j; then
      echo "build failed; retrying in 2s"
      sleep 2
      continue
    fi
  fi

  echo "--- run loop ---"
  ./arrival_board
  rc=$?
  echo "arrival_board exited rc=$rc at $(date); restarting in 2s"
  sleep 2
done
