/* SPDX-License-Identifier: GPL-3.0-only */
/* FM-303, the instrument: see fm303.h. */
#include "fm303.h"
#include "bass303.h"
#include "fastmath.h"

#define FM_BARRIER() __asm__ volatile("" ::: "memory")
#define BLK 256u                                    /* frames per chunk (the voice's maximum) */

/* ------------------------------------------------------------- parameters --- */
static const char *const WAVE_N[] = {"Saw", "Square"};
static const char *const DRV_N[] = {"Off", "Soft", "RAT"};
static const char *const ONOFF_N[] = {"Off", "On"};

/* The voice's pots are 0..127 (bass303.c maps them to physical ranges). The factory sound is a
 * classic acid setting: cutoff low, resonance high, plenty of env mod, a short decay. */
static const fm_param_t PARAMS[P_NPARAMS] = {
    {"Cutoff", 0, 127, 36},
    {"Reso", 0, 127, 100},
    {"EnvMod", 0, 127, 80},
    {"Decay", 0, 127, 40},
    {"Accent", 0, 127, 90},
    {"Wave", 0, 1, 0},
    {"Tune", 0, 127, 64},
    {"Volume", 0, 127, 104},
    {"Drive", 0, 127, 0},
    {"DrvType", 0, 2, 1},
    {"Slide", 0, 127, 21},
    {"AccDec", 0, 127, 7},
    {"Tempo", 40, 300, 125},
    {"MIDI ch", 1, 16, 1},
    {"NoteOut", 0, 1, 1},
    {"ClkOut", 0, 1, 1},
};

const fm_param_t *fm_param_info(int i) { return &PARAMS[(unsigned)i < P_NPARAMS ? i : 0]; }

static char *put_s(char *b, const char *s)
{
    while (*s)
        *b++ = *s++;
    *b = 0;
    return b;
}

static char *put_i(char *b, int v)
{
    char t[12];
    int n = 0;
    if (v < 0) {
        *b++ = '-';
        v = -v;
    }
    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        *b++ = t[--n];
    *b = 0;
    return b;
}

static int round_i(float x) { return (int)(x < 0.0f ? x - 0.5f : x + 0.5f); }

/* the physical value a pot stands for (bass303_set's mappings) */
void fm_param_text(int i, int v, char *b)
{
    float n = (float)v * (1.0f / 127.0f);
    switch (i) {
    case P_CUTOFF: {
        int hz = round_i(314.0f * fm_exp2f(n * 2.930586688f));
        if (hz >= 1000) {                           /* "1.25k" */
            b = put_i(b, hz / 1000);
            *b++ = '.';
            *b++ = (char)('0' + hz / 100 % 10);
            *b++ = (char)('0' + hz / 10 % 10);
            put_s(b, "k");
        } else {
            put_s(put_i(b, hz), "Hz");
        }
        break;
    }
    case P_DECAY:
        put_s(put_i(b, round_i(200.0f * fm_exp2f(n * 3.321928095f))), "ms");
        break;
    case P_RESO: case P_ENVMOD: case P_ACCENT: case P_DRIVE:
        put_s(put_i(b, round_i(n * 100.0f)), "%");
        break;
    case P_WAVE:
        put_s(b, WAVE_N[v & 1]);
        break;
    case P_TUNE: {                                  /* A4 = 440 + (v - 64) * 0.625 Hz, in cents */
        int c = round_i(1200.0f * fm_log2f((440.0f + (float)(v - 64) * 0.625f) * (1.0f / 440.0f)));
        if (c > 0)
            *b++ = '+';
        put_s(put_i(b, c), "c");
        break;
    }
    case P_VOLUME:
        put_s(put_i(b, round_i(-60.0f + 60.0f * n)), "dB");
        break;
    case P_DRVTYPE:
        put_s(b, DRV_N[v < 0 ? 0 : v > 2 ? 2 : v]);
        break;
    case P_SLIDE:
        put_s(put_i(b, round_i(2.0f + 358.0f * n)), "ms");
        break;
    case P_ACCDEC:
        put_s(put_i(b, round_i(30.0f + 2970.0f * n)), "ms");
        break;
    case P_NOTEOUT: case P_CLKOUT:
        put_s(b, ONOFF_N[v & 1]);
        break;
    default:
        put_i(b, v);
        break;
    }
}

/* -------------------------------------------------------------- commands --- */
enum { C_SET, C_PLAY, C_SELECT, C_TRANSPOSE, C_NOTE, C_NOTEOFF, C_PANIC, C_CLOCK };
#define QN 64u
static uint32_t q[QN];
static volatile uint32_t q_w, q_r;

