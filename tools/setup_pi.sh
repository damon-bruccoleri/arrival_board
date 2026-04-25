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
BOOT_CMDLINE="/boot/firmware/cmdline.txt"
[ -f "$BOOT_CMDLINE" ] || BOOT_CMDLINE="/boot/cmdline.txt"
USB_GADGET_IP="${USB_GADGET_IP:-10.55.0.1/24}"
USB_GADGET_HOST="${USB_GADGET_IP%%/*}"
USB_GADGET_CON="${USB_GADGET_CON:-ArrivalBoard-USB-Gadget}"

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
  # GPIO input for setup switch
  libgpiod-dev gpiod
  # Fonts
  fonts-noto-core fonts-noto-color-emoji
  # Audio (PipeWire + paplay)
  pipewire pipewire-pulse wireplumber pulseaudio-utils alsa-utils
  # Phone setup hotspot / WiFi configuration
  hostapd dnsmasq iw iptables network-manager python3
  # USB commissioning and local discovery
  openssh-server avahi-daemon
  # Utilities
  ca-certificates curl unzip git sox
  # Dev convenience
  htop
  # zram (step 6)
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

ensure_boot_config_line() {
  local line="$1"
  if ! grep -Fxq "$line" "$BOOT_CONFIG" 2>/dev/null; then
    echo "$line" | sudo tee -a "$BOOT_CONFIG" >/dev/null
  fi
}

ensure_config gpu_mem 128
ensure_config arm_boost 1
ensure_config disable_overscan 1
ensure_boot_config_line "dtoverlay=dwc2"

# Ensure vc4-kms-v3d overlay is present (usually default on Lite)
if ! grep -q '^dtoverlay=vc4-kms-v3d' "$BOOT_CONFIG" 2>/dev/null; then
  ensure_boot_config_line "dtoverlay=vc4-kms-v3d"
fi

echo "  gpu_mem=128, arm_boost=1, disable_overscan=1, dwc2, vc4-kms-v3d"

# ---------------------------------------------------------------------------
# 4. USB gadget Ethernet for commissioning
# ---------------------------------------------------------------------------
log "4/12  Enabling USB gadget Ethernet for commissioning"

