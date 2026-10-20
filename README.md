# FM30 network front-end (ESP8266)

This is the ESP8266 firmware that puts the FM30 transmitter on the network. The STM32 controller (NUCLEO-G431KB, the `fm30_ctrl` project) runs the transmitter, generates RDS and drives the 2×8 LCD. This board has no direct access to the radio hardware. Everything goes over the STM32's plain-text UART protocol, one command at a time.

It provides:

- WiFi setup through a captive portal (WiFiManager), with the WiFi state shown on the LCD
- a mobile-friendly web UI, served from LittleFS
- a JSON REST API that mirrors the STM32 commands, plus `rt`/`ps` endpoints for playlist systems
- optional MQTT, with Home Assistant discovery
- password-protected OTA updates
- host-side tests and a Python mock of the STM32, so the firmware can be tested without hardware

Target board: Olimex MOD-WIFI-ESP8266(-DEV) (`board = modwifi`, 2 MB flash). Any ESP8266 with GPIO13 and GPIO15 broken out will work. See [Other boards](#other-esp8266-boards).

## Wiring

UART: 115200 8N1, 3.3 V logic on both sides, no flow control.

| ESP8266 | Direction | Nucleo pin | STM32 | Notes |
|---|---|---|---|---|
| GPIO15 (UART0 TX after `Serial.swap()`) | → | A2 | PA3 (USART2 RX) | boot strap: must read low at ESP reset, see below |
| GPIO13 (UART0 RX after `Serial.swap()`) | ← | A7 | PA2 (USART2 TX) | |
| GND | — | GND | GND | tie to the FM30 ground as well |
| GPIO2 (Serial1 TX) | → | — | — | debug log, 115200. Optional: wire it to a USB-UART RX |
| GPIO1 / GPIO3 | — | — | — | USB serial for flashing only. **Don't connect these to the STM32.** |

The firmware swaps UART0 onto GPIO13/GPIO15. The ESP boot ROM (74880 baud) and flashing traffic stay on GPIO1/GPIO3 and never reach the STM32. Once the swap happens, `pio device monitor` on the USB port shows only the boot ROM and one line saying the UART has moved. The log is on GPIO2.

Power the ESP from a 3.3 V supply that can handle WiFi transmit peaks of about 400 mA.

### Check 1: GPIO15 boot strap

GPIO15 must read low when the ESP comes out of reset, or the ESP won't boot from flash. The ESP module holds it low with a pull-down. The STM32 enables an internal pull-up on PA3 (`cli.c`, `GPIO_PULLUP` on PA2/PA3). That pull-up is 25–55 kΩ (typically 40 kΩ), and while the ESP is in reset the two resistors form a divider on GPIO15:

V(GPIO15) = 3.3 V × R<sub>pd</sub> / (R<sub>pd</sub> + R<sub>pu</sub>), which must stay below the ESP8266 V<sub>IL</sub> of 0.25 × VDD = 0.825 V.

| Module pull-down on GPIO15 | Typical (40k pull-up) | Worst case (25k pull-up) | |
|---|---|---|---|
| 10 kΩ (common on dev boards) | 0.66 V | **0.94 V** | marginal: may fail to boot |
| 4.7 kΩ | 0.35 V | 0.52 V | OK |
| 10 kΩ ∥ added 4.7 kΩ = 3.2 kΩ | 0.24 V | 0.37 V | OK |

**Check the GPIO15 pull-down on your module.** If it's above about 4.7 kΩ, add a 4.7 kΩ resistor from GPIO15 to GND at the ESP. That costs the ESP TX about 1 mA when high, which is fine. A series resistor in the GPIO15 → PA3 line doesn't help. It adds to the pull-up side of the divider, and once the ESP is running it forms a divider with the STM32 pull-up that keeps a low from reaching V<sub>IL</sub>.

Alternative without hardware: change PA3 to `GPIO_NOPULL` in the STM32's `cli.c`. The ESP then holds the line, high when idle and low through its pull-down while in reset. The drawback is that PA3 floats if the ESP is unplugged. The STM32 drops the NUL bytes it reads from a held-low line, and noise would have to form a whole valid command to do anything.

### Check 2: Nucleo solder bridges (not verified)

PA2/PA3 are the ST-LINK virtual COM port by default. To use them for the ESP:

- **Disconnect the ST-LINK's VCP TX from PA3**, or the ST-LINK and the ESP both drive PA3.
- **Confirm that A2 and A7 on the header connect to PA3 and PA2.**

The solder bridge numbers for the NUCLEO-G431KB haven't been checked for this README. Look them up in the board's user manual and schematic (MB1430) before changing anything. Leaving the ST-LINK's VCP RX on PA2 is harmless and lets a PC watch the STM32's replies.

### Other ESP8266 boards

Only `Serial.swap()` in [src/Station.cpp](src/Station.cpp) ties the firmware to GPIO13/GPIO15. Any ESP8266 module has those pins, but check that your board breaks them out and that nothing else on the board uses them, such as an SPI flash or display on HSPI. If you use different pins, you'll have to use SoftwareSerial or give up the swap. Without the swap, the boot ROM output reaches the STM32, which is harmless but noisy, since it only answers `ERR unknown command`.

## Build and flash

```
pio run -t upload            # firmware over USB serial
pio run -t uploadfs          # web UI (data/) into LittleFS. Needed once, and after editing data/
```

### Quick setup with `data/setup.json` (recommended)

This file lets you skip the setup portal and the Settings page:

1. Copy `setup.example.json` to `data/setup.json`.
2. Fill in your WiFi network, hostname and passwords, plus MQTT and display messages if you want them. Leave out anything you don't need.
3. Upload Filesystem Image (`pio run -t uploadfs`), then reset the ESP.

At boot the ESP applies the file, saves the settings and the WiFi network, and **deletes the file from the board**. The passwords don't stay on the board in a second file, and later changes in the web UI aren't overwritten at every boot. If the file has an error, it's left in place and the reason is in the debug log.

`data/setup.json` is in `.gitignore`. Keep it on your PC: the OTA environment reads the hostname and OTA password from it. To re-provision, edit it and upload the filesystem again; that also wipes the old settings.

### Setup portal (without setup.json)

First boot:

1. With no saved network, the ESP opens an access point named `FM30-Setup-xxxx`. Slot 1 on the LCD shows `WiFi` / `setup AP FM30-Setup-xxxx`.
2. Join it from a phone. The portal opens; if it doesn't, browse to 192.168.4.1.
3. Pick the network. The same page has fields for the MQTT broker, the OTA password and an optional web UI password. You can set all of these later in the web UI.
4. Once connected, LCD slot 1 shows `WiFi OK` / `<ip>`. The UI is at `http://<ip>/` or `http://fm30-xxxxxx.local/`.

If the network goes away, slot 1 shows `WiFi` / `disconnected` and the ESP keeps retrying. It nudges the reconnect every 30 s.

### OTA

OTA stays off until an OTA password is set (portal or Settings tab). After setting it, reboot.

```
pio run -e ota -t upload
pio run -e ota -t uploadfs
```

The hostname (as `<hostname>.local`) and OTA password come from `data/setup.json`. The environment variables `FM30_HOST` and `FM30_OTA_PASS` override them, for example to use an IP address.

`uploadfs` over OTA also replaces the board's settings. With `data/setup.json` present they're re-applied from it. Without it, the board goes back to defaults, and its WiFi network is kept.

During an update, LCD slot 1 shows `OTA` / `updating`.

## How it talks to the STM32

- **One command at a time.** The ESP waits for the reply before sending the next command, with a 500 ms timeout and one retry. It never streams commands, because the STM32 receive ring is 256 bytes and its main loop stalls during FREQ (a few ms), STATUS (~40 ms) and SAVE (~25 ms). Up to 10 commands queue on the ESP.
- **Reply matching.** The STM32 echoes the command word (`OK FREQ 98.1`, `OK SAVED`; VTSCALE and HELP answer a bare `OK`). A reply that doesn't match the command in flight is dropped as stale, so a late answer to a timed-out command can't be taken as the answer to the next one. `ERR …` always ends the command in flight.
- **STATUS** is polled every 2 s. The last JSON line is kept verbatim. JSON lines are accepted at any time, so `STATUS n` (unsolicited output) works too.
- **STM32 resets** are detected two ways: from the boot banner (`fm30ctl 0.3 ready`), and from `uptime` going backwards. After a reset, the ESP re-sends all four LCD message slots, because the STM32 doesn't save message text. It doesn't re-send anything else: after a reset, the STM32 runs on whatever was last SAVEd, by design.
- **No automatic SAVE.** SAVE happens only when a user asks for it (web button, `/api/save`, `/api/cmd`, MQTT). It's sent without retry and rate-limited to once per 10 s to limit flash wear.
- **Everything from the network is untrusted.** Commands are parsed, range-checked against the same limits the STM32 uses, and rebuilt from the parsed values. Raw input is never forwarded. Input containing CR, LF or other control characters is rejected. Text for PS, RT and the LCD is converted to printable ASCII: accents and smart quotes are transliterated, anything else becomes `?`. `|` is stripped from LCD text, and the text is clipped to 8 characters (PS), 64 (RT), or 8 top / 48 bottom (LCD).
- **Power.** The UI and the MQTT/HA power control take 0–100 %. `/api/cmd` also accepts `POWER <0-3968>`, the STM32's own range. The STM32 enforces its maximum either way, and nothing on the ESP can exceed it. Numbers are always sent in plain decimal. The STM32 parses POWER with `strtol(…, 0)`, so a raw `POWER 080` would have been read as octal.

### LCD message slots

| Slot | LCD page | Owner |
|---|---|---|
| 1 | 5 | this board: WiFi / OTA status. User writes to slot 1 are rejected. |
| 2–4 | 6–8 | user: web UI, `/api/lcd/msg`, `LCD MSG n …` via `/api/cmd` or MQTT |

Slots 2–4 are kept in `/config.json` on the ESP. They survive a reboot of either board.

## Web UI

The **TX button** sits at the top of every tab. It shows ON AIR (red, with the frequency), TUNING…, FAULT or TX OFF, and tapping it switches TX. Turning TX off asks for confirmation. The browser tab's icon and title follow the same state: a red tower with waves and "● ON AIR 98.1" while on air, grey when off, amber while tuning.

The tabs are:

- **Status:** frequency, state, power %, Vtune vs expected, pilot lock, RDS on/muted, faults, PS/RT, plus controls for frequency, power, volume and balance.
- **RDS:** PS, RT, RDS on/off, PI, PTY (RBDS names). Each field has a short explanation underneath.
- **Display:** a preview of every LCD page, with message pages 6–8 animated the way the LCD scrolls them, example messages, then rotation, pinned page and LCD on/off.
- **Advanced:** U6TRIM, U6, VTSCALE (leave at 3.0), RDSLEVEL, RDSPHASE, RDSFREE, TP/TA/MS/STEREO (with explanations), a raw command console, HELP and DEFAULTS.
- **Settings:** board info and link statistics, passwords, MQTT (with a table of what's shared and what can be changed), hostname, reboot, forget WiFi, and a curl example for playlist software.

**Save** in the header sends SAVE after you confirm.

### Passwords

| Password | Protects |
|---|---|
| **Control password** | changing frequency, power and the tuning internals (FREQ, POWER, U6TRIM, U6, VTSCALE, RDSLEVEL, DACTEST, DEFAULTS), plus the settings, reboot and forget-WiFi. Checked on the ESP, not just hidden in the page. TX on/off, audio, RDS text, display messages and SAVE stay open. |
| **Web password** | the whole page and API, with HTTP basic auth (user `admin` by default). |
| **OTA password** | firmware updates over WiFi. |

With a control password set, the protected controls are greyed out until you tap **🔒 Unlock** and enter it. The unlock lasts until 15 minutes of inactivity, and only in that browser tab. After 5 wrong tries, unlocking is blocked for 30 s.

With no passwords set, anyone on the LAN can use everything.

## REST API

All responses are JSON. POST bodies are JSON with `Content-Type: application/json`. The exception is `rt` and `ps`, which also take a form field or a plain-text body. Browsers can't send a cross-site JSON POST without a CORS preflight, and this board never approves one, so another web page can't drive the transmitter through your browser. The `rt`/`ps` plain-text forms are the exception. If that matters on your network, set a web password.

| Method | Path | Body | Result |
|---|---|---|---|
| GET | `/api/status` | | last STATUS JSON from the STM32, verbatim. Headers: `X-Status-Age-Ms`, `X-Link: up\|down`. 503 before the first STATUS |
| GET | `/api/info` | | ESP info, link stats, LCD slots, rotation/page (null = unknown since the STM32 booted), MQTT state |
| POST | `/api/cmd` | `{"cmd":"FREQ 98.1"}` | `{"ok":true,"cmd":"FREQ 98.1","reply":"OK FREQ 98.1"}`. HELP adds `"help":[...]` |
| POST | `/api/rt` | `{"text":"Now playing: …"}`, `text=…` or a text/plain body | as above. Empty text clears RT |
| POST | `/api/ps` | same forms | as above |
| POST | `/api/lcd/msg` | `{"slot":2,"top":"Studio","bottom":"Live from the garage"}` | as above. Empty top and bottom clears the slot |
| POST | `/api/save` | `{}` | SAVE (rate-limited) |
| POST | `/api/unlock` | `{"password":"…"}` | `{"ok":true,"token":"…"}`. Send the token as `X-Unlock: <token>` on protected requests. 403 wrong password, 429 after 5 wrong tries |
| POST | `/api/lock` | `{}` | ends the session for the `X-Unlock` token |
| GET | `/api/config` | | ESP settings. Secrets are shown only as `*_set` flags |
| POST | `/api/config` 🔒 | any subset: `{"mqtt":{"host":"10.0.0.2","allow_locked":false},"ota_pass":"…","lock_pass":"…"}` | saved to LittleFS. Absent fields are unchanged. `reboot_needed` is true for hostname or OTA changes. A new `lock_pass` ends every unlock session |
| POST | `/api/reboot` 🔒 | `{}` | reboots the ESP |
| POST | `/api/wifi/reset` 🔒 | `{}` | forgets WiFi and reboots into the portal |

🔒 = needs `X-Unlock` when a control password is set. So do `/api/cmd` requests for the protected commands. Without it, the reply is 401 `{"locked":true}`.

Status codes:

| Code | Meaning |
|---|---|
| 200 | the STM32 answered OK |
| 422 | the STM32 answered ERR (`reply` holds it) |
| 400 | rejected by validation before sending (`error` says why) |
| 401 | locked: needs the control password (`"locked":true`) |
| 503 | command queue full |
| 504 | no reply after a retry |

```
curl http://fm30-xxxxxx.local/api/status
curl -X POST -H 'Content-Type: application/json' -d '{"cmd":"POWER 60%"}' http://fm30-xxxxxx.local/api/cmd
curl -X POST --data-urlencode "text=Now playing: $ARTIST - $TITLE" http://fm30-xxxxxx.local/api/rt
```

`/api/cmd` accepts every STM32 command:

- `FREQ`, `POWER <n>%|<0-3968>`, `TX`, `VOL`, `BAL`
- `U6TRIM`, `U6 <code>|AUTO`, `VTSCALE`
- `RDS`, `RDSFREE`, `RDSLEVEL`, `RDSPHASE`, `PI`, `PTY`, `TP`/`TA`/`MS`/`STEREO`, `PS`, `RT`
- `LCD ON|OFF|PAGE|MSG|ROTATE`
- `STATUS [n]`, `SAVE`, `DEFAULTS`, `HELP`
- `DACDBG` (RDS DAC register dump), `DACTEST 0|1|2` (DAC bench test: normal, DC mid-scale, 1 kHz tone)

## MQTT

MQTT is off until a broker is set. The base topic defaults to `fm30`.

| Topic | Direction | Payload |
|---|---|---|
| `fm30/availability` | out, retained | `online` / `offline` (LWT) |
| `fm30/link` | out, retained | `up` / `down`: whether the STM32 is answering |
| `fm30/status` | out, retained | STATUS JSON, every 2 s |
| `fm30/cmd` | in | any command line, e.g. `FREQ 98.1` |
| `fm30/cmd/result` | out | `{"cmd":…,"ok":…,"reply":…}` or `{"error":…}` |
| `fm30/rt/set`, `fm30/ps/set` | in | text |
| `fm30/freq/set` | in | `98.1` |
| `fm30/tx/set` | in | `ON` / `OFF` |
| `fm30/power/set` | in | percent, `0`–`100` |

**Home Assistant discovery** (on by default, prefix `homeassistant`) creates one device. Its entities are:

- state, Frequency (number), Transmit (switch), Power % (number)
- Vtune, Pilot lock, Fault, Fault count, controller uptime
- RDS PS and RadioText (text)
- a Save settings button

Entities show as unavailable when the ESP is offline or the STM32 link is down. Discovery is sent again when HA restarts.

**MQTT and the control password.** MQTT can't ask for a password. So while a control password is set, `freq/set`, `power/set` and the protected commands on `cmd` are refused, with an error on `cmd/result`, unless **Allow frequency, power and tuning changes over MQTT** is ticked in Settings. In that state, Home Assistant shows Frequency and Power as read-only sensors instead of number controls. With no control password set, MQTT can change everything.

## Tests (no hardware)

```
python tools/run_host_tests.py
```

This builds the portable core in `lib/Fm30Core` with g++, clang++ or MSVC (found with vswhere), then runs three suites:

1. **`test/host/test_core.cpp`**: unit tests for the sanitizer and the command validator, plus the link queue on a fake UART and clock. The queue tests cover single outstanding command, pacing gap, timeout and retry, no retry for SAVE, stale-reply rejection, unsolicited JSON, the banner, HELP bodies, a 620-byte STATUS line and line overflow.
2. **`tools/mock_stm32.py --selftest`**: checks the mock against `cli.c` behavior.
3. **`test/host/test_integration.cpp`**: the real link and validator talking to the mock over TCP. It runs every command form the ESP produces, HELP, sanitized RT round-tripping through STATUS, unsolicited `STATUS 1`, and a queued burst that must complete with no retries and no RX-ring overflow.

### Previewing the web UI

```
python tools/web_preview.py              # http://127.0.0.1:8030/
python tools/web_preview.py --lock test  # try the control-password flow
```

This serves `data/` and a fake version of the ESP API on top of the STM32 mock, so you can work on the page with no hardware.

### Running the real ESP against the mock

You don't need the STM32 or the FM30 for this. Wire a 3.3 V USB-UART adapter to the ESP: adapter TX → GPIO13, adapter RX ← GPIO15, and GND. Then run:

```
pip install pyserial
python tools/mock_stm32.py --port COM7 -v                     # log every line
python tools/mock_stm32.py --port COM7 --reset-every 120      # check LCD re-send after resets
python tools/mock_stm32.py --port COM7 --fault-every 45 --no-pilot
python tools/mock_stm32.py --port COM7 --flash mock_flash.json   # SAVE persists across runs
```

The mock prints `[save] SAVE #n` for every SAVE, so you can confirm the ESP never saves on its own. It prints `!! RX RING OVERFLOW` if the ESP ever streams commands.

## Debug log

The debug log is on Serial1 (GPIO2, 115200). It reports link errors and timeouts, stale replies, STM32 resets, WiFi/MQTT/OTA events and config saves:

```
[   1234] link: timeout, retrying: FREQ 98.1
[   5012] STM32 reset detected (uptime 3600 -> 1)
[   5013] LCD messages sent to STM32
```

## Layout

```
platformio.ini           envs: modwifi (USB), ota (espota)
src/main.cpp             WiFiManager, WiFi state -> LCD slot 1, mDNS, OTA
src/Station.*            UART link, STATUS polling, reset detection, LCD slots, sync calls
src/WebApi.*             web UI + REST API
src/Mqtt.*               MQTT + Home Assistant discovery
src/AppConfig.*          /config.json on LittleFS
lib/Fm30Core/            portable: Sanitize, Commands (validation), Link (queue); used by the tests
data/index.html          web UI
tools/mock_stm32.py      STM32 protocol mock (serial or TCP)
tools/run_host_tests.py  builds and runs the host tests
tools/web_preview.py     web UI preview on a PC (fake API on the mock)
tools/pio_ota.py         OTA env: hostname/password from data/setup.json
setup.example.json       template for data/setup.json (git-ignored)
test/host/               host test sources
```
