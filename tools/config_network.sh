#!/usr/bin/env bash
# Root helper for Arrival Board setup hotspot and WiFi apply.
set -euo pipefail

IFACE="${CONFIG_WIFI_IFACE:-wlan0}"
SSID="${CONFIG_AP_SSID:-ArrivalBoard}"
AP_ADDR="${CONFIG_AP_ADDR:-192.168.4.1}"
STATUS_PATH="${CONFIG_STATUS_PATH:-/tmp/arrival_board_config_status}"
SCAN_PATH="${CONFIG_SCAN_PATH:-/tmp/arrival_board_wifi_scan.json}"
HOSTAPD_CONF="/tmp/arrival_board_hostapd.conf"
DNSMASQ_CONF="/tmp/arrival_board_dnsmasq.conf"
HOSTAPD_PID="/tmp/arrival_board_hostapd.pid"
DNSMASQ_PID="/tmp/arrival_board_dnsmasq.pid"

status() {
  rm -f "$STATUS_PATH" 2>/dev/null || true
  printf '%s\n' "$*" > "$STATUS_PATH" 2>/dev/null || true
  if [ -n "${SUDO_USER:-}" ]; then
    chown "$SUDO_USER:$SUDO_USER" "$STATUS_PATH" 2>/dev/null || true
  fi
  chmod 0666 "$STATUS_PATH" 2>/dev/null || true
}

require_root() {
  if [ "$(id -u)" -ne 0 ]; then
    echo "config_network.sh must run as root" >&2
    exit 1
  fi
}

scan_networks() {
  python3 - "$IFACE" "$SCAN_PATH" <<'PY'
import json
import re
import subprocess
import sys

iface, out_path = sys.argv[1], sys.argv[2]
ssids = set()

def run(cmd):
    try:
        return subprocess.check_output(cmd, text=True, stderr=subprocess.DEVNULL, timeout=20)
    except Exception:
        return ""

text = run(["iw", "dev", iface, "scan"])
for match in re.finditer(r"^\s*SSID:\s*(.+)$", text, re.M):
    value = match.group(1).strip()
    if value:
        ssids.add(value)

if not ssids:
    text = run(["nmcli", "-t", "-f", "SSID", "dev", "wifi", "list", "ifname", iface])
    for line in text.splitlines():
        value = line.strip().replace("\\:", ":")
        if value:
            ssids.add(value)

with open(out_path, "w", encoding="utf-8") as f:
    json.dump(sorted(ssids), f)
PY
  chmod 644 "$SCAN_PATH" 2>/dev/null || true
}

stop_ap() {
  if [ -f "$DNSMASQ_PID" ]; then kill "$(cat "$DNSMASQ_PID")" 2>/dev/null || true; rm -f "$DNSMASQ_PID"; fi
  if [ -f "$HOSTAPD_PID" ]; then kill "$(cat "$HOSTAPD_PID")" 2>/dev/null || true; rm -f "$HOSTAPD_PID"; fi
  iptables -t nat -D PREROUTING -i "$IFACE" -p tcp --dport 80 -j REDIRECT --to-ports 8080 2>/dev/null || true
  ip addr flush dev "$IFACE" 2>/dev/null || true
}

restore_client_wifi() {
  rfkill unblock wifi 2>/dev/null || true
  nmcli radio wifi on 2>/dev/null || true
  nmcli dev set "$IFACE" managed yes 2>/dev/null || true
  systemctl start wpa_supplicant 2>/dev/null || true
  ip link set "$IFACE" up 2>/dev/null || true
  nmcli dev connect "$IFACE" 2>/dev/null || true
}

start_ap() {
  require_root
  status "Scanning nearby WiFi"
  scan_networks || true

  status "Starting hotspot"
  stop_ap
  systemctl stop wpa_supplicant 2>/dev/null || true
  nmcli dev set "$IFACE" managed no 2>/dev/null || true
  rfkill unblock wifi 2>/dev/null || true
  ip link set "$IFACE" down 2>/dev/null || true
  ip addr flush dev "$IFACE" 2>/dev/null || true
  ip link set "$IFACE" up
  ip addr add "$AP_ADDR/24" dev "$IFACE"

  cat > "$HOSTAPD_CONF" <<EOF
interface=$IFACE
driver=nl80211
ssid=$SSID
hw_mode=g
channel=6
wmm_enabled=0
auth_algs=1
EOF

  cat > "$DNSMASQ_CONF" <<EOF
interface=$IFACE
bind-interfaces
dhcp-range=192.168.4.20,192.168.4.80,255.255.255.0,12h
address=/#/$AP_ADDR
domain-needed
bogus-priv
EOF

  hostapd -B -P "$HOSTAPD_PID" "$HOSTAPD_CONF"
  dnsmasq --conf-file="$DNSMASQ_CONF" --pid-file="$DNSMASQ_PID"
  iptables -t nat -A PREROUTING -i "$IFACE" -p tcp --dport 80 -j REDIRECT --to-ports 8080 2>/dev/null || true
  status "Hotspot enabled. Connect to ArrivalBoard"
}

