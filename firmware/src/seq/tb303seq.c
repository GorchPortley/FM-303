/* SPDX-License-Identifier: GPL-3.0-only */
/* FM-303 sequencer: see tb303seq.h. */
#include "tb303seq.h"

/* ------------------------------------------------------------------ patterns --- */

void tb_clear(tb_pattern_t *p)
{
    int s;
    for (s = 0; s < TB_STEPS; s++) {
        p->note[s] = TB_NOTE_DEF;
        p->flag[s] = 0;
    }
    p->time = 0x55555555u;                         /* every step TM_NOTE (01) */
    p->len = TB_STEPS;
    p->triplet = 0;
    p->rsv[0] = p->rsv[1] = 0;
}

int tb_valid(const tb_pattern_t *p)
{
    int s;
    if (p->len < 1 || p->len > TB_STEPS || p->triplet > 1)
        return 0;
    for (s = 0; s < TB_STEPS; s++) {
        if (p->note[s] < TB_NOTE_LO || p->note[s] > TB_NOTE_HI || (p->flag[s] & ~(TP_ACCENT | TP_SLIDE)))
            return 0;
        if (tb_time(p, s) > TM_TIE)
            return 0;
    }
    return 1;
}

int tb_resolve(const tb_pattern_t *p, tb_step_t out[TB_STEPS])
{
    int s, pi = 0, held = -1, notes = 0;
    for (s = 0; s < TB_STEPS; s++) {
        tb_step_t *o = &out[s];
        o->time = (uint8_t)tb_time(p, s);
        o->pitch = o->note = TB_NONE;
        o->flag = 0;
        if (s >= p->len) {                         /* past the end: not played */
            continue;
        }
        if (o->time == TM_NOTE) {
            held = pi++;
            notes++;
        } else if (o->time == TM_REST) {
            held = -1;
        }                                          /* a TIE keeps the note before it (none: silent) */
        if (held >= 0) {
            o->pitch = (uint8_t)held;
            o->note = p->note[held];
            o->flag = p->flag[held];
        }
    }
    return notes;
}

int tb_notes_used(const tb_pattern_t *p)
{
    int s, n = 0;
    for (s = 0; s < p->len && s < TB_STEPS; s++)
        n += tb_time(p, s) == TM_NOTE;
    return n;
}

void tb_slot_name(int slot, char *b)
{
    static const char *const G[TB_GROUPS] = {"I", "II", "III", "IV"};
    const char *g = G[(slot / TB_PER_GROUP) & 3];
    int k = slot % TB_PER_GROUP;
    while (*g)
        *b++ = *g++;
    *b++ = ' ';
    *b++ = (char)('1' + (k & 7));
    *b++ = k < 8 ? 'A' : 'B';
    *b = 0;
}

