#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Load drum breaks into CHOPPAGE on an M-VAVE FM-1, over the USB cable (SysEx).

  chp_breaks.py list                              what the FM-1 holds
  chp_breaks.py load SLOT FILE.wav [options]      a break into slot 1..8 (replacing what is there)
  chp_breaks.py delete SLOT
  chp_breaks.py select SLOT                       the break the patterns play
  chp_breaks.py factory                           forget the loaded breaks: the factory ones again

load options:
  --beats N       quarter notes in the loop (default: guessed from the length, tempo near 88..177 BPM)
  --rate HZ       the sample rate on the FM-1: 8000..44100 (default 22050). Lower: longer breaks
                  and more Amiga grit (16574 is ProTracker's C-3, 27928 its top A-3)
  --ulaw          mu-law instead of linear 8-bit: cleaner quiet parts (linear is the Amiga sound)
  --dither        TPDF dither before the 8 bits (smoother tails, a little hiss)
  --name NAME     up to 11 characters (default: the file's name)
  --trim          cut silence at the start
  --syx OUT.syx   write the SysEx to a file instead of sending it (any SysEx tool can send it; the
                  FM-1 answers each message, so send them one at a time, waiting ~50 ms, longer after
                  the last one while it writes the flash)
  --port NAME     the MIDI port (default: the first with "FM-1" in its name)

Needs mido and python-rtmidi to send (pip3 install mido python-rtmidi); numpy makes the
conversion better and faster when it is there. WAV only (8/16/24/32-bit PCM or 32-bit float, any
rate and channels): convert other formats first (e.g. `ffmpeg -i in.mp3 out.wav`).

The protocol (firmware/src/app/sysex.c): F0 7D 43 48 50 cmd seq <8-to-7 packed data> F7, every
message answered with 7F cmd status."""
import argparse
import math
import os
import struct
import sys
import time
import wave
import zlib

ID = bytes([0x7D, 0x43, 0x48, 0x50])
SLOTS = 8
CAPACITY = 258048
CHUNK = 512
STATUS = {0: "ok", 1: "bad arguments", 2: "no room", 3: "out of order", 4: "CRC mismatch", 5: "flash error"}


# ---------------------------------------------------------------- protocol --

def pack7(data):
    """8 bits to 7, least significant first (as the FM-1's update protocol packs)"""
    out, acc, nb = bytearray(), 0, 0
    for b in data:
        acc |= b << nb
        nb += 8
        while nb >= 7:
            out.append(acc & 0x7F)
            acc >>= 7
            nb -= 7
    if nb:
        out.append(acc & 0x7F)
    return bytes(out)


def unpack7(data):
    out, acc, nb = bytearray(), 0, 0
    for b in data:
        acc |= (b & 0x7F) << nb
        nb += 7
        if nb >= 8:
            out.append(acc & 0xFF)
            acc >>= 8
            nb -= 8
    return bytes(out)


def frame(cmd, seq, payload=b""):
    return bytes([0xF0]) + ID + bytes([cmd & 0x7F, seq & 0x7F]) + pack7(payload) + bytes([0xF7])


def clean_name(name):
    n = "".join(c if 32 <= ord(c) < 127 else "?" for c in name.upper())[:11]
    return n.encode("ascii").ljust(12, b"\0")


def begin_payload(slot, length, rate, fmt, beats, name):
    return bytes([slot]) + struct.pack("<IHBB", length, rate, fmt, beats) + clean_name(name)


def break_messages(slot, data, rate, fmt, beats, name):
    """the messages that load one break: BEGIN, DATA..., END (cmd, payload) pairs"""
    msgs = [(0x02, begin_payload(slot, len(data), rate, fmt, beats, name))]
    for off in range(0, len(data), CHUNK):
        msgs.append((0x03, struct.pack("<I", off) + data[off:off + CHUNK]))
    msgs.append((0x04, struct.pack("<I", zlib.crc32(data) & 0xFFFFFFFF)))
    return msgs


# ------------------------------------------------------------------- audio --

def read_wav(path):
    """(mono float samples, rate) from a WAV: PCM 8/16/24/32-bit or 32-bit float"""
    with open(path, "rb") as f:
        raw = f.read()
    if raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise SystemExit(f"{path}: not a WAV file")
    pos, fmt, ch, rate, bits, data = 12, None, 1, 0, 16, None
    while pos + 8 <= len(raw):
        cid, size = raw[pos:pos + 4], struct.unpack("<I", raw[pos + 4:pos + 8])[0]
        body = raw[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            fmt, ch, rate = struct.unpack("<HHI", body[:8])
            bits = struct.unpack("<H", body[14:16])[0]
            if fmt == 0xFFFE and len(body) >= 26:
                fmt = struct.unpack("<H", body[24:26])[0]
        elif cid == b"data":
            data = body
        pos += 8 + size + (size & 1)
    if data is None or not rate or fmt not in (1, 3) or bits not in (8, 16, 24, 32):
        raise SystemExit(f"{path}: unsupported WAV (PCM 8/16/24/32-bit or float 32-bit only)")
    width = bits // 8
    n = len(data) // (width * ch)
    try:
        import numpy as np
        if fmt == 3:
            x = np.frombuffer(data[:n * ch * 4], dtype="<f4").astype(np.float64)
        elif bits == 8:
            x = (np.frombuffer(data[:n * ch], dtype=np.uint8).astype(np.float64) - 128.0) / 128.0
        elif bits == 16:
            x = np.frombuffer(data[:n * ch * 2], dtype="<i2").astype(np.float64) / 32768.0
        elif bits == 24:
            b = np.frombuffer(data[:n * ch * 3], dtype=np.uint8).reshape(-1, 3).astype(np.int32)
            v = (b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16))
            x = np.where(v >= 1 << 23, v - (1 << 24), v).astype(np.float64) / 8388608.0
        else:
            x = np.frombuffer(data[:n * ch * 4], dtype="<i4").astype(np.float64) / 2147483648.0
        return list(x.reshape(-1, ch).mean(axis=1)), rate
    except ImportError:
        pass
    out = []
    for i in range(n):
        s = 0.0
        for c in range(ch):
            p = (i * ch + c) * width
            if fmt == 3:
                v = struct.unpack("<f", data[p:p + 4])[0]
            elif bits == 8:
                v = (data[p] - 128) / 128.0
            elif bits == 16:
                v = struct.unpack("<h", data[p:p + 2])[0] / 32768.0
            elif bits == 24:
                v = int.from_bytes(data[p:p + 3], "little", signed=True) / 8388608.0
            else:
                v = struct.unpack("<i", data[p:p + 4])[0] / 2147483648.0
            s += v
        out.append(s / ch)
    return out, rate


def resample(x, src, dst):
    """to the FM-1's rate: a windowed-sinc lowpass at the new Nyquist, then the new samples"""
    if src == dst:
        return list(x)
    n_out = int(len(x) * dst / src)
    try:
        import numpy as np
        a = np.asarray(x, dtype=np.float64)
        if dst < src:                                    # lowpass first, 0.45 of the new rate
            fc = 0.45 * dst / src
            taps = 63
            k = np.arange(taps) - (taps - 1) / 2
            h = 2 * fc * np.sinc(2 * fc * k) * np.blackman(taps)
            a = np.convolve(a, h / h.sum(), mode="same")
        t = np.arange(n_out) * (src / dst)
        return list(np.interp(t, np.arange(len(a)), a))
    except ImportError:
        pass
    return resample_simple(x, src, dst)


def resample_simple(x, src, dst):
    """without numpy: two one-pole lowpasses, then linear (web/chp_proto.js resampleSimple too)"""
    if src == dst:
        return list(x)
    n_out = int(len(x) * dst / src)
    if dst < src:
        c = 1.0 - math.exp(-2 * math.pi * 0.42 * dst / src)
        s1 = s2 = 0.0
        y = []
        for v in x:
            s1 += c * (v - s1)
            s2 += c * (s1 - s2)
            y.append(s2)
        x = y
    out = []
    for i in range(n_out):
        t = i * src / dst
        k = int(t)
        fr = t - k
        out.append(x[k] + fr * ((x[k + 1] if k + 1 < len(x) else x[k]) - x[k]))
    return out


def ulaw_byte(v):
    """G.711 mu-law (the FM-1 decodes the standard table)"""
    s = int(max(-1.0, min(1.0, v)) * 32124)
    sign = 0x80 if s < 0 else 0
    s = min(abs(s), 32635) + 0x84
    exp = 7
    for e in range(7, -1, -1):
        if s & (0x4000 >> (7 - e)):
            exp = e
            break
    mant = (s >> (exp + 3)) & 0x0F
    return ~(sign | (exp << 4) | mant) & 0xFF


def to_bytes(x, fmt, dither=False, seed=1):
    """peak-normalized 8-bit: linear (signed) or mu-law"""
    peak = max((abs(v) for v in x), default=0.0) or 1.0
    rnd = seed
    out = bytearray(len(x))
    for i, v in enumerate(x):
        v /= peak
        if fmt == 1:
            out[i] = ulaw_byte(v * 0.999)
            continue
        q = v * 127.0
        if dither:                                       # TPDF, one LSB
            rnd = (rnd * 1103515245 + 12345) & 0x7FFFFFFF
            r1 = rnd / 0x7FFFFFFF
            rnd = (rnd * 1103515245 + 12345) & 0x7FFFFFFF
            q += r1 - rnd / 0x7FFFFFFF
        q = int(math.floor(q + 0.5))                     # halves up, as the browser loader rounds
        out[i] = max(-127, min(127, q)) & 0xFF
    return bytes(out)


def trim_start(x, floor=0.02):
    peak = max((abs(v) for v in x), default=0.0)
    for i, v in enumerate(x):
        if abs(v) > peak * floor:
            return x[max(0, i - 8):]
    return x


def guess_beats(n, rate):
    """the beats that put the loop's tempo nearest 125 BPM (as the FM-1 guesses)"""
    sec = n / rate
    return min((1, 2, 4, 8, 16, 32), key=lambda b: abs(math.log2(b * 60 / sec / 125.0)))


def prepare(path, rate=22050, fmt=0, dither=False, trim=False):
    x, src = read_wav(path)
    if trim:
        x = trim_start(x)
    y = resample(x, src, rate)
    if len(y) > CAPACITY:
        raise SystemExit(f"{path}: {len(y)} samples at {rate} Hz; the FM-1 holds {CAPACITY} in all "
                         f"(lower --rate, or a shorter loop)")
    return to_bytes(y, fmt, dither)


# -------------------------------------------------------------------- MIDI --

class Link:
    def __init__(self, port_name=None):
        try:
            import mido
        except ImportError:
            raise SystemExit("sending needs mido and python-rtmidi: pip3 install mido python-rtmidi "
                             "(or write a .syx file with --syx)")
        self.mido = mido
        names = mido.get_output_names()
        want = port_name or next((n for n in names if "FM-1" in n), None)
        if not want or want not in names:
            raise SystemExit(f"no FM-1 MIDI port (ports: {names}); is CHOPPAGE running and the cable in?")
        inn = next((n for n in mido.get_input_names() if n == want or "FM-1" in n), None)
        self.out = mido.open_output(want)
        self.inp = mido.open_input(inn)
        self.seq = 0

    def call(self, cmd, payload=b"", timeout=3.0, tries=3):
        self.seq = (self.seq + 1) & 0x7F
        msg = frame(cmd, self.seq, payload)
        for _ in range(tries):
            self.out.send(self.mido.Message("sysex", data=list(msg[1:-1])))
            t0 = time.time()
            while time.time() - t0 < timeout:
                for m in self.inp.iter_pending():
                    if m.type != "sysex":
                        continue
                    d = bytes(m.data)
                    if d[:4] != ID or len(d) < 6 or d[5] != self.seq:
                        continue
                    body = unpack7(d[6:])
                    if d[4] == 0x7F:
                        return body[1] if len(body) > 1 else 0xFF, body[2:]
                    return 0, body
                time.sleep(0.002)
        raise SystemExit(f"the FM-1 did not answer (command {cmd:#x})")


def run_messages(msgs, link, show=True):
    total = sum(len(p) - 4 for c, p in msgs if c == 0x03)
    sent = 0
    for cmd, payload in msgs:
        st, _ = link.call(cmd, payload, timeout=30.0 if cmd in (0x04, 0x05, 0x06) else 3.0)
        if st != 0:
            raise SystemExit(f"the FM-1 said: {STATUS.get(st, st)} (command {cmd:#x})")
        if cmd == 0x03:
            sent += len(payload) - 4
            if show:
                print(f"\r  {sent * 100 // max(total, 1)}%", end="", flush=True)
        if cmd == 0x04 and show:
            print("\r  written to flash")


def show_list(body):
    ver, slots = body[0], body[1]
    cap, used = struct.unpack("<II", body[2:10])
    print(f"CHOPPAGE (protocol {ver}): {used // 1024} of {cap // 1024} KB used"
          + (" (the factory breaks)" if body[10] else ""))
    for i in range(slots):
        e = body[11 + i * 21: 32 + i * 21]
        ln, rate, fmt, beats, nsl = struct.unpack("<IHBBB", e[:9])
        name = e[9:21].split(b"\0")[0].decode("ascii", "replace")
        if ln:
            bpm = beats * 60 * rate / ln
            print(f"  {i + 1}  {name:<11} {ln / rate:5.2f} s  {rate} Hz {'mu-law' if fmt else '8-bit'}  "
                  f"{beats} beats, {bpm:.1f} BPM, {nsl} slices")
        else:
            print(f"  {i + 1}  -")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("cmd", choices=["list", "load", "delete", "select", "factory"])
    ap.add_argument("slot", nargs="?", type=int)
    ap.add_argument("file", nargs="?")
    ap.add_argument("--beats", type=int, default=0)
    ap.add_argument("--rate", type=int, default=22050)
    ap.add_argument("--ulaw", action="store_true")
    ap.add_argument("--dither", action="store_true")
    ap.add_argument("--trim", action="store_true")
    ap.add_argument("--name")
    ap.add_argument("--syx")
    ap.add_argument("--port")
    a = ap.parse_args()
    if a.cmd in ("load", "delete", "select") and not (a.slot and 1 <= a.slot <= SLOTS):
        raise SystemExit(f"{a.cmd}: a slot 1..{SLOTS}")
    if not 8000 <= a.rate <= 44100:
        raise SystemExit("--rate: 8000..44100")
    msgs = []
    if a.cmd == "load":
        if not a.file:
            raise SystemExit("load: SLOT FILE.wav")
        fmt = 1 if a.ulaw else 0
        data = prepare(a.file, a.rate, fmt, a.dither, a.trim)
        beats = a.beats or guess_beats(len(data), a.rate)
        name = a.name or os.path.splitext(os.path.basename(a.file))[0]
        print(f"{a.file}: {len(data)} samples, {len(data) / a.rate:.2f} s at {a.rate} Hz, {beats} beats "
              f"({beats * 60 * a.rate / len(data):.1f} BPM) -> slot {a.slot}")
        msgs = break_messages(a.slot - 1, data, a.rate, fmt, beats, name)
    elif a.cmd == "delete":
        msgs = [(0x05, bytes([a.slot - 1]))]
    elif a.cmd == "select":
        msgs = [(0x08, bytes([a.slot - 1]))]
    elif a.cmd == "factory":
        msgs = [(0x06, b"")]
    if a.syx:
        with open(a.syx, "wb") as f:
            seq = 0
            for cmd, payload in (msgs or [(0x01, b"")]):
                seq = (seq + 1) & 0x7F
                f.write(frame(cmd, seq, payload))
        print(f"wrote {a.syx}")
        return 0
    link = Link(a.port)
    if msgs:
        run_messages(msgs, link)
    st, body = link.call(0x01)
    show_list(body)
    return 0


if __name__ == "__main__":
    sys.exit(main())
