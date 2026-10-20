#!/usr/bin/env python3
"""
Mock of the FM30 STM32 controller's serial protocol (fm30ctl 0.3, cli.c).

Lets you run the ESP8266 firmware with no STM32 or FM30: wire a 3.3 V USB-UART
adapter to the ESP (adapter TX -> GPIO13, adapter RX <- GPIO15, GND) and run

    python tools/mock_stm32.py --port COM7            # or /dev/ttyUSB0
    python tools/mock_stm32.py --port COM7 -v --reset-every 120 --fault-every 45

or poke at it by hand over TCP (e.g. with netcat or PuTTY raw mode):

    python tools/mock_stm32.py --tcp 7000

The command parser follows cli.c closely, including its quirks (POWER uses
strtol base 0, so "POWER 080" is octal and parses as 0; "TX ON " with a
trailing space is an error). Reply formats, the boot banner, STATUS JSON field
order, unsolicited STATUS n output, the main-loop stalls (FREQ a few ms,
STATUS ~40 ms, SAVE ~25 ms) and the 255-byte receive ring are modeled. Bytes
that overflow the ring are dropped and reported loudly: that means the host
streamed commands instead of waiting for replies.

    python tools/mock_stm32.py --selftest              # check the mock itself

Needs pyserial for --port (pip install pyserial).
"""
import argparse
import json
import random
import re
import socket
import sys
import time

FW = "fm30ctl 0.3"
POWER_MAX = 0xF80
FREQ_MIN, FREQ_MAX = 875, 1080
RING = 255            # uint8 head/tail ring: one slot is always free
LINE_MAX = 127
UI_PAGES, UI_MSG_SLOTS, UI_MSG_LEN = 8, 4, 48

U6_F10, U6_CAL = [881, 931, 981, 1031, 1071], [948, 1194, 1412, 1676, 1914]
VT_F10, VT_VOLT = [881, 981, 1074], [1.72, 3.80, 6.46]

DEFAULTS = dict(f10=881, power_target=0xE1F, tx=False, volume=128, balance=0, u6_trim=0,
                vt_scale=3.0, pi=0x1234, pty=0, tp=0, ta=0, ms=1, stereo=1, ps="FM30    ",
                rt="", rds_enable=True, level=400, phase=0, lcd_on=True, lcd_page=0, lcd_rot=0xFF)

HELP = [
    "FREQ <MHz>            e.g. FREQ 98.1 (87.5-108.0)",
    "POWER <0-3968|n%>     RF power code (U7) or percent of max",
    "TX ON|OFF             RF on/off",
    "VOL <0-255>  BAL <-127..127>",
    "U6TRIM <n>  U6 <code>|AUTO   deviation compensation trim/override",
    "VTSCALE <x>           Vtune = Vadc * x (default 3.0)",
    "RDS ON|OFF  RDSFREE ON|OFF  RDSLEVEL <0-2047>  RDSPHASE <deg>",
    "PI <hex>  PTY <0-31>  TP|TA|MS|STEREO <0|1>",
    "PS <text>  RT <text>  (RT with no text clears it)",
    "STATUS [period_s]     JSON status; period 0 = off",
    "LCD ON|OFF  LCD PAGE <0-8>   pin a page (5-8 = MSG 1-4); 0 = rotate",
    "LCD MSG <1-4> [top]|<bottom> message page; bottom scrolls if > 8; no text = clear",
    "LCD ROTATE <pages>|ALL       pages in rotation, e.g. LCD ROTATE 1 5 6",
    "SAVE  DEFAULTS  HELP",
]


# ---------------------------------------------------------------- C helpers

def c_strtol(s, base=0):
    """strtol(): returns (value, chars consumed); consumed == 0 means no number."""
    i, n = 0, len(s)
    while i < n and s[i] in " \t\n\v\f\r":
        i += 1
    neg = False
    if i < n and s[i] in "+-":
        neg = s[i] == "-"
        i += 1
    if base in (0, 16) and s[i:i + 2].lower() == "0x" and i + 2 < n and s[i + 2] in "0123456789abcdefABCDEF":
        i += 2
        base = 16
    elif base == 0:
        base = 8 if s[i:i + 1] == "0" else 10
    start, v = i, 0
    while i < n:
        d = int(s[i], 36) if s[i].isascii() and s[i].isalnum() else 99
        if d >= base:
            break
        v = v * base + d
        i += 1
    if i == start:
        return 0, 0
    return (-v if neg else v), i


