# Building CHOPPAGE

Everything here runs from the `choppage/` directory. CHOPPAGE has its own build, tools and tests, and
shares no files with FM-303 at build time.

The build makes three files in `build/`:

| File | What |
| --- | --- |
| `choppage.bin` | the firmware app |
| `loader/ota.bin` | the update loader (Felucca's) |
| `choppage.fwsc` | the installable package (app + loader) |

## Prerequisites

- Python 3 with Pillow: `pip3 install Pillow` (the fonts are rendered at build time)
- The JieLi Linux toolchain (clang 4.0.1 for pi32v2, from JieLi's package server):

  ```
  tools/get_toolchain.sh            # installs to ~/.jieli/toolchain
  ```

- Three files of the JieLi AC79 SDK (Apache-2.0): `cpu/wl82/tools/uboot.boot`, `cfg_tool.bin` and
  `cfg/eq_cfg_hw.bin`. They are not part of this tree. Either clone the SDK:

  ```
  git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 \
      https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK
  ```

  or, if the clone fails (it is large), fetch just the three:

  ```
  B=https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK/raw/AC79NN_SDK_V1.2.1_2023-12-13/cpu/wl82/tools
  D=~/fw-AC79_AIoT_SDK/cpu/wl82/tools
  mkdir -p $D/cfg
  for f in uboot.boot cfg_tool.bin cfg/eq_cfg_hw.bin; do curl -fL -o $D/$f $B/$f; done
  ```

  The build checks them against the tested version's SHA-256.

- On Linux x86-64 the toolchain runs natively. On macOS (or another CPU), Docker: the build runs
  each tool in a `linux/amd64` container. On Windows, build inside WSL2, as for FM-303 (see the
  repository's top-level BUILDING.md).

## Build

```
./build.sh                          # build/choppage.fwsc, identity FM-1_600
./build.sh --release 0.1            # build/choppage-0.1.fwsc, identity FM-1_6000100
```

`JIELI_TOOLCHAIN` and `AC79_SDK` override the default locations. `FM_CDC=1` adds Felucca's USB serial
console.

The build fails if a `double` crept in, anything in `src/` touches a hardware register, code
references the chip ROM or `.ram_text` calls into flash, or the image, RAM or pool is too big
(Felucca's checks). The identity range `FM-1_6XXYYZZ` is CHOPPAGE's (Felucca and X0X use `FM-1_9`,
FoMni `FM-1_8`, FM-303 `FM-1_7`).

## Tests (no toolchain needed)

```
tests/run_tests.sh
```

Runs, on the build machine:

- the engine, frame by frame (`tests/host/engine_test.c`), and the DSP units at the firmware's warning
  level (`-Wdouble-promotion -Werror`);
- the break analysis against the factory breaks (`tests/host/ana_test.c`; it also writes them as
  WAVs to `build/host/factory_N.wav`);
- the break loaders: `tools/chp_breaks.py` into the simulator (`tests/loader_test.py`), the browser
  loader against it byte for byte (`tests/web_test.py`, needs node), and the page in headless
  Chromium (`tests/web_page_test.js`, needs Playwright);
- the whole app in the simulator (`tests/scenarios/*.chp`), with screenshots and audio in
  `build/scenarios/`;
- Felucca's tests: storage (torn writes), the update entry, update loader and installer against
  `build/choppage.fwsc` (when built), the encoder, USB audio, TRS MIDI, the maths.

`host/build_host.sh` builds the simulator alone. `build/host/chp_host SCRIPT OUTDIR` runs one script;
the commands are listed at the top of `host/chp_host.c`. `loadwav SLOT FILE` puts a WAV of your own
straight into the simulator, so you can hear (`wav out.wav`) and see (`shot screen.png`) your
breaks chopped before an FM-1 is involved.

## Install

From the command line (`pip3 install mido python-rtmidi`):

```
python3 tools/fm1_install.py --info          # identity of the connected FM-1 (writes nothing)
python3 tools/fm1_install.py build/choppage.fwsc
```

Or the web installer: `tools/make_pages.py build/choppage-X.Y.fwsc X.Y OUT_DIR` builds a static site
with it and the break loader (Chrome or Edge, WebMIDI).

Installing firmware is at your own risk:

- **Back to stock:** the web installer's "Back to the stock firmware", or M-VAVE's own updater
  M-UPGRADE (m-vave.com → Downloads → PC Software), writes FM-1 V15 back.
- **Update mode:** hold OCT− and OCT+ for 5 seconds.
- **Safe mode:** an FM-1 that crashes twice while starting comes up with no sound and USB on, so the
  installer can reach it. PLAY tries CHOPPAGE again.
- **If it no longer starts** but reaches the chip's update mode (`4C4A:8057` on USB),
  `tools/fm1_rescue.sh` puts the stock firmware back from a Mac. Otherwise recovery needs
  [FM-1-transporter](https://github.com/kurogedelic/FM-1-transporter).

Going between firmwares: CHOPPAGE's saved data (patterns, slices, settings, breaks) has its own
magic numbers and checksums, so data left by another firmware is ignored, and CHOPPAGE starts from
its defaults. The panel calibration is shared with the other Felucca firmwares.

## Loading breaks

See the README. The browser loader is `web/breaks.html` (open it from a local web server, or from
the Pages site; it needs `chp_proto.js` next to it). The command line loader is
`tools/chp_breaks.py`.

## A repository of its own

CHOPPAGE lives in `choppage/` of the FM-303 repository, but nothing in it refers outside it except
the link to FM-303's research notes. To give it its own history:

```
git subtree split --prefix=choppage -b choppage-only
git push git@github.com:YOU/choppage.git choppage-only:main
```

Then update `REPO` in `tools/make_pages.py` and the links in `web/choppage_installer.html`.

## Publishing a release

1. `./build.sh --release X.Y`. The identity, `FM-1_6XXYYZZ`, is what the installer checks.
2. `tools/publish_pages.sh X.Y` builds the site and pushes it to `gh-pages`. It refuses to while
   CHOPPAGE is a subdirectory of another project, whose own site it would replace (`PAGES_FORCE=1`
   overrides).
3. Attach `build/choppage-X.Y.fwsc` to a GitHub release for the command-line download.