static void post(uint32_t c, int a, int b)
{
    uint32_t w = q_w;
    if (w - q_r >= QN)
        return;                                     /* full: dropped (never, at the UI's rate) */
    q[w % QN] = c << 24 | ((uint32_t)a & 0xFFu) << 16 | ((uint32_t)b & 0xFFFFu);
    FM_BARRIER();
    q_w = w + 1;
}

void fm_set(int p, int v) { post(C_SET, p, v); }
void fm_play(int on) { post(C_PLAY, on, 0); }
void fm_select(int lo, int hi) { post(C_SELECT, lo, hi); }
void fm_transpose(int st) { post(C_TRANSPOSE, 0, st); }
void fm_note(int note, int accent, int slide) { post(C_NOTE, note, (accent != 0) | (slide != 0) << 1); }
void fm_note_off(void) { post(C_NOTEOFF, 0, 0); }
void fm_panic(void) { post(C_PANIC, 0, 0); }
void fm_clock(int msg) { post(C_CLOCK, msg, 0); }

/* ----------------------------------------------------------------- state --- */
volatile uint8_t fm_playing, fm_cur, fm_lo, fm_hi, fm_cue = TB_NONE, fm_step, fm_pulse, fm_gate, fm_note_now, fm_ext;
volatile uint32_t fm_loops;
volatile float fm_level;

static bass303_t voice;
static tbs_t seq;
static int16_t par[P_NPARAMS];
static float pulse_len = 1.0f, pulse_left;          /* samples a clock pulse; to the next one */
static uint32_t ext_age;                            /* samples since the last clock in */
static uint8_t live_gate, live_note = TB_NONE;      /* a note played by hand */
static uint8_t shown_step;                          /* the step last fired (the screen's) */
static float buf[BLK];

/* the sequencer's sink: the voice, and MIDI out */
static void s_note_on(void *ctx, int note, int accent, int slide)
{
    (void)ctx;
    bass303_note_on(&voice, note, accent, slide);
    fm_note_now = (uint8_t)note;
}
static void s_note_off(void *ctx)
{
    (void)ctx;
    bass303_note_off(&voice);
}
static void s_midi(void *ctx, uint32_t st, uint32_t d1, uint32_t d2)
{
    (void)ctx;
    fm_midi_out(st, d1, d2);
}
static const tbs_sink_t SINK = {s_note_on, s_note_off, s_midi, 0};

static void notes_out_ch(void)
{
    uint8_t ch = par[P_NOTEOUT] ? (uint8_t)(par[P_MIDICH] - 1) : TB_NONE;
    if (ch == seq.midi_ch)
        return;
    if (seq.midi_note != TB_NONE && seq.midi_ch != TB_NONE) {   /* the note held out ends on its own channel */
        fm_midi_out(0x80u | seq.midi_ch, seq.midi_note, 0);
        seq.midi_note = TB_NONE;
    }
    seq.midi_ch = ch;
}

static void apply(int i, int v)
{
    const fm_param_t *p;
    if ((unsigned)i >= P_NPARAMS)
        return;
    p = &PARAMS[i];
    v = v < p->lo ? p->lo : v > p->hi ? p->hi : v;
    par[i] = (int16_t)v;
    if (i < FM_NVOICE)
        bass303_set(&voice, i, v);
    else if (i == P_TEMPO)
        pulse_len = (float)FM_SR * 60.0f / ((float)v * 24.0f);
    else if (i == P_MIDICH || i == P_NOTEOUT)
        notes_out_ch();
}

static int clock_out(void) { return par[P_CLKOUT] && !fm_ext; }

static void pulse(void)
{
    if (seq.pulse == 0)
        shown_step = seq.step;
    tbs_pulse(&seq, &SINK);
    if (clock_out())
        fm_midi_out(0xF8, 0, 0);
}

static void start(int from_top)
{
    if (live_gate) {                                /* the sequencer takes the voice */
        live_gate = 0;
        bass303_note_off(&voice);
    }
    if (from_top)
        tbs_start(&seq, &SINK);
    else
        tbs_continue(&seq, &SINK);
    pulse_left = 0.0f;
    if (clock_out())
        fm_midi_out(from_top ? 0xFA : 0xFB, 0, 0);
}

static void stop(void)
{
    tbs_stop(&seq, &SINK);
    if (clock_out())
        fm_midi_out(0xFC, 0, 0);
}

