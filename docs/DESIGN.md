# FM-303 design

A TB-303 for the M-VAVE FM-1: one bass voice, the 303's own sequencer, the whole panel given to
the two. Built on Felucca's platform (see [RESEARCH.md](RESEARCH.md)).

Status: **milestone 1, untested on hardware.** Everything below runs in the host simulator and
passes its tests. Nothing has been flashed to an FM-1 yet (see "What needs the device").

## The sound

`dsp/bass303.c` is X0X's C port of Open303, unchanged:

- saw / "303 square" oscillator (polyBLEP; the square is Open303's transistor-shaper function)
- the TeeBee filter (Open303's TB-303 mode) with its feedback highpass, 2x oversampled
- the decay envelope driving the cutoff through two RC followers, the accent circuit (its own
  decay, and the cutoff and amp boost), the pitch slew for slides, the 200 Hz de-clicked amp
- a drive stage after the voice (Soft, or RAT after dm-Rat)

The panel's twelve sound controls are pots 0..127, mapped to physical units by `bass303_set()`
(cutoff 314–2394 Hz, decay 200–2000 ms, slide 2–360 ms, accent decay 30–3000 ms, and so on).
The screen shows the physical value.

The output stage (`dsp/fm303.c`): the MASTER pot (Q12 gain, unity at half travel × 2), then a
`tanh` soft clipper, the same signal on left and right. The voice idles (writes zeros, no
filtering) after 500 ms of silence.

## The pattern model (`seq/tb303seq.h`)

As on the TB-303, a pattern is two lists:

```
pitch list   note[16]  flag[16] (ACCENT, SLIDE)      written in PITCH mode
time line    time[16]  (NOTE, TIE, REST)             written in TIME mode
length       1..16 steps; triplet: 6 or 8 clock pulses a step
```

Playback walks the time line. Each NOTE step takes the next pitch (starting from the first one
again at the top of the pattern), a TIE holds the note before it, and a REST is silent. Accent
and slide belong to the *pitch*, so they move with the notes when the time line changes. That
is how the 303 works, and why rewriting its time line turns one riff into another.

40 bytes a pattern; 4 groups (I–IV) × 16 patterns (1A–8A, 1B–8B) = 64 patterns, 2.5 KB, one flash
object. Group I starts with eight factory patterns (written for FM-303); the rest start as the
303's cleared pattern (16 × C, every step a NOTE).

## Timing (`seq/tb303seq.c`, `dsp/fm303.c`)

- 24 pulses a quarter note (as DIN sync and MIDI clock). A step is 6 pulses, or 8 in triplets.
- A step fires on its pulse 0. **The gate of a plain note falls at pulse 3**, half the step.
- **Slide:** a note with SLIDE keeps its gate up. If the next step is a NOTE, that note starts
  legato: no new attack, and the pitch glides (Open303's slew limiter, 60 ms by default). If the
  next step is a REST, the slid note plays its whole step and ends at the rest.
- **Tie:** the gate stays up through the tied step and falls at its half (or slides on, if the
  held note slides). A tie with nothing sounding before it is silent; a tie on step 1 after a
  sounding last step ties across the loop.
- The decision at pulse 3 looks one step ahead, across the end of the pattern into the next one
  (the cue, as it stands).
- **Cueing:** a pattern picked while playing waits for the end of the pattern playing (the 303's
  behaviour). A chain (lo..hi) plays its patterns in turn.
- **Own clock:** the render splits its 256-frame block at each pulse, so steps land on the exact
  sample. MIDI clock out (F8, 24 PPQN) with start / stop, while playing on the own tempo.
- **MIDI clock in:** any clock message hands the clock over (`EXT` on the screen); 0.5 s without a
  tick hands it back. Start / continue / stop are followed.
- **Transpose:** applied as notes start, ±24 semitones.

`tests/host/seq_test.c` checks every rule above against the event log (pulse by pulse).

## Threads

As in every Felucca fork, there are two contexts:

- **the audio interrupt** (`fm_render`, every 5.8 ms): drains the command queue, runs the clock
  and the sequencer, renders the voice. Only it touches the voice and the sequencer state.
- **the main loop** (UI ~60 frames/s, input polled in between): posts commands (`fm_set`,
  `fm_play`, `fm_select`, `fm_note`, ...) into a 64-entry single-producer / single-consumer ring.
  It edits the patterns *in place*. Every field playback reads is a byte or an aligned 32-bit
  word, so a step plays either the old value or the new one.

The render publishes what the screen shows (`fm_playing`, `fm_cur`, `fm_step`, `fm_cue`,
`fm_gate`, `fm_level`, ...) as volatile bytes / words.

## The panel

The FM-1 has 27 keys, 14 buttons and 7 encoders. Here is how the 303's controls land on them:

| 303 | FM-303 |
|---|---|
| CUT OFF, RESONANCE, ENV MOD, DECAY | KNOB 1–4 on PLAY |
| ACCENT | ALGORITHM (always) |
| TEMPO | SELECT (always) |
| TUNING, VOLUME, WAVEFORM | MORE (SEL) and SOUND (FX) pages |
| pattern buttons 1–8, A/B | white keys on PLAY (1A–8A, 1B–8B) |
| PATTERN GROUP I–IV | black keys 1–4 on PLAY; PRESETS steps through all 64 |
| RUN / STOP | PLAY |
| PITCH MODE | SEQ → PITCH view: the keyboard enters notes in turn |
| TIME MODE | SEQ again → TIME view: white keys are the steps |
| NOTE / TIE / REST (DOWN, UP, ACCENT in time mode) | black keys 1–3 on TIME |
| ACCENT, SLIDE (pitch mode) | hold ENV / LFO while entering a note, or tap them alone |
| DOWN / UP (octave) | OCT− / OCT+ |
| BACK (to step 1) | REC on PITCH / TIME |
| CLEAR | SAVE + REC, twice |
| Pattern copy (TD-3 style) | SAVE + white key on PLAY |
| Transpose (TD-3 style) | ARP (KEYS) on PLAY, then a key while playing; or MIDI notes in |

The screen has three bands: a header (view, pattern, cue, tempo, play light), the pattern as a
little piano roll with its time line under it, and the four knobs' values.

## Saving

Two flash objects through Felucca's `storage.c` (A/B sectors, CRC, torn-write safe):
`OBJ_PROJ` (the sound, tempo, MIDI settings, the pattern selected) at `0xFC000`, and `OBJ_PATS`
(the 64 patterns) at `0x97000`. SAVE writes both. A change saves itself 4 s later, but only
while stopped and silent, because erasing flash pauses the audio. A pattern that fails validation
on load is cleared alone. The panel calibration (`OBJ_PANEL`, `0xDC000`) is Felucca's.

## MIDI

- In (USB and the TRS jack): clock, start, continue, stop. Notes on the MIDI channel (default 1)
  play the 303 while stopped (velocity ≥ 100 = accent, a note over a held one = slide) and
  transpose the pattern from C4 while playing.
- Out (USB): the sequencer's notes (accent = velocity 127, legato = the new note-on before the old
  note-off), the keyboard's notes, and clock + start / stop with the own tempo. NoteOut and
  ClkOut on the SETUP page.

## What needs the device

These can only be learned on an FM-1:

1. **Boot.** The first install shows whether the image starts. The crash screen and safe mode
   from the platform report a failure.
2. **CPU load.** One 303 at 2x oversampling cost 60 ns a sample on the build machine. X0X runs
   two of the same voice, plus a 909, an 808 and a break player, on the same chip, so one should
   fit easily. A load readout would confirm it: FM-303 has `plat_cpu_pct()` wired but not yet on
   screen.
3. **Key LEDs.** The lights reuse FoMni's code, which works on the device.
4. **Timing feel** with the real keys and encoders (acceleration, the chain gesture).
5. **Flash save** pauses while stopped.

## Roadmap

- **M1 (this):** the voice, the pattern model and sequencer, PLAY / PITCH / TIME / sound
  pages, save / load, MIDI in / out, the simulator and tests.
- **M2 — on the device:** build with the JieLi toolchain, first install, measure load, fix what
  the device shows. Add the CPU/xrun readout to SETUP. Make a release (`FM-1_7000100`) and the
  GitHub Pages installer.
- **M3 — 303 completeness:** TRACK mode (the 303's 7 tracks of up to 64 measures: a pattern + a
  transpose per measure, black keys 5–11 pick the track), pattern length beyond 16 by chaining
  A+B as one, live REC while playing, shuffle, CC control of the knobs, a factory restore.
- **M4 — sound:** a tempo-synced delay (the classic acid companion), a second 303 or a simple
  drum part (X0X's 909 is GPL and available), an overload guard (X0X's `bass303_set_lite`).
- **M5:** the browser emulator (FoMni's `web/emu`, built with Emscripten) and a manual.
