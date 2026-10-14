# How M-VAVE FM-1 custom firmwares are built

Research notes made before starting FM-303 (October 2026). They cover the hardware as the
open-source firmwares see it, how those projects are laid out, how a firmware gets built, packaged
and installed, and what FM-303 takes from each. Where a fact comes from one source only, it says
so.

## 1. The landscape

The FM-1 is a small 27-key FM synth. Since mid-2026 it has become a popular target for
replacement firmware, mostly because one project, Felucca, published a complete open platform
for it. The firmwares known at the time of writing:

| Firmware | Author | What it is | Source | Licence |
|---|---|---|---|---|
| [Felucca](https://github.com/hugelton/Felucca) | Leo Kuroshita (Hügelton Instruments) | Multi-engine synth: 13 engines, 4 tracks, a 64-step sequencer, USB audio. **The platform the other open firmwares are built on.** | open | GPL-3.0-only (5 engines MIT) |
| [X0X](https://github.com/charlesvestal/fm1-x0x) | Charles Vestal | Groovebox: 909, 808, **two 303s (Open303)** with a TB-3PO line generator, a break player. Fork of Felucca. | open | GPL-3.0-only |
| [FoMni](https://github.com/charlesvestal/fm1-fomni) | Charles Vestal | Omnichord-style chord harp. Felucca's platform by way of X0X; the leanest of the forks. | open | GPL-3.0-only |
| Jangada | (reported in a ranking) | Drone instrument | not checked | not checked |
| JIANT FM | (reported Oct 10, 2026) | Live and song-building firmware, said to be open source | not checked | not checked |
| SLOOP | 3dSam (reported) | Live-jamming groovebox, said to be GPL-3.0 | not checked | not checked |
| FM-1+VA | Baud Girl | Adds a VA engine and a reworked sequencer | closed | free |
| Groove OS | Peter Gombos | Groovebox. Its site runs the firmware in the browser, which inspired the emulators in X0X and FoMni. | closed | paid |
| [FM-1-RE](https://github.com/AL-255/FM-1-RE) | AL-255 | Reverse engineering of the stock firmware and its update protocol. No replacement firmware on `main`. | open | — |

A web "Firmware Hub" ([fm1.designburgapps.com](https://fm1.designburgapps.com/)) offers one-click
installs of several of these, and a way back to the stock firmware.

**What it means for FM-303:** X0X already contains a TB-303 voice that has been checked against
Open303, and a working step sequencer for it. FM-303 does not try to out-groovebox X0X. It is a
single-purpose instrument: one 303, the whole panel given to it, and the 303's own way of writing
patterns (section 6).

## 2. The hardware, as the firmwares see it

From Felucca's HAL (`firmware/hal/`), its linker script and the X0X design notes. The
reverse-engineering repository agrees on the chip.

| | |
|---|---|
| SoC | JieLi **AC791N** (AC79 / WL82 family). CPU **pi32v2**: 32-bit, single-issue, **single-precision FPU only** (doubles are emulated in software). Dual core, up to 320 MHz; the stock firmware runs it at 240 MHz (FM-1-RE). The custom firmwares don't set the clock; they run at whatever the boot code leaves. |
| Memory | ~578 KB SRAM in total. The firmwares use 96 KB for `.data`/`.bss`, a 336 KB "pool" for big buffers, 24 KB of RAM code (`.ram_text`: code that must run while the flash is busy), stacks with guard bands, and a no-init area that survives resets. |
| Flash | 1 MiB SPI NOR (JEDEC id `0x856014`). Code runs in place (XIP) through a 32 KB instruction cache; the app is mapped at `0x02000120`. |
| Screen | 240 × 240 RGB565, ST7789-class, SPI with DMA blits |
| Keys | 27 keys, F3 to G5 (16 white, 11 black), scanned in a matrix with the buttons |
| Buttons | 14: FX, SEL, ENV, LFO, EDIT, GLO, HOME, SAVE, ARP, SEQ, PLAY, REC, OCT−, OCT+, with LEDs (PLAY has a red and a green LED) |
| Encoders | 7 (relative, no push): SELECT, ALGORITHM, PRESETS, KNOB 1–4 |
| Pots | MASTER (volume) on the ADC; the battery is on another ADC channel |
| Audio | 44.1 kHz I2S (ALNK0), double-buffered DMA in 256-frame halves (5.8 ms) |
| USB | Full-speed device. The firmwares present class-compliant MIDI, optionally a UAC audio input (the synth's output, recordable over the cable) and a CDC serial console. |
| MIDI in | TRS jack (UART) |
| Radio | Bluetooth / Wi-Fi exists; every open firmware leaves it off |

The stock firmware (FM-1-RE): a port of Google's **msfa** (the Dexed core, a DX7-style 6-op FM
engine), a FreeRTOS-derived SMP kernel, the JieLi audio pipeline, USB-MIDI + UAC1, BLE-MIDI.

## 3. How the open firmwares are laid out

Every open firmware above is the same two layers. Felucca's platform is at the bottom, and
everything above it is replaced:

```
firmware/
  crt0.S  app.ld           entry stub (the SPL wants "goto +4; rts" first), memory map
  hal/                     every hardware register access: sys/clock/WDT, IRQs, timers, input
                           matrix + LEDs, audio (ALNK0), ADC, LCD, flash (with RAM-resident
                           routines), USB SIE, UART, memory guards, perf counters
  loader/                  the update loader: runs from RAM and writes the new firmware to flash
  src/
    main.c                 ONE compilation unit (a "unity build") that includes the HAL and the
                           platform .c files below, then the app
    libc.c lcd.c gfx.c     a tiny libc, LCD driver, a canvas renderer + bitmap fonts
    usb.c                  USB-MIDI (+ UAC audio in, + CDC), the MIDI rings
    midi_uart.c            MIDI in on the TRS jack
    storage.c              flash objects in A/B sector pairs with CRCs (a torn write keeps the old copy)
    ota.c                  the M-UPGRADE-compatible update entry (see section 5)
    app/                   *the instrument's app*: boot + audio ISR + main loop, panel map,
                           platform glue (plat.h), project save/load, the UI
    dsp/ seq/              *the instrument*: compiled as separate -O2 units, float only, no libm
host/                      the whole app built for a computer, behind the same plat.h
tests/                     host tests; scenario scripts that drive the simulator
tools/                     build, package, install, rescue, fonts
web/                       the web installer (WebMIDI) and, in X0X / FoMni, a WebAssembly emulator
```

Rules all of them inherit from Felucca, enforced by `tools/build.py` on every build:

- **Hardware access lives only in `hal/`.** The build scans `src/` and `loader/` and fails on a
  register address, a `volatile` pointer cast, inline asm, or a HAL register macro.
- **No doubles.** The FPU is single precision. The build fails if a soft-double routine gets linked.
- **No libm.** `dsp/fastmath.h` supplies `exp2`, `log2`, `tanh`, `sin` and the rest, so the host
  and the device compute the same samples.
- **The audio interrupt never touches flash, allocates or waits.** The UI talks to the
  instrument through a small command queue that the render drains at the start of each block.
  Flash writes (saves) silence the audio briefly, so saves happen when nothing is playing.
- **Nothing calls the chip ROM**, `.ram_text` makes no calls into flash, and the RAM and pool
  have headroom. The build checks all of it.
- **Safety nets:** a watchdog, stack/write/bus/PC guards that show a crash screen and reboot,
  and a boot-loop guard. Two failed boots enter a *safe mode* (no audio, USB on, so the
  installer can reach it). Four drop into the chip's own update mode. Holding OCT− and OCT+
  for 5 s enters update mode at any time. Holding both at power-on starts *hardware
  calibration*, which learns which button and encoder is which.

### The host simulator: how these firmwares are actually developed

`app/plat.h` is a small interface for everything the app needs from the hardware: buttons,
keys, encoders, the MASTER pot, LEDs, MIDI in and out, flash objects and the time. The device
implements it over the HAL (`plat_fm1.c`). The simulator (`host/*_host.c`) implements it over a
script:

```
tap PLAY            key w3 down        turn K1 10       midi 90 30 64
wait 2000           shot screen.png    wav out.wav      expect playing 1
```

The whole app (UI, sequencer, DSP) runs unchanged. Audio comes out as WAV, the screen as PNGs,
MIDI out as a log, and `expect` lines turn a script into a test. X0X and FoMni also compile the
same code to WebAssembly for a try-it-in-the-browser page. Almost all of the development
happens here; the device is for timing, CPU load and feel.

## 4. Building

- **Toolchain:** JieLi's Linux toolchain (clang 4.0.1 for `pi32v2`), from
  `pkgman.jieliapp.com`. It is Linux x86-64 only; on a Mac the build runs each tool in a
  `linux/amd64` Docker container.
- **SDK files:** three binaries from the JieLi AC79 SDK (`AC79NN_SDK_V1.2.1`, Apache-2.0, on
  gitee): the SPL `uboot.boot`, `cfg_tool.bin` and `eq_cfg_hw.bin`. They go into the package and
  are not stored in the firmware repositories.
- `tools/build.py` compiles the unity build at `-Os` and the DSP units at `-O2`
  (`-mcpu=r3 -mfprev1 -ffp-contract=off`), links with `--gc-sections`, lays out
  `.text` + `.ram_text` + `.data` into the image, runs the checks above, builds the update
  loader, and writes the package.

## 5. Packaging, installing, recovering

**The package (`.fwsc`)** is what M-VAVE's own updater (M-UPGRADE) installs:

- a flash image: the flash header (SDK defaults), JLFS entries, the SPL, and an `isd_config`
  blob holding the chip key. The app area (app header, `app.bin`, the SDK config blobs) is
  encrypted with JieLi's SFC stream cipher, keyed by the chip key (`0x980F`) mixed with the
  block address;
- `ota.bin`: the update loader, LZ4-compressed blocks behind JLFS headers. All checksums are
  CRC16 (poly `0x1021`);
- an **identity** string, `FM-1_<digits>`, hidden as one marker byte after each of the first 20
  blocks of 0x30 bytes. The installers check the model (`FM-1`) and, after the install, that the
  device reports the identity it was given. Each project owns a number range: Felucca
  `FM-1_9XY`, X0X `FM-1_9XXYYZZ`, FoMni `FM-1_8XXYYZZ`, and **FM-303 `FM-1_7XXYYZZ`**.

**The update protocol** is USB-MIDI SysEx, compatible with M-UPGRADE:

1. A handshake (`F0 00 32 45 00 00 00 40 7F F7`). The device answers with its identity.
2. `F0 22 24 35 7F F7` starts the update. The *running firmware* asks the host for parts of the
   package (command `0x30` read requests on the "logical image"), checks them, stages the update
   loader in flash at `0xE0000`, waits for the host's "success", writes the update record and
   resets.
3. The SPL runs the loader, which asks for the whole image the same way and writes it.
4. The FM-1 restarts, and the installer checks the identity it reports.

Nothing is committed before the host's "success". A failed step erases the staging area and the
old firmware carries on.

**Installing:** the web installer (Chrome or Edge, WebMIDI with SysEx: `web/fm1ota.js`), the
command line (`tools/fm1_install.py`, needs `mido` + `python-rtmidi`), or M-UPGRADE itself.

**Going back to stock:** the installers can write M-VAVE's FM-1 V15 back, as can M-UPGRADE.

**When an FM-1 won't start:** the safe mode and the boot-loop guard catch most crashes. If the
chip's own update mode appears on USB (`4C4A:8057`), `tools/fm1_rescue.sh` puts the stock
firmware back from a Mac. Past that, recovery needs
[FM-1-transporter](https://github.com/kurogedelic/FM-1-transporter). FM-1-RE stresses that the
update path has not been *proven* as a recovery mechanism: flashing always carries some risk.

## 6. What FM-303 takes, and what it adds

| | From | Notes |
|---|---|---|
| HAL, update loader, USB, storage, LCD, gfx, libc, MIDI UART, OTA, build/package/install/rescue tools, web installer | Felucca, through FoMni | unchanged except names, the identity, the USB strings and one more flash object (the pattern bank at `0x97000`) |
| Boot / audio ISR / main loop, panel calibration, `plat.h` + device glue, simulator skeleton | FoMni (adapted from X0X and Felucca) | renamed and rewired to FM-303's engine |
| The 303 voice (`dsp/bass303.c`), `fastmath.h` | X0X: a C port of Open303 (Robin Schmidt, MIT) with Devilfish ranges and a drive stage | unchanged. Its comparison against Open303 runs here: at 2x oversampling (the shipped setting) every check passes |
| **The 303 sequencer** (`seq/tb303seq.c`) | new | Patterns stored the way the TB-303 stores them: a pitch list (note + accent + slide) and a time line (note / tie / rest). 4 groups × 16 patterns, cueing at the end of the pattern, chains, triplets, transpose, 24 PPQN clock in and out |
| **The engine** (`dsp/fm303.c`) | new | One voice, the sequencer and its clock (own tempo or MIDI clock), live notes, the output stage |
| **The UI** (`app/ui.c`) | new, with FoMni's drawing helpers | PLAY / PITCH / TIME / SOUND / MORE / SETUP views mapped onto the FM-1 panel (see the README) |

## Sources

- Felucca: <https://github.com/hugelton/Felucca> (README, BUILDING, `firmware/`, `tools/`)
- X0X: <https://github.com/charlesvestal/fm1-x0x> (README, `docs/plans/`, `firmware/src/dsp/bass303.*`)
- FoMni: <https://github.com/charlesvestal/fm1-fomni> (README, BUILDING, LICENSING, the whole tree)
- FM-1-RE: <https://github.com/AL-255/FM-1-RE> (`docs/architecture.md`, the OTA notes)
- schwung-303 (Open303 as the reference for the voice): <https://github.com/charlesvestal/schwung-303>
- Synth Anatomy on FM-1+VA: <https://synthanatomy.com/2026/09/baud-girl-fm-1-va-custom-m-vave-fm-1-firmware.html>
- Synth Anatomy on X0X: <https://synthanatomy.com/2026/10/charles-vestal-x0x-custom-firmware-turns-the-m-vave-fm-1-into-a-rebirth-like-groovebox.html>
- MATRIXSYNTH on Felucca: <https://www.matrixsynth.com/2026/10/fm-1-custom-firmware-felucca.html>
- MATRIXSYNTH on JIANT FM: <https://www.matrixsynth.com/2026/10/jiant-fm-living-bio-synthetic-firmware.html>
- A ranking of FM-1 firmwares (the source for SLOOP, Groove OS and Jangada details): <https://dreyandersson.com/blog/m-vave-fm-1-custom-firmware/>
- Firmware Hub: <https://fm1.designburgapps.com/>