apply_config() {
  require_root
  local request="${1:?missing request json}"
  status "Applying WiFi configuration"
  python3 - "$request" "$IFACE" <<'PY'
import json
import os
import pathlib
import shlex
import subprocess
import sys

request, iface = sys.argv[1], sys.argv[2]
data = json.loads(pathlib.Path(request).read_text(encoding="utf-8"))
ssid = data.get("ssid", "")
mode = data.get("wifi_mode", "psk")

def quote(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'

def network_block() -> str:
    lines = ["network={", f"    ssid={quote(ssid)}"]
    if mode == "psk":
        lines += ["    key_mgmt=WPA-PSK", f"    psk={quote(data.get('psk', ''))}"]
    else:
        etype = data.get("enterprise_type", "peap-mschapv2")
        lines += ["    key_mgmt=WPA-EAP", f"    identity={quote(data.get('identity', ''))}"]
        if data.get("anonymous_identity"):
            lines.append(f"    anonymous_identity={quote(data['anonymous_identity'])}")
        if data.get("ca_cert"):
            lines.append(f"    ca_cert={quote(data['ca_cert'])}")
        if etype == "peap-mschapv2":
            lines += ["    eap=PEAP", "    phase2=\"auth=MSCHAPV2\"", f"    password={quote(data.get('enterprise_password', ''))}"]
        elif etype == "ttls-pap":
            lines += ["    eap=TTLS", "    phase2=\"auth=PAP\"", f"    password={quote(data.get('enterprise_password', ''))}"]
        elif etype == "tls":
            lines += ["    eap=TLS"]
            if data.get("client_cert"):
                lines.append(f"    client_cert={quote(data['client_cert'])}")
            if data.get("private_key"):
                lines.append(f"    private_key={quote(data['private_key'])}")
            if data.get("private_key_password"):
                lines.append(f"    private_key_passwd={quote(data['private_key_password'])}")
    lines += ["    priority=10", "}"]
    return "\n".join(lines) + "\n"

conf = pathlib.Path("/etc/wpa_supplicant/wpa_supplicant.conf")
conf.parent.mkdir(parents=True, exist_ok=True)
conf.write_text("country=US\nctrl_interface=DIR=/var/run/wpa_supplicant GROUP=netdev\nupdate_config=1\n\n" + network_block(), encoding="utf-8")
os.chmod(conf, 0o600)

if subprocess.call(["sh", "-c", "command -v nmcli >/dev/null 2>&1"]) == 0:
    subprocess.run(["nmcli", "connection", "delete", "ArrivalBoard-WiFi"], stderr=subprocess.DEVNULL)
    subprocess.run(["nmcli", "connection", "add", "type", "wifi", "ifname", iface, "con-name", "ArrivalBoard-WiFi", "ssid", ssid], check=True)
    if mode == "psk":
        subprocess.run(["nmcli", "connection", "modify", "ArrivalBoard-WiFi", "wifi-sec.key-mgmt", "wpa-psk", "wifi-sec.psk", data.get("psk", "")], check=True)
    else:
        etype = data.get("enterprise_type", "peap-mschapv2")
        args = ["nmcli", "connection", "modify", "ArrivalBoard-WiFi", "wifi-sec.key-mgmt", "wpa-eap", "802-1x.identity", data.get("identity", "")]
        if data.get("anonymous_identity"):
            args += ["802-1x.anonymous-identity", data["anonymous_identity"]]
        if data.get("ca_cert"):
            args += ["802-1x.ca-cert", data["ca_cert"]]
        if etype == "peap-mschapv2":
            args += ["802-1x.eap", "peap", "802-1x.phase2-auth", "mschapv2", "802-1x.password", data.get("enterprise_password", "")]
        elif etype == "ttls-pap":
            args += ["802-1x.eap", "ttls", "802-1x.phase2-auth", "pap", "802-1x.password", data.get("enterprise_password", "")]
        else:
            args += ["802-1x.eap", "tls"]
            if data.get("client_cert"):
                args += ["802-1x.client-cert", data["client_cert"]]
            if data.get("private_key"):
                args += ["802-1x.private-key", data["private_key"]]
            if data.get("private_key_password"):
                args += ["802-1x.private-key-password", data["private_key_password"]]
        subprocess.run(args, check=True)
    subprocess.run(["nmcli", "connection", "modify", "ArrivalBoard-WiFi", "connection.autoconnect", "yes"], check=True)
PY
  stop_ap
  restore_client_wifi
  status "Rebooting"
  systemctl reboot
}

case "${1:-}" in
  start-ap) start_ap ;;
  stop-ap) require_root; stop_ap; restore_client_wifi; status "Ready" ;;
  apply) shift; apply_config "$@" ;;
  *)
    echo "Usage: $0 {start-ap|stop-ap|apply REQUEST_JSON}" >&2
    exit 2
    ;;
esac
