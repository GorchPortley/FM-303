/* SPDX-License-Identifier: GPL-3.0-only */
/* CHOPPAGE UI: the panel and the screen. Part of the unity build (after gfx.c, project.c, sysex.c);
 * the host simulator includes it too.
 *
 *   HOME  PLAY    white keys play the break's slices (OCT-/OCT+: 1-16 / 17-32); while the pattern
 *                 runs, a held slice takes over and the pattern comes back in time when it ends.
 *                 Black keys hold the perform fx: repeat 1/8 1/16 1/32 | 1/64 1/24 | REVERSE TAPE
 *                 OCT- | BEAT GATE | SCATTER. ENV + white key: the pattern (cued); LFO + white key
 *                 1-8: the break. REC: record what the keys play into the pattern.
 *                 KNOB 1-4: Filter, Pitch, Crush, Auto
 *   SEQ   STEP    white keys are the steps: a tap picks one (and plays it); held with a black type
 *                 key, writes it. Black keys: K S H | G ? | REV ROLL PITCH | clear CUT | the slice
 *                 played last. KNOB: Src, Pitch, Roll, Level; SEQ again: Rev, Ramp, Chance, Gate
 *   EDIT  SLICE   white keys play and pick slices; black keys tag them: K S H | G none.
 *                 KNOB: Slice, Start, Tag, Beats; EDIT again: Cut, Sens, Snap. REC: tag again
 *   SEL   BREAKS  white keys 1-8 pick the break. KNOB: Break, Beats, Sync, Pool
 *   ARP   GEN     white keys write a new pattern in the style (each key its own); black keys 1-6 pick
 *                 the style, the last a new set of dice; ENV takes the last one back.
 *                 KNOB: Style, Density, Chaos, Fill; ARP again: Length, Vary, Swing, Ramp
 *   FX    SOUND   Drive, Shape, Interp (Paula / Clean), Amiga; FX again: Echo, Time, Feedback, Reso
 *   GLO   SETUP   MIDI ch, Clock out, Note out, Lights; GLO again: Quant, Keys, Rec as, Volume
 *
 *   PLAY start / stop    SELECT tempo    ALGORITHM the break    PRESETS the pattern (cued)
 *   SAVE saves (patterns, slices, settings); held + white key on PLAY: copies the pattern there;
 *        held + REC on STEP (twice): clears the pattern. It also saves by itself while stopped. */

enum { V_PLAY, V_STEP, V_SLICE, V_BREAKS, V_GEN, V_SOUND, V_SETUP, NVIEWS };
static const char *const VIEW_NAME[NVIEWS] = {"Play", "Step", "Slice", "Breaks", "Gen", "Sound", "Setup"};
static const uint8_t VIEW_BTN[NVIEWS] = {B_HOME, B_SEQ, B_EDIT, B_SEL, B_ARP, B_FX, B_GLO};
static const uint8_t VIEW_PAGES[NVIEWS] = {1, 2, 2, 1, 2, 2, 2};

/* the four knobs of each view and page: a parameter, or one of these */
enum { K_SRC = 100, K_SPITCH, K_ROLL, K_LEV, K_REV, K_RAMP, K_PROB, K_GATE, K_LEN, K_SLICE, K_START, K_TAG,
       K_BEATS, K_CUT, K_SENS, K_SNAP, K_BRK, K_LEDS, K_NONE };
static const uint8_t VIEW_KNOB[NVIEWS][2][4] = {
    {{P_FILTER, P_PITCH, P_CRUSH, P_AUTO}, {P_FILTER, P_PITCH, P_CRUSH, P_AUTO}},
    {{K_SRC, K_SPITCH, K_ROLL, K_LEV}, {K_REV, K_RAMP, K_PROB, K_GATE}},
    {{K_SLICE, K_START, K_TAG, K_BEATS}, {K_CUT, K_SENS, K_SNAP, K_NONE}},
    {{K_BRK, K_BEATS, P_SYNC, P_POOL}, {K_BRK, K_BEATS, P_SYNC, P_POOL}},
    {{P_STYLE, P_DENS, P_CHAOS, P_FILL}, {K_LEN, P_VARY, P_SWING, P_RAMP}},
    {{P_DRIVE, P_DRVTYPE, P_INTERP, P_AMIGA}, {P_ECHO, P_ECHOT, P_ECHOFB, P_RESO}},
    {{P_MIDICH, P_CLKOUT, P_NOTEOUT, K_LEDS}, {P_QUANT, P_LIVE, P_RECAS, P_VOLUME}},
};

static const uint8_t WHITE_K[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};
static const uint8_t BLACK_K[11] = {1, 3, 5, 8, 10, 13, 15, 17, 20, 22, 25};

/* the screen: Workbench blue, white print, the orange of the accent */
#define K_BG RGB(0, 85, 170)
#define K_PANEL RGB(0, 62, 132)
#define K_DARK RGB(0, 0, 34)
#define K_LINE RGB(38, 112, 196)
#define K_DIM RGB(150, 178, 218)
#define K_TEXT RGB(255, 255, 255)
#define K_ACC RGB(255, 136, 0)
#define K_RED RGB(240, 60, 50)
#define K_HEAD RGB(255, 214, 150)
static const uint16_t TAG_C[NTAGS] = {RGB(64, 120, 190), RGB(255, 136, 0), RGB(255, 255, 255), RGB(110, 214, 255),
                                      RGB(206, 160, 255)};

static struct {
    uint8_t view, page[NVIEWS], kpage;           /* kpage: the slices / steps page (OCT) */
    uint32_t btn, keys, btn_used;
    uint32_t enc_t[NE];
    uint8_t cur;                                 /* STEP: the step cursor */
    uint8_t scur;                                /* SLICE: the slice cursor */
    int8_t live_last;                            /* the slice played last */
    int8_t live_key[NKEYS];                      /* the slice each held key started */
    uint8_t rec;
    uint8_t dirty_p, dirty_b, dirty_k;           /* settings, patterns, slices changed since the save */
    uint32_t act_t, clear_t;
    char msg[2][24];
    uint32_t msg_until;
    int8_t touched;
    uint32_t touch_until;
    uint32_t frame;
    uint32_t sig[3];
    uint32_t wv_sig;                             /* what the waveform overview was made from */
    int8_t wv_lo[240], wv_hi[240];
    chp_pattern_t undo;                          /* GEN: the pattern before the last one written */
    uint8_t undo_ok, undo_pat;
} ui;

static void say(const char *a, const char *b)
{
    uint32_t i;
    for (i = 0; i < 23u && a && a[i]; i++)
        ui.msg[0][i] = a[i];
    ui.msg[0][i] = 0;
    for (i = 0; i < 23u && b && b[i]; i++)
        ui.msg[1][i] = b[i];
    ui.msg[1][i] = 0;
    ui.msg_until = plat_ms() + 1300u;
}
void ui_say(const char *a, const char *b) { say(a, b); }
int ui_dirty(void) { return ui.dirty_p || ui.dirty_b || ui.dirty_k; }

