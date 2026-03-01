# Audio (PipeWire/Pulse) on Raspberry Pi OS / Debian

The run script starts PipeWire or Pulse so the kiosk gets sound and music+flip mix. The app uses **paplay** (no libpulse link); install `pulseaudio-utils` for the `paplay` command.

## 1. Install packages

**PipeWire (recommended):**
```bash
sudo apt install pipewire pipewire-pulse pulseaudio-utils
```
`paplay` from `pulseaudio-utils` sends audio to PipeWire; the server mixes music and flip.

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
