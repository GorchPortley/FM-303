/* SPDX-License-Identifier: GPL-3.0-only */
/* FM-303, the instrument: one TB-303 voice (bass303.c, X0X's port of Open303), the 303
 * sequencer (seq/tb303seq.c) and its clock, and the output stage.
 *
 *   clock     24 pulses a quarter note, from the instrument's own tempo, or from MIDI clock in
 *             while ticks keep coming (the last within ~0.5 s); with its own tempo it sends
 *             MIDI clock and start / stop (CLOCK OUT on)
 *   voice     bass303: saw / square oscillator, the 303's diode-ladder-style filter with its
 *             feedback highpass, the decay envelope with the accent circuit, slide, drive
 *   output    the MASTER pot, a soft clipper, the same signal on left and right
 *
 * Threads: fm_render() runs in the audio interrupt. Everything else posts a command that the
 * render takes at the start of its next block (fm_* below), so the voice and the sequencer are
 * only ever touched by the render. The state the screen reads (fm_playing ...) is written by the
 * render. Float only, no libm (fastmath.h): the host renders the same samples as the FM-1. */
#pragma once
#include <stdint.h>
#include "../seq/tb303seq.h"

#define FM_SR 44100u

/* parameters (a project stores these values); the first twelve are bass303's, in its order */
enum {
    P_CUTOFF, P_RESO, P_ENVMOD, P_DECAY,     /* the 303's own knobs */
    P_ACCENT, P_WAVE, P_TUNE, P_VOLUME,
    P_DRIVE, P_DRVTYPE, P_SLIDE, P_ACCDEC,   /* drive, and two Devilfish-style ranges */
    P_TEMPO,                                 /* BPM */
    P_MIDICH,                                /* MIDI channel, notes in and out: 1..16 */
    P_NOTEOUT,                               /* the sequencer's notes out over MIDI: off / on */
    P_CLKOUT,                                /* MIDI clock + start / stop out with the own tempo */
    P_NPARAMS
};
#define FM_NVOICE 12                         /* P_CUTOFF..P_ACCDEC go to the voice */

typedef struct {
    const char *name;                        /* <= 7 characters: drawn over a knob */
    int16_t lo, hi, def;
} fm_param_t;
const fm_param_t *fm_param_info(int i);
void fm_param_text(int i, int v, char *buf);   /* the value as the screen shows it (12 bytes) */

/* commands (main loop -> render) */
void fm_init(tb_pattern_t *bank);          /* the pattern bank (the project's TB_NPAT patterns) */
void fm_set(int param, int value);
void fm_play(int on);                      /* start from the top of the chain / stop */
void fm_select(int lo, int hi);            /* the pattern (lo == hi) or chain to play; see tbs_select */
void fm_transpose(int semitones);          /* the sequence, from its next note */
void fm_note(int note, int accent, int slide);   /* a live note (keyboard, MIDI in); slide: legato */
void fm_note_off(void);
void fm_panic(void);                       /* silence, now */
enum { FM_CLK_TICK, FM_CLK_START, FM_CLK_CONTINUE, FM_CLK_STOP };
void fm_clock(int msg);                    /* MIDI clock in (the main loop passes it on) */

/* the render: n stereo frames, 24-bit in int32; gain Q12 from the MASTER pot (4096 = unity x2) */
void fm_render(int32_t *out_lr, uint32_t n, uint32_t gain_q12);

/* MIDI out from the render; the app sends it (USB-MIDI) */
void fm_midi_out(uint32_t st, uint32_t d1, uint32_t d2);

/* state for the screen and the lights (written by the render) */
extern volatile uint8_t fm_playing;        /* the sequencer runs */
extern volatile uint8_t fm_cur, fm_lo, fm_hi;   /* the pattern playing, the chain it is part of */
extern volatile uint8_t fm_cue;            /* the pattern cued to follow, TB_NONE: none */
extern volatile uint8_t fm_step;           /* the step playing */
extern volatile uint8_t fm_pulse;          /* clock pulse within it */
extern volatile uint8_t fm_gate;           /* the voice's gate is up */
extern volatile uint8_t fm_note_now;       /* the note last started */
extern volatile uint8_t fm_ext;            /* following MIDI clock */
extern volatile uint32_t fm_loops;         /* patterns played since the start */
extern volatile float fm_level;            /* output peak of the last block, 0..1 */
