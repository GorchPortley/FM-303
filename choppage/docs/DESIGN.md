# CHOPPAGE design

A break chopper for the M-VAVE FM-1, built on Felucca's platform. How the FM-1 custom firmwares
are laid out (the hardware, the platform layer, the host simulator, building, packaging and
installing) is in FM-303's research notes, [../../docs/RESEARCH.md](../../docs/RESEARCH.md). This
document covers what CHOPPAGE adds on top.

Status: **first version, untested on hardware.** Everything below runs in the host simulator and
passes its tests (`tests/run_tests.sh`), and the firmware builds with JieLi's toolchain, passing
every check of the platform's build. Nothing has been flashed to an FM-1 yet.

## Layout

```
firmware/
  hal/ loader/ crt0.S app.ld           Felucca's (unchanged)
  src/main.c                           the unity build: platform, then the app (-Os)
  src/{libc,lcd,gfx,usb,storage,ota,midi_uart}.c   Felucca's (names, USB strings, the object map)
  src/app/plat.h plat_fm1.c panel.c main_fm1.c     platform glue (from FoMni): + raw sample flash,
                                       SysEx frames, MIDI to and from the render
  src/app/app.h project.c              settings, patterns, slice tables, the sample store
  src/app/sysex.c                      loading breaks over USB
  src/app/ui.c                         the panel and the screen
  src/dsp/ (separate units, -O2, float only, no libm, no libc)
    chp.h engine.c                     the instrument: voices, sequencer, perform fx, output chain
    gen.h gen.c                        the generator
    ana.h ana.c                        slicing and tagging (main loop only)
    factory.h factory.c                the synthesized factory breaks (boot only)
    fastmath.h                         X0X's maths
host/chp_host.c                        the whole app on a computer, driven by a script
tests/                                 see "Tests"
tools/chp_breaks.py, web/breaks.html + chp_proto.js   the loaders
```

## Breaks and slices

