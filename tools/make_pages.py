#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Make the FM-303 site for GitHub Pages (from FoMni's tools/make_pages.py):

  index.html                    what FM-303 is, and the ways in: install, download, source
  img/                          screenshots (docs/img)
  install/index.html            the web installer (web/fm303_installer.html, with fm1pkg.js, fm1ota.js and
                                the package's metadata inlined; Chrome or Edge, Web MIDI)
  firmware/fm303-VERSION.fwsc   the package the installer writes; also the download
  emu/                          (when build/emu exists) FM-303 in the browser - not written yet

  tools/make_pages.py build/fm303-0.1-beta.fwsc 0.1-beta OUT_DIR

The package must be a release build (./build.sh --release X.Y): its identity, FM-1_7XXYYZZ, is what
the installer checks the download against and what the FM-1 reports after the install."""
import html
import json
import re
import shutil
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
REPO = "https://github.com/GorchPortley/FM-303"
BLOCKS, BLK, KEEP = 20, 0x30, 0x2F


def strip_module(src):
    """fm1pkg.js / fm1ota.js are ES modules: inlined, their export / import lines go"""
    src = re.sub(r"^export\s+", "", src, flags=re.M)
    return re.sub(r"^import .*?;\n", "", src, flags=re.M)


def product_of(raw):
    """the package identity: one marker byte after each of the first 20 blocks (fm1pkg.js productOf)"""
    return "".join(chr((m - i - 1) & 0xFF) for i in range(BLOCKS) if (m := raw[i * BLK + KEEP]) != 0x7D)


LANDING = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>FM-303</title>
<style>
  :root { --bg: #e4e5e2; --fg: #18181a; --dim: #6c6e6c; --accent: #d6481a; }
  @media (prefers-color-scheme: dark) {
    :root:not([data-theme="light"]) { --bg: #1a1b1c; --fg: #e8e9e6; --dim: #a2a4a2; --accent: #f0703c; }
  }
  body { margin: 0; background: var(--bg); color: var(--fg);
         font: 16px/1.6 -apple-system, BlinkMacSystemFont, "Helvetica Neue", Arial, sans-serif; }
  main { max-width: 640px; margin: 0 auto; padding: 48px 16px; }
  h1 { font-size: 28px; margin: 0 0 8px; }
  a { color: var(--accent); }
  .shots img { width: 240px; max-width: 100%; height: auto; image-rendering: pixelated; margin: 0 8px 8px 0; }
  .small { color: var(--dim); font-size: 13px; }
</style>
</head>
<body>
<main>
<h1>FM-303</h1>
<p>Custom firmware that turns the M-VAVE FM-1 into a TB-303-style bass line synthesizer: one 303 voice
(Open303, by way of X0X) and the 303's own sequencer, with patterns written the 303 way, in pitch mode
and time mode, accent, slide and tie.</p>
<p class="shots"><img src="img/screen-play.png" alt="FM-303's PLAY view: the group's patterns and the pattern playing as a piano roll">
<img src="img/screen-pitch.png" alt="FM-303's PITCH view: the note at the cursor"></p>
<p><a href="install/">Install __VERSION__</a> (Chrome or Edge, with the FM-1 plugged in) &middot;
<a href="firmware/__PKG__">download the firmware file</a> &middot; <a href="__REPO__">source</a></p>
<p class="small">Installing is at your own risk. The installer can put M-VAVE's own firmware back, and an FM-1 that
keeps crashing starts in a safe mode. FM-303 is built on Felucca by Leo Kuroshita (Hügelton Instruments), by way of
Charles Vestal's X0X and FoMni. GPL-3.0. TB-303 is a trademark of Roland; M-VAVE and FM-1 are trademarks of their
owners. FM-303 is not affiliated with any of them.</p>
</main>
</body>
</html>
"""


def main(pkg, version, out):
    pkg, out = Path(pkg), Path(out)
    raw = pkg.read_bytes()
    product = product_of(raw)
    if not re.fullmatch(r"FM-1_7\d{6}", product):
        raise SystemExit(f"{pkg}: identity {product!r}: make a release build (./build.sh --release X.Y)")
    if b"FELUCCA-LOADER-1" not in raw:
        raise SystemExit(f"{pkg}: no update loader in it")
    name = f"fm303-{re.sub(r'[^A-Za-z0-9.-]', '-', version)}.fwsc"
    if out.exists():
        shutil.rmtree(out)
    for d in ("install", "firmware"):
        (out / d).mkdir(parents=True)
    shutil.copy(pkg, out / "firmware" / name)
    page = (SRC / "web" / "fm303_installer.html").read_text(encoding="utf-8")
    lib = strip_module((SRC / "web" / "fm1pkg.js").read_text(encoding="utf-8")) + "\n" + \
        strip_module((SRC / "web" / "fm1ota.js").read_text(encoding="utf-8"))
    meta = json.dumps({"version": version, "product": product, "pkg": "../firmware/" + name})
    for mark in ("/*LIB*/", "/*META*/"):
        if page.count(mark) != 1:
            raise SystemExit(f"fm303_installer.html must contain {mark} once")
    (out / "install" / "index.html").write_text(page.replace("/*LIB*/", lib).replace("/*META*/", meta), encoding="utf-8")
    if (SRC / "docs" / "img").exists():
        shutil.copytree(SRC / "docs" / "img", out / "img")
    (out / "index.html").write_text(LANDING.replace("__VERSION__", html.escape(version)).replace("__PKG__", name)
                                    .replace("__REPO__", REPO), encoding="utf-8")
    emu = SRC / "build" / "emu"
    if emu.exists():
        shutil.copytree(emu, out / "emu")
    (out / ".nojekyll").write_text("")
    print(f"site: {out}: index.html, install/ ({product}), firmware/{name} ({len(raw)} B)")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:4])