def c_strtof(s):
    m = re.match(r"\s*[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?", s)
    return float(m.group(0)) if m else 0.0


def parse_bool(a):
    a = a.upper()
    if a in ("1", "ON"):
        return True
    if a in ("0", "OFF"):
        return False
    return None


def fmt_fixed(v, d):
    scale = 10 ** d
    x = int(v * scale + (0.5 if v >= 0 else -0.5))
    sign = "-" if x < 0 else ""
    x = abs(x)
    return f"{sign}{x // scale}.{x % scale:0{d}d}"


def json_str(s):
    return "".join("\\" + c if c in '"\\' else c for c in s if ord(c) >= 0x20)


def interp(x, xs, ys):
    i = 0
    while i < len(xs) - 2 and x > xs[i + 1]:
        i += 1
    return ys[i] + (ys[i + 1] - ys[i]) * (x - xs[i]) / (xs[i + 1] - xs[i])


# ---------------------------------------------------------------- model

class Stm32Model:
    """The controller's state and command interpreter. Time comes from clock()."""

    def __init__(self, clock=time.monotonic, pilot=True, saved=None):
        self.clock = clock
        self.pilot = pilot
        self.flash = saved          # dict written by SAVE, or None
        self.saves = 0
        self.boot()

    # -- lifecycle
    def boot(self):
        self.t0 = self.clock()
        s = dict(DEFAULTS)
        if self.flash:
            s.update(self.flash)
        s["tx"] = False                 # TX always starts off; a saved TX state isn't restored
        self.__dict__.update(s)
        self.u6_manual = -1
        self.free_run = False
        self.power_now = 0
        self.fault, self.faults = "", 0
        self.ovm = 0
        self.status_period = 0
        self.t_status = self.clock()
        self.msgs = [("", "")] * UI_MSG_SLOTS
        self.state = "off"
        self.state_t = self.clock()
        self.pilot_t = self.clock()
        self.retune()
        return ["", f"{FW} ready", "settings: " + ("flash" if self.flash else "defaults"), "clock: HSE 24 MHz"]

    def uptime(self):
        return int(self.clock() - self.t0)

    def retune(self):
        self.power_now = 0
        self.state = "tuning"
        self.state_t = self.clock()

    def inject_fault(self, msg="LOCK LST"):
        if self.state in ("on_air", "ramp"):
            self.state, self.fault = "fault", msg
            self.faults += 1
            self.power_now = 0
            self.state_t = self.clock()

    # -- periodic work (state machine, unsolicited STATUS)
    def tick(self):
        now = self.clock()
        if self.state == "tuning" and now - self.state_t >= 1.2:
            self.state = "ramp" if self.tx else "off"
            self.state_t = now
        if self.state == "fault" and now - self.state_t >= 2.0:
            self.retune()
        if self.state == "ramp":
            self.power_now = min(self.power_target, int((now - self.state_t) * 800))
            if self.power_now >= self.power_target:
                self.state = "on_air"
        if self.state == "on_air":
            self.power_now = self.power_target
        out = []
        if self.status_period and now - self.t_status >= self.status_period:
            self.t_status = now
            out.append(self.status_json())
        return out

    def pilot_lock(self):
        return 1 if self.pilot and self.clock() - self.pilot_t > 0.4 else 0

    def u6(self):
        if self.u6_manual >= 0:
            return self.u6_manual
        return max(0, min(4095, int(interp(self.f10, U6_F10, U6_CAL) + self.u6_trim + 0.5)))

    def status_json(self):
        exp = interp(self.f10, VT_F10, VT_VOLT)
        vt = exp + random.uniform(-0.02, 0.02) if self.state != "off" else exp
        lock = self.pilot_lock()
        up = self.uptime()
        return (
            f'{{"fw":"{FW}","state":"{self.state}","fault":"{self.fault}","faults":{self.faults},'
            f'"freq":{self.f10 // 10}.{self.f10 % 10},"tx":{int(self.tx)},"power":{self.power_now},'
            f'"power_target":{self.power_target},"u6":{self.u6()},"u6_trim":{self.u6_trim},'
            f'"vtune":{fmt_fixed(vt, 2)},"vtune_expected":{fmt_fixed(exp, 2)},"vt_scale":{fmt_fixed(self.vt_scale, 3)},'
            f'"vol":{self.volume},"bal":{self.balance},"ovm":{self.ovm},'
            f'"pilot_lock":{lock},"pilot_hz":{fmt_fixed(19000.01 if lock else 0, 2)},'
            f'"pilot_ampl":{fmt_fixed(88.0 if lock else 0.5, 1)},"pilot_err":{fmt_fixed(0.002, 3)},'
            f'"rds":{int(self.rds_enable)},"rds_level":{self.level},"rds_phase":{self.phase},'
            f'"pi":"{self.pi:04X}","pty":{self.pty},"tp":{self.tp},"ta":{self.ta},"ms":{self.ms},'
            f'"stereo":{self.stereo},"ps":"{json_str(self.ps)}","rt":"{json_str(self.rt)}",'
            f'"groups":{int(up * 11.4)},"dsp_load":35,"dsp_overruns":0,"hse":1,"uptime":{up},"loop_max_us":180}}')

    def settings(self):
        return {k: getattr(self, k) for k in DEFAULTS}

    # -- command interpreter (cli.c exec)
    def exec(self, line):
        """Returns (output lines, seconds the main loop is blocked)."""
        line = line.lstrip(" ")
        if not line:
            return [], 0
        cmd, _, rest = line.partition(" ")
        rest = rest.lstrip(" ")
        cmd = cmd.upper()
        o = []
        busy = 0.0

        if cmd == "FREQ":
            f10 = int(c_strtof(rest) * 10.0 + 0.5)
            if f10 < FREQ_MIN or f10 > FREQ_MAX:
                o.append("ERR range 87.5-108.0")
            else:
                self.f10 = f10
                self.retune()
                busy = 0.005
                o.append(f"OK FREQ {f10 // 10}.{f10 % 10}")
        elif cmd == "POWER":
            v, n = c_strtol(rest, 0)
            if n == 0:
                o.append("ERR value")
            else:
                if rest[n:n + 1] == "%":
                    v = int(v * POWER_MAX / 100)     # C long division truncates toward 0
                v = max(0, min(POWER_MAX, v))
                self.power_target = v
                if v < self.power_now:
                    self.power_now = v
                elif self.state == "on_air":
                    self.state, self.state_t = "ramp", self.clock() - self.power_now / 800
                o.append(f"OK POWER {v}")
        elif cmd == "TX":
            b = parse_bool(rest)
            if b is None:
                o.append("ERR ON|OFF")
            else:
                self.tx = b
                if b:
                    self.retune()
                else:
                    self.state, self.power_now = "off", 0
                o.append(f"OK TX {int(b)}")
        elif cmd == "VOL":
            v, _ = c_strtol(rest)
            if not 0 <= v <= 255:
                o.append("ERR 0-255")
            else:
                self.volume = v
                o.append(f"OK VOL {v}")
        elif cmd == "BAL":
            v, _ = c_strtol(rest)
            if not -127 <= v <= 127:
                o.append("ERR -127..127")
            else:
                self.balance = v
                o.append(f"OK BAL {v}")
        elif cmd == "U6TRIM":
            v, _ = c_strtol(rest)
            if not -1000 <= v <= 1000:
                o.append("ERR -1000..1000")
            else:
                self.u6_trim = v
                o.append(f"OK U6TRIM {v}")
        elif cmd == "U6":
            if rest.upper() == "AUTO":
                self.u6_manual = -1
                o.append("OK U6 AUTO")
            else:
                v, _ = c_strtol(rest)
                if not 0 <= v <= 4095:
                    o.append("ERR 0-4095|AUTO")
                else:
                    self.u6_manual = v
                    o.append(f"OK U6 {v}")
        elif cmd == "VTSCALE":
            v = c_strtof(rest)
            if not 1.0 <= v <= 10.0:
                o.append("ERR 1.0-10.0")
            else:
                self.vt_scale = v
                o.append("OK")
        elif cmd in ("RDS", "RDSFREE"):
            b = parse_bool(rest)
            if b is None:
                o.append("ERR ON|OFF")
            else:
                setattr(self, "rds_enable" if cmd == "RDS" else "free_run", b)
                o.append(f"OK {cmd} {int(b)}")
        elif cmd == "RDSLEVEL":
            v, _ = c_strtol(rest)
            if not 0 <= v <= 2047:
                o.append("ERR 0-2047")
            else:
                self.level = v
                o.append(f"OK RDSLEVEL {v}")
        elif cmd == "RDSPHASE":
            v, _ = c_strtol(rest)
            if not -180 <= v <= 180:
                o.append("ERR -180..180")
            else:
                self.phase = v
                o.append(f"OK RDSPHASE {v}")
        elif cmd == "PI":
            v, n = c_strtol(rest, 16)
            if n == 0 or not 0 <= v <= 0xFFFF:
                o.append("ERR hex 0000-FFFF")
            else:
                self.pi = v
                o.append(f"OK PI {v:04X}")
        elif cmd == "PTY":
            v, _ = c_strtol(rest)
            if not 0 <= v <= 31:
                o.append("ERR 0-31")
            else:
                self.pty = v
                o.append(f"OK PTY {v}")
        elif cmd in ("TP", "TA", "MS", "STEREO"):
            b = parse_bool(rest)
            if b is None:
                o.append("ERR 0|1")
            else:
                setattr(self, cmd.lower(), int(b))
                o.append(f"OK {cmd} {int(b)}")
        elif cmd == "PS":
            self.ps = rest[:8].ljust(8)
            o.append(f'OK PS "{self.ps}"')
        elif cmd == "RT":
            self.rt = rest[:64]
            o.append("OK RT")
        elif cmd == "STATUS":
            if rest:
                v, _ = c_strtol(rest)
                self.status_period = max(0, v)
                self.t_status = self.clock()
            busy = 0.040
            o.append(self.status_json())
        elif cmd == "LCD":
            o.append(self._lcd(rest))
        elif cmd == "SAVE":
            self.flash = self.settings()
            self.saves += 1
            busy = 0.025
            o.append("OK SAVED")
        elif cmd == "DEFAULTS":
            self.__dict__.update(dict(DEFAULTS))
            self.retune()
            o.append("OK DEFAULTS (not saved)")
        elif cmd in ("HELP", "?"):
            o.extend(HELP)
            o.append("OK")
        else:
            o.append("ERR unknown command (HELP)")
        return o, busy

    def _lcd(self, rest):
        sub, _, val = rest.partition(" ")
        val = val.lstrip(" ")
        su = sub.upper()
        if su == "PAGE":
            v, n = c_strtol(val)
            if n == 0 or not 0 <= v <= UI_PAGES:
                return f"ERR LCD PAGE 0-{UI_PAGES}"
            self.lcd_page = v
            return f"OK LCD PAGE {v}"
        if su == "MSG":
            slot, n = c_strtol(val)
            if n == 0 or not 1 <= slot <= UI_MSG_SLOTS:
                return f"ERR LCD MSG 1-{UI_MSG_SLOTS} [top]|<bottom>"
            text = val[n:].lstrip(" ")
            top, bar, bot = text.partition("|")
            if not bar:
                top, bot = "", text
            self.msgs[slot - 1] = (top[:8], bot[:UI_MSG_LEN])
            return f"OK LCD MSG {slot}" + ("" if top or bot else " cleared")
        if su == "ROTATE":
            mask = 0
            if val.upper() == "ALL":
                mask = 0xFF
            else:
                for ch in val:
                    if "1" <= ch <= str(UI_PAGES):
                        mask |= 1 << (int(ch) - 1)
                    elif ch not in " ,":
                        mask = 0
                        break
            if not mask:
                return f"ERR LCD ROTATE 1-{UI_PAGES} ... | ALL"
            self.lcd_rot = mask
            return "OK LCD ROTATE " + " ".join(str(p) for p in range(1, 9) if mask & (1 << (p - 1)))
        b = parse_bool(sub)
        if b is not None:
            self.lcd_on = b
            return f"OK LCD {int(b)}"
        return "ERR LCD ON|OFF|PAGE n|MSG n text|ROTATE list"


