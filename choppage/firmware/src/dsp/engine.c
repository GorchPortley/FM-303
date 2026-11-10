/* SPDX-License-Identifier: GPL-3.0-only */
/* CHOPPAGE, the instrument: see chp.h. */
#include "chp.h"
#include "gen.h"
#include "fastmath.h"

#define BARRIER() __asm__ volatile("" ::: "memory")
#define CHUNK 32u                                   /* frames between parameter updates, at most */
#define ATK (1.0f / 24.0f)                          /* a voice fades in over 0.5 ms (no click mid-wave) */
#define ATK_SLOW (1.0f / 96.0f)                     /* coming back in after an override: 2 ms */
#define REL (1.0f / 96.0f)                          /* and out over 2 ms when cut */
#define FSR ((float)CHP_SR)

const uint8_t CHP_ROLL_N[8] = {1, 2, 3, 4, 6, 8, 12, 16};
const char *const CHP_STYLE_NAME[NSTYLES] = {"Straight", "Jungle", "Roller", "Halftime", "Breakcore", "Tracker"};
const char *const CHP_TAG_NAME[NTAGS] = {"-", "K", "S", "H", "G"};
const char *const CHP_PERF_NAME[NPERF] = {"1/8", "1/16", "1/32", "1/64", "1/24", "REV", "TAPE", "OCT-",
                                          "BEAT", "GATE", "SCAT"};

/* --------------------------------------------------------------- parameters --- */
static const chp_param_t PARAMS[P_NPARAMS] = {
    {"Tempo", 40, 300, 165},
    {"Pitch", -24, 24, 0},
    {"Filter", -64, 64, 0},
    {"Crush", 0, 100, 0},
    {"Auto", 0, 100, 0},
    {"Reso", 0, 100, 30},
    {"Drive", 0, 100, 0},
    {"Shape", 0, 2, 0},
    {"Interp", 0, 1, 0},
    {"Amiga", 0, 3, 0},
    {"Echo", 0, 100, 0},
    {"Time", 0, 5, 2},
    {"Feedbk", 0, 100, 45},
    {"Swing", 50, 75, 50},
    {"Sync", 0, 1, 1},
    {"Pool", 0, 1, 0},
    {"Style", 0, NSTYLES - 1, GS_JUNGLE},
    {"Density", 0, 100, 50},
    {"Chaos", 0, 100, 30},
    {"Fill", 0, 3, 2},
    {"Vary", 0, 100, 40},
    {"Ramp", -12, 12, 0},
    {"Quant", 0, 2, 1},
    {"Keys", 0, 1, 0},
    {"Rec as", 0, 1, 0},
    {"MIDI ch", 1, 16, 1},
    {"ClkOut", 0, 1, 1},
    {"NoteOut", 0, 1, 0},
    {"Volume", -24, 6, 0},
};

const chp_param_t *chp_param_info(int i) { return &PARAMS[(unsigned)i < P_NPARAMS ? i : 0]; }

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

static char *put_hz(char *b, float hz)
{
    int h = round_i(hz);
    if (h >= 1000) {                                /* "1.2k" */
        b = put_i(b, h / 1000);
        *b++ = '.';
        *b++ = (char)('0' + h / 100 % 10);
        return put_s(b, "k");
    }
    return put_i(b, h);
}

/* the DJ filter's cutoff: lowpass 18 kHz .. 80 Hz, highpass 20 Hz .. 10 kHz */
static float filter_hz(int f)
{
    return f < 0 ? 18000.0f * fm_exp2f((float)f * (7.8f / 64.0f)) : 20.0f * fm_exp2f((float)f * (9.0f / 64.0f));
}

/* the echo's times, in steps: 1/16, 1/8, 3/16, 1/4, 1/8 triplet, 3/8 */
static const float ECHO_STEPS[6] = {1.0f, 2.0f, 3.0f, 4.0f, 4.0f / 3.0f, 6.0f};

void chp_param_text(int i, int v, char *b)
{
    static const char *const SHAPE_N[] = {"Soft", "Hard", "Fold"};
    static const char *const INTERP_N[] = {"Paula", "Clean"};
    static const char *const AMIGA_N[] = {"Off", "A500", "A500+L", "LED"};
    static const char *const ECHOT_N[] = {"1/16", "1/8", "3/16", "1/4", "1/8T", "3/8"};
    static const char *const STYLE_N[] = {"Straight", "Jungle", "Roller", "Half", "Brkcore", "Tracker"};
    static const char *const FILL_N[] = {"Off", "2 bars", "4 bars", "8 bars"};
    static const char *const QUANT_N[] = {"Off", "1/16", "1/32"};
    static const char *const KEYS_N[] = {"Replace", "Layer"};
    static const char *const RECAS_N[] = {"Slice", "Type"};
    static const char *const POOL_N[] = {"This", "All"};
    static const char *const ONOFF_N[] = {"Off", "On"};
    switch (i) {
    case P_PITCH: case P_RAMP:
        if (v > 0)
            *b++ = '+';
        put_i(b, v);
        break;
    case P_FILTER:
        if (!v)
            put_s(b, "Off");
        else
            put_hz(put_s(b, v < 0 ? "LP " : "HP "), filter_hz(v));
        break;
    case P_CRUSH: case P_AUTO: case P_ECHO:
        if (!v) {
            put_s(b, "Off");
            break;
        }
        /* fall through */
    case P_RESO: case P_DRIVE: case P_ECHOFB: case P_SWING: case P_DENS: case P_CHAOS: case P_VARY:
        put_s(put_i(b, v), "%");
        break;
    case P_DRVTYPE: put_s(b, SHAPE_N[v % 3]); break;
    case P_INTERP: put_s(b, INTERP_N[v & 1]); break;
    case P_AMIGA: put_s(b, AMIGA_N[v & 3]); break;
    case P_ECHOT: put_s(b, ECHOT_N[(unsigned)v % 6u]); break;
    case P_STYLE: put_s(b, STYLE_N[(unsigned)v % NSTYLES]); break;
    case P_FILL: put_s(b, FILL_N[v & 3]); break;
    case P_QUANT: put_s(b, QUANT_N[(unsigned)v % 3u]); break;
    case P_LIVE: put_s(b, KEYS_N[v & 1]); break;
    case P_RECAS: put_s(b, RECAS_N[v & 1]); break;
    case P_POOL: put_s(b, POOL_N[v & 1]); break;
    case P_SYNC: case P_CLKOUT: case P_NOTEOUT: put_s(b, ONOFF_N[v & 1]); break;
    case P_VOLUME:
        if (v > 0)
            *b++ = '+';
        put_s(put_i(b, v), "dB");
        break;
    default:
        put_i(b, v);
        break;
    }
}

/* ------------------------------------------------------------ breaks, steps --- */
float chp_break_bpm(const chp_break_t *b)
{
    if (!b->len || !b->beats)
        return 120.0f;
    return (float)b->beats * 60.0f * (float)b->rate / (float)b->len;
}

uint32_t chp_slice_end(const chp_break_t *b, int s)
{
    return s + 1 < b->nsl ? b->start[s + 1] : b->len;
}

