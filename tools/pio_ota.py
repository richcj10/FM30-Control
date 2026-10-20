# PlatformIO pre-script for the `ota` environment: takes the board's hostname
# and OTA password from data/setup.json (or FM30_HOST / FM30_OTA_PASS, which
# win), so `pio run -e ota -t upload` needs no other setup.
Import("env")  # noqa: F821  (provided by PlatformIO)

import json
import os
from pathlib import Path

setup = Path(env.subst("$PROJECT_DIR")) / "data" / "setup.json"  # noqa: F821
cfg = {}
if setup.exists():
    try:
        cfg = json.loads(setup.read_text(encoding="utf-8"))
    except ValueError as e:
        print(f"pio_ota: can't read {setup}: {e}")

host = os.environ.get("FM30_HOST") or (cfg.get("hostname") and cfg["hostname"] + ".local")
password = os.environ.get("FM30_OTA_PASS") or cfg.get("ota_pass")

if host:
    env.Replace(UPLOAD_PORT=host)  # noqa: F821
else:
    print("pio_ota: no hostname: set \"hostname\" in data/setup.json or FM30_HOST")


def add_auth(source, target, env):
    if password:
        env.Append(UPLOADERFLAGS=["--auth=" + password])
    else:
        print("pio_ota: no OTA password: set \"ota_pass\" in data/setup.json or FM30_OTA_PASS")


env.AddPreAction("upload", add_auth)  # noqa: F821
env.AddPreAction("uploadfs", add_auth)  # noqa: F821
