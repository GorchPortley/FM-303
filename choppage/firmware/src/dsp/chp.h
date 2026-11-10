/* SPDX-License-Identifier: GPL-3.0-only */
/* CHOPPAGE, the instrument: a break chopper for jungle and breakcore.
 *
 *   breaks    up to 8 drum breaks in RAM (8-bit, the Amiga way: linear PCM or mu-law, any rate
 *             up to 44.1 kHz), each cut into up to 32 slices. Every slice carries a tag: KICK,
 *             SNARE, HAT (hats and rides) or GHOST (a quiet snare), or none.
 *   patterns  16 patterns of up to 32 sixteenth steps. A step plays a given slice, or a TYPE: a
 *             random slice of that tag (a bank of kicks, of snares, ...), or nothing (the slice
 *             before plays on, as a tracker's sample offset does). Per step: pitch, roll (2..16
 *             retriggers), reverse, roll ramp, level, probability and gate.
 *   voices    two channels (the sequence and the keyboard), each a voice and a fading tail: four
 *             voices, as many as Paula had. A slice plays on through the break until the next
 *             trigger, its gate, or the end. With SYNC on the playback rate follows the tempo
 *             (pitch = speed, the Amiga jungle sound); off, the break keeps its pitch.
 *   perform   held effects: beat repeat (1/4 .. 1/64, triplets), reverse, tape stop, octave down,
 *             stutter gate, scatter. The sequence runs on underneath and comes back in time.
 *   generator styles that write patterns (straight shuffle, jungle, roller, halftime, breakcore,
 *             tracker), and AUTO: a new variation of the pattern every bar, with fills.
 *   output    Paula (no interpolation) or clean playback, crush, Amiga output filters, drive, a
 *             DJ filter, a tempo-synced echo, the MASTER pot and a soft clipper.
 *
 * Threads, as in every Felucca firmware: chp_render() runs in the audio interrupt and is the only
 * code that touches the voices and the sequencer state. The main loop posts commands (chp_*),
 * edits patterns and slice tables in place (byte / aligned word writes: playback sees the old or
 * the new value), and reads the state below. Float only, no libm: the host renders the same
 * samples as the FM-1. */
#pragma once
#include <stdint.h>

#define CHP_SR 44100u
#define CHP_NBRK 8                       /* breaks */
#define CHP_NSL 32                       /* slices per break */
#define CHP_STEPS 32                     /* steps per pattern, at most */
#define CHP_NPAT 16                      /* patterns */
#define CHP_MEM 258048u                  /* sample memory (bytes = samples): the flash store, mirrored */
#define CHP_NONE 0xFFu

/* ----------------------------------------------------------------- breaks --- */
enum { TAG_NONE, TAG_K, TAG_S, TAG_H, TAG_G, NTAGS };
enum { FMT_PCM8, FMT_ULAW, NFMT };

typedef struct {
    uint32_t off, len;                   /* in the sample memory; len 0 = an empty slot */
    uint32_t uid;                        /* CRC-32 of the data: ties the slice table to it */
    uint16_t rate;                       /* the sample rate, Hz */
    uint8_t fmt;                         /* FMT_* */
    uint8_t beats;                       /* the loop's length in quarter notes: its tempo */
    uint8_t nsl;                         /* slices, 1..32 */
    uint8_t rsv[3];
    char name[12];                       /* NUL-terminated if shorter */
    uint32_t start[CHP_NSL];             /* slice starts, increasing, < len */
    uint8_t tag[CHP_NSL];                /* TAG_* */
} chp_break_t;

float chp_break_bpm(const chp_break_t *b);           /* beats * 60 * rate / len */
uint32_t chp_slice_end(const chp_break_t *b, int s); /* the next slice's start, or len */
int chp_slice_at(const chp_break_t *b, uint32_t pos);   /* the slice holding pos */

