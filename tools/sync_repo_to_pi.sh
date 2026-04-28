#!/usr/bin/env bash
# sync_repo_to_pi.sh — Push the arrival_board tree to a Pi over SSH (developer machine).
#
# Requires rsync (Git Bash, WSL, macOS, Linux). Does not delete extra files on the Pi
# unless SYNC_DELETE=1 is set in the environment.
#
# Usage:
#   bash tools/sync_repo_to_pi.sh [user@]hostname_or_ip
# Example:
#   bash tools/sync_repo_to_pi.sh pi@ArrivalBoard.local
#
# Optional:
#   SYNC_DELETE=1 bash tools/sync_repo_to_pi.sh pi@host   # rsync --delete (dangerous if Pi has local-only files)
#   SYNC_EXCLUDE_GIT=1 ...                                 # omit .git/ for a smaller transfer

set -euo pipefail

DEST="${1:?Usage: $0 [user@]host}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if ! command -v rsync >/dev/null 2>&1; then
  echo "rsync not found. Install it or use Git Bash/WSL, or copy with:" >&2
  echo "  scp -r \"$ROOT\" \"$DEST:~/\"" >&2
  exit 1
fi

RSYNC_OPTS=(-avz -e ssh)
if [ "${SYNC_DELETE:-0}" = 1 ]; then
  RSYNC_OPTS+=(--delete)
fi

EXCLUDES=(
  --exclude 'arrival_board'
  --exclude '*.o'
  --exclude 'boot.log'
  --exclude '.cursor'
)
if [ "${SYNC_EXCLUDE_GIT:-0}" = 1 ]; then
  EXCLUDES+=(--exclude '.git')
fi

# Ensure parent exists on remote
ssh -o ConnectTimeout=15 "$DEST" 'mkdir -p ~/arrival_board'

rsync "${RSYNC_OPTS[@]}" "${EXCLUDES[@]}" \
  "$ROOT/" "$DEST:~/arrival_board/"

echo "Synced $ROOT/ -> $DEST:~/arrival_board/"
echo "On the Pi: cd ~/arrival_board && bash tools/setup_pi.sh   # first time, or: make clean && make && bash tools/install_autostart.sh"
