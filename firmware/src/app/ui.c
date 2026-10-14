/* SPDX-License-Identifier: GPL-3.0-only */
/* FM-303 UI: the panel and the screen. Part of the unity build (after gfx.c and project.c); the host
 * simulator includes it too. Structure and drawing helpers from FoMni's ui.c.
 *
 *   HOME   PLAY    white keys pick the pattern: 1A..8A, then 1B..8B of the group; hold one and press
 *                  another to chain the two and all between. Black keys 1-4 pick the group (I..IV).
 *                  (With KEYS on, the keyboard plays instead.) KNOB 1-4: Cutoff, Reso, EnvMod, Decay
 *   SEQ    PITCH   the 303's pitch mode: each key writes the next note of the pattern; hold ENV for an
 *                  accent, LFO for a slide (tapped alone: they flip the accent / slide at the cursor).
 *                  KNOB 1 cursor, 2 its note, 3 accent, 4 slide
 *   SEQ    TIME    (SEQ again) the 303's time mode: white keys are the 16 steps, a tap turns one
 *                  NOTE > TIE > REST; black keys 1-3 write NOTE, TIE, REST at the cursor and step on.
 *                  KNOB 1 length, 2 sixteenths / triplets, 3 cursor, 4 the step
 *   FX     SOUND   Accent, Wave, Drive, Drive type     (the keyboard plays / transposes here)
 *   SEL    MORE    Tune, Volume, Slide time, Accent decay
 *   GLO    SETUP   MIDI channel, Note out, Clock out, Lights
 *   EDIT           the next view
 *
 *   PLAY   start / stop            ARP   KEYS: on PLAY, the keyboard plays the 303 (stopped) or
 *   OCT- / OCT+    octave                transposes the pattern from C4 (playing)
 *   REC    (PITCH, TIME) the cursor back to the first step
 *   SELECT tempo   ALGORITHM accent   PRESETS the pattern (all 64)
 *   SAVE   saves; held + white key (PLAY): copies the pattern there; held + REC (PITCH, TIME): clears it
 *          (press twice). It also saves by itself, a few seconds after a change, while stopped. */

enum { V_PLAY, V_PITCH, V_TIME, V_SOUND, V_MORE, V_SETUP, NVIEWS };
static const char *const VIEW_NAME[NVIEWS] = {"Play", "Pitch", "Time", "Sound", "More", "Setup"};
/* the four knobs of each view: a parameter, or one of these */
enum { K_PCUR = 100, K_PNOTE, K_PACC, K_PSLIDE, K_LEN, K_TRIP, K_TCUR, K_TIME, K_LEDS, K_NONE };
static const uint8_t VIEW_KNOB[NVIEWS][4] = {
    {P_CUTOFF, P_RESO, P_ENVMOD, P_DECAY},
    {K_PCUR, K_PNOTE, K_PACC, K_PSLIDE},
    {K_LEN, K_TRIP, K_TCUR, K_TIME},
    {P_ACCENT, P_WAVE, P_DRIVE, P_DRVTYPE},
    {P_TUNE, P_VOLUME, P_SLIDE, P_ACCDEC},
    {P_MIDICH, P_NOTEOUT, P_CLKOUT, K_LEDS},
};
static const uint8_t VIEW_BTN[NVIEWS] = {B_HOME, B_SEQ, B_SEQ, B_FX, B_SEL, B_GLO};

static const uint8_t WHITE_K[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};
static const uint8_t BLACK_K[11] = {1, 3, 5, 8, 10, 13, 15, 17, 20, 22, 25};
#define KEY_C4 7                       /* key 0 is F3 (MIDI 53); the C4 key is the transpose centre */

/* the screen's colours: a TB-303's silver panel, black print, the orange of its accent */
#define K_BG RGB(200, 202, 198)
#define K_PANEL RGB(184, 186, 182)
#define K_BEAT RGB(190, 192, 188)
#define K_LINE RGB(158, 160, 156)
#define K_DIM RGB(116, 118, 116)
#define K_TEXT RGB(24, 24, 26)
#define K_ACC RGB(214, 72, 26)
#define K_ACC_T RGB(226, 160, 128)
#define K_SLD RGB(30, 84, 160)
#define K_HEAD RGB(250, 250, 246)
#define K_CUR RGB(236, 206, 188)            /* the cursor's column */

