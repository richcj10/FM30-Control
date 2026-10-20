#!/usr/bin/env python3
"""
Preview the web UI on a PC, no hardware: serves data/index.html and a fake
ESP API backed by the STM32 mock (tools/mock_stm32.py).

    python tools/web_preview.py              # http://127.0.0.1:8030/
    python tools/web_preview.py --lock test  # with control password "test"

It mirrors the ESP's API shapes closely enough for UI work. It doesn't
repeat the ESP's input validation; the C++ tests cover that.
"""
import argparse
import json
import secrets
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mock_stm32 import Stm32Model  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
PROTECTED = {"FREQ", "POWER", "U6TRIM", "U6", "VTSCALE", "RDSLEVEL", "DEFAULTS"}

lock = threading.Lock()
model = Stm32Model()
model.exec("TX ON")
slots = [["WiFi OK", "127.0.0.1"], ["", ""], ["", ""], ["", ""]]
lcd = {"rotate": None, "page": None, "on": None}
config = {"hostname": "fm30-preview", "http_user": "admin", "ota_pass_set": False, "http_pass_set": False,
          "lock_pass_set": False,
          "mqtt": {"host": "", "port": 1883, "user": "", "pass_set": False, "base": "fm30",
                   "ha_discovery": True, "ha_prefix": "homeassistant", "allow_locked": False}}
lock_pass = ""
tokens = set()
stats = {"sent": 0}
t0 = time.time()


def exec_line(line):
    with lock:
        stats["sent"] += 1
        out, _ = model.exec(line)
        reply = out[-1] if out else ""
        help_lines = out[:-1]
        word = line.split(" ")[0].upper()
        if reply.startswith("OK") and word == "LCD":
            parts = line.split(" ", 3)
            sub = parts[1].upper() if len(parts) > 1 else ""
            if sub == "MSG":
                n = int(parts[2])
                text = parts[3] if len(parts) > 3 else ""
                top, bar, bot = text.partition("|")
                slots[n - 1] = [top, bot] if bar else ["", text]
            elif sub == "ROTATE":
                lcd["rotate"] = [int(p) for p in reply.split()[3:]]
            elif sub == "PAGE":
                lcd["page"] = int(parts[2])
            else:
                lcd["on"] = sub == "ON"
        return reply, help_lines


class H(BaseHTTPRequestHandler):
    def log_message(self, fmt, *a):
        pass

    def send_json(self, code, obj, raw=None):
        body = (raw if raw is not None else json.dumps(obj)).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def unlocked(self):
        return not lock_pass or self.headers.get("X-Unlock") in tokens

    def locked(self):
        self.send_json(401, {"ok": False, "locked": True, "error": "locked: enter the control password"})

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n).decode("utf-8", "replace")
        ct = self.headers.get("Content-Type", "")
        if ct.startswith("application/json"):
            return json.loads(raw or "{}")
        if ct.startswith("application/x-www-form-urlencoded"):
            return {k: v[0] for k, v in parse_qs(raw).items()}
        return {"text": raw}

    def run_cmd(self, line):
        if line.split(" ")[0].upper() in PROTECTED and not self.unlocked():
            return self.locked()
        if line.upper().startswith("LCD MSG 1"):
            return self.send_json(400, {"ok": False, "error": "LCD MSG slot 1 is reserved for the WiFi status; use 2-4"})
        reply, help_lines = exec_line(line)
        d = {"cmd": line, "ok": reply.startswith("OK") or reply.startswith("{"), "reply": reply}
        if help_lines:
            d["help"] = help_lines
        self.send_json(200 if d["ok"] else 422, d)

    def do_GET(self):
        if not self.path.startswith("/api/"):
            name = "index.html" if self.path == "/" else self.path.lstrip("/").split("?")[0]
            f = (ROOT / "data" / name).resolve()
            if (ROOT / "data") not in f.parents or not f.is_file():
                return self.send_json(404, {"ok": False, "error": "not found"})
            body = f.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8" if f.suffix == ".html" else "application/octet-stream")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path == "/api/status":
            with lock:
                model.tick()
                raw = model.status_json()
            self.send_json(200, None, raw)
        elif self.path == "/api/info":
            self.send_json(200, {
                "fw": "preview", "hostname": config["hostname"], "ip": "127.0.0.1", "rssi": -55,
                "uptime": int(time.time() - t0), "heap": 30000, "mqtt": "off", "ota": False, "power_max": 3968,
                "lock": {"enabled": bool(lock_pass), "unlocked": self.unlocked(),
                         "mqtt_allowed": not lock_pass or config["mqtt"]["allow_locked"]},
                "link": {"up": True, "status_age_ms": 500, "stm32_resets": 0, "queue": 0, "sent": stats["sent"],
                         "timeouts": 0, "retries": 0, "stale": 0, "errors": 0, "overflows": 0},
                "lcd": {"slots": [{"top": t, "bottom": b} for t, b in slots], **lcd}})
        elif self.path == "/api/config":
            self.send_json(200, config)
        else:
            self.send_json(404, {"ok": False, "error": "not found"})

    def do_POST(self):
        global lock_pass
        b = self.body()
        p = self.path
        if p == "/api/cmd":
            self.run_cmd(str(b.get("cmd", "")))
        elif p in ("/api/rt", "/api/ps"):
            self.run_cmd(p[5:].upper() + " " + str(b.get("text", ""))[:64])
        elif p == "/api/lcd/msg":
            top, bot = b.get("top", "")[:8].replace("|", ""), b.get("bottom", "")[:48].replace("|", "")
            line = f"LCD MSG {b.get('slot')}" + (f" {top}|{bot}" if top else f" {bot}" if bot else "")
            self.run_cmd(line)
        elif p == "/api/save":
            self.run_cmd("SAVE")
        elif p == "/api/unlock":
            if not lock_pass:
                return self.send_json(400, {"ok": False, "error": "no control password is set"})
            if b.get("password") != lock_pass:
                return self.send_json(403, {"ok": False, "error": "wrong password"})
            t = secrets.token_hex(16)
            tokens.add(t)
            self.send_json(200, {"ok": True, "token": t, "idle_timeout_s": 900})
        elif p == "/api/lock":
            tokens.discard(self.headers.get("X-Unlock"))
            self.send_json(200, {"ok": True})
        elif p == "/api/config":
            if not self.unlocked():
                return self.locked()
            for k in ("hostname", "http_user"):
                if k in b:
                    config[k] = b[k]
            config["mqtt"].update({k: v for k, v in b.get("mqtt", {}).items() if k != "pass"})
            if "lock_pass" in b:
                lock_pass = b["lock_pass"]
                config["lock_pass_set"] = bool(lock_pass)
                tokens.clear()
            self.send_json(200, {"ok": True, "reboot_needed": False})
        elif p in ("/api/reboot", "/api/wifi/reset"):
            if not self.unlocked():
                return self.locked()
            self.send_json(200, {"ok": True})
        else:
            self.send_json(404, {"ok": False, "error": "not found"})


def main():
    global lock_pass
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8030)
    ap.add_argument("--lock", default="", metavar="PASSWORD", help="start with this control password")
    a = ap.parse_args()
    lock_pass = a.lock
    config["lock_pass_set"] = bool(lock_pass)
    print(f"web UI preview on http://127.0.0.1:{a.port}/" + (f"  (control password: {lock_pass})" if lock_pass else ""))
    ThreadingHTTPServer(("127.0.0.1", a.port), H).serve_forever()


if __name__ == "__main__":
    main()