ensure_cmdline_modules() {
  local modules="$1"
  if [ ! -f "$BOOT_CMDLINE" ]; then
    warn "Boot cmdline not found at $BOOT_CMDLINE; skipping modules-load=${modules}"
    return
  fi
  sudo cp -n "$BOOT_CMDLINE" "${BOOT_CMDLINE}.bak" 2>/dev/null || true
  if tr ' ' '\n' < "$BOOT_CMDLINE" | grep -q '^modules-load='; then
    local current
    current="$(tr ' ' '\n' < "$BOOT_CMDLINE" | awk -F= '$1 == "modules-load" {print $2; exit}')"
    for module in ${modules//,/ }; do
      if ! printf '%s' "$current" | tr ',' '\n' | grep -qx "$module"; then
        current="${current},${module}"
      fi
    done
    sudo sed -i "s/\(^\| \)modules-load=[^ ]*/ modules-load=${current}/" "$BOOT_CMDLINE"
    sudo sed -i 's/^ //' "$BOOT_CMDLINE"
  else
    sudo sed -i "1s/$/ modules-load=${modules}/" "$BOOT_CMDLINE"
  fi
}

ensure_cmdline_modules "dwc2,g_ether"

sudo systemctl enable --now ssh 2>/dev/null || true
sudo systemctl enable --now avahi-daemon 2>/dev/null || true
sudo systemctl enable --now NetworkManager 2>/dev/null || true

if command -v nmcli >/dev/null 2>&1; then
  if nmcli -t -f NAME connection show | grep -Fxq "$USB_GADGET_CON"; then
    sudo nmcli connection modify "$USB_GADGET_CON" \
      connection.interface-name usb0 \
      ipv4.method shared \
      ipv4.addresses "$USB_GADGET_IP" \
      ipv6.method ignore \
      connection.autoconnect yes
  else
    sudo nmcli connection add type ethernet ifname usb0 con-name "$USB_GADGET_CON" \
      ipv4.method shared \
      ipv4.addresses "$USB_GADGET_IP" \
      ipv6.method ignore \
      connection.autoconnect yes
  fi
  echo "  usb0 gadget enabled at ${USB_GADGET_IP}; SSH via arrivalboard.local or ${USB_GADGET_HOST} after reboot"
else
  warn "nmcli not found; USB gadget kernel mode enabled, but usb0 networking was not configured"
fi

# ---------------------------------------------------------------------------
# 5. CPU governor → performance (persist via systemd)
# ---------------------------------------------------------------------------
log "5/12  Setting CPU governor to performance"

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
# 6. SD card hardening: disable swap file, enable zram, add tmpfs
# ---------------------------------------------------------------------------
log "6/12  Hardening SD card (disable swap, zram, tmpfs)"

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
# 7. Disable unnecessary services
# ---------------------------------------------------------------------------
log "7/12  Disabling unnecessary services"

for svc in bluetooth.service triggerhappy.service; do
  if systemctl is-enabled "$svc" 2>/dev/null | grep -q enabled; then
    sudo systemctl disable --now "$svc" 2>/dev/null || true
    echo "  disabled $svc"
  fi
done
# Keep avahi-daemon for .local hostname resolution (Cursor SSH uses it)
echo "  avahi-daemon kept (needed for hostname.local SSH)"

# ---------------------------------------------------------------------------
# 8. Audio: PipeWire user services
# ---------------------------------------------------------------------------
log "8/12  Setting up PipeWire / PulseAudio user services"

systemctl --user enable pipewire pipewire-pulse wireplumber 2>/dev/null || true
systemctl --user start  pipewire pipewire-pulse wireplumber 2>/dev/null || true

# Enable linger so user services start at boot without a TTY login
sudo loginctl enable-linger "$(id -un)" 2>/dev/null || true
echo "  pipewire enabled, linger enabled for $(id -un)"

# ---------------------------------------------------------------------------
# 9. Create arrival_board.env
# ---------------------------------------------------------------------------
log "9/12  Creating arrival_board.env"

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
# 10. Build the project
# ---------------------------------------------------------------------------
log "10/12  Building Arrival Board"
cd "$PROJECT_DIR"
make clean
make -j2
echo "  binary: $PROJECT_DIR/arrival_board"

# ---------------------------------------------------------------------------
# 11. Auto-start on boot (systemd user service)
# ---------------------------------------------------------------------------
log "11/12  Installing systemd user service for auto-start"
bash "$SCRIPT_DIR/install_autostart.sh"
echo "  arrival-board.service installed (starts on boot after reboot)"

# Allow the kiosk process to start the setup hotspot and apply WiFi without an
# interactive sudo password. The web portal still only exposes this helper while
# the physical GPIO13 setup switch is pressed.
SUDOERS_FILE="/etc/sudoers.d/arrival-board-config"
sudo tee "$SUDOERS_FILE" >/dev/null <<EOF
$(id -un) ALL=(root) NOPASSWD: ${PROJECT_DIR}/tools/config_network.sh *
EOF
sudo chmod 0440 "$SUDOERS_FILE"
sudo visudo -cf "$SUDOERS_FILE" >/dev/null
chmod +x "$PROJECT_DIR/tools/config_mode.sh" "$PROJECT_DIR/tools/config_network.sh" "$PROJECT_DIR/tools/config_portal/portal.py" 2>/dev/null || true
echo "  config-mode sudo helper installed"

# ---------------------------------------------------------------------------
# 12. Summary
# ---------------------------------------------------------------------------
log "12/12  Setup complete"
cat <<'EOF'

  What was done:
    - System packages updated
    - Build deps, SDL2, fonts, PipeWire, zram, SSH, Avahi installed
    - GPIO, hotspot, and WiFi setup dependencies installed
    - USB gadget Ethernet enabled for direct USB commissioning
    - gpu_mem=128, arm_boost=1, dwc2, vc4-kms-v3d confirmed
    - CPU governor set to performance
    - Disk swap disabled, zram enabled, tmpfs for /tmp and /var/log
    - bluetooth + triggerhappy disabled (avahi kept for .local SSH)
    - PipeWire user services enabled with linger
    - arrival_board.env created (if new)
    - Project built (make; SDL2_image required)
    - arrival-board.service enabled for auto-start
    - GPIO13 setup-mode helper permission installed

  Next steps:
    1. Edit ~/arrival_board/arrival_board.env
       - Set MTA_KEY to your MTA Bus Time API key
       - Verify STOP_ID, STOP_LAT, STOP_LON
    2. sudo reboot
       (applies gpu_mem, USB gadget, tmpfs, zram, auto-start)
    3. For USB access, connect the Pi data USB port to your computer and SSH to:
       ssh <pi-user>@arrivalboard.local
       or ssh <pi-user>@10.55.0.1

EOF
