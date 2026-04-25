#!/usr/bin/env python3
"""Phone setup portal for Arrival Board."""

from __future__ import annotations

import csv
from email import policy
from email.parser import BytesParser
import html
import io
import json
import os
import pathlib
import subprocess
import tempfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import sys
from urllib.parse import parse_qs, unquote_plus, urlparse
from urllib.request import urlopen
import zipfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
ENV_PATH = pathlib.Path(os.environ.get("ARRIVAL_BOARD_ENV", ROOT / "arrival_board.env"))
STATUS_PATH = pathlib.Path(os.environ.get("CONFIG_STATUS_PATH", "/tmp/arrival_board_config_status"))
SCAN_PATH = pathlib.Path(os.environ.get("CONFIG_SCAN_PATH", "/tmp/arrival_board_wifi_scan.json"))
STOPS_PATH = pathlib.Path(os.environ.get("CONFIG_STOPS_PATH", "/tmp/arrival_board_bus_stops.json"))
REQUEST_PATH = pathlib.Path(os.environ.get("CONFIG_REQUEST_PATH", "/tmp/arrival_board_wifi_request.json"))
CERT_DIR = pathlib.Path(os.environ.get("CONFIG_CERT_DIR", pathlib.Path.home() / ".config" / "arrival_board" / "certs"))
NETWORK_SCRIPT = ROOT / "tools" / "config_network.sh"
DEFAULT_GTFS_URL = "https://rrgtfsfeeds.s3.amazonaws.com/gtfs_busco.zip"


def set_status(message: str) -> None:
    try:
        STATUS_PATH.unlink(missing_ok=True)
    except Exception:
        pass
    try:
        STATUS_PATH.write_text(message + "\n", encoding="utf-8")
    except PermissionError:
        return


def current_env() -> dict[str, str]:
    data: dict[str, str] = {}
    if not ENV_PATH.exists():
        return data
    for line in ENV_PATH.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line or line.lstrip().startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        data[key.strip()] = value.strip().strip('"')
    return data


def update_env(values: dict[str, str]) -> None:
    ENV_PATH.parent.mkdir(parents=True, exist_ok=True)
    existing = ENV_PATH.read_text(encoding="utf-8", errors="replace").splitlines() if ENV_PATH.exists() else []
    seen: set[str] = set()
    out: list[str] = []
    for line in existing:
        if line.lstrip().startswith("#") or "=" not in line:
            out.append(line)
            continue
        key = line.split("=", 1)[0].strip()
        if key in values:
            out.append(f"{key}={values[key]}")
            seen.add(key)
        else:
            out.append(line)
    for key, value in values.items():
        if key not in seen:
            out.append(f"{key}={value}")
    fd, tmp_name = tempfile.mkstemp(prefix=".arrival_board.env.", dir=str(ENV_PATH.parent))
    with os.fdopen(fd, "w", encoding="utf-8") as tmp:
        tmp.write("\n".join(out).rstrip() + "\n")
    os.chmod(tmp_name, 0o600)
    os.replace(tmp_name, ENV_PATH)


def safe_filename(name: str) -> str:
    base = pathlib.Path(name or "uploaded.pem").name
    return "".join(ch for ch in base if ch.isalnum() or ch in "._-") or "uploaded.pem"


def save_upload(filename: str, content: bytes, prefix: str) -> str:
    if not filename or not content:
        return ""
    CERT_DIR.mkdir(parents=True, exist_ok=True)
    path = CERT_DIR / f"{prefix}-{safe_filename(filename)}"
    with path.open("wb") as f:
        f.write(content)
    os.chmod(path, 0o600)
    return str(path)


def load_scan() -> list[str]:
    if SCAN_PATH.exists():
        try:
            items = json.loads(SCAN_PATH.read_text(encoding="utf-8"))
            return sorted({str(item) for item in items if str(item).strip()})
        except Exception:
            pass
    return []