/* -------------------------------------------------------------- patterns --- */
/* step source: nothing, a type (a random slice with that tag), any slice, cut, or slice i */
enum { SRC_NONE = 0, SRC_K = 1, SRC_S, SRC_H, SRC_G, SRC_ANY = 5, SRC_CUT = 6, SRC_SLICE = 7 };
#define SRC_MAX (SRC_SLICE + CHP_NSL - 1)
#define SRC_IS_SLICE(s) ((s) >= SRC_SLICE)
#define SRC_IS_TYPE(s) ((s) >= SRC_K && (s) <= SRC_G)

/* step fx: roll (retriggers in the step) in bits 0-2, reverse, the roll's pitch ramp */
#define FX_ROLL 0x07u                    /* index into CHP_ROLL_N: 1, 2, 3, 4, 6, 8, 12, 16 */
#define FX_REV 0x08u
#define FX_RAMP_UP 0x10u                 /* the roll climbs an octave over the step */
#define FX_RAMP_DN 0x20u                 /* ... or falls one */
extern const uint8_t CHP_ROLL_N[8];

typedef struct {
    uint8_t src;                         /* SRC_* */
    int8_t pitch;                        /* semitones, -24..24 */
    uint8_t fx;                          /* FX_* */
    uint8_t lev;                         /* level 0..15 (15 = full, -2 dB a step) */
    uint8_t prob;                        /* chance to play, 0..100 % */
    uint8_t gate;                        /* 0: plays on; 1..8: cut after gate/8 of the step */
} chp_step_t;

typedef struct {
    uint8_t len;                         /* steps, 1..32 */
    uint8_t rsv[3];
    chp_step_t st[CHP_STEPS];
} chp_pattern_t;                         /* 196 bytes */

void chp_step_clear(chp_step_t *s);      /* nothing, full level, always, plays on */
void chp_pattern_clear(chp_pattern_t *p, int len);
int chp_pattern_valid(const chp_pattern_t *p);

/* ------------------------------------------------------------ parameters --- */
enum {
    P_TEMPO,                             /* BPM */
    P_PITCH,                             /* semitones, every voice */
    P_FILTER,                            /* DJ filter: < 0 lowpass, > 0 highpass, 0 off */
    P_CRUSH,                             /* rate and bit reduction */
    P_AUTO,                              /* the generator varies the pattern each bar: % */
    P_RESO,                              /* DJ filter resonance */
    P_DRIVE, P_DRVTYPE,                  /* soft / hard / fold */
    P_INTERP,                            /* Paula (none, the 8-bit grit) / clean (linear) */
    P_AMIGA,                             /* output filter: off / A500 / A500+LED / LED */
    P_ECHO, P_ECHOT, P_ECHOFB,           /* send, time (synced), feedback */
    P_SWING,                             /* 50..75 % (MPC style) */
    P_SYNC,                              /* playback rate follows the tempo */
    P_POOL,                              /* types pick from this break / all breaks */
    P_STYLE, P_DENS, P_CHAOS, P_FILL,    /* the generator */
    P_VARY,                              /* how often a type picks a new slice: % */
    P_RAMP,                              /* beat repeat pitch ramp, semitones a beat */
    P_QUANT,                             /* keyboard: off / 1/16 / 1/32 */
    P_LIVE,                              /* keyboard over the sequence: replace / layer */
    P_RECAS,                             /* recording writes the slice / its type */
    P_MIDICH, P_CLKOUT, P_NOTEOUT,
    P_VOLUME,                            /* output level, dB */
    P_NPARAMS
};

typedef struct {
    const char *name;                    /* <= 7 characters: drawn over a knob */
    int16_t lo, hi, def;
} chp_param_t;
const chp_param_t *chp_param_info(int i);
void chp_param_text(int i, int v, char *buf);   /* the value as the screen shows it (12 bytes) */

enum { GS_STRAIGHT, GS_JUNGLE, GS_ROLLER, GS_HALF, GS_BREAKCORE, GS_TRACKER, NSTYLES };
extern const char *const CHP_STYLE_NAME[NSTYLES];
extern const char *const CHP_TAG_NAME[NTAGS];   /* "-", "K", "S", "H", "G" */

