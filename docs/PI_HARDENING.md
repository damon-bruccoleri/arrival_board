# Raspberry Pi Hardening for Power-Loss Safety

Use these steps to reduce SD card writes and corruption risk. Apply in order.

## 1. Pre-create steam_puff.png (no app writes on first run)

Run before going read-only:

```bash
cd /home/damon/arrival_board
python3 tools/gen_steam_puff.py   # requires: pip install Pillow
```

Or use the Makefile: `make prepare-readonly`

If Pillow is not installed, run the arrival board once in read-write mode—it will create the file.

---

## 2. Add tmpfs mounts (no writes to SD for temp/logs)

Append these lines to `/etc/fstab`. Backup first: `sudo cp /etc/fstab /etc/fstab.bak`

```bash
# tmpfs for arrival_board hardening - reduces SD writes
tmpfs    /tmp                    tmpfs    defaults,noatime,mode=1777,size=64M    0  0
tmpfs    /var/log                tmpfs    defaults,noatime,mode=0755,size=32M    0  0
tmpfs    /var/run                tmpfs    defaults,noatime,mode=0755,size=8M     0  0
```

Then reboot. **Warning:** Logs will be lost on reboot. If you need persistent logs, omit `/var/log` or use log2ram instead.

---

## 3. Install and configure log2ram (optional)

Buffers logs in RAM, syncs to SD less often or on shutdown:

```bash
sudo apt install log2ram
```

Edit `/etc/log2ram.conf` if needed (defaults often work). Reboot.

---

## 4. Disable swap (reduces heavy SD writes)

```bash
sudo dphys-swapfile swapoff
sudo systemctl disable dphys-swapfile
```

Or, to use zram (swap in RAM) instead:

```bash
sudo apt install zram-tools
# Configure in /etc/default/zramswap
```

---

## 5. Mount boot/root read-only with overlay (advanced)

Only do this after verifying everything works. Requires editing `/boot/cmdline.txt` and `/etc/fstab`.

**Raspberry Pi OS read-only root:** See official docs:  
https://www.raspberrypi.com/documentation/computers/raspberry-pi-os.html#making-the-file-system-read-only

Key steps:
- Add `boot=overlay` or configure overlay in cmdline
- Add `overlay` and `tmpfs` entries to fstab
- Create writable overlay directory
- Reboot into read-only mode

---

## 6. Do NOT redirect app logs to disk

If you run the arrival board from a systemd service or script, do **not** use:

```bash
./arrival_board >> /var/log/arrival_board.log 2>&1   # BAD - writes to SD
```

Run without redirecting stderr, or redirect to `/dev/null` if you want to suppress output:

```bash
./arrival_board 2>/dev/null
```

---

## Summary: what you can do without code changes

| Item                    | Effort | Effect                          |
|-------------------------|--------|---------------------------------|
| Pre-create steam_puff   | Low    | No app write on first run       |
| tmpfs for /tmp, /var/log| Medium | No temp/log writes to SD        |
| log2ram                 | Low    | Fewer log writes                |
| Disable swap            | Low    | No swap writes                  |
| Read-only root          | High   | No persistent writes to SD      |