void tb_note_name(int note, char *b)
{
    static const char *const N[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const char *n;
    int oct;
    if (note < 0 || note > 127) {
        b[0] = '-';
        b[1] = 0;
        return;
    }
    n = N[note % 12];
    oct = note / 12 - 1;                           /* MIDI 60 = C4 */
    while (*n)
        *b++ = *n++;
    if (oct < 0) {
        *b++ = '-';
        oct = -oct;
    }
    *b++ = (char)('0' + oct);
    *b = 0;
}

/* Factory patterns, written for FM-303 (group I, 1A..8A): a time line ('x' note, '~' tie, '.' rest;
 * its length is the pattern's) and the pitch list, one entry per NOTE: a name and octave, then
 * 'a' for accent, 's' for slide. */
static const struct {
    const char *time, *pitch;
} FACTORY[] = {
    /* 1A: sixteen notes, the octave jumping */
    {"xxxxxxxxxxxxxxxx", "A1a A1 A2s A1 C2 A1 G2a A1 A1 D#2 A1 E2s A2 A1 C2a G1"},
    /* 2A: rests and ties: the line breathes */
    {"x.xx~x.xxx.x~xx.", "C2a C2 C3s G2 C2 D#2a F2 C2s A#1 C2a G1"},
    /* 3A: a slow slide down */
    {"x~x.x~xxx~x.xxxx", "E2as D2s C2 E1a E2 G2s E2 D2 B1a E1 E2s"},
    /* 4A: off-beat accents */
    {".x.x.x.xxx.x.x.x", "F1 F2a F1 D#2a F1 G#1s F2 F1 C2a F1 F2a"},
    /* 5A: in triplets: 12 steps, 8 pulses each */
    {"xxxxxx~xxxxx", "G1a G2 G1 D2s F2 G1 G1a A#1 G1 G2as F2"},
    /* 6A: chromatic squelch */
    {"xx~xxxx.xx~xxx.x", "C2a C#2s D2 D#2 C2 C3as B2 C2 C2a A#1s B1 C2a"},
    /* 7A: two-note bounce, all the slides */
    {"x~xxx~xxx~xxx~xx", "D2s D3 D2s D3a D2s D3 D2s D3a D2s D3 D2s A2a"},
    /* 8A: a riff in eight */
    {"xx.xx.x.", "A1a C2s E2 A1 G2a E2"},
};
#define NFACTORY ((int)(sizeof FACTORY / sizeof FACTORY[0]))

static int parse_note(const char **ps, uint8_t *note, uint8_t *flag)
{
    static const int8_t PC[7] = {9, 11, 0, 2, 4, 5, 7};   /* A B C D E F G */
    const char *s = *ps;
    int n, oct = 0, neg = 0;
    while (*s == ' ')
        s++;
    if (*s < 'A' || *s > 'G')
        return 0;
    n = PC[*s++ - 'A'];
    if (*s == '#') {
        n++;
        s++;
    }
    if (*s == '-') {
        neg = 1;
        s++;
    }
    while (*s >= '0' && *s <= '9')
        oct = oct * 10 + (*s++ - '0');
    if (neg)
        oct = -oct;
    n += 12 * (oct + 1);
    *flag = 0;
    for (; *s == 'a' || *s == 's'; s++)
        *flag |= *s == 'a' ? TP_ACCENT : TP_SLIDE;
    *note = (uint8_t)(n < TB_NOTE_LO ? TB_NOTE_LO : n > TB_NOTE_HI ? TB_NOTE_HI : n);
    *ps = s;
    return 1;
}

int tb_factory_count(void) { return NFACTORY; }

void tb_factory(int i, tb_pattern_t *p)
{
    const char *t, *q;
    int s, k = 0;
    tb_clear(p);
    if (i < 0 || i >= NFACTORY)
        return;
    t = FACTORY[i].time;
    for (s = 0; t[s] && s < TB_STEPS; s++)
        tb_set_time(p, s, t[s] == 'x' ? TM_NOTE : t[s] == '~' ? TM_TIE : TM_REST);
    p->len = (uint8_t)s;
    p->triplet = (uint8_t)(i == 4);                /* 5A is the triplet one */
    q = FACTORY[i].pitch;
    while (k < TB_STEPS && parse_note(&q, &p->note[k], &p->flag[k]))
        k++;
}

/* ------------------------------------------------------------------ playback --- */

static void midi(const tbs_sink_t *o, uint32_t st, uint32_t d1, uint32_t d2)
{
    if (o->midi)
        o->midi(o->ctx, st, d1, d2);
}

static void gate_off(tbs_t *s, const tbs_sink_t *o)
{
    if (s->gate) {
        o->note_off(o->ctx);
        s->gate = 0;
    }
    if (s->midi_note != TB_NONE) {
        if (s->midi_ch != TB_NONE)
            midi(o, 0x80u | s->midi_ch, s->midi_note, 0);
        s->midi_note = TB_NONE;
    }
}

static void note_on(tbs_t *s, int note, int flag, const tbs_sink_t *o)
{
    int legato = s->gate && (s->held_flag & TP_SLIDE);   /* the note before slides into this one */
    uint8_t old = s->midi_note;
    note += s->transpose;
    note = note < 0 ? 0 : note > 127 ? 127 : note;
    o->note_on(o->ctx, note, (flag & TP_ACCENT) != 0, legato);
    s->gate = 1;
    s->held_note = (uint8_t)note;
    s->held_flag = (uint8_t)flag;
    if (s->midi_ch != TB_NONE) {                   /* legato over MIDI: the new note before the old one ends */
        midi(o, 0x90u | s->midi_ch, (uint32_t)note, (flag & TP_ACCENT) ? 127u : 96u);
        if (old != TB_NONE && old != note)
            midi(o, 0x80u | s->midi_ch, old, 0);
        s->midi_note = (uint8_t)note;
    } else {
        s->midi_note = TB_NONE;
    }
}

void tbs_init(tbs_t *s, tb_pattern_t *bank)
{
    uint8_t *b = (uint8_t *)s;
    unsigned k;
    for (k = 0; k < sizeof *s; k++)
        b[k] = 0;
    s->bank = bank;
    s->cue_lo = s->cue_hi = TB_NONE;
    s->midi_ch = TB_NONE;
    s->midi_note = TB_NONE;
}

static const tb_pattern_t *cur(const tbs_t *s) { return &s->bank[s->cur]; }

int tbs_pulses_per_step(const tbs_t *s) { return cur(s)->triplet ? 8 : 6; }

int tbs_next(const tbs_t *s)
{
    if (s->cue_lo != TB_NONE)
        return s->cue_lo;
    return s->cur >= s->hi || s->cur < s->lo ? s->lo : s->cur + 1;
}

void tbs_select(tbs_t *s, int lo, int hi)
{
    if (lo < 0 || lo >= TB_NPAT)
        return;
    if (hi < lo || hi >= TB_NPAT)
        hi = lo;
    if (s->playing) {
        s->cue_lo = (uint8_t)lo;
        s->cue_hi = (uint8_t)hi;
    } else {
        s->lo = (uint8_t)lo;
        s->hi = (uint8_t)hi;
        s->cur = (uint8_t)lo;
        s->cue_lo = s->cue_hi = TB_NONE;
    }
}

void tbs_start(tbs_t *s, const tbs_sink_t *o)
{
    gate_off(s, o);
    if (s->cue_lo != TB_NONE) {                    /* a cue left over: it is what starts */
        s->lo = s->cue_lo;
        s->hi = s->cue_hi;
        s->cue_lo = s->cue_hi = TB_NONE;
    }
    s->cur = s->lo;
    s->step = s->pidx = s->pulse = 0;
    s->loops = 0;
    s->playing = 1;
}

void tbs_continue(tbs_t *s, const tbs_sink_t *o)
{
    (void)o;
    s->playing = 1;
}

void tbs_stop(tbs_t *s, const tbs_sink_t *o)
{
    s->playing = 0;
    gate_off(s, o);
}

/* the time value of the step after the one playing: across the end of the pattern, the first
 * step of the next one (the cue as it stands now) */
static int next_time(const tbs_t *s)
{
    const tb_pattern_t *p = cur(s);
    if (s->step + 1 < p->len)
        return tb_time(p, s->step + 1);
    return tb_time(&s->bank[tbs_next(s)], 0);
}

static void fire_step(tbs_t *s, const tbs_sink_t *o)
{
    const tb_pattern_t *p = cur(s);
    int t = tb_time(p, s->step);
    if (t == TM_NOTE) {
        int k = s->pidx < TB_STEPS ? s->pidx : TB_STEPS - 1;
        s->pidx++;
        note_on(s, p->note[k], p->flag[k], o);
    } else if (t == TM_REST) {
        gate_off(s, o);                            /* a slide into a rest: the note ends here */
    }                                              /* TM_TIE: what sounds goes on (nothing: silence) */
}

/* halfway through the step: the gate falls, unless the note slides on or the next step ties it */
static void half_step(tbs_t *s, const tbs_sink_t *o)
{
    if (!s->gate || (s->held_flag & TP_SLIDE) || next_time(s) == TM_TIE)
        return;
    gate_off(s, o);
}

static void advance(tbs_t *s)
{
    if (s->step + 1 < cur(s)->len) {
        s->step++;
        return;
    }
    s->step = 0;                                   /* the end of the pattern: the next one */
    s->pidx = 0;
    s->loops++;
    if (s->cue_lo != TB_NONE) {
        s->lo = s->cue_lo;
        s->hi = s->cue_hi;
        s->cue_lo = s->cue_hi = TB_NONE;
        s->cur = s->lo;
    } else {
        s->cur = (uint8_t)tbs_next(s);
    }
}

void tbs_pulse(tbs_t *s, const tbs_sink_t *o)
{
    int pps;
    if (!s->playing)
        return;
    pps = tbs_pulses_per_step(s);
    if (s->pulse >= pps)                           /* the pattern became sixteenths mid-step */
        s->pulse = 0;
    if (s->pulse == 0)
        fire_step(s, o);
    if (s->pulse == pps / 2)
        half_step(s, o);
    if (++s->pulse >= pps) {
        s->pulse = 0;
        advance(s);
    }
}