static char *cat(char *b, const char *s)
{
    while (*s)
        *b++ = *s++;
    *b = 0;
    return b;
}

static char *cat_i(char *b, int v, int plus)
{
    char t[12];
    int n = 0;
    if (v < 0) {
        *b++ = '-';
        v = -v;
    } else if (plus && v > 0) {
        *b++ = '+';
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

static chp_pattern_t *edp(void) { return &pats.pat[proj.pat]; }
static chp_step_t *eds(void) { return &edp()->st[ui.cur < edp()->len ? ui.cur : 0]; }
static chp_break_t *edb(void) { return &brk[proj.brk]; }
static int held(int b) { return (int)((ui.btn >> b) & 1u); }
static int white_index(int k)
{
    int i;
    for (i = 0; i < 16; i++)
        if (WHITE_K[i] == k)
            return i;
    return -1;
}
static int black_index(int k)
{
    int i;
    for (i = 0; i < 11; i++)
        if (BLACK_K[i] == k)
            return i;
    return -1;
}

static void src_name(int src, char *b)           /* "-", "K", "12", ... */
{
    static const char *const N[] = {"-", "K", "S", "H", "G", "?", "X"};
    if (SRC_IS_SLICE(src))
        cat_i(b, src - SRC_SLICE + 1, 0);
    else
        cat(b, N[src < SRC_SLICE ? src : 0]);
}

static int src_tag(int src)                      /* the colour a step source draws in */
{
    if (SRC_IS_TYPE(src))
        return src;
    if (SRC_IS_SLICE(src) && edb()->len) {
        int s = (src - SRC_SLICE) % edb()->nsl;
        return edb()->tag[s];
    }
    return TAG_NONE;
}

/* ----------------------------------------------------------------- values --- */
static int knob_get(int k)
{
    chp_step_t *s = eds();
    chp_break_t *b = edb();
    switch (k) {
    case K_SRC: return s->src;
    case K_SPITCH: return s->pitch;
    case K_ROLL: return s->fx & FX_ROLL;
    case K_LEV: return s->lev;
    case K_REV: return (s->fx & FX_REV) != 0;
    case K_RAMP: return (s->fx & FX_RAMP_UP) ? 1 : (s->fx & FX_RAMP_DN) ? 2 : 0;
    case K_PROB: return s->prob;
    case K_GATE: return s->gate;
    case K_LEN: return edp()->len;
    case K_SLICE: return ui.scur;
    case K_START: return b->len ? (int)b->start[ui.scur] : 0;
    case K_TAG: return b->tag[ui.scur];
    case K_BEATS: return b->beats;
    case K_CUT: return proj.slm;
    case K_SENS: return proj.sens;
    case K_SNAP: return proj.snap;
    case K_BRK: return proj.brk;
    case K_LEDS: return proj.leds;
    case K_NONE: return 0;
    default: return proj.par[k];
    }
}

static void knob_range(int k, int *lo, int *hi)
{
    chp_break_t *b = edb();
    *lo = 0;
    switch (k) {
    case K_SRC: *hi = SRC_MAX; break;
    case K_SPITCH: *lo = -24; *hi = 24; break;
    case K_ROLL: *hi = 7; break;
    case K_LEV: *hi = 15; break;
    case K_REV: case K_SNAP: *hi = 1; break;
    case K_RAMP: case K_LEDS: *hi = 2; break;
    case K_PROB: case K_SENS: *hi = 100; break;
    case K_GATE: *hi = 8; break;
    case K_LEN: *lo = 1; *hi = CHP_STEPS; break;
    case K_SLICE: *hi = b->nsl ? b->nsl - 1 : 0; break;
    case K_START:                                /* between the slices either side */
        *lo = ui.scur ? (int)b->start[ui.scur - 1] + (int)(b->rate / 50u) : 0;
        *hi = ui.scur ? (int)chp_slice_end(b, ui.scur) - (int)(b->rate / 50u) : 0;
        if (*hi < *lo)
            *hi = *lo;
        break;
    case K_TAG: *hi = NTAGS - 1; break;
    case K_BEATS: *lo = 1; *hi = 32; break;
    case K_CUT: *hi = NSLM - 1; break;
    case K_BRK: *hi = CHP_NBRK - 1; break;
    case K_NONE: *hi = 0; break;
    default:
        *lo = chp_param_info(k)->lo;
        *hi = chp_param_info(k)->hi;
        break;
    }
}

static const char *knob_name(int k)
{
    static const char *const N[] = {"Src", "Pitch", "Roll", "Level", "Rev", "Ramp", "Chance", "Gate", "Length",
                                    "Slice", "Start", "Tag", "Beats", "Cut", "Sens", "Snap", "Break", "Lights", ""};
    return k >= K_SRC ? N[k - K_SRC] : chp_param_info(k)->name;
}

static void knob_text(int k, char *b)
{
    static const char *const TAG_N[NTAGS] = {"None", "Kick", "Snare", "Hat", "Ghost"};
    static const char *const RAMP_N[3] = {"Off", "Up", "Down"};
    static const char *const LEDS_N[3] = {"Off", "On", "Keys"};
    int v = knob_get(k);
    b[0] = 0;
    switch (k) {
    case K_SRC: src_name(v, b); break;
    case K_SPITCH: if (!v) cat(b, "0"); else cat_i(b, v, 1); break;
    case K_ROLL: if (!v) cat(b, "Off"); else cat(cat_i(b, CHP_ROLL_N[v], 0), "x"); break;
    case K_LEV: cat_i(b, v, 0); break;
    case K_REV: case K_SNAP: cat(b, v ? "On" : "Off"); break;
    case K_RAMP: cat(b, RAMP_N[v % 3]); break;
    case K_PROB: case K_SENS: cat(cat_i(b, v, 0), "%"); break;
    case K_GATE: if (!v) cat(b, "Run"); else cat(cat_i(b, v, 0), "/8"); break;
    case K_LEN: case K_BEATS: cat_i(b, v, 0); break;
    case K_SLICE: cat_i(b, v + 1, 0); break;
    case K_START: {                              /* in ms from the break's top */
        uint32_t r = edb()->rate ? edb()->rate : 1u;
        cat(cat_i(b, (int)((uint32_t)v * 1000u / r), 0), "ms");
        break;
    }
    case K_TAG: cat(b, TAG_N[v % NTAGS]); break;
    case K_CUT: cat(b, ANA_MODE_NAME[v % NSLM]); break;
    case K_BRK: cat_i(b, v + 1, 0); break;
    case K_LEDS: cat(b, LEDS_N[v % 3]); break;
    case K_NONE: break;
    default: chp_param_text(k, v, b); break;
    }
}

static void knob_set(int k, int v)
{
    int lo, hi;
    chp_step_t *s = eds();
    chp_break_t *b = edb();
    knob_range(k, &lo, &hi);
    v = v < lo ? lo : v > hi ? hi : v;
    if (v == knob_get(k))
        return;
    switch (k) {
    case K_SRC: s->src = (uint8_t)v; break;
    case K_SPITCH: s->pitch = (int8_t)v; break;
    case K_ROLL: s->fx = (uint8_t)((s->fx & ~FX_ROLL) | (uint32_t)v); break;
    case K_LEV: s->lev = (uint8_t)v; break;
    case K_REV: s->fx ^= FX_REV; break;
    case K_RAMP: s->fx = (uint8_t)((s->fx & ~(FX_RAMP_UP | FX_RAMP_DN)) | (v == 1 ? FX_RAMP_UP : v == 2 ? FX_RAMP_DN : 0u)); break;
    case K_PROB: s->prob = (uint8_t)v; break;
    case K_GATE: s->gate = (uint8_t)v; break;
    case K_LEN:
        edp()->len = (uint8_t)v;
        if (ui.cur >= v)
            ui.cur = (uint8_t)(v - 1);
        break;
    case K_SLICE: ui.scur = (uint8_t)v; chp_audition(proj.brk, v); return;
    case K_START: b->start[ui.scur] = (uint32_t)v; ui.dirty_k = 1; return;
    case K_TAG: b->tag[ui.scur] = (uint8_t)v; ui.dirty_k = 1; return;
    case K_BEATS: b->beats = (uint8_t)v; ui.dirty_k = 1; return;
    case K_CUT: case K_SENS: case K_SNAP:            /* the cut, again: what it gives is heard and seen */
        if (k == K_CUT)
            proj.slm = (uint8_t)v;
        else if (k == K_SENS)
            proj.sens = (uint8_t)v;
        else
            proj.snap = (uint8_t)v;
        brk_reslice(proj.brk);                   /* the render reads a table as it is: old cut or new */
        if (ui.scur >= b->nsl)
            ui.scur = (uint8_t)(b->nsl - 1);
        ui.dirty_k = ui.dirty_p = 1;
        return;
    case K_BRK:
        if (!brk[v].len) {                       /* to the next break there is, that way */
            int d = v > (int)proj.brk ? 1 : -1, i;
            for (i = v; i >= 0 && i < CHP_NBRK && !brk[i].len; i += d)
                ;
            if (i < 0 || i >= CHP_NBRK)
                return;
            v = i;
        }
        proj.brk = (uint8_t)v;
        chp_break(v);
        if (ui.scur >= brk[v].nsl)
            ui.scur = 0;
        ui.dirty_p = 1;
        return;
    case K_LEDS: proj.leds = (uint8_t)v; ui.dirty_p = 1; return;
    default:
        proj.par[k] = (int16_t)v;
        chp_set(k, v);
        ui.dirty_p = 1;
        return;
    }
    ui.dirty_b = 1;                              /* a step or the pattern changed */
}

/* knob acceleration by speed: a slow turn is one step a detent, a quick one up to 8 */
static uint8_t accel_off;                        /* the host simulator's "spin": exact steps */
static int32_t accel(int role, int32_t s, int range)
{
    uint32_t now = plat_ms(), dt = now - ui.enc_t[role], a = (uint32_t)(s < 0 ? -s : s), m;
    ui.enc_t[role] = now;
    if (range <= 24 || !a || accel_off)
        return s;
    if (a > 1)
        dt /= a;
    m = dt < 15u ? (range > 1000 ? 64u : 8u) : dt < 30u ? (range > 1000 ? 16u : 4u) : dt < 60u ? 2u : 1u;
    return s * (int32_t)m;
}

static void turn(int role, int k, int32_t e)
{
    int lo, hi;
    if (!e || k == K_NONE)
        return;
    knob_range(k, &lo, &hi);
    if (k == K_START)                            /* a slice start moves a millisecond a detent */
        e *= (int32_t)(edb()->rate / 1000u ? edb()->rate / 1000u : 1u);
    knob_set(k, knob_get(k) + accel(role, e, hi - lo));
}

/* --------------------------------------------------------------- actions --- */
static void select_pattern(int p)
{
    if (p < 0 || p >= CHP_NPAT)
        return;
    proj.pat = (uint8_t)p;
    if (ui.cur >= edp()->len)
        ui.cur = 0;
    chp_pattern(p);
    ui.dirty_p = 1;
}

static void select_break(int b)
{
    if (b < 0 || b >= CHP_NBRK || !brk[b].len)
        return;
    knob_set(K_BRK, b);
}

static void generate(int k)                      /* GEN: a new pattern in the style, key k's own dice */
{
    gen_cfg_t c;
    chp_pattern_t *p = edp();
    ui.undo = *p;
    ui.undo_ok = 1;
    ui.undo_pat = proj.pat;
    c.style = (uint8_t)proj.par[P_STYLE];
    c.dens = (uint8_t)proj.par[P_DENS];
    c.chaos = (uint8_t)proj.par[P_CHAOS];
    gen_write(p, &c, edb(), gen_hash(proj.seed, (uint32_t)k));
    ui.dirty_b = 1;
    {
        char t[12];
        cat_i(cat(t, "VARIATION "), k + 1, 0);
        say(t, CHP_STYLE_NAME[c.style % NSTYLES]);
    }
}

static void save_all(void)
{
    int ok = pats_save() == 0 && project_save() == 0 && kits_save() == 0;
    if (ok)
        ui.dirty_p = ui.dirty_b = ui.dirty_k = 0;
    say(ok ? "SAVED" : "SAVE", ok ? "" : "FAILED");
}

static void set_view(int v)
{
    if (ui.view == v && VIEW_PAGES[v] > 1)
        ui.page[v] ^= 1u;                        /* the same button again: its other page */
    ui.view = (uint8_t)v;
    ui.touched = -1;
}

static void button(int b)
{
    int i;
    for (i = 0; i < NVIEWS; i++)
        if (VIEW_BTN[i] == b) {
            set_view(i);
            return;
        }
    switch (b) {
    case B_PLAY:
        chp_play(!chp_playing);
        break;
    case B_REC:
        if (ui.view == V_STEP && held(B_SAVE)) {  /* SAVE + REC, twice: clear the pattern */
            ui.btn_used |= 1u << B_SAVE;
            if (plat_ms() - ui.clear_t < 1500u) {
                chp_pattern_clear(edp(), edp()->len);
                ui.dirty_b = 1;
                say("PATTERN", "CLEARED");
                ui.clear_t = 0;
            } else {
                ui.clear_t = plat_ms();
                say("CLEAR?", "AGAIN");
            }
        } else if (ui.view == V_SLICE) {         /* tag the slices again, from their sound */
            ana_tag(edb(), mem + edb()->off);
            ui.dirty_k = 1;
            say("TAGGED", "BY EAR");
        } else {
            ui.rec ^= 1u;
            chp_rec(ui.rec);
            say(ui.rec ? "RECORD" : "RECORD", ui.rec ? "ON" : "OFF");
        }
        break;
    case B_SAVE:
        save_all();
        break;
    case B_OCTDN:
        ui.kpage = 0;
        break;
    case B_OCTUP:
        ui.kpage = 1;
        break;
    case B_ENV:
        if (ui.view == V_GEN && ui.undo_ok) {    /* take the last generated pattern back */
            pats.pat[ui.undo_pat] = ui.undo;
            ui.undo_ok = 0;
            ui.dirty_b = 1;
            say("UNDONE", "");
        }
        break;
    default:
        break;
    }
}

/* a step written from a black key in STEP: types, flags, clear */
static void step_key(chp_step_t *s, int bi)
{
    switch (bi) {
    case 0: case 1: case 2: case 3:                    /* K S H G: on, or off again */
        s->src = (uint8_t)(s->src == SRC_K + bi ? SRC_NONE : SRC_K + bi);
        if (s->src == SRC_G && s->lev == 15)
            s->lev = 10;
        break;
    case 4: s->src = (uint8_t)(s->src == SRC_ANY ? SRC_NONE : SRC_ANY); break;
    case 5: s->fx ^= FX_REV; break;
    case 6: s->fx = (uint8_t)((s->fx & ~FX_ROLL) | (((s->fx & FX_ROLL) + 1u) % 6u)); break;   /* off 2 3 4 6 8 */
    case 7: s->pitch = (int8_t)(s->pitch == 0 ? 12 : s->pitch == 12 ? -12 : 0); break;
    case 8: chp_step_clear(s); break;
    case 9: s->src = SRC_CUT; break;
    case 10:
        if (ui.live_last >= 0)
            s->src = (uint8_t)(SRC_SLICE + ui.live_last);
        break;
    default:
        break;
    }
    ui.dirty_b = 1;
}

static void key(int k, int down, uint32_t others)
{
    int wi = white_index(k), bi = black_index(k);
    (void)others;
    if (ui.view == V_STEP) {
        int len = edp()->len;
        if (wi >= 0 && down) {
            int st = ui.kpage * 16 + wi;
            uint32_t i, painted = 0;
            if (st >= len)
                return;
            ui.cur = (uint8_t)st;
            for (i = 0; i < 11u; i++)            /* a type key held: paint the step with it */
                if ((ui.keys >> BLACK_K[i]) & 1u) {
                    step_key(eds(), (int)i);
                    painted = 1;
                }
            if (!painted) {
                const chp_step_t *s = eds();
                if (SRC_IS_SLICE(s->src))
                    chp_audition(proj.brk, (s->src - SRC_SLICE) % (edb()->nsl ? edb()->nsl : 1));
            }
        } else if (bi >= 0 && down) {
            uint32_t i, any = 0;
            for (i = 0; i < 16u; i++)            /* steps held: write to them; else the cursor */
                if ((ui.keys >> WHITE_K[i]) & 1u) {
                    int st = ui.kpage * 16 + (int)i;
                    if (st < len) {
                        step_key(&edp()->st[st], bi);
                        any = 1;
                    }
                }
            if (!any)
                step_key(eds(), bi);
        }
        return;
    }
    if (ui.view == V_SLICE) {
        chp_break_t *b = edb();
        if (wi >= 0 && down) {
            int s = ui.kpage * 16 + wi;
            if (s < b->nsl) {
                ui.scur = (uint8_t)s;
                chp_audition(proj.brk, s);
                ui.live_last = (int8_t)s;
            }
        } else if (bi >= 0 && bi <= 4 && down && b->len) {
            b->tag[ui.scur] = (uint8_t)(bi == 4 ? TAG_NONE : TAG_K + bi);
            ui.dirty_k = 1;
        }
        return;
    }
    if (ui.view == V_GEN && down) {
        if (wi >= 0) {
            generate(wi);
        } else if (bi >= 0 && bi < NSTYLES) {
            knob_set(P_STYLE, bi);
            say("STYLE", CHP_STYLE_NAME[bi]);
        } else if (bi == 10) {
            proj.seed = gen_hash(proj.seed, plat_ms());
            chp_seed(proj.seed);
            ui.dirty_p = 1;
            say("NEW DICE", "");
        }
        return;
    }
    if (ui.view == V_BREAKS && wi >= 0) {
        if (down && wi < CHP_NBRK) {
            select_break(wi);
            if (brk[wi].len)
                chp_audition(wi, 0);
        }
        return;
    }
    /* PLAY, SOUND, SETUP: the keyboard plays */
    if (wi >= 0) {
        if (down && held(B_ENV)) {
            ui.btn_used |= 1u << B_ENV;
            select_pattern(wi);
            return;
        }
        if (down && held(B_LFO)) {
            ui.btn_used |= 1u << B_LFO;
            select_break(wi);
            return;
        }
        if (down && held(B_SAVE) && ui.view == V_PLAY) {   /* SAVE + key: the pattern copied there */
            ui.btn_used |= 1u << B_SAVE;
            if (wi != proj.pat) {
                pats.pat[wi] = *edp();
                ui.dirty_b = 1;
                {
                    char t[12];
                    cat_i(cat(t, "TO P"), wi + 1, 0);
                    say("COPIED", t);
                }
            }
            return;
        }
        if (down) {
            int s = ui.kpage * 16 + wi;
            if (s < edb()->nsl) {
                ui.live_key[k] = (int8_t)s;
                ui.live_last = (int8_t)s;
                chp_live(s, 1, 15);
            }
        } else if (ui.live_key[k] >= 0) {
            chp_live(ui.live_key[k], 0, 0);
            ui.live_key[k] = -1;
        }
    } else if (bi >= 0) {
        chp_perf(bi, down);                      /* the black keys are PF_* in order */
    }
}

static void take_records(void)                    /* what the render recorded, into the pattern */
{
    chp_rec_t r;
    while (chp_rec_take(&r))
        if (r.pat < CHP_NPAT && r.step < CHP_STEPS) {
            pats.pat[r.pat].st[r.step] = r.st;
            ui.dirty_b = 1;
        }
}

static void input(void)
{
    uint32_t btn = plat_buttons(), keys = plat_keys(), ch, i;
    int32_t e;
    int k;
    ch = btn ^ ui.btn;
    ui.btn = btn;
    if ((btn & ((1u << B_OCTDN) | (1u << B_OCTUP))) == ((1u << B_OCTDN) | (1u << B_OCTUP)))
        ui.btn_used |= (1u << B_OCTDN) | (1u << B_OCTUP);   /* both held: update mode (main loop) */
    for (i = 0; i < NB; i++) {
        uint32_t m = 1u << i;
        if (!(ch & m))
            continue;
        if (btn & m) {
            ui.btn_used &= ~m;
            if (i == B_PLAY || i == B_REC || i == B_OCTDN || i == B_OCTUP)
                button((int)i);                  /* on the press: timing matters */
        } else if (!(ui.btn_used & m) && i != B_PLAY && i != B_REC && i != B_OCTDN && i != B_OCTUP) {
            button((int)i);
        }
    }
    ch = keys ^ ui.keys;
    ui.keys = keys;
    for (k = 0; k < NKEYS; k++)
        if (ch >> k & 1u)
            key(k, (int)(keys >> k & 1u), keys & ~(1u << k));
    if (btn || keys || ch)
        ui.act_t = plat_ms();
    take_records();
    if ((e = plat_enc(EN_SELECT)) != 0)
        turn(EN_SELECT, P_TEMPO, e);
    if ((e = plat_enc(EN_ALGO)) != 0) {           /* the break, through the ones there are */
        int b = proj.brk, d = e > 0 ? 1 : -1;
        do
            b += d;
        while (b >= 0 && b < CHP_NBRK && !brk[b].len);
        select_break(b);
    }
    if ((e = plat_enc(EN_PRESET)) != 0)
        select_pattern((int)proj.pat + (e > 0 ? 1 : -1));
    for (i = 0; i < 4; i++)
        if ((e = plat_enc(EN_K1 + (int)i)) != 0) {
            turn(EN_K1 + (int)i, VIEW_KNOB[ui.view][ui.page[ui.view]][i], e);
            ui.touched = (int8_t)i;
            ui.touch_until = plat_ms() + 1200u;
            ui.act_t = plat_ms();
        }
}

/* ---------------------------------------------------------------- screen --- */
/* three bands: the header (28 px), the main area (144), the knobs (68) */
#define HDR_H 28
#define MAIN_Y 28
#define MAIN_H 144
#define KNB_Y 172
#define KNB_H 68

static uint32_t hash(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }

static void cv_round(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t c)
{
    static const uint8_t IN[7][6] = {{0}, {1}, {1, 0}, {2, 1, 0}, {2, 1, 0, 0}, {3, 2, 1, 0, 0}, {4, 2, 1, 1, 0, 0}};
    int32_t j;
    if (r > 6)
        r = 6;
    if (r > h / 2)
        r = h / 2;
    cv_rect(x, y + r, w, h - 2 * r, c);
    for (j = 0; j < r; j++) {
        int32_t d = IN[r][j];
        cv_rect(x + d, y + j, w - 2 * d, 1, c);
        cv_rect(x + d, y + h - 1 - j, w - 2 * d, 1, c);
    }
}

static void text_c(int32_t cx, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    cv_text(cx - text_w(f, s) / 2, y, f, s, c);
}

static int32_t tag(int32_t x, const char *s, uint16_t bg, uint16_t fg)
{
    int32_t w = text_w(&FONT_XS, s) + 10;
    cv_round(x, 6, w, 16, 6, bg);
    cv_text(x + 5, 7, &FONT_XS, s, fg);
    return x + w + 4;
}

static int beat_on(void) { return chp_playing && chp_mstep % 4u == 0u && chp_pulse < 3u; }

static void draw_header(void)
{
    char t[24];
    int msg = plat_ms() < ui.msg_until;
    uint32_t h = hash(hash(2166136261u, ui.view | (uint32_t)msg << 8 | (uint32_t)ui.page[ui.view] << 9 |
                                            (uint32_t)ui.rec << 10 | (uint32_t)chp_ext << 11),
                      (uint32_t)chp_playing | (uint32_t)ui_dirty() << 2 | (uint32_t)beat_on() << 3 |
                          (uint32_t)proj.par[P_AUTO] << 8 | (uint32_t)proj.pat << 20 | (uint32_t)proj.brk << 26);
    h = hash(h, (uint32_t)proj.par[P_TEMPO] | (uint32_t)chp_cue << 12 | (uint32_t)chp_cur << 20);
    h = hash(h, (uint32_t)sx_loading() | (sx_total() ? sx_progress() * 20u / sx_total() : 0u) << 4);
    if (msg)
        h = hash(hash(h, (uint32_t)ui.msg[0][0] << 8 | (uint8_t)ui.msg[1][0]), ui.msg_until);
    if (chp_cue != CHP_NONE)
        h = hash(h, (ui.frame / 8u) & 1u);
    if (h == ui.sig[0])
        return;
    ui.sig[0] = h;
    cv_begin(240, HDR_H, K_DARK);
    if (sx_loading()) {                              /* a break coming in from the computer */
        int32_t w = sx_total() ? (int32_t)(sx_progress() * 220u / sx_total()) : 0;
        cv_text(10, 4, &FONT_B, "LOADING", K_ACC);
        cv_rect(10, 22, 220, 3, K_PANEL);
        cv_rect(10, 22, w, 3, K_ACC);
    } else if (msg) {
        int32_t x = cv_text(10, 6, &FONT_B, ui.msg[0], K_TEXT);
        cv_text(x + 8, 6, &FONT_B, ui.msg[1], K_ACC);
    } else {
        int32_t x = cv_text(10, 6, &FONT_B, VIEW_NAME[ui.view], K_TEXT);
        if (VIEW_PAGES[ui.view] > 1 && ui.page[ui.view])
            x = cv_text(x + 2, 6, &FONT_B, "+", K_DIM);
        cat_i(cat(t, "P"), (chp_playing ? chp_cur : proj.pat) + 1, 0);
        x = cv_text(x + 8, 6, &FONT_B, t, K_ACC);
        if (chp_cue != CHP_NONE && ((ui.frame / 8u) & 1u)) {
            cat_i(cat(t, ">"), chp_cue + 1, 0);
            x = cv_text(x + 3, 6, &FONT_S, t, K_HEAD);
        }
        x += 6;
        if (ui.rec)
            x = tag(x, "REC", K_RED, K_TEXT);
        if (proj.par[P_AUTO])
            x = tag(x, "AUTO", K_ACC, K_DARK);
        if (ui_dirty())
            cv_round(x, 12, 5, 5, 2, K_DIM);
        if (chp_ext)
            cat(t, "EXT");
        else
            cat_i(t, proj.par[P_TEMPO], 0);
        x = 230 - text_w(&FONT_S, t);
        cv_text(x, 6, &FONT_S, t, chp_playing ? K_TEXT : K_DIM);
        x -= 16;
        if (chp_playing) {
            cv_round(x, 9, 10, 10, 5, beat_on() ? K_ACC : K_LINE);
        } else {
            int32_t j;
            for (j = 0; j < 5; j++)
                cv_rect(x + j, 9 + j, 1, 11 - 2 * j, K_DIM);
        }
    }
    cv_blit(0, 0);
}

/* the break as a waveform: 240 columns, min and max, each column in its slice's tag colour */
static void wave_overview(void)
{
    chp_break_t *b = edb();
    uint32_t sig = hash(hash(sx_gen, proj.brk), b->len ^ b->off), c;
    if (sig == ui.wv_sig)
        return;
    ui.wv_sig = sig;
    for (c = 0; c < 240u; c++) {
        uint32_t a = b->len ? b->len * c / 240u : 0, e = b->len ? b->len * (c + 1u) / 240u : 0, i;
        int lo = 0, hi = 0;
        for (i = a; i < e; i += 2u) {
            int v = b->fmt == FMT_ULAW ? 0 : (int8_t)mem[b->off + i];
            if (b->fmt == FMT_ULAW) {                /* the mu-law byte's size, roughly */
                int u = ~mem[b->off + i] & 0xFF, m = (u >> 4) & 7;
                v = (m * 16 + 8) * ((u & 0x80) ? -1 : 1);
            }
            lo = v < lo ? v : lo;
            hi = v > hi ? v : hi;
        }
        ui.wv_lo[c] = (int8_t)lo;
        ui.wv_hi[c] = (int8_t)hi;
    }
}

static void draw_wave(int32_t y0, int32_t h, int hl_slice, int show_head)
{
    chp_break_t *b = edb();
    int32_t mid = y0 + h / 2, c;
    int s = 0;
    wave_overview();
    cv_rect(0, y0, 240, h, K_PANEL);
    if (!b->len) {
        text_c(120, mid - 8, &FONT_S, "NO BREAK", K_DIM);
        return;
    }
    for (c = 0; c < 240; c++) {
        uint32_t pos = b->len * (uint32_t)c / 240u;
        int32_t lo = ui.wv_lo[c] * (h / 2 - 2) / 128, hi = ui.wv_hi[c] * (h / 2 - 2) / 128;
        uint16_t col;
        while (s + 1 < b->nsl && b->start[s + 1] <= pos)
            s++;
        col = TAG_C[b->tag[s]];
        if (s == hl_slice)
            cv_rect(c, y0, 1, h, K_LINE);
        cv_rect(c, mid - hi, 1, hi - lo + 1, col);
    }
    for (s = 0; s < b->nsl; s++) {                  /* the cuts */
        int32_t x = (int32_t)(b->start[s] * 240u / b->len);   /* < 2^32: 258048 x 240 */
        cv_rect(x, y0, 1, 5, s == hl_slice ? K_ACC : K_TEXT);
    }
    if (show_head && chp_vbrk == proj.brk && chp_vslice != CHP_NONE) {
        int32_t x = (int32_t)(chp_vpos * 240u / b->len);
        cv_rect(x, y0, 2, h, K_ACC);
    }
}

/* the steps of a page: a cell each, the source in its tag's colour, rolls as dots, reverse as a bar */
static void draw_steps(const chp_step_t *st, int len, int page, int32_t y0, int32_t h, int head, int cursor)
{
    int i;
    for (i = 0; i < 16; i++) {
        int n = page * 16 + i;
        int32_t x = 8 + i * 14;
        const chp_step_t *s = &st[n];
        char t[4];
        uint16_t bg = n >= len ? K_DARK : (i & 3) == 0 ? K_LINE : K_PANEL, fg;
        if (n == head)
            bg = K_HEAD;
        cv_rect(x, y0, 13, h, bg);
        if (n == cursor) {
            cv_rect(x, y0, 13, 2, K_ACC);
            cv_rect(x, y0 + h - 2, 13, 2, K_ACC);
        }
        if (n >= len)
            continue;
        t[0] = 0;
        src_name(s->src, t);
        fg = n == head ? K_DARK : s->src == SRC_NONE ? K_LINE : TAG_C[src_tag(s->src)];
        if (s->src == SRC_NONE && n != head)
            fg = RGB(70, 130, 200);
        text_c(x + 7, y0 + 3, t[1] ? &FONT_XS : &FONT_S, t, fg);
        if (s->fx & FX_ROLL) {                      /* roll: a dot per doubling */
            int r, nr = (s->fx & FX_ROLL) > 4 ? 3 : (s->fx & FX_ROLL) > 2 ? 2 : 1;
            for (r = 0; r < nr; r++)
                cv_rect(x + 2 + r * 4, y0 + h - 7, 2, 2, (s->fx & FX_RAMP_DN) ? K_DIM : K_ACC);
        }
        if (s->fx & FX_REV)
            cv_rect(x + 2, y0 + h - 4, 9, 1, K_RED);
        if (s->pitch)
            cv_rect(x + 11, s->pitch > 0 ? y0 + 3 : y0 + h - 9, 1, 5, K_HEAD);
        if (s->lev < 12)
            cv_rect(x + 1, y0 + 3, 1, 3, K_DIM);
    }
}

static int shown_page(void)
{
    if (chp_playing && proj.pat == chp_cur && (ui.view == V_PLAY || ui.view >= V_SOUND))
        return chp_step / 16u;
    return ui.kpage;
}

static void draw_info_play(int32_t y0)
{
    char t[40], *e;
    uint32_t i;
    int32_t x = 8;
    chp_break_t *b = edb();
    e = cat(t, b->len ? b->name : "-");
    cv_text(8, y0, &FONT_B, t, K_TEXT);
    e = cat_i(t, (int)(chp_break_bpm(b) + 0.5f), 0);
    e = cat(e, " BPM  ");
    e = cat_i(e, b->beats, 0);
    cat(e, " BEATS");
    cv_text(232 - text_w(&FONT_XS, t), y0 + 2, &FONT_XS, t, K_DIM);
    y0 += 20;
    for (i = 0; i < NPERF; i++)                       /* the perform fx held */
        if (chp_perf_on & (1u << i))
            x = cv_text(x, y0, &FONT_B, CHP_PERF_NAME[i], K_ACC) + 8;
    if (x == 8) {
        e = cat(t, ui.kpage ? "KEYS 17-32" : "KEYS 1-16");
        if (chp_work_on)
            e = cat(e, "  VARIED");
        cv_text(8, y0, &FONT_XS, t, K_DIM);
    }
}

static void draw_main(void)
{
    const chp_pattern_t *p = edp();
    uint32_t h = hash(2166136261u, ui.view | (uint32_t)ui.page[ui.view] << 4 | (uint32_t)proj.pat << 8 |
                                       (uint32_t)proj.brk << 16 | (uint32_t)ui.kpage << 20), s;
    const chp_step_t *st = p->st;
    int head = chp_playing && chp_cur == proj.pat ? chp_step : -1;
    if (ui.view == V_PLAY && chp_playing && chp_cur == proj.pat && chp_work_on)
        st = chp_work;                                /* what AUTO plays this bar */
    for (s = 0; s < CHP_STEPS; s++)
        h = hash(h, st[s].src | (uint32_t)st[s].fx << 8 | (uint32_t)(uint8_t)st[s].pitch << 16 | (uint32_t)st[s].lev << 24);
    h = hash(h, p->len | (uint32_t)ui.cur << 8 | (uint32_t)ui.scur << 16 | (uint32_t)head << 24);
    h = hash(hash(h, chp_vslice | (uint32_t)chp_vbrk << 8 | (uint32_t)chp_perf_on << 16), sx_gen);
    h = hash(h, edb()->nsl | (uint32_t)proj.par[P_STYLE] << 8 | (uint32_t)ui.wv_sig << 12);
    if (ui.view == V_SLICE)
        h = hash(hash(h, edb()->start[ui.scur]), edb()->tag[ui.scur] | (uint32_t)edb()->beats << 8);
    if (ui.view == V_BREAKS)
        h = hash(h, (uint32_t)store_used());
    if (h == ui.sig[1])
        return;
    ui.sig[1] = h;
    cv_begin(240, MAIN_H, K_BG);
    switch (ui.view) {
    case V_STEP: {
        const chp_step_t *c = eds();
        char t[24], *e;
        int32_t x;
        t[0] = 0;
        e = cat_i(cat(t, "STEP "), ui.cur + 1, 0);
        cv_text(8, 6, &FONT_S, t, K_DIM);
        t[0] = 0;
        src_name(c->src, t);
        x = cv_text(70, 0, &FONT_L, c->src == SRC_NONE ? "RUN" : c->src == SRC_CUT ? "CUT" : t,
                    TAG_C[src_tag(c->src)] == TAG_C[0] ? K_TEXT : TAG_C[src_tag(c->src)]);
        x += 8;
        if (c->fx & FX_ROLL) {
            e = cat(cat_i(t, CHP_ROLL_N[c->fx & FX_ROLL], 0), "x");
            if (c->fx & FX_RAMP_UP)
                e = cat(e, "^");
            if (c->fx & FX_RAMP_DN)
                e = cat(e, "v");
            x = cv_text(x, 8, &FONT_B, t, K_ACC) + 6;
        }
        if (c->fx & FX_REV)
            x = cv_text(x, 8, &FONT_B, "REV", K_RED) + 6;
        if (c->pitch) {
            cat_i(t, c->pitch, 1);
            x = cv_text(x, 8, &FONT_B, t, K_HEAD) + 6;
        }
        (void)e;
        draw_steps(p->st, p->len, 0, 40, 30, head, ui.cur);
        if (p->len > 16)
            draw_steps(p->st, p->len, 1, 74, 30, head, ui.cur);
        draw_wave(110, 32, SRC_IS_SLICE(c->src) ? (c->src - SRC_SLICE) % (edb()->nsl ? edb()->nsl : 1) : -1, 1);
        break;
    }
    case V_SLICE: {
        chp_break_t *b = edb();
        char t[24], *e;
        e = cat_i(cat(t, "SLICE "), ui.scur + 1, 0);
        e = cat(e, "/");
        cat_i(e, b->nsl, 0);
        cv_text(8, 6, &FONT_S, t, K_DIM);
        cv_text(100, 0, &FONT_L, CHP_TAG_NAME[b->tag[ui.scur]][0] == '-' ? "-" : CHP_TAG_NAME[b->tag[ui.scur]],
                TAG_C[b->tag[ui.scur]] == TAG_C[0] ? K_TEXT : TAG_C[b->tag[ui.scur]]);
        draw_wave(40, 70, ui.scur, 1);
        {   /* the slices as keys: their tags */
            int i;
            for (i = 0; i < 16; i++) {
                int sl = ui.kpage * 16 + i;
                int32_t x = 8 + i * 14;
                char k[4];
                if (sl >= b->nsl)
                    continue;
                k[0] = 0;
                cat(k, CHP_TAG_NAME[b->tag[sl]]);
                cv_rect(x, 116, 13, 22, sl == ui.scur ? K_HEAD : K_PANEL);
                text_c(x + 7, 119, &FONT_S, k, sl == ui.scur ? K_DARK : TAG_C[b->tag[sl]]);
            }
        }
        break;
    }
    case V_BREAKS: {
        int i;
        char t[32], *e;
        for (i = 0; i < CHP_NBRK; i++) {
            const chp_break_t *b = &brk[i];
            int32_t y = 4 + i * 15;
            uint16_t c = i == proj.brk ? K_ACC : b->len ? K_TEXT : K_LINE;
            if (i == proj.brk)
                cv_rect(0, y - 1, 240, 15, K_PANEL);
            e = cat_i(t, i + 1, 0);
            cv_text(8, y, &FONT_XS, t, c);
            if (!b->len) {
                cv_text(26, y, &FONT_XS, "-", K_LINE);
                continue;
            }
            cv_text(26, y, &FONT_XS, b->name, c);
            e = cat_i(t, (int)(chp_break_bpm(b) + 0.5f), 0);
            e = cat(e, " BPM  ");
            e = cat_i(e, b->beats, 0);
            e = cat(e, "b  ");
            e = cat_i(e, b->nsl, 0);
            e = cat(e, "sl  ");
            e = cat_i(e, b->rate / 1000u, 0);
            cat(e, b->fmt == FMT_ULAW ? "k u" : "k");
            cv_text(232 - text_w(&FONT_XS, t), y, &FONT_XS, t, c);
        }
        {
            int32_t w = (int32_t)(store_used() * 224u / CHP_MEM);
            cv_rect(8, 128, 224, 6, K_PANEL);
            cv_rect(8, 128, w, 6, K_DIM);
            e = cat_i(t, (int)(store_used() / 1024u), 0);
            e = cat(e, " / ");
            e = cat_i(e, (int)(CHP_MEM / 1024u), 0);
            cat(e, store_is_factory() ? " KB  FACTORY" : " KB");
            cv_text(8, 112, &FONT_XS, t, K_DIM);
        }
        break;
    }
    case V_GEN: {
        char t[24];
        text_c(120, 2, &FONT_L, CHP_STYLE_NAME[proj.par[P_STYLE] % NSTYLES], K_ACC);
        cat_i(cat(t, "P"), proj.pat + 1, 0);
        cv_text(8, 12, &FONT_S, t, K_DIM);
        draw_steps(p->st, p->len, 0, 44, 30, head, -1);
        if (p->len > 16)
            draw_steps(p->st, p->len, 1, 78, 30, head, -1);
        cv_text(8, 116, &FONT_XS, "WHITE KEYS: A NEW PATTERN   ENV: UNDO", K_DIM);
        break;
    }
    case V_SOUND: {
        static const char *const STAGE[] = {"PAULA", "CRUSH", "AMIGA", "DRIVE", "FILTER", "ECHO"};
        int on[6], i;
        int32_t x = 8, y = 96;
        on[0] = proj.par[P_INTERP] == 0;
        on[1] = proj.par[P_CRUSH] != 0;
        on[2] = proj.par[P_AMIGA] != 0;
        on[3] = proj.par[P_DRIVE] != 0;
        on[4] = proj.par[P_FILTER] != 0;
        on[5] = proj.par[P_ECHO] != 0;
        draw_wave(4, 60, -1, 1);
        draw_info_play(68);
        for (i = 0; i < 6; i++) {                    /* the chain, lit where it does something */
            int32_t w = text_w(&FONT_XS, STAGE[i]) + 8;
            if (i == 3) {
                x = 8;
                y += 22;
            }
            cv_round(x, y, w, 18, 5, on[i] ? K_ACC : K_PANEL);
            cv_text(x + 4, y + 2, &FONT_XS, STAGE[i], on[i] ? K_DARK : K_DIM);
            x += w + 14;
            if (i % 3 != 2)
                cv_rect(x - 12, y + 9, 10, 1, K_DIM);
        }
        break;
    }
    case V_SETUP: {
        char t[40], *e;
        e = cat(t, "CHOPPAGE ");
        cat(e, FM_VERSION);
        cv_text(8, 6, &FONT_B, t, K_TEXT);
        e = cat_i(cat(t, "AUDIO "), (int)plat_cpu_pct(), 0);
        e = cat(e, "%  XRUNS ");
        cat_i(e, (int)plat_xruns(), 0);
        cv_text(8, 30, &FONT_XS, t, K_DIM);
        e = cat_i(cat(t, "BREAKS "), store_count(), 0);
        e = cat(e, "  ");
        e = cat_i(e, (int)(store_used() / 1024u), 0);
        cat(e, " KB");
        cv_text(8, 46, &FONT_XS, t, K_DIM);
        cv_text(8, 70, &FONT_XS, "LOAD BREAKS FROM A COMPUTER:", K_DIM);
        cv_text(8, 86, &FONT_XS, "TOOLS/CHP_BREAKS.PY OR BREAKS.HTML", K_TEXT);
        cv_text(8, 110, &FONT_XS, "OCT- + OCT+ 5 S: UPDATE MODE", K_DIM);
        break;
    }
    default:                                          /* PLAY */
        draw_wave(4, 64, chp_vbrk == proj.brk ? (int)chp_vslice : -1, 1);
        draw_steps(st, p->len, shown_page(), 72, 30, head, -1);
        draw_info_play(106);
        break;
    }
    cv_blit(0, MAIN_Y);
}

static void draw_knobs(void)
{
    const uint8_t *kn = VIEW_KNOB[ui.view][ui.page[ui.view]];
    uint32_t h = hash(2166136261u, ui.view | (uint32_t)ui.touched << 8 | (uint32_t)ui.page[ui.view] << 16), i;
    int now_touch = plat_ms() < ui.touch_until;
    for (i = 0; i < 4; i++)
        h = hash(h, (uint32_t)knob_get(kn[i]));
    h = hash(hash(h, (uint32_t)now_touch), (uint32_t)ui.cur | (uint32_t)ui.scur << 8 | (uint32_t)proj.brk << 16);
    if (h == ui.sig[2])
        return;
    ui.sig[2] = h;
    cv_begin(240, KNB_H, K_BG);
    cv_round(4, 2, 232, KNB_H - 6, 6, K_PANEL);
    for (i = 0; i < 4; i++) {
        int k = kn[i], lo, hi, v;
        int32_t x = (int32_t)i * 60, cx = x + 30, bw;
        int hot = now_touch && ui.touched == (int)i;
        char t[16];
        if (k == K_NONE)
            continue;
        if (i)
            cv_rect(x, 14, 1, KNB_H - 30, K_LINE);
        knob_range(k, &lo, &hi);
        v = knob_get(k);
        text_c(cx, 6, &FONT_XS, knob_name(k), hot ? K_TEXT : K_DIM);
        knob_text(k, t);
        {   /* the value: big, unless it has lower case (the big face has none) or is long */
            const char *q = t;
            int lower = 0;
            for (; *q; q++)
                lower |= *q >= 'a' && *q <= 'z';
            if (lower || text_w(&FONT_M, t) > 56)
                text_c(cx, 26, &FONT_B, t, hot ? K_ACC : K_TEXT);
            else
                text_c(cx, 22, &FONT_M, t, hot ? K_ACC : K_TEXT);
        }
        bw = hi > lo ? (int32_t)(44 * (v - lo) / (hi - lo)) : 0;
        cv_round(cx - 22, 50, 44, 6, 3, K_LINE);
        if (bw)
            cv_round(cx - 22, 50, bw < 6 ? 6 : bw, 6, 3, hot ? K_ACC : K_TEXT);
    }
    cv_blit(0, KNB_Y);
}

static void leds(void)
{
    uint32_t b = 0, k = 0, i, blink = (ui.frame / 8u) & 1u;
    const chp_pattern_t *p = edp();
    b |= 1u << VIEW_BTN[ui.view];
    if (VIEW_PAGES[ui.view] > 1 && ui.page[ui.view] && blink)  /* the second page: its button blinks */
        b &= ~(1u << VIEW_BTN[ui.view]);
    if (chp_playing)
        b |= chp_pulse < 3u || chp_mstep % 4u == 0u ? 1u << B_PLAY : 0u;
    if (ui.rec)
        b |= 1u << B_PLAY;
    if (proj.par[P_AUTO])
        b |= 1u << B_ARP;
    if (held(B_ENV))
        b |= 1u << B_ENV;
    if (held(B_LFO))
        b |= 1u << B_LFO;
    b |= ui.kpage ? 1u << B_OCTUP : 1u << B_OCTDN;
    if (proj.leds) {
        if (ui.view == V_STEP || ui.view == V_GEN) {   /* the steps that play, the playhead dark */
            for (i = 0; i < 16u; i++) {
                int n = ui.kpage * 16 + (int)i;
                int on = n < p->len && p->st[n].src != SRC_NONE;
                if (chp_playing && chp_cur == proj.pat && chp_step == n)
                    on = !on;
                if (ui.view == V_STEP && n == ui.cur && blink)
                    on = 1;
                if (on)
                    k |= 1u << WHITE_K[i];
            }
            if (ui.view == V_GEN)
                k |= 1u << BLACK_K[proj.par[P_STYLE] % NSTYLES];
        } else if (ui.view == V_BREAKS) {
            for (i = 0; i < CHP_NBRK; i++)
                if (brk[i].len && (i != proj.brk || blink))
                    k |= 1u << WHITE_K[i];
        } else {                                      /* the slice sounding; the perform fx held */
            if (chp_vbrk == proj.brk && chp_vslice != CHP_NONE && chp_vslice / 16u == ui.kpage)
                k |= 1u << WHITE_K[chp_vslice % 16u];
            if (ui.view == V_SLICE && ui.scur / 16u == ui.kpage)
                k |= 1u << WHITE_K[ui.scur % 16u];
            if (ui.view != V_SLICE)
                for (i = 0; i < NPERF; i++)
                    if (chp_perf_on & (1u << i))
                        k |= 1u << BLACK_K[i];
        }
    }
    plat_glow(proj.leds == 1);
    plat_play_red(ui.rec);
    plat_leds(b, k);
}

/* AUTOSAVE: a few seconds after a change, while stopped, nothing sounds and nothing is touched (a
 * flash erase silences the audio for a moment) */
#define AUTOSAVE_QUIET 4000u
static void autosave(void)
{
    uint32_t now = plat_ms();
    if (!ui_dirty() || chp_playing || ui.btn || ui.keys || now - ui.act_t < AUTOSAVE_QUIET || chp_level > 0.001f ||
        sx_loading())
        return;
    if ((!ui.dirty_b || pats_save() == 0) && (!ui.dirty_p || project_save() == 0) && (!ui.dirty_k || kits_save() == 0))
        ui.dirty_p = ui.dirty_b = ui.dirty_k = 0;
    else
        ui.act_t = now;                          /* failed: try again later, not every frame */
}

void ui_init(void)
{
    uint32_t i;
    memset(&ui, 0, sizeof ui);
    ui.touched = -1;
    ui.live_last = -1;
    for (i = 0; i < NKEYS; i++)
        ui.live_key[i] = -1;
    ui.wv_sig = 0xFFFFFFFFu;
    lcd_fill(0, 0, 240, 240, K_BG);
}

void ui_frame(void)
{
    input();
    autosave();
    draw_header();
    draw_main();
    draw_knobs();
    leds();
    ui.frame++;
}

void ui_input_only(void) { input(); }