def refresh_stop_options() -> int:
    env = current_env()
    url = os.environ.get("GTFS_BUS_URL") or env.get("GTFS_BUS_URL") or DEFAULT_GTFS_URL
    try:
        STOPS_PATH.unlink(missing_ok=True)
    except Exception:
        pass

    with urlopen(url, timeout=25) as response:
        data = response.read()

    stops: list[dict[str, str]] = []
    seen: set[str] = set()
    with zipfile.ZipFile(io.BytesIO(data)) as zf:
        with zf.open("stops.txt") as raw:
            text = io.TextIOWrapper(raw, encoding="utf-8-sig", newline="")
            for row in csv.DictReader(text):
                value = (row.get("stop_code") or row.get("stop_id") or "").strip()
                if not value or value in seen:
                    continue
                seen.add(value)
                name = (row.get("stop_name") or "").strip()
                stops.append({"id": value, "label": name})

    stops.sort(key=lambda item: (not item["id"].isdigit(), item["id"]))
    STOPS_PATH.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp_name = tempfile.mkstemp(prefix=".arrival_board_bus_stops.", dir=str(STOPS_PATH.parent))
    with os.fdopen(fd, "w", encoding="utf-8") as tmp:
        json.dump(stops, tmp, separators=(",", ":"))
    os.chmod(tmp_name, 0o644)
    os.replace(tmp_name, STOPS_PATH)
    return len(stops)


def load_stop_options() -> list[dict[str, str]]:
    if not STOPS_PATH.exists():
        return []
    try:
        items = json.loads(STOPS_PATH.read_text(encoding="utf-8"))
    except Exception:
        return []
    out: list[dict[str, str]] = []
    for item in items:
        if not isinstance(item, dict):
            continue
        stop_id = str(item.get("id", "")).strip()
        if not stop_id:
            continue
        out.append({"id": stop_id, "label": str(item.get("label", "")).strip()})
    return out


def compact_stop_label(stop_id: str, label: str, max_len: int = 58) -> str:
    text = f"{stop_id} - {label}" if label else stop_id
    return text if len(text) <= max_len else text[: max_len - 1].rstrip() + "..."


