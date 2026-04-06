#!/bin/bash
# Copy local arrival_board.env to another Pi (e.g. PI2).
# Prerequisites: SSH works non-interactively (ssh-copy-id user@pi2) or run this and enter password when prompted.
# Usage: ./copy_env_to_pi2.sh [user@]hostname_or_ip
# Example: ./copy_env_to_pi2.sh admin@192.168.1.212
set -euo pipefail
DEST="${1:?Usage: $0 [user@]host}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/arrival_board.env"
if [[ ! -f "$SRC" ]]; then
  echo "Missing $SRC — create it from arrival_board.env.example first." >&2
  exit 1
fi
ssh -o ConnectTimeout=10 "$DEST" 'mkdir -p ~/arrival_board'
scp -p "$SRC" "$DEST:~/arrival_board/arrival_board.env"
ssh "$DEST" 'chmod 600 ~/arrival_board/arrival_board.env'
echo "Copied to $DEST:~/arrival_board/arrival_board.env"