# ---------------------------------------------------------------- UART front end

class Uart:
    """Models the ISR ring (255 bytes) and cli_task's 128-byte line buffer."""

    def __init__(self, model, log):
        self.m, self.log = model, log
        self.ring = bytearray()
        self.line = bytearray()
        self.dropped = 0

    def rx(self, data):
        space = RING - len(self.ring)
        if len(data) > space:
            self.dropped += len(data) - space
            self.log(f"!! RX RING OVERFLOW: dropped {len(data) - space} bytes "
                     f"({self.dropped} total). The host is streaming commands instead of waiting for replies.")
            data = data[:space]
        self.ring += data

    def process(self, read_more, write, block):
        """Runs cli_task once: execute complete lines, then the periodic work."""
        while self.ring:
            c = self.ring.pop(0)
            if c in (0x0D, 0x0A):
                if self.line:
                    text = self.line.decode("latin-1")
                    self.line.clear()
                    self.log(f"<< {text}")
                    out, busy = self.m.exec(text)
                    if busy:
                        block(busy)          # main loop stalls; the ISR keeps filling the ring
                        self.rx(read_more())
                    for o in out:
                        self.log(f">> {o}" if len(o) < 160 else f">> {o[:150]}... ({len(o)} B)")
                        write(o)
            elif len(self.line) < LINE_MAX and c >= 0x20:
                self.line.append(c)
        for o in self.m.tick():
            self.log(">> " + o[:100] + "... (periodic)")
            write(o)


