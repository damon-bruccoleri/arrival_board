# Phone Configuration Mode

Arrival Board can enter a local setup mode from a physical switch on GPIO13.
Wire the switch between GPIO13 and ground. The application requests GPIO13 as
an input with the internal pull-up enabled, so pressing the switch grounds the
input.

## User Flow

1. Press the GPIO13 setup switch.
2. Arrival Board pauses normal arrivals/weather updates.
3. The screen shows setup instructions.
4. On a phone, join WiFi network `ArrivalBoard`. No password is required.
5. Open `http://192.168.4.1`.
6. Enter the MTA key and WiFi settings, then tap `Configure`.
7. The Pi applies the WiFi configuration, disconnects the phone setup hotspot,
   and reboots.

## WiFi Support

The setup page supports:

- Personal WPA/WPA2 with SSID and password.
- WPA/WPA2 Enterprise PEAP/MSCHAPv2.
- WPA/WPA2 Enterprise TTLS/PAP.
- WPA/WPA2 Enterprise TLS with CA, client certificate, and private key upload.

Uploaded enterprise certificates are stored under
`~/.config/arrival_board/certs` with owner-only permissions. The network helper
also writes `/etc/wpa_supplicant/wpa_supplicant.conf` and, when NetworkManager
is present, creates an `ArrivalBoard-WiFi` connection.

## Installation Notes

Run `tools/setup_pi.sh` after pulling this change. It installs GPIO/hotspot
dependencies and creates a narrow sudoers rule that allows the kiosk user to run
`tools/config_network.sh` without an interactive password only while setup mode
is being launched.