A break is 8-bit samples, linear (signed, the Amiga's format) or G.711 mu-law, at any rate from 1 to
48 kHz (the loaders offer 8363 to 44100). Its tempo is `beats × 60 × rate / length`. Up to 8 breaks
share 258 048 bytes of RAM (`mem`, in the pool), a mirror of the flash store, since the audio
interrupt never reads flash.

Up to 32 slices a break. A slice has a start and a tag (KICK, SNARE, HAT, GHOST or none) and ends
where the next begins. The table lives in the break (`chp_break_t`) and, saved, in `OBJ_KITS` with the
CRC of the samples it belongs to. If a break is replaced, its old table no longer matches and is
dropped.

### Cutting (`ana.c`)

*Hits*: 5 ms frames (longer for a long break: 1024 frames at most). In each frame, the energy is
measured three ways: as it is, after a first difference (the crack of a snare, the tick of a hat),
and below 150 Hz (a kick). A hit is a frame where any of these jumps against the recent level (a
rectified log difference: "flux"), standing out from the break's average flux by an amount set by
SENS, and a local peak, at least 35 ms after the hit before it. Each hit is then placed on the
sample where its sound rises: on the first difference for snares and hats (a ringing low tail
vanishes there), on the signal itself for kicks (a kick has little top), using the first 0.5 ms
block that climbs 30% of the way from the level before to the peak, and moved back to a zero
crossing.

*Grids* (Auto: sixteenths, eighths for a break over 8 beats; or 8 / 16 / 32): equal cuts, each moved
onto a hit within 35% of a slice if Snap is on. Real breaks swing and drag, so snapping matters.
*Hits*: a cut at every hit.

### Tagging

For every slice, the first 70 ms go through three bands: below 160 Hz (a kick's body),
250 Hz–2.5 kHz (a snare's body), above 4 kHz (wires, hats, rides). A slice only counts as a hit if a
band **rises** at its start: the low band over 40 ms (low frequencies need longer to show), the others
over 12 ms. Without that check, the tail of a kick would be tagged as another kick. Then, judged
against the other slices of the break:

- **KICK**: the low band rises, is big (a quarter of the break's biggest) and is a fifth of the
  slice; or any band rises, the low band is not falling, and the slice is nearly all low (a kick
  re-struck while the last one rings).
- **SNARE**: the body rises and is strong; **GHOST**: a snare well below the loudest snare (60%).
- **HAT**: only the top rises.

Against the factory breaks (exact truth known), 94% of the tags are right, every hit found is a real
hit, and 87–100% of the cuts fall within 4 ms of the true step (`tests/host/ana_test.c`). On real
breaks, check the tags in EDIT.

### Factory breaks (`factory.c`)

No sampled audio ships with CHOPPAGE. Three breaks are played at boot by a small drum machine into
the sample memory: a kick (a falling sine and a click), a snare (two tones and band-passed noise;
ghosts quieter and shorter), hats and a ride (six square waves as analog machines make cymbals, high
passed, with noise). The machine plays each break twice, once to find the peak and once to write
8-bit samples normalized to it. The patterns are CHOPPAGE's own. Their slices and tags are exact.

## Patterns

16 patterns of 1–32 sixteenth steps, 6 bytes a step (`chp_step_t`): source (nothing, a type, any,
cut, or slice 1–32), pitch, fx (roll 1/2/3/4/6/8/12/16, reverse, ramp up/down), level (−2 dB a step),
chance, gate. 3144 bytes in all: one flash object. The factory patterns are written in a small
notation in `project.c` (`"K - H - S - H Gl9 ..."`).

## The engine (`engine.c`)

### Voices

Two channels, the sequence and the keyboard, each a voice and a tail. When a channel starts a new
voice, the old one fades out over 2 ms underneath it, so there are four voices, as many as Paula had.
A voice plays a break from a position at a signed increment, between bounds (normally the whole
break): **a slice plays on** through the slices after it until the next trigger, its gate, or the end.
Reversed, it starts at the slice's end and runs backwards. It fades in over 0.5 ms (no click when it
starts mid-wave).

The increment is `rate / 44100 × 2^(pitch/12)`, divided by the break's tempo when SYNC is on. Per
chunk it is multiplied by the current tempo (SYNC), the PITCH knob, and the perform effects (octave
down, tape). Turning the tempo or pitch bends what is already playing.

**Paula** reads the nearest sample (zero-order hold, as Paula plays a sample at its period with no
interpolation: the images above the source's Nyquist are its sound). **Clean** interpolates linearly.

### Time

Sample-accurate. The render runs in chunks of at most 32 frames, cut short at the next event: a clock
pulse (6 a step; the 6th is the next step), a step's trigger (late by the swing on odd steps), a roll's
next retrigger, a quantized keyboard trigger, a beat repeat's start or restart. Events land on their
frame (`tests/host/engine_test.c` checks every trigger to ±2 frames).

Swing (50–75%, MPC style) delays the odd sixteenths. Rolls divide the step's own (swung) length.
MIDI clock in drives the steps (a step every 6 ticks, starting on the tick after START), and the tempo
is measured from the ticks for SYNC and the sub-step timing. 0.5 s without a tick hands the clock
back.

### Types

A type step picks among the slices with that tag, in this break or (POOL ALL) in all of them. With
VARY at 0 the pick is a hash of the seed, the pattern and the step: each step has its own hit and
keeps it. At 100 it is new each time. In between, a share of the picks are new. A new pick is never
the one before if there is another. A tag with no slices falls back to a snare for a ghost, a ghost
for a snare, or else any slice.

### The shadow, and coming back in time

The sequence always runs. When something takes the sound over (a held keyboard slice in Replace
mode, a beat repeat), its triggers still run "in the shadow": the voice it would be playing, and when
it started, are kept. When the override ends (the key's slice is over, the repeat key is let go,
reverse, tape stop or octave down released), the voice starts again where the shadow says it would
be by now, fading in over 2 ms. That is why the break always comes back in time.

### Beat repeat

A repeat of W steps (1/8: 2, 1/16: 1, 1/32: ½, 1/64: ¼, 1/24: ⅔, beat: 4) starts on the next
multiple of W from the bar's start (for whole steps, on that step's swung trigger). It snapshots both
channels' voices and restores them every W, with a fade-in and the ramp's pitch. For W of a step or
more, the steps inside the window are replayed from a copy of the pattern taken at the start. The
last repeat key held wins.

### AUTO and the generator (`gen.c`)

At the top of each pattern pass, if AUTO is above 0 or a fill is due, `gen_vary` writes the pass into
`chp_work`: each step, with AUTO's chance (a quarter of it on the backbone: 1, the backbeats), becomes
what the style would play there, another slice of the same kind, a roll (often ramped), a reverse, a
stutter of the step before, a pitch jump, or nothing. A fill rewrites the last beat (four kinds:
climbing snare roll; kick, snares, reversed snare; ghosts into a snare and a stutter; a breakcore 16×
roll falling an octave). Otherwise the render plays the pattern itself, so edits are heard at once.

Styles are bars of 16 in types, with optional hits that DENSITY decides (`"K.h.S.hgk.K.S.hg"`). CHAOS
adds rolls before the backbeats and at the bar's end, reversed snares sucked into the backbeat, pitch
jumps and stutters. STRAIGHT plays the break's own order (the slice nearest each sixteenth) and
swaps hits for others of the same kind. TRACKER jumps around the break by slice number on the beats,
the way a tracker's 9xx does, with E9x-like retriggers. The generator is a pure function of its
arguments and seed, cheap enough to run in the audio interrupt once a pass.

### Output

`crush` (sample and hold 44.1 kHz down to 1.95 kHz, 16 down to 4 bits) → `Amiga` (A500: the 4.4 kHz
RC low pass that is always there; LED: the 3.1 kHz 12 dB Butterworth; or both) → `drive` (soft tanh,
hard clip, or a triangle fold) → `DJ filter` (one knob: low pass below centre, high pass above; a
TPT state-variable filter, its cutoff smoothed) → `echo` (half rate, 0.74 s, tempo-synced times, the
feedback high and low passed and saturated: dub) → `stutter gate` → volume, MASTER, soft clip.

## Threads

As in every Felucca firmware: the render (audio interrupt, every 5.8 ms) is the only code that touches
voices and sequencer state. The main loop posts commands through a 64-entry single-producer queue,
edits patterns and slice tables in place (byte and aligned-word writes: a trigger sees the old value or
the new one), and reads the state the render publishes. MIDI in is read by the render itself, so
clock and notes keep their timing; nothing in the render posts to the queue. Recorded steps go
back to the main loop through a second ring.

`chp_hold(1)` makes the render stop, silence everything and stop reading the sample memory
(`chp_held` says when it has). The store only moves samples while held.

## Memory and flash

| | |
|---|---|
| RAM `.data`+`.bss` | 78 KB of 96 (the echo line is 32 KB, the analysis frames 8 KB) |
| Pool | 327 KB of 336: the samples (252 KB) and the screen canvas (69 KB) |
| Image | 139 KB, of which 66 KB are fonts |

Flash (inside Felucca's data areas, which the build's flash driver allows):

| Offset | What |
|---|---|
| 0x97000–0x98FFF | `OBJ_PATS`: the patterns (A/B sectors, CRC, torn-write safe: Felucca's `storage.c`) |
| 0x99000–0x9AFFF | `OBJ_KITS`: the slice tables |
| 0x9C000 | the sample store's header: magic, used, and per break offset, length, rate, format, beats, name, CRC-32 |
| 0x9D000–0xDBFFF | the samples (63 sectors) |
| 0xDC000–0xDDFFF | `OBJ_PANEL`: the panel calibration (Felucca's place: one made under another Felucca firmware holds) |
| 0xE0000–0xE4FFF | the update loader's staging area |
| 0xFC000–0xFDFFF | `OBJ_PROJ`: the settings |

Loading a break pulls the breaks after its slot down (in RAM), adds the new one at the end, and
rewrites the flash from the first byte that moved. The header is erased first, the samples written,
and the header written last and read back. If power is lost halfway, there is no header: the factory
breaks come back, and the breaks can be loaded again. Each break's CRC is checked at boot, and a
damaged break is left out on its own.

## Loading breaks (`sysex.c`)

`F0 7D 43 48 50 cmd seq <data packed 8 bits to 7> F7` (7D: the manufacturer ID for non-commercial
use). HELLO, BEGIN (slot, length, rate, format, beats, name), DATA (offset, up to 512 bytes), END
(CRC-32), DELETE, FACTORY, SLICES (a table from the computer), SELECT. Every message is answered
`7F cmd status` with its seq, one at a time (the platform holds one received SysEx frame). BEGIN waits
for the engine to be held before it answers; the engine stays held until END or 5 s of silence.
The frames share Felucca's SysEx path with the update protocol; the update entry only sees frames
that are not CHOPPAGE's.

The two loaders build the same bytes (`tests/web_test.*`). They convert to mono, resample (the
browser's resampler, numpy's windowed sinc, or two one-pole low passes and linear), normalize the
peak, and quantize to 8 bits (TPDF dither optional) or mu-law.

## Tests

| | |
|---|---|
| `tests/host/engine_test.c` | 77 checks, frame by frame: timing, types, VARY, POOL, rolls, swing, gate, cut, chance, beat repeat and resync, punch-in, layer, quantize, tape stop, reverse, MIDI clock, recording, hold, the generator |
| `tests/host/ana_test.c` | hits, cuts and tags against the factory breaks' truth; writes them as WAVs |
| `tests/loader_test.py` | `chp_breaks.py` → .syx → the simulator: load, replace, μ-law, persist over reboot, a bad CRC refused, delete, select, factory |
| `tests/web_test.py`, `web_test.js`, `web_page_test.js` | the browser loader: the same bytes as the Python tool; the page in headless Chromium against a pretend FM-1 |
| `tests/scenarios/*.chp` | the panel as played: views, perform keys, steps, slices, generator, recording, MIDI, persistence, the sound chain at its extremes (never NaN, never over 0 dBFS) |
| Felucca's | storage (torn writes), the update entry and loader against the package, the installer, encoder, USB audio, TRS MIDI, fastmath |

## What needs the device

These can only be learned on an FM-1:

1. **Boot.** Does it start? The crash screen and safe mode from the platform report a failure.
2. **CPU load.** Shown on SETUP. Four voices at most, light per sample, plus the filters and the
   echo. X0X runs much more on the same chip, but this is not measured yet. The 8-bit LUT reads
   should be cheap.
3. **Boot time.** The factory breaks are synthesized at boot (three breaks, two passes each), and a
   loaded store is copied from flash into RAM. Both should be well under a second.
4. **Loading speed** over USB, and the flash write after END (up to 64 sectors, a few seconds, the
   audio silent meanwhile).
5. **Feel.** Quantize and the late-press window, knob acceleration on Start, and what the perform keys
   are like under the fingers.

## Roadmap

- **Next: on the device.** First install, the load and timing, fixes. A release (`FM-1_6000100`)
  and the Pages site (in a repository of its own, so its site doesn't replace FM-303's).
- **Chopping:** more slices (64) for 4-bar breaks; slice gain and per-slice reverse; record perform
  repeats into steps; pattern chains and song mode; MIDI CC for the perform knobs.
- **Sound:** a time-stretch mode (granular) to keep pitch with SYNC; per-break gain; a sub-bass voice
  for reese and 808 lines under the breaks.
- **Analysis:** better tags on real breaks (a learned threshold per break, ride vs hat).
- **Emulator:** the simulator in the browser (as FoMni and X0X do), with the break loader built in.