static struct {
    uint8_t view;
    uint32_t btn, keys, btn_used;
    uint32_t enc_t[NE];
    uint8_t keyb;                      /* KEYS (ARP): on PLAY, the keyboard plays / transposes */
    int8_t transpose;
    uint8_t pcur, tcur;                /* the pitch and time cursors */
    int8_t chain_key;                  /* PLAY: the white key held first (a chain's start), -1 none */
    uint8_t midi_held;                 /* MIDI in: notes held on our channel */
    uint8_t dirty_p, dirty_b;          /* the settings / the patterns changed since the last save */
    uint32_t act_t, clear_t;
    char msg[2][24];
    uint32_t msg_until;
    int8_t touched;                    /* the knob turned last (0..3), shown bright for a moment */
    uint32_t touch_until;
    uint32_t frame;
    uint32_t sig[3];                   /* what each band shows: redrawn when it changes */
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
int ui_dirty(void) { return ui.dirty_p || ui.dirty_b; }

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

static tb_pattern_t *edp(void) { return &bank.pat[proj.edit]; }
static int held(int b) { return (int)((ui.btn >> b) & 1u); }
static int key_note(int k)             /* the note key k plays at the keyboard's octave */
{
    int n = 53 + k + ((int)proj.oct - 1) * 12 - 24;
    return n < TB_NOTE_LO ? TB_NOTE_LO : n > TB_NOTE_HI ? TB_NOTE_HI : n;
}
static int pitch_wrap(void)            /* the pitch cursor wraps at the notes the time line plays */
{
    int n = tb_notes_used(edp());
    return n < 1 ? 1 : n;
}

/* ------------------------------------------------------------ values --- */
static int knob_get(int k)
{
    tb_pattern_t *p = edp();
    switch (k) {
    case K_PCUR: return ui.pcur;
    case K_PNOTE: return p->note[ui.pcur];
    case K_PACC: return (p->flag[ui.pcur] & TP_ACCENT) != 0;
    case K_PSLIDE: return (p->flag[ui.pcur] & TP_SLIDE) != 0;
    case K_LEN: return p->len;
    case K_TRIP: return p->triplet;
    case K_TCUR: return ui.tcur;
    case K_TIME: return tb_time(p, ui.tcur);
    case K_LEDS: return proj.leds;
    case K_NONE: return 0;
    default: return proj.par[k];
    }
}
static void knob_range(int k, int *lo, int *hi)
{
    *lo = 0;
    switch (k) {
    case K_PCUR: case K_TCUR: *hi = TB_STEPS - 1; break;
    case K_PNOTE: *lo = TB_NOTE_LO; *hi = TB_NOTE_HI; break;
    case K_PACC: case K_PSLIDE: case K_TRIP: *hi = 1; break;
    case K_LEN: *lo = 1; *hi = TB_STEPS; break;
    case K_TIME: case K_LEDS: *hi = 2; break;
    case K_NONE: *hi = 0; break;
    default:
        *lo = fm_param_info(k)->lo;
        *hi = fm_param_info(k)->hi;
        break;
    }
}
static const char *knob_name(int k)
{
    static const char *const N[] = {"Pitch", "Note", "Accent", "Slide", "Length", "Timing", "Step", "Time",
                                    "Lights", ""};
    return k >= K_PCUR ? N[k - K_PCUR] : fm_param_info(k)->name;
}
static void knob_text(int k, char *b)
{
    static const char *const TIME_N[3] = {"Rest", "Note", "Tie"};
    static const char *const LEDS_N[3] = {"Off", "On", "Keys"};
    int v = knob_get(k);
    b[0] = 0;
    switch (k) {
    case K_PCUR: case K_TCUR:
        cat_i(b, v + 1, 0);
        break;
    case K_LEN:
        cat_i(b, v, 0);
        break;
    case K_PNOTE: tb_note_name(v, b); break;
    case K_PACC: case K_PSLIDE: cat(b, v ? "On" : "Off"); break;
    case K_TRIP: cat(b, v ? "Trip" : "16th"); break;
    case K_TIME: cat(b, TIME_N[v % 3]); break;
    case K_LEDS: cat(b, LEDS_N[v % 3]); break;
    case K_NONE: break;
    default: fm_param_text(k, v, b); break;
    }
}

static void knob_set(int k, int v)
{
    int lo, hi;
    tb_pattern_t *p = edp();
    knob_range(k, &lo, &hi);
    v = v < lo ? lo : v > hi ? hi : v;
    if (v == knob_get(k))
        return;
    switch (k) {
    case K_PCUR: ui.pcur = (uint8_t)v; return;
    case K_TCUR: ui.tcur = (uint8_t)v; return;
    case K_PNOTE: p->note[ui.pcur] = (uint8_t)v; break;
    case K_PACC: p->flag[ui.pcur] ^= TP_ACCENT; break;
    case K_PSLIDE: p->flag[ui.pcur] ^= TP_SLIDE; break;
    case K_LEN:
        p->len = (uint8_t)v;
        if (ui.tcur >= v)
            ui.tcur = (uint8_t)(v - 1);
        break;
    case K_TRIP: p->triplet = (uint8_t)v; break;
    case K_TIME: tb_set_time(p, ui.tcur, v); break;
    case K_LEDS: proj.leds = (uint8_t)v; ui.dirty_p = 1; return;
    default:
        proj.par[k] = (int16_t)v;
        fm_set(k, v);
        ui.dirty_p = 1;
        return;
    }
    ui.dirty_b = 1;                              /* the pattern changed */
}

/* knob acceleration by speed: a slow turn is one step a detent, a quick one up to 6 */
static uint8_t accel_off;              /* the host simulator's "spin": exact steps */
static int32_t accel(int role, int32_t s, int range)
{
    uint32_t now = plat_ms(), dt = now - ui.enc_t[role], a = (uint32_t)(s < 0 ? -s : s), m;
    ui.enc_t[role] = now;
    if (range <= 24 || !a || accel_off)
        return s;
    if (a > 1)
        dt /= a;
    m = dt < 15u ? 6u : dt < 30u ? 4u : dt < 60u ? 2u : 1u;
    return s * (int32_t)m;
}

static void turn(int role, int k, int32_t e)
{
    int lo, hi;
    if (!e || k == K_NONE)
        return;
    knob_range(k, &lo, &hi);
    knob_set(k, knob_get(k) + accel(role, e, hi - lo));
}

/* ---------------------------------------------------------- patterns --- */
static void select_pattern(int lo, int hi)
{
    char a[8], b[8], t[20];
    proj.edit = (uint8_t)lo;
    proj.group = (uint8_t)(lo / TB_PER_GROUP);
    ui.dirty_p = 1;
    if (ui.pcur >= pitch_wrap())
        ui.pcur = 0;
    if (ui.tcur >= edp()->len)
        ui.tcur = 0;
    fm_select(lo, hi);
    tb_slot_name(lo, a);
    if (hi > lo) {                               /* "I 1A - I 2B" */
        tb_slot_name(hi, b);
        cat(cat(cat(t, a), "-"), b);
    } else {
        cat(t, a);
    }
    say(fm_playing ? "NEXT" : "PATTERN", t);
}

static void copy_pattern(int dst)
{
    char a[8], b[8], t[20];
    if (dst == proj.edit)
        return;
    bank.pat[dst] = *edp();
    ui.dirty_b = 1;
    tb_slot_name(proj.edit, a);
    tb_slot_name(dst, b);
    cat(cat(cat(t, a), " > "), b);
    say("COPIED", t);
}

static void clear_pattern(void)
{
    char a[8];
    tb_slot_name(proj.edit, a);
    if (plat_ms() < ui.clear_t) {                /* the second press */
        tb_clear(edp());
        ui.pcur = ui.tcur = 0;
        ui.dirty_b = 1;
        ui.clear_t = 0;
        say("CLEARED", a);
    } else {
        ui.clear_t = plat_ms() + 2000u;
        say("CLEAR?", "SAVE+REC AGAIN");
    }
}

/* ------------------------------------------------------------- keyboard --- */
/* a key played: a live note while stopped (legato over a key still held: a slide), or the pattern's
 * transposition while playing (the C4 key is none) */
static void set_transpose(int t)
{
    char b[8];
    t = t < -24 ? -24 : t > 24 ? 24 : t;
    ui.transpose = (int8_t)t;
    fm_transpose(t);
    cat_i(b, t, 1);
    say("TRANSPOSE", b);
}

static void live_key(int k, int down, uint32_t others)
{
    if (fm_playing) {
        if (down)
            set_transpose(k - KEY_C4);
        return;
    }
    if (down) {
        fm_note(key_note(k), held(B_ENV), others != 0 || held(B_LFO));
        ui.btn_used |= (ui.btn & ((1u << B_ENV) | (1u << B_LFO)));
    } else if (!others) {
        fm_note_off();
    }
}

static void pitch_key(int k)
{
    tb_pattern_t *p = edp();
    int note = key_note(k), acc = held(B_ENV), sld = held(B_LFO);
    p->note[ui.pcur] = (uint8_t)note;
    p->flag[ui.pcur] = (uint8_t)((acc ? TP_ACCENT : 0) | (sld ? TP_SLIDE : 0));
    ui.btn_used |= (ui.btn & ((1u << B_ENV) | (1u << B_LFO)));
    ui.dirty_b = 1;
    if (!fm_playing)                             /* stopped: hear what was written */
        fm_note(note, acc, 0);
    ui.pcur = (uint8_t)((ui.pcur + 1) % pitch_wrap());
}

static void time_write(int t)
{
    tb_pattern_t *p = edp();
    tb_set_time(p, ui.tcur, t);
    ui.dirty_b = 1;
    ui.tcur = (uint8_t)((ui.tcur + 1) % p->len);
}

static void time_tap(int s)
{
    tb_pattern_t *p = edp();
    ui.tcur = (uint8_t)s;
    if (s >= p->len) {                           /* past the end: the pattern grows to it */
        char b[8];
        p->len = (uint8_t)(s + 1);
        cat_i(b, s + 1, 0);
        say("LENGTH", b);
    } else {                                     /* NOTE > TIE > REST > NOTE */
        int t = tb_time(p, s);
        tb_set_time(p, s, t == TM_NOTE ? TM_TIE : t == TM_TIE ? TM_REST : TM_NOTE);
    }
    ui.dirty_b = 1;
}

static int white_of(int k)
{
    int i;
    for (i = 0; i < 16; i++)
        if (WHITE_K[i] == k)
            return i;
    return -1;
}
static int black_of(int k)
{
    int i;
    for (i = 0; i < 11; i++)
        if (BLACK_K[i] == k)
            return i;
    return -1;
}

static void play_key(int k, int down)
{
    int w = white_of(k), b = black_of(k);
    if (!down) {
        if (w >= 0 && w == ui.chain_key)
            ui.chain_key = -1;
        return;
    }
    if (w >= 0) {
        int slot = proj.group * TB_PER_GROUP + w;
        if (held(B_SAVE)) {                      /* SAVE + white key: copy here */
            ui.btn_used |= 1u << B_SAVE;
            copy_pattern(slot);
        } else if (ui.chain_key >= 0) {          /* a second key while the first is held: a chain */
            int a = proj.group * TB_PER_GROUP + ui.chain_key;
            select_pattern(a < slot ? a : slot, a < slot ? slot : a);
        } else {
            ui.chain_key = (int8_t)w;
            select_pattern(slot, slot);
        }
    } else if (b >= 0 && b < TB_GROUPS) {
        static const char *const G[TB_GROUPS] = {"I", "II", "III", "IV"};
        proj.group = (uint8_t)b;
        ui.dirty_p = 1;
        say("GROUP", G[b]);
    }
}

static void key(int k, int down, uint32_t others)
{
    if (ui.view == V_PITCH) {
        if (down)
            pitch_key(k);
        else if (!others && !fm_playing)
            fm_note_off();
    } else if (ui.view == V_TIME) {
        int w = white_of(k), b = black_of(k);
        if (!down)
            return;
        if (w >= 0)
            time_tap(w);
        else if (b >= 0 && b < 3)                /* the 303's NOTE / TIE / REST keys */
            time_write(b == 0 ? TM_NOTE : b == 1 ? TM_TIE : TM_REST);
    } else if (ui.view == V_PLAY && !ui.keyb) {
        play_key(k, down);
    } else {
        live_key(k, down, others);
    }
}

/* ------------------------------------------------------------ buttons --- */
static void set_view(int v)
{
    ui.view = (uint8_t)v;
    ui.touched = -1;
}

static void octave(int d)
{
    char b[8];
    int o = (int)proj.oct + d;
    if (o < 0 || o > 3)
        return;
    proj.oct = (uint8_t)o;
    ui.dirty_p = 1;
    cat_i(b, o - 1, 1);
    say("OCTAVE", b);
}

static void save_all(void)
{
    int rc = 0;
    if (ui.dirty_b || !ui.dirty_p)
        rc |= bank_save();
    rc |= project_save();
    if (rc == 0) {
        ui.dirty_p = ui.dirty_b = 0;
        say("SAVED", 0);
    } else {
        say("NOT SAVED", "FLASH ERROR");
    }
}

static void button(int b)
{
    switch (b) {
    case B_HOME: set_view(V_PLAY); break;
    case B_SEQ: set_view(ui.view == V_PITCH ? V_TIME : V_PITCH); break;
    case B_FX: set_view(V_SOUND); break;
    case B_SEL: set_view(V_MORE); break;
    case B_GLO: set_view(V_SETUP); break;
    case B_EDIT: set_view((ui.view + 1) % NVIEWS); break;
    case B_PLAY:
        fm_play(!fm_playing);
        break;
    case B_REC:
        if (held(B_SAVE)) {                      /* SAVE + REC: clear the pattern */
            ui.btn_used |= 1u << B_SAVE;
            if (ui.view == V_PITCH || ui.view == V_TIME)
                clear_pattern();
        } else if (ui.view == V_PITCH || ui.view == V_TIME) {
            ui.pcur = ui.tcur = 0;
            say("CURSOR", "STEP 1");
        }
        break;
    case B_ARP:
        ui.keyb = (uint8_t)!ui.keyb;
        if (!ui.keyb && ui.transpose && !fm_playing)
            set_transpose(0);
        say("KEYS", ui.keyb ? "ON" : "OFF");
        break;
    case B_ENV: case B_LFO:                      /* tapped alone on PITCH: flip the cursor's accent / slide */
        if (ui.view == V_PITCH) {
            edp()->flag[ui.pcur] ^= b == B_ENV ? TP_ACCENT : TP_SLIDE;
            ui.dirty_b = 1;
        }
        break;
    case B_OCTDN: octave(-1); break;
    case B_OCTUP: octave(1); break;
    case B_SAVE: save_all(); break;
    default: break;
    }
}

/* MIDI in (USB and the TRS jack): clock and start / stop drive the sequencer; notes on our channel
 * play the 303 (stopped; velocity 100 and up is an accent, a note over a held one slides) or
 * transpose the pattern from C4 (playing) */
static void midi_in(void)
{
    uint32_t pkt, ch = (uint32_t)(proj.par[P_MIDICH] - 1);
    while (plat_midi_in(&pkt)) {
        uint32_t st = (pkt >> 8) & 0xFFu, n = (pkt >> 16) & 0x7Fu, v = pkt >> 24;
        if (st == 0xF8u || st == 0xFAu || st == 0xFBu || st == 0xFCu) {
            fm_clock(st == 0xF8u ? FM_CLK_TICK : st == 0xFAu ? FM_CLK_START : st == 0xFBu ? FM_CLK_CONTINUE
                                                                                      : FM_CLK_STOP);
        } else if ((st & 0x0Fu) == ch && (st & 0xF0u) == 0x90u && v) {
            if (fm_playing) {
                set_transpose((int)n - 60);
            } else {
                int note = (int)n < TB_NOTE_LO ? TB_NOTE_LO : (int)n > TB_NOTE_HI ? TB_NOTE_HI : (int)n;
                fm_note(note, v >= 100u, ui.midi_held > 0);
            }
            ui.midi_held++;
        } else if ((st & 0x0Fu) == ch && ((st & 0xF0u) == 0x80u || (st & 0xF0u) == 0x90u)) {
            if (ui.midi_held)
                ui.midi_held--;
            if (!ui.midi_held && !fm_playing)
                fm_note_off();
        }
    }
}

static void input(void)
{
    uint32_t btn = plat_buttons(), keys = plat_keys(), ch, i;
    int32_t e;
    ch = btn ^ ui.btn;
    ui.btn = btn;
    if ((btn & ((1u << B_OCTDN) | (1u << B_OCTUP))) == ((1u << B_OCTDN) | (1u << B_OCTUP)))
        ui.btn_used |= (1u << B_OCTDN) | (1u << B_OCTUP);   /* both held: update mode (main loop), no octave */
    for (i = 0; i < NB; i++) {
        uint32_t m = 1u << i;
        if (!(ch & m))
            continue;
        if (btn & m) {
            ui.btn_used &= ~m;
            if (i == B_PLAY || i == B_REC)       /* transport and REC on the press */
                button((int)i);
        } else if (!(ui.btn_used & m) && i != B_PLAY && i != B_REC) {
            button((int)i);
        }
    }
    ch = keys ^ ui.keys;
    ui.keys = keys;
    for (i = 0; i < NKEYS; i++)
        if (ch >> i & 1u)
            key((int)i, (int)(keys >> i & 1u), keys & ~(1u << i));
    if (!keys)                                   /* all let go (wherever): no chain is being held */
        ui.chain_key = -1;
    if (btn || keys || ch)
        ui.act_t = plat_ms();
    midi_in();
    if ((e = plat_enc(EN_SELECT)) != 0)
        turn(EN_SELECT, P_TEMPO, e);
    if ((e = plat_enc(EN_ALGO)) != 0)
        turn(EN_ALGO, P_ACCENT, e);
    if ((e = plat_enc(EN_PRESET)) != 0) {
        int s = (int)proj.edit + (e > 0 ? 1 : -1);
        if (s >= 0 && s < TB_NPAT)
            select_pattern(s, s);
    }
    for (i = 0; i < 4; i++)
        if ((e = plat_enc(EN_K1 + (int)i)) != 0) {
            turn(EN_K1 + (int)i, VIEW_KNOB[ui.view][i], e);
            ui.touched = (int8_t)i;
            ui.touch_until = plat_ms() + 1200u;
            ui.act_t = plat_ms();
        }
}

/* ------------------------------------------------------------- screen --- */
/* three bands: the header (28 px), the pattern (144), the knobs (68) */
#define HDR_H 28
#define MAIN_Y 28
#define MAIN_H 144
#define KNB_Y 172
#define KNB_H 68

static uint32_t hash(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }

/* a rectangle with rounded corners (radius r, up to 6) */
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

static int beat_on(void) { return fm_playing && fm_step % 4u == 0u && fm_pulse < 4u; }
/* the pattern PLAY shows: the one playing, else the one selected */
static int shown_slot(void) { return fm_playing ? fm_cur : proj.edit; }

static void draw_header(void)
{
    char t[24];
    int msg = plat_ms() < ui.msg_until;
    uint32_t h = hash(hash(hash(2166136261u, ui.view | (uint32_t)msg << 8 | (uint32_t)ui.keyb << 9),
                           (uint32_t)fm_playing | (uint32_t)fm_ext << 1 | (uint32_t)ui_dirty() << 2 |
                               (uint32_t)beat_on() << 3 | (uint32_t)(uint8_t)ui.transpose << 8),
                      (uint32_t)proj.par[P_TEMPO] | (uint32_t)proj.edit << 12 | (uint32_t)fm_cue << 20);
    h = hash(h, (uint32_t)shown_slot());
    if (msg)
        h = hash(hash(h, (uint32_t)ui.msg[0][0] << 8 | (uint8_t)ui.msg[1][0]), ui.msg_until);
    if (fm_cue != TB_NONE)
        h = hash(h, (ui.frame / 8u) & 1u);
    if (h == ui.sig[0])
        return;
    ui.sig[0] = h;
    cv_begin(240, HDR_H, K_TEXT);
    if (msg) {
        int32_t x = cv_text(10, 6, &FONT_B, ui.msg[0], K_BG);
        cv_text(x + 8, 6, &FONT_B, ui.msg[1], K_ACC);
    } else {
        int32_t x = cv_text(10, 6, &FONT_B, VIEW_NAME[ui.view], K_BG);
        tb_slot_name(ui.view == V_PLAY ? shown_slot() : proj.edit, t);
        x = cv_text(x + 10, 6, &FONT_B, t, K_ACC);
        if (fm_cue != TB_NONE && ((ui.frame / 8u) & 1u)) {   /* the next pattern, blinking */
            char c[12];
            tb_slot_name(fm_cue, c);
            x = cv_text(x + 6, 6, &FONT_S, c, K_ACC_T);
        }
        x += 6;
        if (ui.keyb && ui.view == V_PLAY)
            x = tag(x, "KEYS", K_ACC, K_TEXT);
        if (ui.transpose) {
            cat_i(t, ui.transpose, 1);
            x = tag(x, t, K_DIM, K_BG);
        }
        if (ui_dirty())
            cv_round(x, 12, 5, 5, 2, K_DIM);
        /* right: the tempo, and a play state light */
        if (fm_ext)
            cat(t, "EXT");
        else
            cat_i(t, proj.par[P_TEMPO], 0);
        x = 230 - text_w(&FONT_S, t);
        cv_text(x, 6, &FONT_S, t, fm_playing ? K_BG : K_DIM);
        x -= 16;
        if (fm_playing) {
            cv_round(x, 8, 10, 10, 5, beat_on() ? K_ACC : K_DIM);
        } else {
            int32_t j;                           /* stopped: a play triangle */
            for (j = 0; j < 5; j++)
                cv_rect(x + j, 9 + j, 1, 11 - 2 * j, K_DIM);
        }
    }
    cv_blit(0, 0);
}

/* The pattern as a little piano roll: one column a step, a bar at the note's height (accents in
 * orange), ties joined to the note before, slides drawn into the next note, the time line below. */
#define ROLL_X 8
#define ROLL_CW 14
static void draw_roll(int slot, int32_t y0, int32_t h, int cursor_pitch, int cursor_step)
{
    const tb_pattern_t *p = &bank.pat[slot];
    tb_step_t r[TB_STEPS];
    int s, lo = 127, hi = 0, here = fm_playing && fm_cur == slot ? fm_step : -1;
    int32_t ry = y0 + 2, rh = h - 20;
    tb_resolve(p, r);
    for (s = 0; s < p->len; s++)
        if (r[s].note != TB_NONE) {
            lo = r[s].note < lo ? r[s].note : lo;
            hi = r[s].note > hi ? r[s].note : hi;
        }
    if (lo > hi)
        lo = hi = TB_NOTE_DEF;
    if (hi - lo < 12) {                          /* at least an octave tall, centred */
        int pad = (12 - (hi - lo)) / 2;
        lo -= pad;
        hi = lo + 12;
    }
    for (s = 0; s < TB_STEPS; s++) {             /* the columns: beats, past the end, the playhead */
        int32_t x = ROLL_X + s * ROLL_CW;
        uint16_t c = s >= p->len ? K_PANEL : s % 4 == 0 ? K_BEAT : K_BG;
        int cur = s == cursor_step || (cursor_pitch >= 0 && r[s].pitch == cursor_pitch && r[s].time == TM_NOTE);
        if (cur)
            c = K_CUR;
        if (s == here)
            c = K_HEAD;
        cv_rect(x, ry, ROLL_CW - 1, rh, c);
        if (cur) {
            cv_rect(x, ry, ROLL_CW - 1, 2, K_ACC);
            cv_rect(x, ry + rh - 2, ROLL_CW - 1, 2, K_ACC);
        }
    }
    for (s = 0; s < p->len; s++) {
        int32_t x = ROLL_X + s * ROLL_CW, y;
        uint16_t c;
        if (r[s].note == TB_NONE)
            continue;
        y = ry + rh - 8 - (int32_t)(r[s].note - lo) * (rh - 10) / (hi - lo);
        c = (r[s].flag & TP_ACCENT) ? K_ACC : K_TEXT;
        if (r[s].time == TM_TIE)                 /* a tie: joined to the column before */
            cv_rect(x - 1, y, ROLL_CW, 6, c);
        else
            cv_round(x + 1, y, ROLL_CW - 2, 6, 2, c);
        if ((r[s].flag & TP_SLIDE) && s + 1 < p->len && r[s + 1].time != TM_REST) {
            int nx = s + 1;                      /* a slide: into the next NOTE, over any ties */
            while (nx < p->len && r[nx].time == TM_TIE)
                nx++;
            if (nx < p->len && r[nx].note != TB_NONE) {
                int32_t y2 = ry + rh - 8 - (int32_t)(r[nx].note - lo) * (rh - 10) / (hi - lo);
                cv_line(x + ROLL_CW - 2, y + 3, ROLL_X + nx * ROLL_CW + 1, y2 + 3, K_SLD);
                cv_line(x + ROLL_CW - 2, y + 2, ROLL_X + nx * ROLL_CW + 1, y2 + 2, K_SLD);
            }
        }
    }
    for (s = 0; s < TB_STEPS; s++) {             /* the time line: note, tie, rest */
        int32_t x = ROLL_X + s * ROLL_CW, y = y0 + h - 14;
        int t = r[s].time;
        uint16_t c = s >= p->len ? K_LINE : s == here ? K_ACC : K_TEXT;
        if (s == cursor_step)
            cv_round(x, y - 2, ROLL_CW - 1, 14, 3, K_ACC_T);
        if (t == TM_NOTE)
            cv_round(x + 3, y + 1, ROLL_CW - 7, 8, 2, c);
        else if (t == TM_TIE)
            cv_rect(x, y + 4, ROLL_CW - 1, 3, c);
        else
            cv_rect(x + 5, y + 4, ROLL_CW - 11, 2, s >= p->len ? K_LINE : K_DIM);
    }
}

static void draw_slots(int32_t y0)
{
    int k;
    for (k = 0; k < TB_PER_GROUP; k++) {
        int slot = proj.group * TB_PER_GROUP + k, row = k / 8, col = k % 8;
        int32_t x = 30 + col * 24, y = y0 + row * 18;
        int playing = fm_playing && slot == fm_cur, cued = slot == fm_cue, chained = slot >= fm_lo && slot <= fm_hi;
        uint16_t bg = playing ? K_ACC : chained ? K_LINE : K_PANEL, fg = playing ? K_HEAD : K_TEXT;
        char t[4] = {(char)('1' + col), 0, 0, 0};
        if (cued && ((ui.frame / 8u) & 1u))
            bg = K_ACC_T;
        cv_round(x, y, 22, 16, 4, bg);
        if (slot == proj.edit)                   /* the pattern selected: underlined */
            cv_rect(x + 2, y + 14, 18, 2, K_TEXT);
        text_c(x + 11, y, &FONT_S, t, fg);
    }
    {
        static const char *const G[TB_GROUPS] = {"I", "II", "III", "IV"};
        text_c(14, y0 + 8, &FONT_B, G[proj.group], K_TEXT);
        cv_text(224, y0 + 2, &FONT_XS, "A", K_DIM);
        cv_text(224, y0 + 20, &FONT_XS, "B", K_DIM);
    }
}

static void draw_info(int32_t y0)
{
    tb_pattern_t *p = edp();
    char t[24], *e;
    if (ui.view == V_PITCH) {                    /* the pitch at the cursor, big */
        int32_t x;
        e = cat(t, "PITCH ");
        e = cat_i(e, ui.pcur + 1, 0);
        e = cat(e, "/");
        cat_i(e, pitch_wrap(), 0);
        cv_text(10, y0 + 8, &FONT_S, t, K_DIM);
        tb_note_name(p->note[ui.pcur], t);
        x = cv_text(92, y0, &FONT_L, t, K_TEXT);
        x += 8;
        if (p->flag[ui.pcur] & TP_ACCENT)
            x = cv_text(x, y0 + 8, &FONT_B, "ACC", K_ACC) + 6;
        if (p->flag[ui.pcur] & TP_SLIDE)
            cv_text(x, y0 + 8, &FONT_B, "SLIDE", K_SLD);
    } else if (ui.view == V_TIME) {
        static const char *const TIME_N[3] = {"REST", "NOTE", "TIE"};
        e = cat(t, "STEP ");
        e = cat_i(e, ui.tcur + 1, 0);
        cv_text(10, y0 + 8, &FONT_S, t, K_DIM);
        cv_text(92, y0, &FONT_L, TIME_N[tb_time(p, ui.tcur)], K_TEXT);
        e = cat_i(t, p->len, 0);
        cat(e, p->triplet ? " TRIP" : " STEPS");
        cv_text(236 - text_w(&FONT_S, t), y0 + 8, &FONT_S, t, K_DIM);
    } else {                                     /* the sound pages: the voice's state */
        char n[8];
        e = cat(t, "OCT ");
        cat_i(e, (int)proj.oct - 1, 1);
        cv_text(10, y0 + 8, &FONT_S, t, K_DIM);
        tb_note_name(fm_note_now, n);
        cv_text(92, y0, &FONT_L, fm_gate ? n : "", K_TEXT);
        {
            int32_t w = (int32_t)(fm_level * 8.0f) * 10;   /* the output level, in the hash's 8 steps */
            cv_round(150, y0 + 12, 80, 8, 4, K_PANEL);
            if (w > 0)
                cv_round(150, y0 + 12, w < 8 ? 8 : w, 8, 4, fm_level > 0.9f ? K_ACC : K_TEXT);
        }
    }
}

static void draw_main(void)
{
    const tb_pattern_t *p = &bank.pat[ui.view == V_PLAY ? shown_slot() : proj.edit];
    uint32_t h = hash(2166136261u, ui.view | (uint32_t)proj.edit << 8 | (uint32_t)proj.group << 16), s;
    for (s = 0; s < TB_STEPS; s++)
        h = hash(h, p->note[s] | (uint32_t)p->flag[s] << 8);
    h = hash(hash(h, p->time), p->len | (uint32_t)p->triplet << 8 | (uint32_t)ui.pcur << 16 | (uint32_t)ui.tcur << 24);
    h = hash(h, fm_playing ? (uint32_t)fm_step | (uint32_t)fm_cur << 8 : 0xFFFFu);
    h = hash(h, (uint32_t)fm_cue | (uint32_t)fm_lo << 8 | (uint32_t)fm_hi << 16);
    if (fm_cue != TB_NONE)                       /* the cued slot blinks */
        h = hash(h, (ui.frame / 8u) & 1u);
    if (ui.view >= V_SOUND)                      /* the level in 8 steps: a redraw only when it moves */
        h = hash(hash(h, (uint32_t)(fm_level * 8.0f) | (uint32_t)fm_gate << 8), fm_note_now | proj.oct << 8);
    if (h == ui.sig[1])
        return;
    ui.sig[1] = h;
    cv_begin(240, MAIN_H, K_BG);
    if (ui.view == V_PLAY) {                     /* PLAY: the pattern playing; else the one selected */
        draw_slots(4);
        draw_roll(shown_slot(), 44, MAIN_H - 44, -1, -1);
    } else {
        draw_info(4);
        draw_roll(proj.edit, 44, MAIN_H - 44, ui.view == V_PITCH ? ui.pcur : -1, ui.view == V_TIME ? ui.tcur : -1);
    }
    cv_blit(0, MAIN_Y);
}

static void draw_knobs(void)
{
    uint32_t h = hash(2166136261u, ui.view | (uint32_t)ui.touched << 8), i;
    int now_touch = plat_ms() < ui.touch_until;
    for (i = 0; i < 4; i++)
        h = hash(h, (uint32_t)knob_get(VIEW_KNOB[ui.view][i]));
    h = hash(h, (uint32_t)now_touch);
    if (h == ui.sig[2])
        return;
    ui.sig[2] = h;
    cv_begin(240, KNB_H, K_BG);
    cv_round(4, 2, 232, KNB_H - 6, 6, K_PANEL);
    for (i = 0; i < 4; i++) {
        int k = VIEW_KNOB[ui.view][i], lo, hi, v;
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
    tb_pattern_t *p = edp();
    b |= 1u << VIEW_BTN[ui.view];
    if (ui.view == V_TIME && blink)              /* TIME: SEQ blinks (PITCH: steady) */
        b &= ~(1u << B_SEQ);
    if (fm_playing)
        b |= fm_pulse < 3u || fm_step % 4u == 0u ? 1u << B_PLAY : 0u;
    if (ui.keyb)
        b |= 1u << B_ARP;
    if (ui.view == V_PITCH) {                    /* the cursor's accent / slide */
        if ((p->flag[ui.pcur] & TP_ACCENT) || held(B_ENV))
            b |= 1u << B_ENV;
        if ((p->flag[ui.pcur] & TP_SLIDE) || held(B_LFO))
            b |= 1u << B_LFO;
    }
    if (proj.leds) {
        if (ui.view == V_PLAY && !ui.keyb) {     /* the group's patterns: playing, cued, the group */
            for (i = 0; i < TB_PER_GROUP; i++) {
                int slot = proj.group * TB_PER_GROUP + (int)i;
                if ((fm_playing && slot == fm_cur) || (!fm_playing && slot == proj.edit) || (slot == fm_cue && blink))
                    k |= 1u << WHITE_K[i];
            }
            k |= 1u << BLACK_K[proj.group];
        } else if (ui.view == V_TIME) {          /* the time line: notes lit, ties blinking, the playhead dark */
            for (i = 0; i < TB_STEPS; i++) {
                int t = tb_time(p, (int)i);
                int on = (int)i < p->len && (t == TM_NOTE || (t == TM_TIE && blink));
                if (fm_playing && fm_cur == proj.edit && fm_step == i)
                    on = !on;
                if (on)
                    k |= 1u << WHITE_K[i];
            }
        } else if (fm_gate) {                    /* elsewhere: the key of the note sounding */
            int kk = fm_note_now - (53 + ((int)proj.oct - 1) * 12 - 24);
            if (kk >= 0 && kk < NKEYS)
                k |= 1u << kk;
        }
    }
    plat_glow(proj.leds == 1);
    plat_leds(b, k);
}

/* AUTOSAVE: a few seconds after a change, while stopped, nothing sounds and nothing is touched (a
 * flash erase silences the audio for a moment) */
#define AUTOSAVE_QUIET 4000u
static void autosave(void)
{
    uint32_t now = plat_ms();
    if (!ui_dirty() || fm_playing || fm_gate || ui.btn || ui.keys || now - ui.act_t < AUTOSAVE_QUIET ||
        fm_level > 0.001f)
        return;
    if ((!ui.dirty_b || bank_save() == 0) && (!ui.dirty_p || project_save() == 0))
        ui.dirty_p = ui.dirty_b = 0;
    else
        ui.act_t = now;                          /* failed: try again later, not every frame */
}

void ui_init(void)
{
    memset(&ui, 0, sizeof ui);
    ui.touched = -1;
    ui.chain_key = -1;
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
