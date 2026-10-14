# Building FM-303

The build makes three files in `build/`:

| File | What |
| --- | --- |
| `fm303.bin` | the firmware app |
| `loader/ota.bin` | the update loader (Felucca's) |
| `fm303.fwsc` | the installable package (app + loader) |

## Prerequisites

- Python 3 with Pillow: `pip3 install Pillow` (the fonts are rendered at build time)
- The JieLi Linux toolchain (clang 4.0.1 for pi32v2, from JieLi's package server):

  ```
  tools/get_toolchain.sh            # installs to ~/.jieli/toolchain
  ```

- The JieLi AC79 SDK (Apache-2.0). The package uses three of its files
  (`cpu/wl82/tools/uboot.boot`, `cfg_tool.bin`, `cfg/eq_cfg_hw.bin`); they are not part of this tree.

  ```
  git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 \
      https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK
  ```

- On Linux x86-64 the toolchain runs natively. On macOS (or another CPU), Docker: the build runs
  each tool in a `linux/amd64` `debian:bookworm-slim` container. With podman, put a `docker` script
  that runs `exec podman "$@"` first on your PATH; `FM_JOBS` (default 4) limits parallel compiles.
- On Windows, build inside WSL2 (see below). `./build.sh` in PowerShell does not run the build: Windows
  hands the `.sh` file to whatever app is associated with it, usually an editor.

### Windows: WSL2

The toolchain is a Linux x86-64 program, and WSL2 runs it natively. In PowerShell (as administrator),
once:

```
wsl --install -d Ubuntu          # then restart, and set up the Ubuntu user it asks for
```

Then, in the Ubuntu terminal (not PowerShell):

```
sudo apt update
sudo apt install -y git curl xz-utils build-essential python3 python3-pil
git clone https://github.com/GorchPortley/FM-303.git ~/FM-303
cd ~/FM-303
tests/run_tests.sh                 # the host tests first: no toolchain needed
tools/get_toolchain.sh
git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 \
    https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK
./build.sh
```

Clone into the Linux home directory (`~`), not under `/mnt/c`: builds there are slow, and a checkout
made by Git for Windows may have CRLF line endings that break the scripts. WSL can't see the FM-1's
USB MIDI port, so install from Windows (see Install below): Windows sees the build at
`\\wsl$\Ubuntu\home\<you>\FM-303\build\`.

## Build

```
./build.sh                          # build/fm303.fwsc, identity FM-1_700
./build.sh --release 0.1            # build/fm303-0.1.fwsc, identity FM-1_7000100
```

`JIELI_TOOLCHAIN` and `AC79_SDK` override the default locations (`~/.jieli/toolchain`,
`~/fw-AC79_AIoT_SDK`). `FM_CDC=1` adds Felucca's USB serial console (off by default: one plain
MIDI interface).

The build fails if:

- a `double` crept in (a soft-double routine is linked; the FPU is single precision);
- anything in `src/` or `loader/` touches a hardware register (that belongs in `hal/`);
- code references the chip ROM, or `.ram_text` calls into flash;
- the image, RAM or pool is too big.

## Tests (no toolchain needed)

```
tests/run_tests.sh
```

Runs, on the build machine:

- the maths library against libm;
- the encoder, USB audio and TRS MIDI pieces kept from Felucca / X0X;
- the 303 sequencer against the TB-303's rules (`tests/host/seq_test.c`);
- the engine at the firmware's warning level (`-Wdouble-promotion -Werror`);
- flash storage, including torn writes;
- the update-path tests against `build/fm303.fwsc` when there is one (Felucca's);
- the whole app in the simulator (`tests/scenarios/*.f303`), with screenshots and audio in
  `build/scenarios/`.

`host/build_host.sh` builds the simulator alone. `build/host/fm303_host SCRIPT OUTDIR` runs one
script; the commands are listed at the top of `host/fm303_host.c`.

`tests/host/run_bass303.sh` compares the 303 voice with Open303 itself (C++, 4x oversampled,
mip-mapped wavetables), at 1x, 2x and 4x oversampling. It needs a checkout of
[schwung-303](https://github.com/charlesvestal/schwung-303) (`SCHWUNG303=path`). FM-303 ships
2x, where every check passes. 1x is informational only. 4x currently misses two spectral-centroid
limits; FM-303 doesn't use it.

## Install

From the command line (`pip3 install mido python-rtmidi`):

```
python3 tools/fm1_install.py build/fm303.fwsc
python3 tools/fm1_install.py --info          # identity of the connected FM-1
```

On Windows, run it with Windows' own Python (`py -m pip install mido python-rtmidi`, then
`py tools\fm1_install.py \\wsl$\Ubuntu\home\<you>\FM-303\build\fm303.fwsc`), with nothing else
(M-UPGRADE, a DAW) holding the FM-1's MIDI port. Run `--info` first: it shows that the FM-1 is found
and what it runs, without writing anything.

Or the web installer: `tools/make_pages.py build/fm303-X.Y.fwsc X.Y OUT_DIR` builds a static site
with it (Chrome or Edge, WebMIDI), and `tools/publish_pages.sh X.Y` pushes it to the `gh-pages`
branch.

Installing firmware is at your own risk:

- **Back to stock:** the web installer's "Back to the stock firmware", or M-VAVE's own updater
  M-UPGRADE (m-vave.com → Downloads → PC Software), writes FM-1 V15 back.
- **Update mode:** hold OCT− and OCT+ for 5 seconds.
- **Safe mode:** an FM-1 that crashes twice while starting comes up with no sound and USB on, so
  the installer can reach it. PLAY tries FM-303 again.
- **If it no longer starts** but reaches the chip's update mode (`4C4A:8057` on USB),
  `tools/fm1_rescue.sh` puts the stock firmware back from a Mac. Otherwise recovery needs
  [FM-1-transporter](https://github.com/kurogedelic/FM-1-transporter).

## Publishing a release

1. `./build.sh --release X.Y`. The identity, `FM-1_7XXYYZZ`, is what the installer checks.
2. `tools/publish_pages.sh X.Y` builds the site and pushes it to `gh-pages`.
3. Attach `build/fm303-X.Y.fwsc` to a GitHub release for the command-line download.