/* ------------------------------------------------------------ perform fx --- */
enum {
    PF_R8, PF_R16, PF_R32, PF_R64, PF_R24,   /* beat repeat: 1/8, 1/16, 1/32, 1/64, 1/24 (triplet) */
    PF_REV,                                  /* everything plays backwards */
    PF_TAPE,                                 /* tape stop: the speed falls to nothing in a beat */
    PF_OCT,                                  /* an octave down (half speed) */
    PF_LOOP,                                 /* beat repeat, one beat */
    PF_GATE,                                 /* stutter gate, 1/32 */
    PF_SCATTER,                              /* random slices, rolls and reverses */
    NPERF
};
extern const char *const CHP_PERF_NAME[NPERF];

/* ------------------------------------------------------- commands (main) --- */
void chp_init(chp_pattern_t *bank, chp_break_t *brk, const uint8_t *mem);
void chp_set(int param, int value);
void chp_play(int on);                   /* start from step 1 / stop */
void chp_pattern(int pat);               /* stopped: now; playing: at the end of the pattern */
void chp_break(int brk);                 /* the break the steps play (from the next trigger) */
void chp_seed(uint32_t seed);            /* the generator's and the types' random choices */
void chp_live(int slice, int on, int lev);   /* a keyboard slice of the current break; lev 0..15 */
void chp_perf(int fx, int on);           /* PF_*, held */
void chp_rec(int on);                    /* record keyboard slices into the pattern playing */
void chp_audition(int brk, int slice);   /* play one slice now, alone (editing) */
void chp_hold(int on);                   /* silence and stop touching the sample memory */
void chp_panic(void);
enum { CHP_CLK_TICK, CHP_CLK_START, CHP_CLK_CONTINUE, CHP_CLK_STOP };
void chp_clock(int msg);                 /* MIDI clock in */

/* the render: n stereo frames, 24-bit in int32; gain Q12 from the MASTER pot (4096 = unity x2) */
void chp_render(int32_t *out_lr, uint32_t n, uint32_t gain_q12);

/* MIDI from the render (the app supplies both): out, and the USB / TRS ring in (4-byte USB-MIDI
 * packets). The render takes clock, start / stop, notes (C2 = slice 1 of the break) and program
 * changes (the pattern) itself, so they keep their timing. */
void chp_midi_out(uint32_t st, uint32_t d1, uint32_t d2);
int chp_midi_in(uint32_t *pkt);          /* 1 = got one */

/* recorded steps (render -> main loop): the main loop writes them into the pattern */
typedef struct { uint8_t pat, step; chp_step_t st; } chp_rec_t;
int chp_rec_take(chp_rec_t *r);          /* 1 = got one */

/* ------------------------------------------------ state (render -> main) --- */
extern volatile uint8_t chp_playing;     /* the sequence runs */
extern volatile uint8_t chp_cur;         /* the pattern playing */
extern volatile uint8_t chp_cue;         /* the pattern cued, CHP_NONE: none */
extern volatile uint8_t chp_step;        /* the step heard (beat repeat replays old ones) */
extern volatile uint8_t chp_mstep;       /* the step the sequence is at, underneath */
extern volatile uint8_t chp_pulse;       /* clock pulse in the step, 0..5 */
extern volatile uint32_t chp_bars;       /* bars since the start */
extern volatile uint8_t chp_ext;         /* following MIDI clock */
extern volatile uint8_t chp_held;        /* chp_hold took effect: the sample memory is free */
extern volatile uint8_t chp_recording;
extern volatile uint8_t chp_vbrk;        /* the voice heard: its break (CHP_NONE: silence) */
extern volatile uint8_t chp_vslice;      /* ... and its slice */
extern volatile uint32_t chp_vpos;       /* ... and its position, in samples */
extern volatile uint16_t chp_perf_on;    /* perform fx in effect, bit PF_* */
extern volatile float chp_level;         /* output peak of the last block, 0..1 */
extern volatile uint8_t chp_trig;        /* counts triggers (lights flash on a change) */
extern chp_step_t chp_work[CHP_STEPS];   /* the steps this bar plays (AUTO's variation) */
extern volatile uint8_t chp_work_on;     /* chp_work is in use (else the pattern itself) */
extern volatile float chp_tempo_now;     /* BPM in effect (MIDI clock: measured) */
