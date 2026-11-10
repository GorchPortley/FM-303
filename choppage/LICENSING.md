# CHOPPAGE licensing

CHOPPAGE is free software under the GNU General Public License, version 3 only (`GPL-3.0-only`, full
text in `LICENSE`). It is built on Felucca's platform layer (through X0X and FoMni) and keeps
Felucca's licence. If you distribute CHOPPAGE, or firmware derived from it, you must give your
recipients its complete corresponding source under the same licence.

## Where the code comes from

| What | Origin | Licence |
| --- | --- | --- |
| Platform: `firmware/hal/`, `firmware/loader/`, `firmware/crt0.S`, `firmware/app.ld`, `firmware/src/{main,libc,lcd,gfx,usb,storage,ota,midi_uart}.c`, `tools/` (build, package, install, rescue, fonts, pages), `web/fm1*.js`, `web/choppage_installer.html`, `tests/{storage,ota,ldr}_test.c`, `tests/install_test.py`, `tests/host/{encoder,uac,trs,fastmath}_test.c` | [Felucca](https://github.com/hugelton/Felucca), Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments, as changed for [X0X](https://github.com/charlesvestal/fm1-x0x), [FoMni](https://github.com/charlesvestal/fm1-fomni), FM-303 and CHOPPAGE (names, identity, USB strings, the flash object map, `memmove`) | GPL-3.0-only |
| `firmware/src/app/{panel,plat_fm1,main_fm1}.c`, `firmware/src/app/plat.h`, the skeleton of `host/chp_host.c` | adapted from FoMni, itself from X0X and Felucca's `panel.c`, `main.c`, `audio.c`; extended for the sample store and SysEx | GPL-3.0-only |
| `firmware/src/dsp/fastmath.h` | X0X's single-precision maths | GPL-3.0-only |
| Everything else: `firmware/src/dsp/{chp.h,engine.c,gen.*,ana.*,factory.*}`, `firmware/src/app/{app.h,project.c,sysex.c,ui.c}`, `tools/chp_breaks.py`, `web/breaks.html`, `web/chp_proto.js`, `tests/host/{engine,ana}_test.c`, `tests/{loader,web}_test.*`, `tests/web_page_test.js`, `tests/scenarios/`, `docs/` | CHOPPAGE | GPL-3.0-only |

The factory breaks are synthesized by `factory.c` from patterns written for CHOPPAGE; no recorded
audio is included. Breaks you load are your own responsibility.

## Third-party material

| What | Licence | Where |
| --- | --- | --- |
| Barlow Semi Condensed (The Barlow Project Authors), the UI face | SIL OFL 1.1 | `assets/fonts/BarlowSemiCondensed-*.ttf`, `assets/fonts/Barlow-OFL.txt` |
| JieLi AC79 SDK: `uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin` are read from your SDK checkout at build time and placed in the package; no SDK files are in this tree | Apache-2.0 | <https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK> |

## Trademarks

"Felucca" and "Hügelton Instruments" are names of Hügelton Instruments. "M-VAVE" and "FM-1" are
trademarks of their respective owners. "Amiga" is a trademark of its owner; CHOPPAGE only imitates
the sound of its audio hardware. CHOPPAGE is independent firmware; it is not affiliated with,
endorsed by or supported by any of them.

## Radio

CHOPPAGE never enables the Bluetooth / Wi-Fi radio of the hardware.
