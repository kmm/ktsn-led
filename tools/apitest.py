#!/usr/bin/env python3
"""Exercise the serial JSON API (`api` console command) on a connected board.

    source ~/.espressif/tools/activate_idf_v6.1.sh      # provides pyserial
    python3 tools/apitest.py /dev/cu.usbserial-0001      # or /dev/cu.usbmodem*

Protocol: send `api`, then one JSON request per line; responses are lines starting with 0x1E.

Reset on open: ESP32 dev boards (and the C6's USB-Serial/JTAG, which emulates the same circuit) reset the chip when
RTS is asserted while DTR is not. Opening the port asserts both at once (harmless); de-asserting them afterwards goes
DTR first in pyserial, briefly giving RTS=1/DTR=0 = reset. So DTR/RTS are left as the OS sets them on open.
`--release-lines` restores the old behaviour, to compare. The board's uptime is printed as evidence either way.
"""
import json
import re
import sys
import time

import serial

RS = b"\x1e"


class Board:
    def __init__(self, port, release_lines=False):
        self.s = serial.Serial()
        self.s.port, self.s.baudrate, self.s.timeout = port, 115200, 0.2
        if release_lines:
            self.s.dtr = self.s.rts = False             # old behaviour: pyserial drops DTR first -> reset pulse
        self.s.open()
        self.buf = b""
        self.next_id = 1
        self.rebooted = False

    def other(self, line):
        """A non-response line (log, prompt, echo): show it (without terminal escapes) and spot reboots."""
        text = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", line.decode(errors="replace")).strip()
        if text:
            print(f"    (other) {text}")
        if "Returned from app_main" in text or "rst:" in text or "boot:" in text:
            self.rebooted = True

    def frame(self, timeout=5.0):
        """Next 0x1E response line as JSON; other lines are shown and skipped."""
        end = time.time() + timeout
        while time.time() < end:
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                if line.startswith(RS):
                    return json.loads(line[1:])
                self.other(line)
            self.buf += self.s.read(4096)
        raise TimeoutError("no response frame")

    def settle(self, quiet=1.0, limit=6.0):
        """Wait until the board has been silent for `quiet` s (it may have rebooted when the port opened)."""
        end, last = time.time() + limit, time.time()
        while time.time() < end and time.time() - last < quiet:
            chunk = self.s.read(4096)
            if chunk:
                last = time.time()
                self.buf += chunk
        while b"\n" in self.buf:                       # show and classify what arrived (boot log etc.)
            line, self.buf = self.buf.split(b"\n", 1)
            self.other(line)

    def call(self, op, **params):
        req = {"id": self.next_id, "op": op, **params}
        self.next_id += 1
        self.s.write((json.dumps(req) + "\n").encode())
        f = self.frame()
        assert f["id"] == req["id"], f"id mismatch: {f}"
        return f["status"], f["body"]


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    b = Board(args[0] if args else "/dev/cu.usbserial-0001", release_lines="--release-lines" in sys.argv)
    b.settle()                                          # a C6 reboots when its USB port is opened
    b.s.write(b"\nexit\n")                               # in case a previous session left it in api mode
    b.settle(quiet=0.3)
    b.buf = b""
    b.s.write(b"api\n")
    ready = b.frame()
    print("ready:", ready)
    assert ready["body"].get("ready"), "no ready frame"

    checks = []
    raw_call = b.call

    def call(op, **params):
        st, body = raw_call(op, **params)
        last["st"], last["body"] = st, body
        return st, body
    b.call = call

    last = {}

    def check(name, cond, detail=""):
        checks.append(cond)
        print(f"{'ok  ' if cond else 'FAIL'} {name} {detail}")
        if not cond:
            print(f"     status {last.get('st')}, body {json.dumps(last.get('body'))[:400]}")

    st, body = b.call("ping")
    check("ping", st == 200 and body.get("pong"))
    st, body = b.call("status")
    check("status", st == 200 and "fps" in body and "channels" in body, f"fps={body.get('fps')}")
    uptime = body.get("uptime_s", -1)
    print(f"     board uptime {uptime} s{'  <- just booted: the port open reset it' if 0 <= uptime < 15 else ''}")
    st, cfg = b.call("config")
    check("config", st == 200 and "channels" in cfg, f"{len(cfg.get('channels', []))} channel(s)")
    check("config: wire format", all(isinstance(c.get("wire"), str) and c["wire"] for c in cfg.get("channels", [])),
          ", ".join(str(c.get("wire")) for c in cfg.get("channels", [])))
    st, body = b.call("patch", body={"channels": {"0": {"wmax": 101}}})
    check("patch: wmax out of range", st == 400 and "channels.0.wmax" in body.get("errors", {}), json.dumps(body))
    st, body = b.call("patch", body={"channels": {"0": {"wmode": "sideways", "wpoint": "#12345"}}})
    errs = body.get("errors", {})
    check("patch: bad white mode/point", st == 400 and "channels.0.wmode" in errs and "channels.0.wpoint" in errs, json.dumps(body))
    st, body = b.call("patch", body={"channels": {"0": {"wire": "RGX"}}})
    check("patch: bad wire format", st == 400 and "channels.0.wire" in body.get("errors", {}), json.dumps(body))
    old = cfg["channels"][0]["prog"] or ""
    st, body = b.call("prog", ch=0, src="(fill (buf O) #000000")
    check("prog: compile error", st == 400 and "unclosed" in body.get("error", ""), body.get("error"))
    long_src = "; long multi-line program, beyond the 511-char console line\n" + \
               "".join(f"(set (buf O) {i % 10} #{i * 4:06X})  ; pixel {i % 10}\n" for i in range(40))
    st, body = b.call("prog", ch=0, src=long_src)
    check("prog: long multi-line", st == 200, f"{len(long_src)} chars")
    st, body = b.call("patch", body={"intensity": 300})
    check("patch: validation error", st == 400 and "intensity" in body.get("errors", {}), json.dumps(body))
    old_gamma = cfg["channels"][0].get("gamma", 1)
    st, body = b.call("patch", body={"channels": {"0": {"gamma": 2.2}}})
    st2, c2 = b.call("config")
    check("patch: gamma", st == 200 and c2["channels"][0].get("gamma") == 2.2, json.dumps(body))
    st, body = b.call("patch", body={"channels": {"0": {"gamma": 3.5}}})
    check("patch: gamma out of range", st == 400 and "channels.0.gamma" in body.get("errors", {}), json.dumps(body))
    st, body = b.call("cmd", line="gamma")
    check("cmd gamma", st == 200 and "ch0: gamma 2.20" in body.get("output", ""), repr(body.get("output", "")[:60]))
    old_dither = cfg["channels"][0].get("dither", 0)
    st, body = b.call("patch", body={"channels": {"0": {"dither": 3}}})
    st2, c2 = b.call("config")
    check("patch: dither", st == 200 and c2["channels"][0].get("dither") == 3, json.dumps(body))
    st, body = b.call("patch", body={"channels": {"0": {"dither": 5}}})
    check("patch: dither out of range", st == 400 and "channels.0.dither" in body.get("errors", {}), json.dumps(body))
    b.call("patch", body={"channels": {"0": {"gamma": old_gamma, "dither": old_dither}}})
    old_out = cfg.get("output", {"refresh": 1, "parallel": False})
    st, body = b.call("patch", body={"output": {"refresh": 2}})
    st2, c2 = b.call("config")
    check("patch: output refresh", st == 200 and c2.get("output", {}).get("refresh") == 2, json.dumps(body))
    st, body = b.call("patch", body={"output": {"refresh": 9}})
    check("patch: refresh out of range", st == 400 and "output.refresh" in body.get("errors", {}), json.dumps(body))
    st, body = b.call("status")
    check("status: output_hz", st == 200 and body.get("output_hz", 0) > 0, f"output_hz={body.get('output_hz')}")
    b.call("patch", body={"output": {"refresh": old_out.get("refresh", 1), "parallel": old_out.get("parallel", False)}})
    old_ci = cfg["channels"][0].get("intensity", 1)
    st, body = b.call("patch", body={"channels": {"0": {"intensity": 0.5}}, "fade": 1})
    st2, s_mid = b.call("status")
    time.sleep(1.5)
    st3, s_end = b.call("status")
    mid, end = s_mid["channels"][0].get("intensity", -1), s_end["channels"][0].get("intensity", -1)
    check("channel intensity fades", st == 200 and old_ci - 0.01 >= mid >= 0.49 - (0 if old_ci > 0.5 else 1) and abs(end - 0.5) < 0.01,
          f"mid {mid}, end {end}")
    st, body = b.call("patch", body={"channels": {"0": {"intensity": 1.5}}})
    check("channel intensity out of range", st == 400 and "channels.0.intensity" in body.get("errors", {}), json.dumps(body))
    b.call("patch", body={"channels": {"0": {"intensity": old_ci}}})
    old_auto, old_every = cfg.get("automation"), cfg.get("automation_every", 10)
    st, body = b.call("patch", body={"automation": "(fill (buf O) #FF0000)"})
    check("automation: render code refused", st == 400 and "automation" in body.get("errors", {}), json.dumps(body))
    st, body = b.call("patch", body={"automation": "(set-intensity _ (intensity))", "automation_every": 2})
    time.sleep(0.5)
    st2, s1 = b.call("status")
    time.sleep(2.5)
    st3, s2 = b.call("status")
    runs1, runs2 = s1.get("automation", {}).get("runs", -1), s2.get("automation", {}).get("runs", -1)
    check("automation runs (at once, then every 2 s)", st == 200 and runs1 >= 1 and runs2 > runs1 and s2["automation"]["faults"] == 0,
          f"runs {runs1} -> {runs2}")
    b.call("patch", body={"automation": old_auto, "automation_every": old_every})
    st, cfg = b.call("config")
    if "loc" in cfg:
        old_loc = cfg["loc"]
        st, body = b.call("patch", body={"loc": {"lat": 51.48, "lon": -0.5}})
        st2, cfg = b.call("config")
        st3, bad = b.call("patch", body={"loc": {"lat": 91, "lon": 0}})
        check("location: set, read back, out of range refused", st == 200 and cfg["loc"] == {"lat": 51.48, "lon": -0.5} and st3 == 400, json.dumps(cfg["loc"]))
        b.call("patch", body={"loc": old_loc})
    st, body = b.call("lib_put", name="apitest-tmp", src="(fill (buf O) #010203)")
    st2, lst = b.call("lib")
    st3, got = b.call("lib_get", name="apitest-tmp")
    check("library: store, list, read", st == 200 and any(p["name"] == "apitest-tmp" for p in lst.get("programs", []))
          and got.get("src") == "(fill (buf O) #010203)", f"storage {lst.get('used')}/{lst.get('total')}, separate={lst.get('separate')}")
    st, body = b.call("lib_put", name="apitest-bad", src="(fill (buf O)")
    check("library: compile error refused", st == 400, json.dumps(body))
    st, body = b.call("lib_del", name="apitest-tmp")
    st2, got = b.call("lib_get", name="apitest-tmp")
    check("library: delete", st == 200 and st2 == 404)
    st, body = b.call("cmd", line="list 0")
    check("cmd", st == 200 and "ch0" in body.get("output", ""), repr(body.get("output", "")[:60]))
    st, body = b.call("cmd", line="reboot")
    check("cmd reboot refused", st == 400)
    st, body = b.call("nope")
    check("unknown op", st == 404)
    st, body = b.call("prog", ch=0, src=old)               # put the original program back
    check("prog: restore", st == 200)
    st, body = b.call("exit")
    check("exit", st == 200)
    print(f"\n{sum(checks)}/{len(checks)} passed")
    if b.rebooted:
        print("note: boot messages seen (check the uptime above: under ~15 s means the open reset the board)")
    return 0 if all(checks) else 1


if __name__ == "__main__":
    sys.exit(main())
