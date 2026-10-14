# FM-303 licensing

FM-303 is free software under the GNU General Public License, version 3 only
(`GPL-3.0-only`, full text in `LICENSE`). It is built on Felucca's platform layer (through X0X and
FoMni) and keeps Felucca's licence. If you distribute FM-303, or firmware derived from it, you
must give your recipients its complete corresponding source under the same licence.

## Where the code comes from

| What | Origin | Licence |
| --- | --- | --- |
| Platform: `firmware/hal/`, `firmware/loader/`, `firmware/crt0.S`, `firmware/app.ld`, `firmware/src/{main,libc,lcd,gfx,usb,storage,ota,midi_uart}.c`, `tools/` (build, package, install, rescue, fonts), `web/fm1*.js`, `web/fm303_installer.html`, `tests/{storage,ota,ldr}_test.c`, `tests/install_test.py`, `tests/host/{encoder,uac,trs,fastmath}_test.c` | [Felucca](https://github.com/hugelton/Felucca), Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments, as changed for [X0X](https://github.com/charlesvestal/fm1-x0x), [FoMni](https://github.com/charlesvestal/fm1-fomni) and FM-303 | GPL-3.0-only |
| `firmware/src/app/{panel,plat_fm1,main_fm1}.c`, `firmware/src/app/plat.h`, `host/fm303_host.c` (its skeleton) | adapted from FoMni, itself from X0X and Felucca's `panel.c`, `main.c`, `audio.c` | GPL-3.0-only |
| The 303 voice: `firmware/src/dsp/bass303.{c,h}`, `x0x_param.h`, `fastmath.h`, `tests/host/bass303_test.c`, `tests/host/run_bass303.sh` | X0X, ported from [schwung-303](https://github.com/charlesvestal/schwung-303): Open303 by Robin Schmidt (MIT), Devilfish extensions after jc303 (midilab), RAT drive after dm-Rat (Dave Mollen) | GPL-3.0 (Open303 parts MIT) |
| The idea of a browser emulator for FM-1 firmware (planned) | [Groove OS](https://www.groove-os.com/) by Peter Gombos; no code from it | credit only |
| Everything else: `firmware/src/seq/`, `firmware/src/dsp/fm303.*`, `firmware/src/app/{ui,project}.c`, `firmware/src/app/app.h`, `tests/host/seq_test.c`, `tests/scenarios/`, `docs/` | FM-303 | GPL-3.0-only |

The factory patterns in `seq/tb303seq.c` were written for FM-303.

## Third-party material

| What | Licence | Where |
| --- | --- | --- |
| Barlow Semi Condensed (The Barlow Project Authors), the UI face | SIL OFL 1.1 | `assets/fonts/BarlowSemiCondensed-*.ttf`, `assets/fonts/Barlow-OFL.txt` |
| Terminus (Dimitar Toshkov Zhekov), an alternative font set | SIL OFL 1.1 | `assets/fonts/ter-u*.bdf`, `assets/fonts/Terminus-LICENSE.txt` |
| JieLi AC79 SDK: `uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin` are read from your SDK checkout at build time and placed in the package; no SDK files are in this tree | Apache-2.0 | <https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK> |

## Trademarks

TB-303 is a trademark of Roland Corporation, used here only to say what FM-303 is modelled on.
"Felucca" and "Hügelton Instruments" are names of Hügelton Instruments. "M-VAVE" and "FM-1" are
trademarks of their respective owners. FM-303 is independent firmware; it is not affiliated
with, endorsed by or supported by any of them.

## Radio

FM-303 never enables the Bluetooth / Wi-Fi radio of the hardware.