def page(message: str = "") -> str:
    env = current_env()
    ssids = load_scan()
    stops = load_stop_options()
    if ssids:
        options = "\n".join(f'<option value="{html.escape(s)}">{html.escape(s)}</option>' for s in ssids)
    else:
        options = '<option value="">No networks found - enter manually below</option>'
    stop_hint = "Enter the MTA bus stop ID printed on the stop sign or shown in BusTime."
    current_key = html.escape(env.get("MTA_KEY", ""))
    current_stop = html.escape(env.get("STOP_ID", ""))
    stop_picker = ""
    if stops:
        stop_options = ['<option value="">Choose a bus stop, or enter manually below</option>']
        for item in stops:
            stop_id = item["id"]
            selected = " selected" if stop_id == env.get("STOP_ID", "") else ""
            label = compact_stop_label(stop_id, item["label"])
            stop_options.append(f'<option value="{html.escape(stop_id)}"{selected}>{html.escape(label)}</option>')
        stop_picker = f"""
      <label for="stop_select">Choose Bus Stop</label>
      <select id="stop_select" name="stop_select">
        {"".join(stop_options)}
      </select>"""
        stop_hint = "Choose a stop from the list, or type the MTA bus stop ID manually."
    msg = f'<p class="message">{html.escape(message)}</p>' if message else ""
    return f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Arrival Board Setup</title>
  <style>
    body {{ margin: 0; font-family: system-ui, sans-serif; background: #10131a; color: #f5f7fb; }}
    main {{ max-width: 760px; margin: 0 auto; padding: 24px; }}
    section {{ background: #1b202b; border-radius: 16px; padding: 18px; margin: 16px 0; }}
    label {{ display: block; font-weight: 700; margin-top: 14px; }}
    input, select, textarea, button {{ box-sizing: border-box; width: 100%; font: inherit; margin-top: 6px; padding: 12px; border-radius: 10px; border: 1px solid #465062; }}
    input, select, textarea {{ background: #0e1117; color: #fff; }}
    button {{ background: #38bdf8; color: #051018; font-weight: 800; border: 0; margin-top: 20px; }}
    a {{ color: #7dd3fc; }}
    .hint, .message {{ color: #cbd5e1; }}
    .enterprise, .tls, .ssid-other {{ display: none; }}
  </style>
</head>
<body>
<main>
  <h1>Arrival Board Setup</h1>
  {msg}
  <p class="hint">Get an MTA Bus Time API key at <a href="https://api.mta.info/" target="_blank" rel="noreferrer">https://api.mta.info/</a>, then enter your MTA key, bus stop ID, and home WiFi settings below.</p>
  <form method="post" action="/configure" enctype="multipart/form-data">
    <section>
      <h2>Bus Stop</h2>
      {stop_picker}
      <label for="stop_id">MTA Bus Stop ID / Number</label>
      <input id="stop_id" name="stop_id" value="{current_stop}" inputmode="numeric" autocomplete="off" placeholder="501627" required>
      <p class="hint">{html.escape(stop_hint)}</p>
    </section>
    <section>
      <h2>MTA</h2>
      <label for="mta_key">MTA API Key</label>
      <input id="mta_key" name="mta_key" value="{current_key}" autocomplete="off" required>
    </section>
    <section>
      <h2>WiFi</h2>
      <label for="ssid_select">Network SSID</label>
      <select id="ssid_select" name="ssid" required>
        {options}
        <option value="__other__">Other / hidden network</option>
      </select>
      <label for="ssid_other" class="ssid-other">Manual SSID</label>
      <input id="ssid_other" class="ssid-other" name="ssid_other" autocomplete="off">
      <label for="wifi_mode">Security</label>
      <select id="wifi_mode" name="wifi_mode">
        <option value="psk">Personal WPA/WPA2</option>
        <option value="enterprise">Enterprise WPA/WPA2</option>
      </select>
      <div class="personal">
        <label for="psk">WiFi Password</label>
        <input id="psk" name="psk" type="password" autocomplete="new-password">
      </div>
      <div class="enterprise">
        <label for="enterprise_type">Enterprise Type</label>
        <select id="enterprise_type" name="enterprise_type">
          <option value="peap-mschapv2">PEAP/MSCHAPv2</option>
          <option value="ttls-pap">TTLS/PAP</option>
          <option value="tls">TLS with client certificate</option>
        </select>
        <label for="identity">Username / Identity</label>
        <input id="identity" name="identity" autocomplete="username">
        <label for="enterprise_password">Enterprise Password</label>
        <input id="enterprise_password" name="enterprise_password" type="password" autocomplete="new-password">
        <label for="anonymous_identity">Anonymous Identity (optional)</label>
        <input id="anonymous_identity" name="anonymous_identity">
        <label for="ca_cert">CA Certificate (optional)</label>
        <input id="ca_cert" name="ca_cert" type="file">
        <div class="tls">
          <label for="client_cert">Client Certificate</label>
          <input id="client_cert" name="client_cert" type="file">
          <label for="private_key">Private Key</label>
          <input id="private_key" name="private_key" type="file">
          <label for="private_key_password">Private Key Password (optional)</label>
          <input id="private_key_password" name="private_key_password" type="password">
        </div>
      </div>
      <button type="submit">Configure</button>
    </section>
  </form>
</main>
<script>
const mode = document.querySelector("#wifi_mode");
const etype = document.querySelector("#enterprise_type");
const ssidSelect = document.querySelector("#ssid_select");
const stopSelect = document.querySelector("#stop_select");
const stopInput = document.querySelector("#stop_id");
function refresh() {{
  const ent = mode.value === "enterprise";
  document.querySelector(".enterprise").style.display = ent ? "block" : "none";
  document.querySelector(".personal").style.display = ent ? "none" : "block";
  document.querySelector(".tls").style.display = ent && etype.value === "tls" ? "block" : "none";
  document.querySelectorAll(".ssid-other").forEach(el => {{
    el.style.display = ssidSelect.value === "__other__" ? "block" : "none";
  }});
}}
mode.addEventListener("change", refresh);
etype.addEventListener("change", refresh);
ssidSelect.addEventListener("change", refresh);
if (stopSelect && stopInput) {{
  stopSelect.addEventListener("change", () => {{
    if (stopSelect.value) stopInput.value = stopSelect.value;
  }});
}}
refresh();
</script>
</body>
</html>"""


def parse_form(handler: BaseHTTPRequestHandler) -> tuple[dict[str, str], dict[str, tuple[str, bytes]]]:
    length = int(handler.headers.get("Content-Length", "0") or "0")
    body = handler.rfile.read(length)
    ctype = handler.headers.get("Content-Type", "")
    fields: dict[str, str] = {}
    files: dict[str, tuple[str, bytes]] = {}

    if ctype.startswith("multipart/form-data"):
        raw = (
            f"Content-Type: {ctype}\r\n"
            f"Content-Length: {len(body)}\r\n"
            "\r\n"
        ).encode("utf-8") + body
        msg = BytesParser(policy=policy.default).parsebytes(raw)
        for part in msg.iter_parts():
            name = part.get_param("name", header="content-disposition")
            if not name:
                continue
            payload = part.get_payload(decode=True) or b""
            filename = part.get_filename()
            if filename:
                files[name] = (filename, payload)
            else:
                charset = part.get_content_charset() or "utf-8"
                fields[name] = payload.decode(charset, errors="replace").strip()
    else:
        text = body.decode("utf-8", errors="replace")
        for key, values in parse_qs(text, keep_blank_values=True).items():
            fields[unquote_plus(key)] = values[-1].strip() if values else ""
    return fields, files


class Handler(BaseHTTPRequestHandler):
    def do_GET(self) -> None:
        path = urlparse(self.path).path
        if path == "/scan":
            body = json.dumps(load_scan()).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_html(page())

    def do_POST(self) -> None:
        if urlparse(self.path).path != "/configure":
            self.send_error(404)
            return
        form, uploads = parse_form(self)

        mta_key = form.get("mta_key", "")
        stop_id = form.get("stop_id", "") or form.get("stop_select", "")
        ssid = form.get("ssid_other", "") if form.get("ssid", "") == "__other__" else form.get("ssid", "")
        wifi_mode = form.get("wifi_mode", "")
        if not mta_key or not stop_id or not ssid:
            self.send_html(page("MTA key, bus stop number, and SSID are required."), 400)
            return
        if any(ch.isspace() for ch in stop_id):
            self.send_html(page("Bus stop number cannot contain spaces."), 400)
            return

        payload: dict[str, str] = {
            "ssid": ssid,
            "wifi_mode": wifi_mode,
            "psk": form.get("psk", ""),
            "enterprise_type": form.get("enterprise_type", ""),
            "identity": form.get("identity", ""),
            "enterprise_password": form.get("enterprise_password", ""),
            "anonymous_identity": form.get("anonymous_identity", ""),
            "private_key_password": form.get("private_key_password", ""),
        }
        for name in ("ca_cert", "client_cert", "private_key"):
            if name in uploads:
                filename, content = uploads[name]
                payload[name] = save_upload(filename, content, name)

        if wifi_mode == "psk" and not payload["psk"]:
            self.send_html(page("WiFi password is required for personal WiFi."), 400)
            return
        if wifi_mode == "enterprise" and not payload["enterprise_type"]:
            self.send_html(page("Enterprise type is required."), 400)
            return

        set_status("Saving configuration")
        update_env({"MTA_KEY": mta_key, "STOP_ID": stop_id})
        REQUEST_PATH.write_text(json.dumps(payload), encoding="utf-8")
        os.chmod(REQUEST_PATH, 0o600)
        set_status("Applying WiFi and rebooting")

        try:
            subprocess.Popen(["sudo", "-n", str(NETWORK_SCRIPT), "apply", str(REQUEST_PATH)])
        except Exception as exc:
            set_status(f"Apply failed: {exc}")
            self.send_html(page(f"Could not apply WiFi settings: {exc}"), 500)
            return

        self.send_html(page("Configuration saved. The board is applying WiFi settings and will reboot. Your phone will disconnect from the setup network."))

    def send_html(self, text: str, code: int = 200) -> None:
        body = text.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Cache-Control", "no-store, max-age=0")
        self.send_header("Pragma", "no-cache")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt: str, *args: object) -> None:
        print("portal:", fmt % args)


def main() -> None:
    if len(sys.argv) > 1 and sys.argv[1] == "--refresh-stops":
        try:
            count = refresh_stop_options()
            set_status(f"Loaded {count} bus stops")
            print(f"Loaded {count} bus stops")
            raise SystemExit(0)
        except Exception as exc:
            set_status("Bus stop list unavailable")
            print(f"Bus stop list unavailable: {exc}", file=sys.stderr)
            raise SystemExit(1)

    set_status("Hotspot enabled. Connect to ArrivalBoard")
    server = ThreadingHTTPServer(("0.0.0.0", 8080), Handler)
    server.serve_forever()


if __name__ == "__main__":
    main()