int chp_slice_at(const chp_break_t *b, uint32_t pos)
{
    int lo = 0, hi = b->nsl - 1;
    while (lo < hi) {                               /* the last start <= pos */
        int mid = (lo + hi + 1) / 2;
        if (b->start[mid] <= pos)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

void chp_step_clear(chp_step_t *s)
{
    s->src = SRC_NONE;
    s->pitch = 0;
    s->fx = 0;
    s->lev = 15;
    s->prob = 100;
    s->gate = 0;
}

void chp_pattern_clear(chp_pattern_t *p, int len)
{
    int i;
    p->len = (uint8_t)(len < 1 ? 1 : len > CHP_STEPS ? CHP_STEPS : len);
    p->rsv[0] = p->rsv[1] = p->rsv[2] = 0;
    for (i = 0; i < CHP_STEPS; i++)
        chp_step_clear(&p->st[i]);
}

int chp_pattern_valid(const chp_pattern_t *p)
{
    int i;
    if (p->len < 1 || p->len > CHP_STEPS)
        return 0;
    for (i = 0; i < CHP_STEPS; i++) {
        const chp_step_t *s = &p->st[i];
        if (s->src > SRC_MAX || s->pitch < -24 || s->pitch > 24 || s->lev > 15 || s->prob > 100 || s->gate > 8 ||
            (s->fx & 0xC0u) || ((s->fx & FX_RAMP_UP) && (s->fx & FX_RAMP_DN)))
            return 0;
    }
    return 1;
}

/* ------------------------------------------------------------- the queue --- */
enum { C_SET, C_PLAY, C_PAT, C_BRK, C_SEED, C_LIVE, C_PERF, C_REC, C_AUD, C_HOLD, C_PANIC, C_CLOCK };
typedef struct {
    uint8_t c;
    int32_t a, b, d;
} cmd_t;
#define QN 64u
static cmd_t q[QN];
static volatile uint32_t q_w, q_r;

static void post(int c, int32_t a, int32_t b, int32_t d)
{
    cmd_t *m;
    if (q_w - q_r >= QN)
        return;                                     /* full: the render is stuck; drop */
    m = &q[q_w % QN];
    m->c = (uint8_t)c;
    m->a = a;
    m->b = b;
    m->d = d;
    BARRIER();
    q_w++;
}

void chp_set(int param, int value) { post(C_SET, param, value, 0); }
void chp_play(int on) { post(C_PLAY, on, 0, 0); }
void chp_pattern(int pat) { post(C_PAT, pat, 0, 0); }
void chp_break(int b) { post(C_BRK, b, 0, 0); }
void chp_seed(uint32_t seed) { post(C_SEED, (int32_t)seed, 0, 0); }
void chp_live(int slice, int on, int lev) { post(C_LIVE, slice, on, lev); }
void chp_perf(int fx, int on) { post(C_PERF, fx, on, 0); }
void chp_rec(int on) { post(C_REC, on, 0, 0); }
void chp_audition(int b, int s) { post(C_AUD, b, s, 0); }
void chp_hold(int on) { post(C_HOLD, on, 0, 0); }
void chp_panic(void) { post(C_PANIC, 0, 0, 0); }
void chp_clock(int msg) { post(C_CLOCK, msg, 0, 0); }

/* recorded steps, render -> main loop */
#define RN 32u
static chp_rec_t rq[RN];
static volatile uint32_t rq_w, rq_r;
static void rec_push(int pat, int step, const chp_step_t *st)
{
    if (rq_w - rq_r >= RN)
        return;
    rq[rq_w % RN].pat = (uint8_t)pat;
    rq[rq_w % RN].step = (uint8_t)step;
    rq[rq_w % RN].st = *st;
    BARRIER();
    rq_w++;
}
int chp_rec_take(chp_rec_t *r)
{
    if (rq_r == rq_w)
        return 0;
    BARRIER();
    *r = rq[rq_r % RN];
    BARRIER();
    rq_r++;
    return 1;
}

/* --------------------------------------------------------- published state --- */
volatile uint8_t chp_playing, chp_cur, chp_cue = CHP_NONE, chp_step, chp_mstep, chp_pulse, chp_ext, chp_held;
volatile uint8_t chp_recording, chp_vbrk = CHP_NONE, chp_vslice = CHP_NONE, chp_trig, chp_work_on;
volatile uint32_t chp_bars, chp_vpos;
volatile uint16_t chp_perf_on;
volatile float chp_level, chp_tempo_now = 165.0f;
chp_step_t chp_work[CHP_STEPS];

/* ------------------------------------------------------------------ state --- */
static chp_pattern_t *bank;
static chp_break_t *brks;
static const uint8_t *mem;
static int16_t par[P_NPARAMS];
static float lut[NFMT][256];                         /* 8-bit sample -> float */
static uint8_t cur_brk, held, rec_on;
static uint32_t seed = 0x9E3779B9u, rng = 0x12345678u;

static uint32_t rnd(void)                           /* xorshift32 */
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

/* ----------------------------------------------------------------- voices --- */
typedef struct {
    const uint8_t *d;
    const float *lut;
    uint32_t len;                                   /* the break's length: reads stay inside */
    float pos, inc0;                                /* position; signed increment before the speed */
    float lo, hi;                                   /* it stops outside [lo, hi) */
    float gain, env, env_d;                         /* level; the de-click envelope and its slope */
    uint32_t gate;                                  /* frames until it fades out (0: none) */
    uint8_t on, sync, brk, slice;
} voice_t;

typedef struct {
    voice_t v, t;                                   /* the voice, and the one fading out under it */
} chan_t;
enum { CH_SEQ, CH_LIVE };
static chan_t ch[2];

static float lev_gain(int lev) { return fm_exp2f((float)(lev - 15) * (1.0f / 3.0f)); }   /* -2 dB a step */

/* a voice for slice s of break b: forward from its start or backward from its end, playing on */
static int voice_set(voice_t *v, int b, int s, int rev, float semis, float gain)
{
    const chp_break_t *k;
    uint32_t st, en;
    if (b < 0 || b >= CHP_NBRK || !brks[b].len)
        return 0;
    k = &brks[b];
    if (s < 0 || s >= k->nsl)
        return 0;
    st = k->start[s];
    en = chp_slice_end(k, s);
    if (st >= k->len || en <= st)
        return 0;
    v->d = mem + k->off;
    v->lut = lut[k->fmt < NFMT ? k->fmt : 0];
    v->len = k->len;
    v->sync = (uint8_t)par[P_SYNC];
    v->brk = (uint8_t)b;
    v->slice = (uint8_t)s;
    v->inc0 = (float)k->rate * (1.0f / FSR) * fm_exp2f(semis * (1.0f / 12.0f));
    if (v->sync)
        v->inc0 /= chp_break_bpm(k);                /* times the tempo, in the render */
    v->pos = rev ? (float)(en - 1u) : (float)st;
    if (rev)
        v->inc0 = -v->inc0;
    v->lo = 0.0f;
    v->hi = (float)k->len;
    v->gain = gain;
    v->env = 0.0f;
    v->env_d = ATK;
    v->gate = 0;
    v->on = 1;
    return 1;
}

static void chan_start(chan_t *c, const voice_t *nv)
{
    if (c->v.on) {                                  /* the old one fades out under the new */
        c->t = c->v;
        c->t.env_d = -REL;
        c->t.gate = 0;
    }
    c->v = *nv;
}

static void chan_stop(chan_t *c)
{
    if (c->v.on) {
        c->v.env_d = -REL;
        c->v.gate = 0;
    }
}

/* k frames of a voice added into buf; mul: the speed (tempo factor, pitch, tape) */
static void voice_run(voice_t *v, float *buf, uint32_t k, float mul, int clean)
{
    const uint8_t *d = v->d;
    const float *L = v->lut;
    float pos = v->pos, inc = v->inc0 * mul, env = v->env, ed = v->env_d, g = v->gain, lo = v->lo, hi = v->hi;
    uint32_t j, last = v->len - 1u, gate = v->gate;
    for (j = 0; j < k; j++) {
        uint32_t i;
        float s;
        if (pos < lo || pos >= hi) {
            v->on = 0;
            break;
        }
        i = (uint32_t)pos;
        s = L[d[i]];
        if (clean) {                                /* linear; Paula holds each sample (no interpolation) */
            float s1 = L[d[i < last ? i + 1u : i]];
            s += (pos - (float)i) * (s1 - s);
        }
        buf[j] += s * g * env;
        env += ed;
        if (env >= 1.0f) {
            env = 1.0f;
            ed = 0.0f;
        } else if (env <= 0.0f) {
            v->on = 0;
            break;
        }
        pos += inc;
        if (gate && --gate == 0)
            ed = -REL;
    }
    v->pos = pos;
    v->env = env;
    v->env_d = ed;
    v->gate = gate;
}

/* ------------------------------------------------------------- sequencer --- */
static struct {
    uint8_t playing, pat, cue, pos, ext;
    uint32_t m;                                     /* master steps since the start */
    float ph, len;                                  /* frames into the step; frames a step */
    uint8_t pulse;                                  /* clock pulses sent in this step */
    uint32_t clock;                                 /* frames since the start */
    uint32_t bars, loops;
    const chp_step_t *steps;                        /* this pass: the pattern, or chp_work (AUTO) */
    uint8_t work;
    uint32_t ext_ticks, ext_age;
    float ext_gap, ext_t;                           /* MIDI clock: frames between ticks, since the last */
    float free_ph;                                  /* stopped: a free-running step phase (gate, rolls) */
    uint8_t note_last;
} sq;

static float tempo(void) { return sq.ext ? chp_tempo_now : (float)par[P_TEMPO]; }
static float swing_delay(void) { return (float)(par[P_SWING] - 50) * 0.02f * sq.len; }
static float step_at(uint32_t m) { return (m & 1u) ? swing_delay() : 0.0f; }
static float step_dur(uint32_t m) { return (m & 1u) ? sq.len - swing_delay() : sq.len + swing_delay(); }

/* the nominal speed: the tempo factor (SYNC) and the pitch knob; perform fx come on top */
static float pitch_mul;                              /* 2^(P_PITCH / 12) */
static float perf_mul = 1.0f;                        /* OCT-, tape stop */
static float tape = 1.0f;

/* the shadow: what the sequence would be playing, under the keyboard, a beat repeat, tape stop... */
static struct {
    uint8_t valid;
    voice_t v;
    uint32_t at;                                    /* sq.clock at its trigger */
} shadow;

/* a step's triggers: lane 0 plays the sequence (it updates the shadow; it sounds unless something
 * overrides it), lane 1 replays the steps of a beat repeat */
typedef struct {
    uint8_t pend;                                   /* the step's first trigger waits for `at` */
    uint8_t restore;                                /* lane 1: a beat repeat's restart */
    chp_step_t st;
    uint8_t idx;                                    /* its step in the pattern (a type's stable pick) */
    uint32_t m;                                     /* the master step it belongs to */
    float at;
    uint8_t rn, rk;                                 /* retriggers: how many, the next */
    float rat, rint, ramp;                          /* next at, interval, semitones over the roll */
    voice_t rv;                                     /* the voice as triggered */
} lane_t;
static lane_t lane[2];

static struct {
    int8_t key;                                     /* the slice held, -1: none */
    uint8_t pend, plev, ovr, released;
    int8_t pslice;
    float pleft;                                    /* frames until the quantized trigger */
} live = {-1, 0, 0, 0, 0, -1, 0.0f};

static uint16_t perf;                               /* held perform fx */
static uint8_t ext_start;                           /* MIDI start came: the next tick is step 1 */
static uint8_t rstack[8], rdepth;                   /* repeat keys, in the order pressed */
static const float REP_W[NPERF] = {2.0f, 1.0f, 0.5f, 0.25f, 2.0f / 3.0f, 0, 0, 0, 4.0f, 0, 0};
static struct {
    uint8_t armed, on, fx;
    float W;                                        /* steps */
    float wait, left;                               /* frames until it starts / the next restart */
    uint32_t m0, n;                                 /* the master step it started on; restarts */
    uint8_t pos0;
    chp_step_t st[4];                               /* the steps it replays (a beat or less) */
    voice_t snap[2];                                /* the voices at its start */
} rep;

static int ovr(void) { return live.ovr || rep.on; }

static void note_out(int slice, int lev)
{
    uint32_t c = (uint32_t)(par[P_MIDICH] - 1) & 15u;
    if (!par[P_NOTEOUT])
        return;
    if (sq.note_last)
        chp_midi_out(0x80u | c, sq.note_last, 0);
    sq.note_last = (uint8_t)(36 + slice);
    chp_midi_out(0x90u | c, sq.note_last, (uint32_t)(40 + lev * 5));
}

/* a type's slice: stable for the step (vary 0) or new each time (vary 100), from this break or all */
static uint8_t last_pick[NTAGS];
static int pick(int tag, uint32_t key, int *pb, int *ps)
{
    int n = 0, b, s, k, b0 = par[P_POOL] ? 0 : cur_brk, b1 = par[P_POOL] ? CHP_NBRK - 1 : cur_brk, t = tag;
    for (k = 0; k < 3 && !n; k++) {                 /* none of the tag: a snare for a ghost, ... */
        for (b = b0; b <= b1; b++)
            for (s = 0; s < brks[b].nsl && brks[b].len; s++)
                n += brks[b].tag[s] == t;
        if (!n)
            t = k == 0 ? (tag == TAG_G ? TAG_S : tag == TAG_S ? TAG_G : -1) : -1;
        if (t < 0)
            break;
    }
    if (!n) {                                       /* nothing tagged: any slice of the break */
        const chp_break_t *kb = &brks[cur_brk];
        if (!kb->len || !kb->nsl)
            return 0;
        *pb = cur_brk;
        *ps = (int)(gen_hash(seed, key) % kb->nsl);
        return 1;
    }
    if ((int)(rnd() % 100u) < par[P_VARY]) {
        k = (int)(rnd() % (uint32_t)n);
        if (k == last_pick[tag] && n > 1)           /* a new one, not the same twice */
            k = (k + 1) % n;
    } else {
        k = (int)(gen_hash(seed, key) % (uint32_t)n);
    }
    last_pick[tag] = (uint8_t)k;
    for (b = b0; b <= b1; b++)
        for (s = 0; s < brks[b].nsl && brks[b].len; s++)
            if (brks[b].tag[s] == t && k-- == 0) {
                *pb = b;
                *ps = s;
                return 1;
            }
    return 0;
}

static int resolve(int src, uint32_t key, int *pb, int *ps)
{
    const chp_break_t *k = &brks[cur_brk];
    if (SRC_IS_SLICE(src)) {
        if (!k->len || !k->nsl)
            return 0;
        *pb = cur_brk;
        *ps = (src - SRC_SLICE) % k->nsl;           /* a pattern written for more slices wraps */
        return 1;
    }
    if (SRC_IS_TYPE(src))
        return pick(src, key, pb, ps);
    if (src == SRC_ANY && k->len && k->nsl) {
        *pb = cur_brk;
        *ps = (int)(rnd() % k->nsl);
        return 1;
    }
    return 0;
}

static void flip(voice_t *v) { v->inc0 = -v->inc0; }

/* lane L's next trigger (k = rk) */
static void lane_trig(int L)
{
    lane_t *ln = &lane[L];
    voice_t v = ln->rv;
    if (ln->rk)
        v.inc0 *= fm_exp2f(ln->ramp * (float)ln->rk / (float)ln->rn * (1.0f / 12.0f));
    if (L == 0) {
        shadow.v = v;
        shadow.at = sq.clock;
        shadow.valid = 1;
    }
    if (L == 1 || !ovr()) {
        if (perf & (1u << PF_REV))
            flip(&v);
        chan_start(&ch[CH_SEQ], &v);
        if (ln->rk == 0)
            note_out(v.slice, ln->st.lev);
        chp_trig++;
    }
    ln->rk++;
    if (ln->rk < ln->rn)
        ln->rat += ln->rint;
    else
        ln->rn = 0;
}

/* a step's first trigger, on lane L */
static void lane_fire(int L)
{
    lane_t *ln = &lane[L];
    chp_step_t st = ln->st;
    int b, s, n, rev;
    float dur = step_dur(ln->m);
    ln->pend = 0;
    ln->rn = 0;
    if (ln->restore) {                              /* a beat repeat starts over */
        uint32_t c;
        float sm;
        ln->restore = 0;
        rep.n++;
        sm = (float)rep.n * (float)par[P_RAMP] * rep.W * 0.25f;
        for (c = 0; c < 2u; c++)
            if (rep.snap[c].on) {
                voice_t v = rep.snap[c];
                v.inc0 *= fm_exp2f(sm * (1.0f / 12.0f));
                v.env = 0.0f;
                v.env_d = ATK;
                chan_start(&ch[c], &v);
            } else {
                chan_stop(&ch[c]);
            }
        chp_trig++;
        return;
    }
    if (L == 0 && (perf & (1u << PF_SCATTER))) {    /* scatter: any slice, sometimes rolled, reversed */
        uint32_t r = rnd();
        st.src = SRC_ANY;
        st.fx = (uint8_t)((r & 3u) == 0 ? 1u + (r >> 2) % 4u : 0u);
        if ((r >> 8) % 4u == 0)
            st.fx |= FX_REV;
        st.pitch = (int8_t)((r >> 12) % 6u == 0 ? ((r >> 16) & 1u ? 12 : -12) : 0);
        st.prob = 100;
    }
    if (st.prob < 100 && (int)(rnd() % 100u) >= st.prob)
        return;                                     /* missed: the slice before plays on */
    if (st.src == SRC_NONE)
        return;
    if (st.src == SRC_CUT) {
        if (L == 0)
            shadow.valid = 0;
        if (L == 1 || !ovr())
            chan_stop(&ch[CH_SEQ]);
        return;
    }
    if (!resolve(st.src, gen_hash((uint32_t)sq.pat << 8 | ln->idx, ln->st.src), &b, &s))
        return;
    n = CHP_ROLL_N[st.fx & FX_ROLL];
    rev = (st.fx & FX_REV) != 0;
    if (!voice_set(&ln->rv, b, s, rev, (float)st.pitch, lev_gain(st.lev)))
        return;
    if (st.gate)
        ln->rv.gate = (uint32_t)(dur / (float)n * (float)st.gate * 0.125f) + 1u;
    ln->st = st;
    ln->rn = (uint8_t)n;
    ln->rk = 0;
    ln->rint = dur / (float)n;
    ln->rat = ln->at;
    ln->ramp = (st.fx & FX_RAMP_UP) ? 12.0f : (st.fx & FX_RAMP_DN) ? -12.0f : 0.0f;
    lane_trig(L);
}

/* back in: the sequence takes over where it would be now (after the keyboard, a repeat, ...) */
static void resync(void)
{
    voice_t v;
    float el, mul;
    if (!sq.playing || ovr() || !shadow.valid)
        return;
    v = shadow.v;
    el = (float)(sq.clock - shadow.at);
    mul = (v.sync ? tempo() : 1.0f) * pitch_mul;
    v.pos += el * v.inc0 * mul;
    if (v.pos < v.lo || v.pos >= v.hi)
        return;                                     /* it would have ended */
    if (v.gate) {
        if (el >= (float)v.gate)
            return;
        v.gate -= (uint32_t)el;
    }
    v.env = 0.0f;
    v.env_d = ATK_SLOW;
    if (perf & (1u << PF_REV))
        flip(&v);
    chan_start(&ch[CH_SEQ], &v);
}

static void pass_begin(void)                        /* the top of a pattern: a cue takes over, AUTO varies */
{
    const chp_pattern_t *p;
    int nb, fill = 0, n = par[P_FILL] == 1 ? 2 : par[P_FILL] == 2 ? 4 : par[P_FILL] == 3 ? 8 : 0;
    if (sq.cue != CHP_NONE) {
        sq.pat = sq.cue;
        sq.cue = CHP_NONE;
    }
    p = &bank[sq.pat];
    nb = (p->len + 15) / 16;
    if (n && (sq.bars + (uint32_t)nb) % (uint32_t)n == 0u)
        fill = 1;
    if (par[P_AUTO] > 0 || fill) {
        gen_cfg_t c;
        c.style = (uint8_t)par[P_STYLE];
        c.dens = (uint8_t)par[P_DENS];
        c.chaos = (uint8_t)par[P_CHAOS];
        gen_vary(chp_work, p, &c, &brks[cur_brk], seed ^ (sq.loops * 0x9E3779B1u), par[P_AUTO], fill);
        sq.work = 1;
        sq.steps = chp_work;
    } else {
        sq.work = 0;
        sq.steps = p->st;
    }
}

static int cur_len(void)
{
    int l = bank[sq.pat].len;
    return l < 1 ? 1 : l > CHP_STEPS ? CHP_STEPS : l;
}

static void rec_none_if_held(void)
{
    if (rec_on && live.key >= 0 && !live.pend && par[P_LIVE] == 0) {   /* held over the step: it plays on */
        chp_step_t st;
        chp_step_clear(&st);
        rec_push(sq.pat, sq.pos, &st);
    }
}

/* a new step: the master moves on; lane 0 gets the step, lane 1 the beat repeat's */
static void step_begin(void)
{
    lane_t *l0 = &lane[0];
    l0->pend = 1;
    l0->restore = 0;
    l0->st = sq.steps[sq.pos];
    l0->idx = sq.pos;
    l0->m = sq.m;
    l0->at = step_at(sq.m);
    l0->rn = 0;
    lane[1].rn = 0;
    if (rep.on && rep.W >= 1.0f) {
        uint32_t w = (uint32_t)rep.W, o = (sq.m - rep.m0) % w;
        lane_t *l1 = &lane[1];
        l1->pend = 1;
        l1->m = sq.m;
        l1->at = l0->at;
        l1->restore = o == 0u;
        if (o) {
            l1->st = rep.st[o];
            l1->idx = (uint8_t)((rep.pos0 + o) % (uint32_t)cur_len());
        }
    }
}

static void step_advance(void)
{
    sq.m++;
    sq.pos++;
    sq.pulse = 0;
    if (sq.pos >= cur_len()) {
        sq.pos = 0;
        sq.loops++;
        sq.bars += (uint32_t)(cur_len() + 15) / 16u;
        pass_begin();
    }
    rec_none_if_held();
    step_begin();
}

static void start(void)
{
    sq.playing = 1;
    sq.m = 0;
    sq.pos = 0;
    sq.ph = 0.0f;
    sq.pulse = 0;
    sq.clock = 0;
    sq.bars = 0;
    sq.loops = 0;
    sq.ext_ticks = 0;
    shadow.valid = 0;
    lane[0].pend = lane[1].pend = 0;
    lane[0].rn = lane[1].rn = 0;
    pass_begin();
    step_begin();
    if (par[P_CLKOUT] && !sq.ext) {
        chp_midi_out(0xFA, 0, 0);
        chp_midi_out(0xF8, 0, 0);
    }
    if (rep.armed || rep.on) {                      /* a repeat key held at the start: from the top */
        rep.on = 0;
        rep.armed = 1;
        rep.wait = 0.0f;
    }
}

static void stop(void)
{
    sq.playing = 0;
    lane[0].pend = lane[1].pend = 0;
    lane[0].rn = lane[1].rn = 0;
    chan_stop(&ch[CH_SEQ]);
    shadow.valid = 0;
    if (rep.on) {
        rep.on = 0;
        rep.armed = rdepth > 0;
        rep.wait = 0.0f;
    }
    if (par[P_CLKOUT] && !sq.ext)
        chp_midi_out(0xFC, 0, 0);
    if (sq.note_last) {
        chp_midi_out(0x80u | ((uint32_t)(par[P_MIDICH] - 1) & 15u), sq.note_last, 0);
        sq.note_last = 0;
    }
}

/* -------------------------------------------------------------- beat repeat --- */
static void rep_arm(int fx)
{
    float wf, pos, r;
    rep.fx = (uint8_t)fx;
    rep.W = REP_W[fx];
    rep.on = 0;
    rep.armed = 1;
    if (!sq.playing) {
        rep.wait = 0.0f;
        return;
    }
    wf = rep.W * sq.len;                            /* on the grid of its own length, from the bar */
    pos = (float)(sq.m % 16u) * sq.len + sq.ph;
    r = pos - fm_floorf(pos / wf) * wf;
    rep.wait = r < 0.5f ? 0.0f : wf - r;
    if (rep.W >= 1.0f) {                            /* a whole step or more: from the step's trigger */
        uint32_t steps = (uint32_t)((sq.ph + rep.wait) / sq.len + 0.5f);
        rep.wait += step_at(sq.m + steps);
        if (rep.wait > 0.5f && steps == 0u)
            rep.wait = step_at(sq.m) - sq.ph;       /* this step's own (swung) trigger is still ahead */
        if (rep.wait < 0.0f)
            rep.wait = 0.0f;
    }
}

static void rep_begin(void)
{
    uint32_t i;
    rep.armed = 0;
    rep.on = 1;
    rep.n = 0;
    rep.snap[0] = ch[CH_SEQ].v;
    rep.snap[1] = ch[CH_LIVE].v;
    rep.m0 = sq.m;
    rep.pos0 = sq.pos;
    rep.left = rep.W * sq.len;
    if (sq.playing && rep.W >= 1.0f) {
        int len = cur_len();
        for (i = 0; i < 4u; i++)
            rep.st[i] = sq.steps[(sq.pos + i) % (uint32_t)len];
    }
}

static void rep_restart(void)                       /* W < 1 step, or stopped: on its own clock */
{
    lane[1].restore = 1;
    lane_fire(1);
    rep.left += rep.W * sq.len;
}

static void rep_end(void)
{
    rep.on = 0;
    rep.armed = 0;
    lane[1].pend = 0;
    lane[1].rn = 0;
    resync();
}

static void rep_update(void)                        /* the top repeat key held wins */
{
    int want = rdepth ? rstack[rdepth - 1] : -1;
    if (want < 0) {
        if (rep.on || rep.armed)
            rep_end();
        return;
    }
    if ((rep.on || rep.armed) && rep.fx == want)
        return;
    if (rep.on) {
        rep.on = 0;
        lane[1].pend = 0;
        lane[1].rn = 0;
    }
    rep_arm(want);
}

static void perf_key(int fx, int on)
{
    uint16_t m = (uint16_t)(1u << fx);
    if (fx < 0 || fx >= NPERF)
        return;
    if (on == ((perf & m) != 0))
        return;
    perf = on ? perf | m : perf & (uint16_t)~m;
    if (REP_W[fx] > 0.0f) {
        uint32_t i, j;
        for (i = j = 0; i < rdepth; i++)            /* out of the stack ... */
            if (rstack[i] != fx)
                rstack[j++] = rstack[i];
        rdepth = (uint8_t)j;
        if (on && rdepth < sizeof rstack)           /* ... and on top */
            rstack[rdepth++] = (uint8_t)fx;
        rep_update();
        return;
    }
    switch (fx) {
    case PF_REV: {                                  /* what plays turns around, now */
        uint32_t c;
        for (c = 0; c < 2u; c++) {
            flip(&ch[c].v);
            flip(&ch[c].t);
        }
        if (!on)
            resync();
        break;
    }
    case PF_TAPE:
        if (!on) {
            tape = 1.0f;
            resync();
        }
        break;
    case PF_OCT:
        if (!on)
            resync();
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------- keyboard --- */
static void rec_live(int slice, int lev, int next)
{
    chp_step_t st;
    int pos = sq.pos, len = cur_len();
    const chp_break_t *k = &brks[cur_brk];
    if (!rec_on || !sq.playing)
        return;
    if (next)
        pos = (pos + 1) % len;
    chp_step_clear(&st);
    st.src = (uint8_t)(par[P_RECAS] && k->tag[slice] != TAG_NONE ? k->tag[slice] : SRC_SLICE + slice);
    st.lev = (uint8_t)lev;
    if (perf & (1u << PF_REV))
        st.fx |= FX_REV;
    rec_push(sq.pat, pos, &st);
}

static void live_fire(int slice, int lev)
{
    voice_t v;
    live.pend = 0;
    if (!voice_set(&v, cur_brk, slice, (perf & (1u << PF_REV)) != 0, 0.0f, lev_gain(lev)))
        return;
    if (live.released) {                            /* tapped: the slice, whole */
        float sp = v.inc0 * (v.sync ? tempo() : 1.0f) * pitch_mul;
        uint32_t len = chp_slice_end(&brks[cur_brk], slice) - brks[cur_brk].start[slice];
        v.gate = (uint32_t)((float)len / fm_fabsf(sp)) + 1u;
    }
    chan_start(&ch[CH_LIVE], &v);
    chp_trig++;
    if (sq.playing && par[P_LIVE] == 0) {           /* replace: the sequence goes quiet under it */
        live.ovr = 1;
        chan_stop(&ch[CH_SEQ]);
    }
}

static void live_on(int slice, int lev)
{
    const chp_break_t *k = &brks[cur_brk];
    if (!k->len || slice < 0 || slice >= k->nsl)
        return;
    live.key = (int8_t)slice;
    live.released = 0;
    if (sq.playing && par[P_QUANT]) {
        float g = par[P_QUANT] == 2 ? sq.len * 0.5f : sq.len, r = sq.ph - fm_floorf(sq.ph / g) * g;
        if (r < g * 0.25f) {                        /* a little late: now, as if on the beat */
            live_fire(slice, lev);
            rec_live(slice, lev, r > g * 0.5f);
        } else {
            live.pend = 1;
            live.pslice = (int8_t)slice;
            live.plev = (uint8_t)lev;
            live.pleft = g - r;
        }
    } else {
        live_fire(slice, lev);
        rec_live(slice, lev, sq.ph > sq.len * 0.5f);
    }
}

static void live_off(int slice)
{
    voice_t *v = &ch[CH_LIVE].v;
    if (slice != live.key)
        return;
    live.key = -1;
    live.released = 1;
    if (live.pend || !v->on)
        return;                                     /* still to come: it plays its slice, whole */
    {
        const chp_break_t *k = &brks[v->brk];
        int s = v->slice;
        float sp = fm_fabsf(v->inc0 * (v->sync ? tempo() : 1.0f) * pitch_mul), left;
        left = v->inc0 > 0.0f ? (float)chp_slice_end(k, s) - v->pos : v->pos - (float)k->start[s];
        if (left <= 0.0f || sp <= 0.0f) {
            chan_stop(&ch[CH_LIVE]);
        } else {
            uint32_t g = (uint32_t)(left / sp) + 1u;
            if (!v->gate || g < v->gate)
                v->gate = g;
        }
    }
}

/* ------------------------------------------------------------------ output --- */
typedef struct {
    float ic1, ic2;
} svf_t;
static svf_t dj, led;
static float dj_a1, dj_a2, dj_a3, dj_k, dj_f;         /* the DJ filter's coefficients, its knob smoothed */
static float led_a1, led_a2, led_a3;
static float rc;                                     /* the A500's RC lowpass */
static float cr_acc, cr_hold, cr_step, cr_q;         /* crush: sample and hold, quantizer */
static float gate_g = 1.0f;                          /* stutter gate gain */

#define ECHO_N 16384u                                /* half rate: 0.74 s */
static int16_t echo_buf[ECHO_N];
static uint32_t echo_w;
static float echo_half, echo_d, echo_lp, echo_hp, echo_y;
static uint8_t echo_odd;

static void svf_coef(float fc, float k, float *a1, float *a2, float *a3)
{
    float g = fm_tanf(FM_PI * fm_clampf(fc, 10.0f, 20000.0f) / FSR);
    *a1 = 1.0f / (1.0f + g * (g + k));
    *a2 = g * *a1;
    *a3 = g * *a2;
}

static inline float svf_tick(svf_t *s, float x, float a1, float a2, float a3, float *v1o)
{
    float v3 = x - s->ic2, v1 = a1 * s->ic1 + a2 * v3, v2 = s->ic2 + a2 * s->ic1 + a3 * v3;
    s->ic1 = 2.0f * v1 - s->ic1;
    s->ic2 = 2.0f * v2 - s->ic2;
    *v1o = v1;
    return v2;                                       /* lowpass */
}

static void out_params(void)                         /* once a chunk */
{
    float c = (float)par[P_CRUSH] * 0.01f;
    float f = (float)par[P_FILTER];
    dj_f += (f - dj_f) * 0.08f;                      /* a turned knob sweeps, it does not step */
    if (fm_fabsf(dj_f) > 0.5f) {
        dj_k = 2.0f - 1.94f * (float)par[P_RESO] * 0.01f;
        svf_coef(dj_f < 0.0f ? 18000.0f * fm_exp2f(dj_f * (7.8f / 64.0f)) : 20.0f * fm_exp2f(dj_f * (9.0f / 64.0f)),
                 dj_k, &dj_a1, &dj_a2, &dj_a3);
    }
    cr_step = fm_exp2f(-c * 4.5f);                   /* 44.1 kHz .. 1.95 kHz */
    cr_q = fm_exp2f(15.0f - c * 12.0f);              /* 16 .. 4 bits */
}

static float shape(float x)
{
    float d = (float)par[P_DRIVE] * 0.01f, g;
    if (d <= 0.0f)
        return x;
    g = 1.0f + d * d * 40.0f;
    x *= g;
    switch (par[P_DRVTYPE]) {
    case 1:
        x = fm_clampf(x, -1.0f, 1.0f);
        break;
    case 2: {                                        /* fold: a triangle of the input */
        float t = (x + 1.0f) * 0.25f;
        t -= fm_floorf(t);
        x = 1.0f - 4.0f * fm_fabsf(t - 0.5f);
        break;
    }
    default:
        x = fm_tanhf(x);
        break;
    }
    return x * (1.0f / (1.0f + 0.08f * (g - 1.0f))) * (1.0f + d);
}

/* --------------------------------------------------------------------- init --- */
static void zero(void *p, uint32_t n)                /* no libc in the DSP units */
{
    uint8_t *b = p;
    while (n--)
        *b++ = 0;
}

static void apply(int i, int v)
{
    if (i < 0 || i >= P_NPARAMS)
        return;
    par[i] = (int16_t)(v < PARAMS[i].lo ? PARAMS[i].lo : v > PARAMS[i].hi ? PARAMS[i].hi : v);
    if (i == P_PITCH)
        pitch_mul = fm_exp2f((float)par[i] * (1.0f / 12.0f));
}

void chp_init(chp_pattern_t *bk, chp_break_t *b, const uint8_t *m)
{
    int i;
    q_r = q_w = 0;
    rq_r = rq_w = 0;
    bank = bk;
    brks = b;
    mem = m;
    for (i = 0; i < 256; i++) {
        int e, mant, mag, u = ~i & 0xFF;
        lut[FMT_PCM8][i] = (float)(int8_t)i * (1.0f / 128.0f);
        e = (u >> 4) & 7;                            /* G.711 mu-law */
        mant = u & 15;
        mag = (((mant << 3) + 0x84) << e) - 0x84;
        lut[FMT_ULAW][i] = (float)((u & 0x80) ? -mag : mag) * (1.0f / 32768.0f);
    }
    for (i = 0; i < P_NPARAMS; i++)
        apply(i, PARAMS[i].def);
    svf_coef(3090.0f, 1.41421356f, &led_a1, &led_a2, &led_a3);   /* the LED filter: Butterworth */
    zero(&sq, sizeof sq);                            /* everything that runs, from scratch */
    zero(ch, sizeof ch);
    zero(lane, sizeof lane);
    zero(&rep, sizeof rep);
    zero(&shadow, sizeof shadow);
    zero(&live, sizeof live);
    perf = 0;
    rdepth = 0;
    held = rec_on = 0;
    cur_brk = 0;
    ext_start = 0;
    dj = led = (svf_t){0.0f, 0.0f};
    rc = cr_acc = cr_hold = dj_f = 0.0f;
    gate_g = 1.0f;
    sq.pat = 0;
    sq.cue = CHP_NONE;
    sq.len = FSR * 60.0f / (165.0f * 4.0f);
    sq.steps = bank[0].st;
    live.key = -1;
    tape = 1.0f;
    perf_mul = 1.0f;
}

/* ------------------------------------------------------------------- render --- */
static void clock_msg(int msg)                       /* MIDI clock in: six ticks a step */
{
    sq.ext = 1;
    sq.ext_age = 0;
    switch (msg) {
    case CHP_CLK_TICK:
        if (sq.ext_t > 0.0f) {
            float g = sq.ext_t;
            sq.ext_gap = sq.ext_gap > 0.0f ? sq.ext_gap + (g - sq.ext_gap) * 0.2f : g;
            chp_tempo_now = FSR * 60.0f / (sq.ext_gap * 24.0f);
        }
        sq.ext_t = 0.0f;
        if (ext_start) {                             /* the tick after a start is the downbeat */
            ext_start = 0;
            sq.len = FSR * 60.0f / (tempo() * 4.0f);
            start();
            return;
        }
        if (sq.playing && ++sq.ext_ticks % 6u == 0u) {
            sq.ph = 0.0f;
            step_advance();
        }
        break;
    case CHP_CLK_START:
    case CHP_CLK_CONTINUE:                           /* no song position: from the top, too */
        if (sq.playing && msg == CHP_CLK_CONTINUE)
            break;
        if (sq.playing)
            stop();
        ext_start = 1;
        break;
    default:
        ext_start = 0;
        if (sq.playing)
            stop();
        break;
    }
}

static void midi_in(void)                            /* straight from the ring: no queue round trip */
{
    uint32_t pkt;
    while (chp_midi_in(&pkt)) {
        uint32_t st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu;
        uint32_t c = (uint32_t)(par[P_MIDICH] - 1) & 15u;
        if (st == 0xF8u)
            clock_msg(CHP_CLK_TICK);
        else if (st == 0xFAu)
            clock_msg(CHP_CLK_START);
        else if (st == 0xFBu)
            clock_msg(CHP_CLK_CONTINUE);
        else if (st == 0xFCu)
            clock_msg(CHP_CLK_STOP);
        else if (st == (0x90u | c) && d2)            /* C2 (36) and up: the slices */
            live_on((int)d1 - 36, d2 >= 100u ? 15 : (int)(d2 / 9u) + 1);
        else if (st == (0x80u | c) || st == (0x90u | c))
            live_off((int)d1 - 36);
        else if (st == (0xC0u | c) && d1 < CHP_NPAT) {
            if (sq.playing)
                sq.cue = (uint8_t)d1 == sq.pat ? CHP_NONE : (uint8_t)d1;
            else
                sq.pat = (uint8_t)d1;
        }
    }
}

static void drain(void)
{
    while (q_r != q_w) {
        cmd_t m;
        BARRIER();
        m = q[q_r % QN];
        BARRIER();
        q_r++;
        if (held && (m.c == C_PLAY || m.c == C_LIVE || m.c == C_PERF || m.c == C_AUD || m.c == C_CLOCK))
            continue;                                /* held: nothing plays; selections still count */
        switch (m.c) {
        case C_SET:
            apply(m.a, m.b);
            break;
        case C_PLAY:
            if (m.a && !sq.playing)
                start();
            else if (!m.a && sq.playing)
                stop();
            break;
        case C_PAT:
            if (m.a >= 0 && m.a < CHP_NPAT) {
                if (sq.playing && (uint8_t)m.a != sq.pat)
                    sq.cue = (uint8_t)m.a;
                else if (sq.playing)
                    sq.cue = CHP_NONE;
                else
                    sq.pat = (uint8_t)m.a;
            }
            break;
        case C_BRK:
            if (m.a >= 0 && m.a < CHP_NBRK)
                cur_brk = (uint8_t)m.a;
            break;
        case C_SEED:
            seed = (uint32_t)m.a;
            rng = seed | 1u;
            break;
        case C_LIVE:
            if (m.b)
                live_on(m.a, m.d < 0 ? 0 : m.d > 15 ? 15 : m.d);
            else
                live_off(m.a);
            break;
        case C_PERF:
            perf_key(m.a, m.b != 0);
            break;
        case C_REC:
            rec_on = (uint8_t)(m.a != 0);
            break;
        case C_AUD: {
            voice_t v;
            if (voice_set(&v, m.a, m.b, 0, 0.0f, 1.0f)) {
                const chp_break_t *k = &brks[m.a];
                float sp = v.inc0 * (v.sync ? tempo() : 1.0f) * pitch_mul;
                v.gate = (uint32_t)((float)(chp_slice_end(k, m.b) - k->start[m.b]) / fm_fabsf(sp)) + 1u;
                chan_start(&ch[CH_LIVE], &v);
                chp_trig++;
            }
            break;
        }
        case C_HOLD:
            held = (uint8_t)(m.a != 0);
            if (held) {
                if (sq.playing)
                    stop();
                ch[0].v.on = ch[0].t.on = ch[1].v.on = ch[1].t.on = 0;
                live.key = -1;
                live.pend = live.ovr = 0;
                rep.on = rep.armed = 0;
                shadow.valid = 0;
            }
            break;
        case C_PANIC:
            live.key = -1;
            live.pend = live.ovr = 0;
            chan_stop(&ch[0]);
            chan_stop(&ch[1]);
            break;
        case C_CLOCK:
            clock_msg(m.a);
            break;
        }
    }
}

/* frames until the next event, from now */
static float next_event(void)
{
    float t = 1e9f;
    int L;
    if (sq.playing) {
        if (!sq.ext) {
            float pn = (float)(sq.pulse + 1u) * sq.len * (1.0f / 6.0f);
            t = fm_minf(t, pn - sq.ph);              /* the next clock pulse (the step's end is the 6th) */
        }
        for (L = 0; L < 2; L++) {
            if (lane[L].pend)
                t = fm_minf(t, lane[L].at - sq.ph);
            if (lane[L].rn)
                t = fm_minf(t, lane[L].rat - sq.ph);
        }
    }
    if (live.pend)
        t = fm_minf(t, live.pleft);
    if (rep.armed)
        t = fm_minf(t, rep.wait);
    if (rep.on && (rep.W < 1.0f || !sq.playing))
        t = fm_minf(t, rep.left);
    return t < 0.0f ? 0.0f : t;
}

static void events(void)                             /* everything due now, in order */
{
    int guard = 64, L;
    while (guard--) {
        int any = 0;
        if (sq.playing && !sq.ext && sq.ph >= (float)(sq.pulse + 1u) * sq.len * (1.0f / 6.0f) - 0.001f) {
            sq.pulse++;
            if (sq.pulse >= 6u) {
                sq.ph -= sq.len;
                if (sq.ph < 0.0f)
                    sq.ph = 0.0f;
                step_advance();
            }
            if (par[P_CLKOUT])
                chp_midi_out(0xF8, 0, 0);
            any = 1;
        }
        if (live.pend && live.pleft <= 0.001f) {     /* before the step: it may replace it */
            int s = live.pslice;
            live_fire(s, live.plev);
            rec_live(s, live.plev, 0);
            any = 1;
        }
        for (L = 0; L < 2 && sq.playing; L++) {
            if (lane[L].pend && sq.ph >= lane[L].at - 0.001f) {
                lane_fire(L);
                any = 1;
            } else if (!lane[L].pend && lane[L].rn && sq.ph >= lane[L].rat - 0.001f) {
                lane_trig(L);
                any = 1;
            }
        }
        if (rep.armed && rep.wait <= 0.001f) {       /* after the step: its trigger is in the snapshot */
            rep_begin();
            any = 1;
        }
        if (rep.on && (rep.W < 1.0f || !sq.playing) && rep.left <= 0.001f) {
            rep_restart();
            any = 1;
        }
        if (!any)
            break;
    }
}

static float mix[CHUNK];

void chp_render(int32_t *out, uint32_t n, uint32_t gain_q12)
{
    const float gain = (float)gain_q12 * (1.0f / 4096.0f) * 2.0f * fm_exp2f((float)par[P_VOLUME] * (1.0f / 6.0206f));
    float peak = 0.0f;
    drain();
    if (held) {
        uint32_t j;
        for (j = 0; j < 2u * n; j++)
            out[j] = 0;
        chp_held = 1;
        chp_playing = 0;
        chp_level = 0.0f;
        return;
    }
    chp_held = 0;
    midi_in();
    if (sq.ext) {
        sq.ext_age += n;
        if (sq.ext_age > CHP_SR / 2u) {              /* no tick for 0.5 s: back to the own tempo */
            sq.ext = 0;
            sq.ph = 0.0f;
            sq.pulse = 0;
        }
    }
    sq.len = FSR * 60.0f / (tempo() * 4.0f);
    events();                                        /* a start or a tick just now */
    while (n) {
        uint32_t k = n > CHUNK ? CHUNK : n, j, c;
        float t = next_event(), mul, tm;
        int clean = par[P_INTERP];
        if (t < (float)k) {
            uint32_t kk = (uint32_t)t;
            if ((float)kk < t)
                kk++;
            k = kk ? kk : 1u;
        }
        /* the speed: tempo (SYNC), pitch, tape stop, octave down */
        if (perf & (1u << PF_TAPE)) {
            tape -= (float)k / (4.0f * sq.len);
            if (tape < 0.0f)
                tape = 0.0f;
        }
        perf_mul = ((perf & (1u << PF_OCT)) ? 0.5f : 1.0f) * tape;
        for (j = 0; j < k; j++)
            mix[j] = 0.0f;
        tm = tempo();
        for (c = 0; c < 2u; c++) {
            chan_t *h = &ch[c];
            if (h->t.on) {
                mul = (h->t.sync ? tm : 1.0f) * pitch_mul * perf_mul;
                voice_run(&h->t, mix, k, mul, clean);
            }
            if (h->v.on) {
                mul = (h->v.sync ? tm : 1.0f) * pitch_mul * perf_mul;
                voice_run(&h->v, mix, k, mul, clean);
            }
        }
        if (live.ovr && !ch[CH_LIVE].v.on && !live.pend) {   /* the keyboard's slice is over: back in */
            live.ovr = 0;
            resync();
        }
        out_params();
        {
            float tg = tape < 0.05f ? tape * 20.0f : 1.0f;   /* a stopped tape is silent, not DC */
            uint32_t a = (uint32_t)par[P_AMIGA];
            float send = (float)par[P_ECHO] * 0.01f, fb = (float)par[P_ECHOFB] * 0.0095f;
            float dsteps = ECHO_STEPS[(unsigned)par[P_ECHOT] % 6u] * sq.len * 0.5f;
            float gph = sq.playing ? sq.ph : sq.free_ph, ginc = 2.0f / sq.len;
            int gate_on = (perf & (1u << PF_GATE)) != 0;
            if (dsteps > (float)(ECHO_N - 4u))
                dsteps = (float)(ECHO_N - 4u);
            echo_d += (dsteps - echo_d) * 0.05f;
            gph *= ginc;
            for (j = 0; j < k; j++) {
                float x = mix[j] * tg, v1;
                if (par[P_CRUSH]) {
                    cr_acc += cr_step;
                    if (cr_acc >= 1.0f) {
                        cr_acc -= 1.0f;
                        cr_hold = fm_floorf(x * cr_q + 0.5f) / cr_q;
                    }
                    x = cr_hold;
                }
                if (a == 1u || a == 2u) {             /* the A500: 4.4 kHz RC, always */
                    rc += 0.4675f * (x - rc);
                    x = rc;
                }
                if (a >= 2u)                          /* the LED filter: 3.1 kHz, 12 dB */
                    x = svf_tick(&led, x, led_a1, led_a2, led_a3, &v1);
                x = shape(x);
                if (fm_fabsf(dj_f) > 0.5f) {
                    float lp = svf_tick(&dj, x, dj_a1, dj_a2, dj_a3, &v1);
                    x = dj_f < 0.0f ? lp : x - dj_k * v1 - lp;
                }
                if (send > 0.0f || echo_y != 0.0f) { /* the echo: half rate, dub-filtered feedback */
                    float rp = (float)echo_w - echo_d, fr, y, in;
                    uint32_t i0;
                    if (rp < 0.0f)
                        rp += (float)ECHO_N;
                    i0 = (uint32_t)rp;
                    fr = rp - (float)i0;
                    y = ((float)echo_buf[i0 % ECHO_N] * (1.0f - fr) + (float)echo_buf[(i0 + 1u) % ECHO_N] * fr) *
                        (1.0f / 16384.0f);
                    echo_lp += 0.3f * (y - echo_lp);
                    echo_hp += 0.02f * (echo_lp - echo_hp);
                    in = x * send + fm_tanhf((echo_lp - echo_hp) * fb * 1.2f);
                    if (echo_odd) {
                        float h = (echo_half + in) * 0.5f * 16384.0f;
                        echo_buf[echo_w % ECHO_N] = (int16_t)fm_clampf(h, -32767.0f, 32767.0f);
                        echo_w = (echo_w + 1u) % ECHO_N;
                    } else {
                        echo_half = in;
                    }
                    echo_odd ^= 1u;
                    echo_y = fm_fabsf(y) > 1e-5f || send > 0.0f ? y : 0.0f;
                    x += y;
                }
                if (gate_on) {                        /* stutter: 1/32 on, 1/32 off */
                    float tgt = ((uint32_t)gph & 1u) ? 0.0f : 1.0f;
                    gate_g += (tgt - gate_g) * 0.05f;
                    gph += ginc;
                    x *= gate_g;
                } else {
                    gate_g = 1.0f;
                }
                x = fm_tanhf(x * gain);              /* soft clip: loud is fine, harsh is not */
                peak = fm_maxf(peak, fm_fabsf(x));
                out[2u * j] = out[2u * j + 1u] = (int32_t)(x * 8300000.0f);
            }
        }
        out += 2u * k;
        n -= k;
        /* time moves on */
        if (sq.playing) {
            sq.ph += (float)k;
            sq.clock += k;
            if (lane[0].rn == 0 && lane[1].rn == 0 && sq.ph > 8.0f * sq.len)
                sq.ph = 8.0f * sq.len;                /* MIDI clock stopped coming: wait */
        } else {
            sq.free_ph += (float)k;
            if (sq.free_ph >= sq.len)
                sq.free_ph -= sq.len;
        }
        if (sq.ext)
            sq.ext_t += (float)k;
        if (live.pend)
            live.pleft -= (float)k;
        if (rep.armed)
            rep.wait -= (float)k;
        if (rep.on && (rep.W < 1.0f || !sq.playing))
            rep.left -= (float)k;
        events();
    }
    /* what the screen shows */
    {
        const voice_t *v = ch[CH_LIVE].v.on ? &ch[CH_LIVE].v : ch[CH_SEQ].v.on ? &ch[CH_SEQ].v : 0;
        chp_vbrk = v ? v->brk : CHP_NONE;
        chp_vslice = v ? (uint8_t)chp_slice_at(&brks[v->brk], (uint32_t)v->pos) : CHP_NONE;
        chp_vpos = v ? (uint32_t)v->pos : 0u;
    }
    chp_playing = sq.playing;
    chp_cur = sq.pat;
    chp_cue = sq.cue;
    chp_mstep = sq.pos;
    chp_step = rep.on && rep.W >= 1.0f && sq.playing
                   ? (uint8_t)((rep.pos0 + (sq.m - rep.m0) % (uint32_t)rep.W) % (uint32_t)cur_len())
                   : sq.pos;
    chp_pulse = sq.pulse;
    chp_bars = sq.bars + sq.pos / 16u;
    chp_ext = sq.ext;
    chp_recording = rec_on;
    chp_perf_on = (uint16_t)(perf | (rep.on ? 0x8000u : 0u));
    chp_work_on = sq.work;
    if (!sq.ext)
        chp_tempo_now = (float)par[P_TEMPO];
    chp_level = peak;
}
