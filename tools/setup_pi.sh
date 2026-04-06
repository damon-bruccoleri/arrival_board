#!/usr/bin/env bash
# setup_pi.sh — Prepare a fresh Raspberry Pi Zero 2 W (64-bit Lite) for Arrival Board.
#
# Usage:
#   git clone git@github.com:damon-bruccoleri/arrival_board.git ~/arrival_board
#   cd ~/arrival_board && bash tools/setup_pi.sh
#
# Safe to re-run (idempotent). Requires sudo for system changes.
# After the script finishes, edit ~/arrival_board/arrival_board.env and reboot.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BOOT_CONFIG="/boot/firmware/config.txt"
[ -f "$BOOT_CONFIG" ] || BOOT_CONFIG="/boot/config.txt"

log()  { printf '\n\033[1;32m>>> %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33mWARN: %s\033[0m\n' "$*"; }

# ---------------------------------------------------------------------------
# Sanity checks
# ---------------------------------------------------------------------------
if [ ! -f "$PROJECT_DIR/Makefile" ]; then
  echo "ERROR: Run this script from inside the arrival_board repo."
  echo "  cd ~/arrival_board && bash tools/setup_pi.sh"
  exit 1
fi

# ---------------------------------------------------------------------------
# 1. System update
# ---------------------------------------------------------------------------
log "1/11  Updating system packages"
sudo apt-get update -qq
sudo DEBIAN_FRONTEND=noninteractive apt-get full-upgrade -y -qq

# ---------------------------------------------------------------------------
# 2. Install build and runtime dependencies
# ---------------------------------------------------------------------------
log "2/11  Installing Arrival Board dependencies"
PKGS=(
  # Build
  build-essential pkg-config
  # SDL2
  libsdl2-dev libsdl2-ttf-dev libsdl2-image-dev
  # JSON
  libcjson-dev
  # Fonts
  fonts-noto-core fonts-noto-color-emoji
  # Audio (PipeWire + paplay)
  pipewire pipewire-pulse wireplumber pulseaudio-utils alsa-utils
  # Utilities
  curl unzip git
  # Dev convenience
  htop
  # zram (step 5)
  zram-tools
)
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq "${PKGS[@]}"

# ---------------------------------------------------------------------------
# 3. GPU memory split
# ---------------------------------------------------------------------------
log "3/11  Configuring GPU memory (128 MB)"
sudo cp -n "$BOOT_CONFIG" "${BOOT_CONFIG}.bak" 2>/dev/null || true

ensure_config() {
  local key="$1" value="$2"
  if grep -q "^${key}=" "$BOOT_CONFIG" 2>/dev/null; then
    sudo sed -i "s/^${key}=.*/${key}=${value}/" "$BOOT_CONFIG"
  else
    # Append under [all] if present, otherwise at end of file
    if grep -q '^\[all\]' "$BOOT_CONFIG" 2>/dev/null; then
      sudo sed -i "/^\[all\]/a ${key}=${value}" "$BOOT_CONFIG"
    else
      echo "${key}=${value}" | sudo tee -a "$BOOT_CONFIG" >/dev/null
    fi
  fi
}

ensure_config gpu_mem 128
ensure_config arm_boost 1
ensure_config disable_overscan 1

# Ensure vc4-kms-v3d overlay is present (usually default on Lite)
if ! grep -q '^dtoverlay=vc4-kms-v3d' "$BOOT_CONFIG" 2>/dev/null; then
  ensure_config dtoverlay vc4-kms-v3d
fi

echo "  gpu_mem=128, arm_boost=1, disable_overscan=1, vc4-kms-v3d"

# ---------------------------------------------------------------------------
# 4. CPU governor → performance (persist via systemd)
# ---------------------------------------------------------------------------
log "4/11  Setting CPU governor to performance"

GOVERNOR_SERVICE="/etc/systemd/system/cpu-performance.service"
if [ ! -f "$GOVERNOR_SERVICE" ]; then
  sudo tee "$GOVERNOR_SERVICE" >/dev/null <<'UNIT'
[Unit]
Description=Set CPU governor to performance
After=sysinit.target

[Service]
Type=oneshot
ExecStart=/bin/sh -c 'echo performance | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor'
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
UNIT
  sudo systemctl daemon-reload
  sudo systemctl enable cpu-performance.service
fi
# Apply immediately
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor >/dev/null 2>&1 || true
echo "  governor=performance"

# ---------------------------------------------------------------------------
# 5. SD card hardening: disable swap file, enable zram, add tmpfs
# ---------------------------------------------------------------------------
log "5/11  Hardening SD card (disable swap, zram, tmpfs)"

# Disable disk swap
if systemctl is-enabled dphys-swapfile 2>/dev/null | grep -q enabled; then
  sudo dphys-swapfile swapoff 2>/dev/null || true
  sudo systemctl disable dphys-swapfile 2>/dev/null || true
  echo "  dphys-swapfile disabled"
fi

