#!/usr/bin/env bash
# startup_self_check.sh — Validate setup-mode prerequisites at boot/runtime.
# Safe/read-only checks only; no network state changes.

set -u

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BOOTLOG="${BOOT_LOG_PATH:-$ROOT_DIR/boot.log}"
NETWORK_SCRIPT="$ROOT_DIR/tools/config_network.sh"
MODE_SCRIPT="$ROOT_DIR/tools/config_mode.sh"
PORTAL_SCRIPT="$ROOT_DIR/tools/config_portal/portal.py"
SUDOERS_FILE="/etc/sudoers.d/arrival-board-config"
SELF_CHECK_STATUS_PATH="${CONFIG_SELF_CHECK_STATUS_PATH:-/tmp/arrival_board_self_check_status}"

failed=0
first_failure=""

rm -f "$SELF_CHECK_STATUS_PATH" 2>/dev/null || true

log_fail() {
  local msg="$1"
  failed=1
  if [ -z "$first_failure" ]; then
    first_failure="$msg"
  fi
  echo "$(date -Iseconds): SELF_CHECK_FAIL: $msg" >> "$BOOTLOG"
  echo "SELF_CHECK_FAIL: $msg" >&2
}

log_warn() {
  local msg="$1"
  echo "$(date -Iseconds): SELF_CHECK_WARN: $msg" >> "$BOOTLOG"
  echo "SELF_CHECK_WARN: $msg" >&2
}

check_file_present() {
  local path="$1"
  local label="$2"
  if [ ! -f "$path" ]; then
    log_fail "$label missing at $path"
    return
  fi
  if [ ! -r "$path" ]; then
    log_fail "$label is not readable: $path"
  fi
}

check_executable() {
  local path="$1"
  local label="$2"
  if [ ! -x "$path" ]; then
    log_warn "$label is not executable ($path); bash wrapper may still work"
  fi
}

check_file_present "$NETWORK_SCRIPT" "config_network.sh"
check_file_present "$MODE_SCRIPT" "config_mode.sh"
check_file_present "$PORTAL_SCRIPT" "config portal"

check_executable "$NETWORK_SCRIPT" "config_network.sh"
check_executable "$MODE_SCRIPT" "config_mode.sh"

if [ -f "$NETWORK_SCRIPT" ] && ! bash -n "$NETWORK_SCRIPT" 2>/dev/null; then
  log_fail "config_network.sh has shell syntax errors"
fi
if [ -f "$MODE_SCRIPT" ] && ! bash -n "$MODE_SCRIPT" 2>/dev/null; then
  log_fail "config_mode.sh has shell syntax errors"
fi
if [ -f "$PORTAL_SCRIPT" ] && ! python3 -m py_compile "$PORTAL_SCRIPT" 2>/dev/null; then
  log_fail "portal.py failed python compile-check"
fi

if [ ! -f "$SUDOERS_FILE" ]; then
  log_fail "sudoers helper file missing: $SUDOERS_FILE"
fi

# Validate that the kiosk user can invoke the helper command through sudoers
# without executing state-changing subcommands.
if [ -f "$NETWORK_SCRIPT" ] && ! sudo -n "$NETWORK_SCRIPT" --help >/dev/null 2>&1; then
  log_fail "sudo -n access to config_network.sh failed (check $SUDOERS_FILE)"
fi

if [ "$failed" -eq 1 ]; then
  printf 'Self-check failed: %s\n' "$first_failure" > "$SELF_CHECK_STATUS_PATH" 2>/dev/null || true
  chmod 666 "$SELF_CHECK_STATUS_PATH" 2>/dev/null || true
  exit 1
fi
exit 0