static void live_on(int note, int flags)
{
    int ch = par[P_MIDICH] - 1;
    uint8_t old = live_note;
    bass303_note_on(&voice, note, flags & 1, (flags & 2) && live_gate);
    live_gate = 1;
    live_note = (uint8_t)note;
    fm_note_now = (uint8_t)note;
    if (par[P_NOTEOUT]) {                           /* the keyboard plays MIDI out too */
        fm_midi_out(0x90u | (uint32_t)ch, (uint32_t)note, (flags & 1) ? 127u : 96u);
        if (old != TB_NONE && old != note)
            fm_midi_out(0x80u | (uint32_t)ch, old, 0);
    }
}

static void live_off(void)
{
    if (!live_gate)
        return;
    bass303_note_off(&voice);
    live_gate = 0;
    if (par[P_NOTEOUT] && live_note != TB_NONE)
        fm_midi_out(0x80u | (uint32_t)(par[P_MIDICH] - 1), live_note, 0);
    live_note = TB_NONE;
}

static void drain(void)
{
    while (q_r != q_w) {
        uint32_t c = q[q_r % QN];
        int a = (int)(c >> 16 & 0xFFu), b = (int)(int16_t)(c & 0xFFFFu);
        FM_BARRIER();
        q_r++;
        switch (c >> 24) {
        case C_SET:
            apply(a, b);
            break;
        case C_PLAY:
            if (a && !seq.playing)
                start(1);
            else if (!a && seq.playing)
                stop();
            break;
        case C_SELECT:
            tbs_select(&seq, a, b);
            break;
        case C_TRANSPOSE:
            seq.transpose = (int8_t)(b < -24 ? -24 : b > 24 ? 24 : b);
            break;
        case C_NOTE:
            if (!seq.playing)                       /* while the sequence plays it has the voice */
                live_on(a, b);
            break;
        case C_NOTEOFF:
            live_off();
            break;
        case C_CLOCK:
            fm_ext = 1;                             /* clock in: someone else leads */
            ext_age = 0;
            if (a == FM_CLK_TICK) {
                if (seq.playing)
                    pulse();
            } else if (a == FM_CLK_START) {
                start(1);
            } else if (a == FM_CLK_CONTINUE) {
                if (!seq.playing)
                    start(0);
            } else if (seq.playing) {
                stop();
            }
            break;
        case C_PANIC:
            live_off();
            tbs_stop(&seq, &SINK);
            bass303_all_off(&voice);
            break;
        }
    }
}

void fm_init(tb_pattern_t *bank)
{
    int i;
    q_r = q_w = 0;
    bass303_init(&voice);
    tbs_init(&seq, bank);
    fm_ext = 0;
    for (i = 0; i < P_NPARAMS; i++)
        apply(i, PARAMS[i].def);
}

void fm_render(int32_t *out, uint32_t n, uint32_t gain_q12)
{
    const float gain = (float)gain_q12 * (1.0f / 4096.0f) * 2.0f;
    float peak = 0.0f;
    drain();
    if (fm_ext) {
        ext_age += n;
        if (ext_age > FM_SR / 2u) {                 /* no tick for 0.5 s: back to the own tempo */
            fm_ext = 0;
            pulse_left = 0.0f;
        }
    }
    while (n) {
        uint32_t k = n > BLK ? BLK : n, j;
        if (seq.playing && !fm_ext) {               /* the own clock: chunks end on the pulses */
            uint32_t kk;
            if (pulse_left <= 0.0f) {
                pulse();
                pulse_left += pulse_len;
            }
            kk = (uint32_t)pulse_left;
            if ((float)kk < pulse_left)
                kk++;
            if (kk < k)
                k = kk ? kk : 1u;
            pulse_left -= (float)k;
        }
        bass303_render(&voice, buf, (int)k);
        for (j = 0; j < k; j++) {
            float x = fm_tanhf(buf[j] * gain);      /* the soft clipper: loud is fine, harsh is not */
            int32_t s = (int32_t)(x * 8300000.0f);
            peak = fm_maxf(peak, fm_fabsf(x));
            out[2u * j] = s;
            out[2u * j + 1u] = s;
        }
        out += 2u * k;
        n -= k;
    }
    fm_playing = seq.playing;
    fm_cur = seq.cur;
    fm_lo = seq.lo;
    fm_hi = seq.hi;
    fm_cue = seq.cue_lo;
    fm_step = shown_step;
    fm_pulse = seq.pulse;
    fm_gate = (uint8_t)(seq.gate || live_gate);
    fm_loops = seq.loops;
    fm_level = peak;
}
