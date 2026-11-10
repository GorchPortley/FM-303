#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The browser loader (web/chp_proto.js) makes the same bytes as tools/chp_breaks.py: a fixture from
the Python tool, checked by tests/web_test.js under node (skipped without node)."""
import json
import math
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import chp_breaks as cb  # noqa: E402

node = shutil.which("node")
if not node:
    print("web: skipped (no node)")
    sys.exit(0)
x = [0.6 * math.sin(i * 0.37) + 0.3 * math.sin(i * 2.1) * math.exp(-i / 900.0) for i in range(3000)]
x[5] = 0.5 / 127 * 1.7                               # a value near a rounding half
data = cb.to_bytes(cb.resample_simple(x, 44100, 22050), 0)
syx = b"".join(cb.frame(c, i + 1, p) for i, (c, p) in enumerate(cb.break_messages(1, data, 22050, 0, 8, "amen 2bar")))
hello = bytes([1, 8]) + struct.pack("<II", cb.CAPACITY, 70000) + b"\0"
for i in range(8):
    hello += struct.pack("<IHBBB", 70000 if i == 1 else 0, 22050, 0, 8, 32) + cb.clean_name("amen" if i == 1 else "")
fx = {"x": x, "lin": list(cb.to_bytes(x, 0)), "ulaw": list(cb.to_bytes(x, 1)), "dith": list(cb.to_bytes(x, 0, True)),
      "res": cb.resample_simple(x, 44100, 22050), "crc": __import__("zlib").crc32(cb.to_bytes(x, 0)),
      "slot": 1, "data": list(data), "rate": 22050, "fmt": 0, "beats": 8, "name": "amen 2bar", "syx": list(syx),
      "hello": list(hello)}
out = ROOT / "build" / "host"
out.mkdir(parents=True, exist_ok=True)
(out / "web_fixture.json").write_text(json.dumps(fx))
sys.exit(subprocess.run([node, str(ROOT / "tests" / "web_test.js"), str(out / "web_fixture.json")]).returncode)
