#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""CHOPPAGE break loading, end to end: tools/chp_breaks.py writes the SysEx, the simulator (the
firmware's own sysex.c, project.c, engine) takes it. A two-bar break at 44.1 kHz stereo goes into
slot 2 and is then replaced, kept over a reboot, deleted; a factory reset brings the factory breaks
back; a damaged transfer (wrong CRC) is refused. Also: the 8-to-7 packing and mu-law against the
firmware's decoder.

  tests/loader_test.py        (after host/build_host.sh)"""
import math
import os
import struct
import subprocess
import sys
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import chp_breaks as cb  # noqa: E402

OUT = ROOT / "build" / "loader"
HOST = ROOT / "build" / "host" / "chp_host"
fails = 0


def check(ok, what):
    global fails
    print(f"  {'ok  ' if ok else 'FAIL'} {what}")
    if not ok:
        fails += 1


def make_wav(path, bpm=140.0, beats=8, rate=44100):
    """a break: kicks, snares and hats as noise and sine bursts, stereo 16-bit"""
    n = int(beats * 60.0 / bpm * rate)
    step = n // (beats * 4)
    x = [0.0] * n
    seed = 7
    for s in range(beats * 4):
        kind = "K" if s % 8 in (0, 3) else "S" if s % 8 == 4 else "H"
        a = s * step
        for i in range(min(step * 2, n - a)):
            t = i / rate
            seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
            nz = seed / 0x7FFFFFFF * 2 - 1
            if kind == "K":
                v = math.sin(2 * math.pi * (50 * t + 2.5 * (1 - math.exp(-t / 0.03)))) * math.exp(-t / 0.15)
            elif kind == "S":
                v = (0.5 * math.sin(2 * math.pi * 190 * t) + nz) * math.exp(-t / 0.1) * 0.8
            else:
                v = nz * math.exp(-t / 0.02) * 0.3
            x[a + i] += v
    with wave.open(str(path), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(struct.pack("<hh", int(v * 12000), int(v * 11000)) for v in x))
    return n


def tool(*args):
    r = subprocess.run([sys.executable, str(ROOT / "tools" / "chp_breaks.py"), *map(str, args)],
                       capture_output=True, text=True)
    if r.returncode:
        print(r.stdout + r.stderr)
    return r.returncode


def host(script):
    p = OUT / "test.chp"
    p.write_text(script)
    r = subprocess.run([str(HOST), str(p), str(OUT)], capture_output=True, text=True)
    print("\n".join("    " + ln for ln in r.stdout.splitlines() if ln.strip()))
    return r.returncode


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    # packing and mu-law
    for data in (b"", b"\x00", bytes(range(256)), b"\xff" * 513):
        check(cb.unpack7(cb.pack7(data))[:len(data)] == data, f"pack7 round trip ({len(data)} bytes)")
    check(all(b < 0x80 for b in cb.pack7(bytes(range(256)))), "packed bytes are 7-bit")

    def ulaw_dec(u):                                     # the firmware's table (engine.c chp_init)
        u = ~u & 0xFF
        e, m = (u >> 4) & 7, u & 15
        mag = (((m << 3) + 0x84) << e) - 0x84
        return (-mag if u & 0x80 else mag) / 32768.0
    fs = 32124 / 32768                                   # mu-law's full scale
    worst = max(abs(ulaw_dec(cb.ulaw_byte(v / 100.0)) - v / 100.0 * fs) for v in range(-100, 101))
    check(worst < 0.017, f"mu-law: the FM-1 decodes what the tool encodes, within half a step (worst {worst:.4f})")
    check(cb.guess_beats(77836, 22050) == 8 and cb.guess_beats(38918, 22050) == 4, "beats guessed as the FM-1 does")

    wav = OUT / "break140.wav"
    n44 = make_wav(wav)
    data = cb.prepare(str(wav), 22050, 0)
    check(abs(len(data) - n44 // 2) <= 2, f"resampled to 22050 Hz ({len(data)} samples)")
    check(max(abs((b ^ 0x80) - 0x80) for b in data) >= 126, "peak-normalized to 8 bits")
    check(tool("load", 2, wav, "--syx", OUT / "load2.syx") == 0, "chp_breaks.py load -> .syx")
    check(tool("load", 2, wav, "--syx", OUT / "load2u.syx", "--ulaw", "--rate", 16574, "--name", "amiga") == 0,
          "chp_breaks.py load --ulaw --rate 16574")
    check(tool("load", 5, wav, "--syx", OUT / "load5.syx", "--beats", 8) == 0, "a second break, slot 5")
    check(tool("delete", 2, "--syx", OUT / "del2.syx") == 0, "chp_breaks.py delete")
    check(tool("factory", "--syx", OUT / "factory.syx") == 0, "chp_breaks.py factory")
    check(tool("select", 5, "--syx", OUT / "sel5.syx") == 0, "chp_breaks.py select")
    # a damaged transfer: the END's CRC is wrong
    msgs = cb.break_messages(3, data, 22050, 0, 8, "bad")
    msgs[-1] = (0x04, struct.pack("<I", 0x12345678))
    with open(OUT / "bad.syx", "wb") as f:
        for i, (c, p) in enumerate(msgs):
            f.write(cb.frame(c, i + 1, p))
    n = len(data)
    rc = host(f"""
wait 200
expect factory 1
expect nbreaks 3
tap PLAY
wait 500
sysex {OUT}/load2.syx
expect sx_err 0
expect factory 0
expect nbreaks 1
expect blen1 {n}
expect blen0 0
expect playing 0
tap SEL
wait 100
shot breaks_loaded.png
spin ALGO 1
wait 50
expect brk 1
expect beats 8
expect rate 22050
expect nsl 32
tap HOME
tap PLAY
wait 1500
expect playing 1
expect vbrk 1
shot play_loaded.png
tap PLAY
sysex {OUT}/load2u.syx
expect sx_err 0
expect nbreaks 1
expect rate 16574
sysex {OUT}/load5.syx
expect nbreaks 2
expect blen4 {n}
reboot
wait 200
expect factory 0
expect nbreaks 2
expect blen4 {n}
sysex {OUT}/bad.syx
expect sx_err 1
expect nbreaks 2
expect blen3 0
sysex {OUT}/del2.syx
expect nbreaks 1
expect blen1 0
sysex {OUT}/sel5.syx
expect brk 4
reboot
wait 200
expect nbreaks 1
expect brk 4
sysex {OUT}/factory.syx
expect factory 1
expect nbreaks 3
reboot
wait 200
expect factory 1
expect nbreaks 3
""")
    check(rc == 0, "the simulator takes the SysEx: load, replace, keep, refuse, delete, select, factory")
    print("loader: FAILED" if fails else "loader: ok")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
