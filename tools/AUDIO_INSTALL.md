# Audio (PipeWire/Pulse) on Raspberry Pi OS / Debian

The run script starts PipeWire or Pulse so the kiosk gets sound and music+flip mix.

## 1. Install packages (no `pipewire-audio-client-utils` on Debian)

**PipeWire (recommended):**
```bash
sudo apt install pipewire pipewire-pulse pulseaudio-utils
sudo apt install libpulse-dev   # so run script can build with USE_PULSE=1
```

**For `paplay` (optional if you build with USE_PULSE=1):**
```bash
sudo apt install pulseaudio-utils
```
`paplay` comes from `pulseaudio-utils` and works with PipeWire when `pipewire-pulse` is installed.

**Or use libpulse only (no paplay needed):**  
Build with `USE_PULSE=1` (the run script does this when `libpulse-dev` is installed). Then install:
```bash
sudo apt install libpulse-dev   # for build
sudo apt install pipewire pipewire-pulse
```
Music and flip use the Pulse API; no `pulseaudio-utils` required.

**PulseAudio instead of PipeWire:**
```bash
sudo apt install pulseaudio pulseaudio-utils
```

## 2. One-time setup so Pulse runs after reboot (kiosk)

Run **once** as the kiosk user (e.g. `pi` or `damon`):

```bash
cd "$HOME/arrival_board"
./tools/setup_pulse_at_login.sh
```

This enables PipeWire user services so they start at login. If your kiosk autologs in, optionally enable linger so the user’s systemd runs at boot:

```bash
sudo loginctl enable-linger YOUR_USER
```

Then reboot. The run script will start PipeWire if it isn’t already running and use it for sound (music+flip mix).
