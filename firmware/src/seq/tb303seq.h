/* SPDX-License-Identifier: GPL-3.0-only */
/* FM-303 sequencer: the TB-303's pattern model and its playback.
 *
 * A pattern is kept the way the TB-303 keeps it, as two separate lists:
 *
 *   PITCH  up to 16 notes, each with ACCENT and SLIDE ("pitch mode" on the 303)
 *   TIME   16 steps, each NOTE, TIE or REST          ("time mode" on the 303)
 *
 * When the pattern plays, every NOTE step takes the next note from the pitch list (from the
 * first one again at the top of the pattern), a TIE holds the note before it, and a REST is
 * silent. The same pitches over another time line make another riff: half the fun of a 303.
 *
 * Timing is the 303's own: 24 clock pulses a quarter note (DIN sync and MIDI clock count the
 * same), 6 pulses a sixteenth step, 8 in triplet mode. The gate of a plain note falls halfway
 * through its step. A note with SLIDE keeps its gate up into the next note, which then glides
 * to its pitch without a new attack. A TIE keeps the gate up through the tied step.
 *
 * Threads: the playback functions (tbs_*) run in the audio interrupt only, called by the engine
 * (which takes the UI's requests through its command queue); the UI edits the patterns in place
 * from the main loop. Every field playback reads is a byte
 * or an aligned 32-bit word, so it never sees half a write: at worst a step plays the old
 * value or the new one.
 *
 * Plain C99, no libm, no floats: the host tests run this file exactly as the FM-1 does. */
#pragma once
#include <stdint.h>

#define TB_STEPS 16                    /* steps (and pitches) in a pattern */
#define TB_GROUPS 4                    /* pattern groups I..IV */
#define TB_PER_GROUP 16                /* patterns in a group: 1A..8A, then 1B..8B */
#define TB_NPAT (TB_GROUPS * TB_PER_GROUP)
#define TB_NONE 0xFFu

enum { TM_REST, TM_NOTE, TM_TIE };     /* a time step: 2 bits */
#define TP_ACCENT 0x01u                /* pitch flags */
#define TP_SLIDE 0x02u

#define TB_NOTE_LO 12                  /* the notes a pattern may hold (MIDI): C0 .. C6 */
#define TB_NOTE_HI 84
#define TB_NOTE_DEF 36                 /* C2: the 303's "C" with neither UP nor DOWN */

typedef struct {
    uint8_t note[TB_STEPS];            /* the pitch list: MIDI notes */
    uint8_t flag[TB_STEPS];            /* TP_ACCENT | TP_SLIDE, per pitch */
    uint32_t time;                     /* the time line: step s in bits 2s, 2s+1 (TM_*) */
    uint8_t len;                       /* steps played, 1..16 */
    uint8_t triplet;                   /* 0: sixteenths (6 pulses a step), 1: triplets (8) */
    uint8_t rsv[2];
} tb_pattern_t;                        /* 40 bytes: 64 of them fit one flash object */

static inline int tb_time(const tb_pattern_t *p, int s) { return (int)((p->time >> (2 * s)) & 3u); }
static inline void tb_set_time(tb_pattern_t *p, int s, int t)
{
    uint32_t sh = (uint32_t)(2 * s);
    p->time ^= ((((p->time >> sh) & 3u) ^ ((uint32_t)t & 3u)) << sh);   /* (no ~mask: see tools/build.py) */
}

/* what each step plays, the time and pitch lists put together (for the screen and the tests) */
typedef struct {
    uint8_t time;                      /* TM_* */
    uint8_t pitch;                     /* index into the pitch list: a NOTE's own, a TIE's held note; TB_NONE */
    uint8_t note;                      /* the MIDI note sounding (TB_NONE when silent) */
    uint8_t flag;                      /* that note's TP_* */
} tb_step_t;

void tb_clear(tb_pattern_t *p);        /* the 303's cleared pattern: 16 x C, every step a NOTE */
int tb_valid(const tb_pattern_t *p);   /* 1: every value in range (a pattern from flash) */
int tb_resolve(const tb_pattern_t *p, tb_step_t out[TB_STEPS]);   /* returns the NOTE steps in it */
int tb_notes_used(const tb_pattern_t *p);   /* pitches the time line plays (its NOTE steps up to len) */
int tb_factory_count(void);           /* the built-in patterns (group I, from 1A) */
void tb_factory(int i, tb_pattern_t *p);

/* name of a pattern slot: "I 1A" .. "IV 8B" (8 bytes) */
void tb_slot_name(int slot, char *b);
/* a note as the screen shows it: "C2", "F#3" (5 bytes) */
void tb_note_name(int note, char *b);

/* ------------------------------------------------------------------ playback --- */

/* where playback sends what it does (the engine: the voice and MIDI out) */
typedef struct {
    void (*note_on)(void *ctx, int note, int accent, int slide);   /* slide: legato, no attack */
    void (*note_off)(void *ctx);
    void (*midi)(void *ctx, uint32_t st, uint32_t d1, uint32_t d2);   /* 0: no MIDI */
    void *ctx;
} tbs_sink_t;

typedef struct {
    tb_pattern_t *bank;                /* TB_NPAT patterns (the project's) */
    uint8_t playing;
    uint8_t cur;                       /* the pattern playing */
    uint8_t lo, hi;                    /* the chain it is part of: lo..hi play in turn (lo == hi: one) */
    uint8_t cue_lo, cue_hi;            /* the next chain, from the end of this pattern (TB_NONE: none) */
    uint8_t step;                      /* the step playing (or next, before the first pulse) */
    uint8_t pidx;                      /* the next pitch a NOTE takes */
    uint8_t pulse;                     /* clock pulse within the step */
    uint8_t gate;                      /* the voice's gate is up */
    uint8_t held_note, held_flag;      /* the note sounding (transposed) and its TP_* */
    int8_t transpose;                  /* semitones, applied as notes start */
    uint8_t midi_ch;                   /* notes out on this channel 0..15, TB_NONE: none */
    uint8_t midi_note;                 /* the note out held, TB_NONE: none */
    uint32_t loops;                    /* patterns completed since the start (the UI's bar counter) */
} tbs_t;

void tbs_init(tbs_t *s, tb_pattern_t *bank);
/* the pattern, or chain lo..hi, to play: at once when stopped, from the end of the pattern
 * playing when playing (as the 303 changes pattern) */
void tbs_select(tbs_t *s, int lo, int hi);
void tbs_start(tbs_t *s, const tbs_sink_t *o);        /* from the top of the chain */
void tbs_continue(tbs_t *s, const tbs_sink_t *o);     /* from where it stopped */
void tbs_stop(tbs_t *s, const tbs_sink_t *o);         /* the gate falls */
void tbs_pulse(tbs_t *s, const tbs_sink_t *o);        /* one clock pulse (24 a quarter note) */
int tbs_pulses_per_step(const tbs_t *s);              /* of the pattern playing: 6 or 8 */
int tbs_next(const tbs_t *s);                         /* the pattern that follows the one playing */