class SerialPort:
    def __init__(self, port):
        import serial  # pyserial
        self.s = serial.Serial(port, 115200, timeout=0)

    def read(self):
        return self.s.read(4096)

    def write(self, b):
        self.s.write(b)


class TcpPort:
    def __init__(self, port):
        self.srv = socket.socket()
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", port))
        self.srv.listen(1)
        self.srv.setblocking(False)
        self.c = None
        self.new_client = False      # boot banner goes to each new client
        print(f"listening on 127.0.0.1:{port}", flush=True)

    def read(self):
        if not self.c:
            try:
                self.c, addr = self.srv.accept()
                self.c.setblocking(False)
                self.new_client = True
                print(f"client {addr}", flush=True)
            except BlockingIOError:
                return b""
        try:
            d = self.c.recv(4096)
            if d == b"":
                self.c.close()
                self.c = None
            return d
        except BlockingIOError:
            return b""

    def write(self, b):
        if self.c:
            try:
                self.c.sendall(b)
            except OSError:
                self.c = None


def run(port, args):
    flash = json.load(open(args.flash)) if args.flash else None
    m = Stm32Model(pilot=not args.no_pilot, saved=flash)
    if args.verbose:
        def log(s):
            print(time.strftime("%H:%M:%S"), s, flush=True)
    else:
        def log(s):
            if s.startswith("!!"):
                print(s, flush=True)
    u = Uart(m, log)

    def write(line):
        port.write((line + "\r\n").encode("latin-1"))

    def boot():
        for line in m.boot():
            write(line)
        print(f"[boot] {FW} ready" + (" (settings from flash)" if m.flash else ""), flush=True)

    boot()
    t_reset = t_fault = time.monotonic()
    saves_seen = 0
    while True:
        u.rx(port.read())
        if getattr(port, "new_client", False):
            port.new_client = False
            boot()
        u.process(port.read, write, time.sleep)
        now = time.monotonic()
        if args.reset_every and now - t_reset >= args.reset_every:
            t_reset = now
            print("[reset] simulated STM32 reset")
            boot()
        if args.fault_every and now - t_fault >= args.fault_every:
            t_fault = now
            m.inject_fault()
            print("[fault] injected LOCK LST")
        if m.saves != saves_seen:
            saves_seen = m.saves
            print(f"[save] SAVE #{m.saves}", flush=True)
            if args.flash:
                json.dump(m.flash, open(args.flash, "w"))
        time.sleep(0.001)