# Remove /swapfile entry from fstab if present
if grep -q '^/swapfile' /etc/fstab 2>/dev/null; then
  sudo sed -i '/^\/swapfile/d' /etc/fstab
  echo "  removed /swapfile from fstab"
fi

# Configure zram (compressed in-RAM swap, no SD writes)
ZRAM_CONF="/etc/default/zramswap"
if [ -f "$ZRAM_CONF" ]; then
  sudo sed -i 's/^#\?PERCENTAGE=.*/PERCENTAGE=25/' "$ZRAM_CONF"
  sudo sed -i 's/^#\?PRIORITY=.*/PRIORITY=100/' "$ZRAM_CONF"
  echo "  zram configured (25% of RAM)"
fi

# tmpfs for /tmp and /var/log (reduce SD writes)
add_tmpfs() {
  local mount="$1" size="$2" mode="$3"
  local line="tmpfs  ${mount}  tmpfs  defaults,noatime,mode=${mode},size=${size}  0  0"
  if ! grep -q "^tmpfs.*${mount}" /etc/fstab 2>/dev/null; then
    echo "$line" | sudo tee -a /etc/fstab >/dev/null
    echo "  added tmpfs ${mount} (${size})"
  fi
}
add_tmpfs /tmp        64M 1777
add_tmpfs /var/log    16M 0755

# ---------------------------------------------------------------------------
# 6. Disable unnecessary services
# ---------------------------------------------------------------------------
log "6/11  Disabling unnecessary services"

for svc in bluetooth.service triggerhappy.service; do
  if systemctl is-enabled "$svc" 2>/dev/null | grep -q enabled; then
    sudo systemctl disable --now "$svc" 2>/dev/null || true
    echo "  disabled $svc"
  fi
done
# Keep avahi-daemon for .local hostname resolution (Cursor SSH uses it)
echo "  avahi-daemon kept (needed for hostname.local SSH)"

# ---------------------------------------------------------------------------
# 7. Audio: PipeWire user services
# ---------------------------------------------------------------------------
log "7/11  Setting up PipeWire / PulseAudio user services"

systemctl --user enable pipewire pipewire-pulse wireplumber 2>/dev/null || true
systemctl --user start  pipewire pipewire-pulse wireplumber 2>/dev/null || true

# Enable linger so user services start at boot without a TTY login
sudo loginctl enable-linger "$(id -un)" 2>/dev/null || true
echo "  pipewire enabled, linger enabled for $(id -un)"

# ---------------------------------------------------------------------------
# 8. Create arrival_board.env
# ---------------------------------------------------------------------------
log "8/11  Creating arrival_board.env"

ENV_FILE="$PROJECT_DIR/arrival_board.env"
if [ ! -f "$ENV_FILE" ]; then
  cp "$PROJECT_DIR/arrival_board.env.example" "$ENV_FILE"
  # Set kmsdrm for Lite (console) and blank the MTA key placeholder
  sed -i 's/^MTA_KEY=.*/MTA_KEY=/' "$ENV_FILE"
  # Append SDL_VIDEODRIVER if not already present
  if ! grep -q '^SDL_VIDEODRIVER=' "$ENV_FILE"; then
    printf '\n# Lite / console: KMS DRM full-screen on HDMI\nSDL_VIDEODRIVER=kmsdrm\n' >> "$ENV_FILE"
  fi
  chmod 600 "$ENV_FILE"
  echo "  created $ENV_FILE (edit MTA_KEY before running)"
else
  echo "  $ENV_FILE already exists, skipping"
fi

# ---------------------------------------------------------------------------
# 9. Build the project
# ---------------------------------------------------------------------------
log "9/11  Building Arrival Board"
cd "$PROJECT_DIR"
make clean
make -j2
echo "  binary: $PROJECT_DIR/arrival_board"

# ---------------------------------------------------------------------------
# 10. Auto-start on boot (systemd user service)
# ---------------------------------------------------------------------------
log "10/11  Installing systemd user service for auto-start"
bash "$SCRIPT_DIR/install_autostart.sh"
echo "  arrival-board.service installed (starts on boot after reboot)"

# ---------------------------------------------------------------------------
# 11. Summary
# ---------------------------------------------------------------------------
log "11/11  Setup complete"
cat <<'EOF'

  What was done:
    - System packages updated
    - Build deps, SDL2, fonts, PipeWire, zram installed
    - gpu_mem=128, arm_boost=1, vc4-kms-v3d confirmed
    - CPU governor set to performance
    - Disk swap disabled, zram enabled, tmpfs for /tmp and /var/log
    - bluetooth + triggerhappy disabled (avahi kept for .local SSH)
    - PipeWire user services enabled with linger
    - arrival_board.env created (if new)
    - Project built (make; SDL2_image required)
    - arrival-board.service enabled for auto-start

  Next steps:
    1. Edit ~/arrival_board/arrival_board.env
       - Set MTA_KEY to your MTA Bus Time API key
       - Verify STOP_ID, STOP_LAT, STOP_LON
    2. sudo reboot
       (applies gpu_mem, tmpfs, zram, auto-start)

EOF
