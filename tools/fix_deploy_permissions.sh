#!/usr/bin/env bash
# fix_deploy_permissions.sh — Restore execute bits on launcher scripts after a deploy
# that did not preserve Unix modes (e.g. some tar/scp workflows from Windows).
#
# Idempotent. Safe to run after every sync. Used by tools/install_autostart.sh,
# tools/sync_repo_to_pi.sh, and may be run manually on the Pi.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

chmod +x "${ROOT}/run_arrival_board.sh"
find "${ROOT}/tools" -type f -name '*.sh' -exec chmod +x {} +
if [ -f "${ROOT}/tools/config_portal/portal.py" ]; then
  chmod +x "${ROOT}/tools/config_portal/portal.py"
fi