# ---------------------------------------------------------------- self-test

def selftest():
    t = [1000.0]
    m = Stm32Model(clock=lambda: t[0])
    fails = 0

    def check(cmd, want):
        nonlocal fails
        out, _ = m.exec(cmd)
        got = out[-1] if out else ""
        ok = got == want if not want.endswith("*") else got.startswith(want[:-1])
        if not ok:
            fails += 1
            print(f"FAIL {cmd!r}: {got!r} != {want!r}")

    check("FREQ 98.1", "OK FREQ 98.1")
    check("freq 87.4", "ERR range 87.5-108.0")
    check("POWER 50%", "OK POWER 1984")
    check("POWER 080", "OK POWER 0")             # octal quirk the ESP must avoid
    check("POWER 5000", "OK POWER 3968")
    check("POWER x", "ERR value")
    check("TX ON", "OK TX 1")
    check("TX ON ", "ERR ON|OFF")                # trailing space
    check("VOL 300", "ERR 0-255")
    check("BAL -127", "OK BAL -127")
    check("VTSCALE 3.000", "OK")
    check("U6 AUTO", "OK U6 AUTO")
    check("PI c0de", "OK PI C0DE")
    check("tp 1", "OK TP 1")
    check("PS My Station", 'OK PS "My Stati"')
    check("RT", "OK RT")
    check("LCD MSG 2 Hi|there", "OK LCD MSG 2")
    check("LCD MSG 2", "OK LCD MSG 2 cleared")
    check("LCD MSG 5 x", "ERR LCD MSG 1-4 [top]|<bottom>")
    check("LCD ROTATE 6,1 5", "OK LCD ROTATE 1 5 6")
    check("LCD ROTATE 15", "OK LCD ROTATE 1 5")  # digit-by-digit quirk
    check("LCD PAGE 9", "ERR LCD PAGE 0-8")
    check("LCD OFF", "OK LCD 0")
    check("STATUS", '{"fw":"fm30ctl 0.3"*')
    check("SAVE", "OK SAVED")
    check("DEFAULTS", "OK DEFAULTS (not saved)")
    check("HELP", "OK")
    check("BOGUS", "ERR unknown command (HELP)")

    st = json.loads(m.exec("STATUS")[0][0])
    fields = ("fw state fault faults freq tx power power_target u6 u6_trim vtune vtune_expected vt_scale "
              "vol bal ovm pilot_lock pilot_hz pilot_ampl pilot_err rds rds_level rds_phase pi pty tp ta ms "
              "stereo ps rt groups dsp_load dsp_overruns hse uptime loop_max_us").split()
    if list(st) != fields:
        fails += 1
        print("FAIL STATUS fields", list(st))
    m.exec("RT " + '"\\' * 32)
    if len(m.exec("STATUS")[0][0]) > 640:
        fails += 1
        print("FAIL STATUS longer than expected")

    m.exec("STATUS 2")
    t[0] += 2.01
    if not m.tick():
        fails += 1
        print("FAIL periodic STATUS")

    # RX ring overflow is detected.
    u = Uart(m, lambda s: None)
    u.rx(b"VOL 1\n" * 60)
    if u.dropped != 360 - RING:
        fails += 1
        print("FAIL ring overflow", u.dropped)

    print("selftest:", "OK" if not fails else f"{fails} failed")
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--port", help="serial port wired to the ESP8266 (GPIO13/GPIO15)")
    g.add_argument("--tcp", type=int, metavar="PORT", help="listen on 127.0.0.1:PORT instead")
    g.add_argument("--selftest", action="store_true", help="check the mock against cli.c behavior and exit")
    ap.add_argument("-v", "--verbose", action="store_true", help="log every line in and out")
    ap.add_argument("--reset-every", type=float, metavar="S", help="simulate an STM32 reset every S seconds")
    ap.add_argument("--fault-every", type=float, metavar="S", help="inject a lock fault every S seconds")
    ap.add_argument("--no-pilot", action="store_true", help="never lock the pilot (RDS muted)")
    ap.add_argument("--flash", metavar="FILE", help="persist SAVEd settings in this JSON file")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    port = SerialPort(args.port) if args.port else TcpPort(args.tcp)
    try:
        run(port, args)
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
