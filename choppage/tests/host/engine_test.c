/* SPDX-License-Identifier: GPL-3.0-only */
/* CHOPPAGE engine tests: the render driven one frame at a time, so every trigger is seen on its
 * frame. The test break is 32 slices of a constant level each, so the slice playing can be told from
 * the output too.
 *   - straight playback in time: a trigger on every step, on its frame, each slice in turn
 *   - types pick only slices with their tag; VARY 0 picks the same one each pass; POOL ALL reaches
 *     the other breaks
 *   - rolls, swing, gate, cut, probability
 *   - beat repeat: restarts on its grid while held, and the break comes back where it would be
 *   - the keyboard: quantized to the step, replacing the sequence while held, back in after
 *   - tape stop, octave down, reverse
 *   - MIDI clock in: steps follow the ticks, the tempo is measured
 *   - recording: keyboard slices land in the pattern's steps
 *   - the generator: deterministic, valid patterns in every style, AUTO and fills */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../../firmware/src/dsp/chp.h"
#include "../../firmware/src/dsp/gen.h"

static uint32_t midi_q[256], mq_w, mq_r, clocks_out;
int chp_midi_in(uint32_t *pkt)
{
    if (mq_r == mq_w)
        return 0;
    *pkt = midi_q[mq_r++ % 256u];
    return 1;
}
void chp_midi_out(uint32_t st, uint32_t d1, uint32_t d2)
{
    (void)d1;
    (void)d2;
    if (st == 0xF8u)
        clocks_out++;
}

static uint8_t mem[CHP_MEM];
static chp_break_t brk[CHP_NBRK];
static chp_pattern_t pat[CHP_NPAT];
static int fails, checks;
static uint32_t frame;

#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* break b: 32 slices, slice s a constant level 2 + 3s (8-bit), `beats` beats at `rate` */
static void make_break(int b, uint32_t off, uint32_t len, uint32_t rate, int beats)
{
    chp_break_t *k = &brk[b];
    int s;
    memset(k, 0, sizeof *k);
    k->off = off;
    k->len = len;
    k->rate = (uint16_t)rate;
    k->fmt = FMT_PCM8;
    k->beats = (uint8_t)beats;
    k->nsl = 32;
    for (s = 0; s < 32; s++) {
        uint32_t a = len / 32u * (uint32_t)s, e = len / 32u * (uint32_t)(s + 1), i;
        k->start[s] = a;
        k->tag[s] = (uint8_t)(s % 4 == 0 ? TAG_K : s % 4 == 2 ? TAG_S : s % 4 == 1 ? TAG_H : TAG_G);
        for (i = a; i < e; i++)
            mem[off + i] = (uint8_t)(2 + 3 * s);
    }
    snprintf(k->name, sizeof k->name, "T%d", b);
}

static int32_t out[2 * 4096];
static uint8_t last_trig;
static int trig_frames[4096], ntrig;
static int trig_slice[4096];

/* n frames, one at a time; records the frames where a trigger happened */
static void run(uint32_t n)
{
    while (n--) {
        chp_render(out, 1, 2048);
        frame++;
        if (chp_trig != last_trig) {
            last_trig = chp_trig;
            if (ntrig < 4096) {
                trig_frames[ntrig] = (int)frame - 1;
                trig_slice[ntrig] = chp_vslice;
                ntrig++;
            }
        }
    }
}
static void run_block(uint32_t n)                      /* n frames, in the device's blocks */
{
    while (n) {
        uint32_t k = n > 256u ? 256u : n;
        chp_render(out, k, 2048);
        frame += k;
        n -= k;
    }
}
static void reset_trigs(void) { ntrig = 0; }

static void setup(int bpm_param, int break_bpm)
{
    int i, p;
    float sec = 8.0f * 60.0f / (float)break_bpm;
    memset(mem, 0, sizeof mem);
    memset(brk, 0, sizeof brk);
    make_break(0, 0, (uint32_t)(sec * 22050.0f), 22050, 8);
    make_break(1, 100000, (uint32_t)(sec * 22050.0f), 22050, 8);
    for (i = 0; i < 32; i++)                           /* break 2: every slice a snare, and loud */
        brk[1].tag[i] = TAG_S;
    for (p = 0; p < CHP_NPAT; p++) {
        chp_pattern_clear(&pat[p], 16);
        for (i = 0; i < 16; i++)
            pat[p].st[i].src = (uint8_t)(SRC_SLICE + i);
    }
    chp_pattern_clear(&pat[0], 32);
    for (i = 0; i < 32; i++)
        pat[0].st[i].src = (uint8_t)(SRC_SLICE + i);
    chp_init(pat, brk, mem);
    chp_set(P_TEMPO, bpm_param);
    chp_set(P_FILL, 0);
    chp_set(P_AUTO, 0);
    chp_set(P_CLKOUT, 1);
    chp_break(0);
    chp_pattern(0);
    chp_seed(1234);
    run(1);
    frame = 0;
    last_trig = chp_trig;
    reset_trigs();
}

static float step_frames(int bpm) { return 44100.0f * 60.0f / ((float)bpm * 4.0f); }

static void t_straight(void)
{
    int i, bad = 0;
    float sl = step_frames(165);
    setup(165, 165);
    chp_play(1);
    run((uint32_t)(sl * 32.0f + 10.0f));
    CHECK(ntrig >= 32, "a trigger a step: %d", ntrig);
    for (i = 0; i < 32 && i < ntrig; i++) {
        int want = (int)(sl * (float)i + 0.5f);
        if (abs(trig_frames[i] - want) > 2 || trig_slice[i] != i % 32)
            bad++;
    }
    CHECK(bad == 0, "every step on its frame (+-2), slice in turn: %d wrong", bad);
    CHECK(clocks_out >= 6u * 32u, "MIDI clock out: 6 a step (%u)", clocks_out);
    /* the slice heard mid-step, from the published state */
    chp_play(0);
    run(100);
}

static void t_types(void)
{
    int i, bad = 0, seen[32] = {0}, other = 0, kinds = 0;
    setup(165, 165);
    chp_set(P_VARY, 100);
    for (i = 0; i < 16; i++)
        pat[1].st[i].src = SRC_S;
    chp_pattern(1);
    chp_play(1);
    run((uint32_t)(step_frames(165) * 64.0f));
    for (i = 0; i < ntrig; i++) {
        if (trig_slice[i] < 0 || trig_slice[i] >= 32 || brk[0].tag[trig_slice[i]] != TAG_S)
            bad++;
        else
            seen[trig_slice[i]]++;
    }
    for (i = 0; i < 32; i++)
        kinds += seen[i] > 0;
    CHECK(ntrig >= 60 && bad == 0, "SNARE steps play only snare slices (%d triggers, %d not)", ntrig, bad);
    CHECK(kinds >= 6, "VARY 100: the snares change (%d different of 8)", kinds);
    /* VARY 0: each step its own snare, the same every pass */
    setup(165, 165);
    chp_set(P_VARY, 0);
    for (i = 0; i < 16; i++)
        pat[1].st[i].src = SRC_S;
    chp_pattern(1);
    chp_play(1);
    run((uint32_t)(step_frames(165) * 48.0f));
    for (i = 16; i < ntrig && i < 48; i++)
        if (trig_slice[i] != trig_slice[i - 16])
            bad++;
    for (i = 1, kinds = 1; i < 16; i++)
        kinds += trig_slice[i] != trig_slice[0];
    CHECK(bad == 0, "VARY 0: a step picks the same slice each pass (%d differ)", bad);
    CHECK(kinds > 1, "VARY 0: but not the same slice on every step");
    /* POOL ALL: break 2 (all snares) joins */
    setup(165, 165);
    chp_set(P_VARY, 100);
    chp_set(P_POOL, 1);
    for (i = 0; i < 16; i++)
        pat[1].st[i].src = SRC_S;
    chp_pattern(1);
    chp_play(1);
    for (i = 0; i < 64; i++) {
        run((uint32_t)step_frames(165));
        other += chp_vbrk == 1;
    }
    CHECK(other > 10, "POOL ALL: snares from the other break too (%d of 64 steps)", other);
}

static void t_roll_swing_gate(void)
{
    float sl = step_frames(120);
    int i, n4 = 0;
    setup(120, 120);
    pat[1].st[0].fx = 3;                              /* roll 4x */
    for (i = 1; i < 16; i++)
        pat[1].st[i].src = SRC_NONE;
    chp_pattern(1);
    chp_play(1);
    run((uint32_t)(sl * 1.0f) - 5u);
    n4 = ntrig;
    CHECK(n4 == 4, "a 4x roll: four triggers in the step (%d)", n4);
    for (i = 1; i < n4; i++)
        CHECK(abs(trig_frames[i] - trig_frames[i - 1] - (int)(sl / 4.0f)) <= 2, "roll spacing %d", trig_frames[i] - trig_frames[i - 1]);
    /* swing 66: the off sixteenths a third of a step late */
    setup(120, 120);
    chp_set(P_SWING, 66);
    chp_pattern(2);
    chp_play(1);
    run((uint32_t)(sl * 4.0f));
    CHECK(ntrig >= 4, "swing: triggers (%d)", ntrig);
    if (ntrig >= 4) {
        int d1 = trig_frames[1] - (int)sl, d3 = trig_frames[3] - (int)(3.0f * sl), d2 = trig_frames[2] - (int)(2.0f * sl);
        CHECK(abs(d1 - (int)(sl * 0.32f)) <= 3 && abs(d3 - (int)(sl * 0.32f)) <= 3, "odd steps late by 32%% (%d, %d)", d1, d3);
        CHECK(abs(d2) <= 2, "even steps on time (%d)", d2);
    }
    /* gate 4/8: the slice is quiet by the second half of the step */
    setup(120, 120);
    for (i = 0; i < 16; i++)
        pat[3].st[i].gate = 4;
    chp_pattern(3);
    chp_play(1);
    run((uint32_t)(sl * 0.25f));
    CHECK(chp_level > 0.01f, "gated: sounding at a quarter step");
    run((uint32_t)(sl * 0.5f));
    CHECK(chp_vslice == CHP_NONE, "gate 4/8: silent by three quarters of the step");
    /* cut */
    setup(120, 120);
    pat[4].st[1].src = SRC_CUT;
    for (i = 2; i < 16; i++)
        pat[4].st[i].src = SRC_NONE;
    chp_pattern(4);
    chp_play(1);
    run((uint32_t)(sl * 0.5f));
    CHECK(chp_vslice == 0, "plays slice 1");
    run((uint32_t)(sl * 1.2f));
    CHECK(chp_vslice == CHP_NONE, "CUT: silence");
    /* probability 0: nothing new, the slice before plays on */
    setup(120, 120);
    for (i = 1; i < 16; i++)
        pat[5].st[i].prob = 0;
    chp_pattern(5);
    chp_play(1);
    run((uint32_t)(sl * 3.5f));
    CHECK(ntrig == 1, "chance 0%%: only step 1 plays (%d)", ntrig);
    CHECK(chp_vslice == 3, "and the break runs on through slice 4 (%d)", chp_vslice);
}

static void t_repeat(void)
{
    float sl = step_frames(165);
    int i, n0, bad = 0;
    setup(165, 165);
    chp_play(1);
    run((uint32_t)(sl * 4.3f));                        /* into step 5 */
    reset_trigs();
    chp_perf(PF_R32, 1);                               /* 1/32: half a step */
    run((uint32_t)(sl * 4.0f));
    n0 = ntrig;
    CHECK(n0 >= 7 && n0 <= 9, "1/32 repeat: a restart every half step (%d in 4 steps)", n0);
    for (i = 1; i < n0; i++)
        if (abs(trig_frames[i] - trig_frames[i - 1] - (int)(sl * 0.5f)) > 2)
            bad++;
    CHECK(bad == 0, "1/32 repeat: evenly spaced (%d off)", bad);
    for (i = 0; i < n0; i++)
        if (trig_slice[i] != trig_slice[0])
            bad++;
    CHECK(bad == 0, "it repeats the same slice");
    CHECK((chp_perf_on & 0x8000u) != 0, "repeat in effect");
    chp_perf(PF_R32, 0);
    run(300);
    CHECK(chp_mstep >= 8 && chp_vslice == chp_mstep, "back in: the break where it would be (slice %d, step %d)",
          chp_vslice, chp_mstep);
    /* a beat repeat (1/4) replays four steps */
    setup(165, 165);
    chp_play(1);
    run((uint32_t)(sl * 3.5f));
    chp_perf(PF_LOOP, 1);
    run((uint32_t)(sl * 1.0f));                        /* it starts on the beat, step 5 (4) */
    reset_trigs();
    run((uint32_t)(sl * 8.0f));
    CHECK(ntrig >= 8, "beat repeat: triggers (%d)", ntrig);
    for (i = 0, bad = 0; i < ntrig && i < 8; i++)
        if (trig_slice[i] != 4 + (i + 1) % 4 && trig_slice[i] != 4 + i % 4)
            bad++;
    CHECK(bad <= 1, "beat repeat: slices 5-8 over and over (%d off)", bad);
    chp_perf(PF_LOOP, 0);
    run(200);
    CHECK(chp_vslice == chp_mstep, "and back in (slice %d, step %d)", chp_vslice, chp_mstep);
}

static void t_live(void)
{
    float sl = step_frames(165);
    int i, seq_trigs;
    setup(165, 165);
    chp_play(1);
    run((uint32_t)(sl * 2.5f));                        /* mid step 3 */
    chp_live(20, 1, 15);                               /* slice 21, quantized to step 4 */
    run((uint32_t)(sl * 0.3f));
    CHECK(chp_vslice == 2, "quantized: not yet (%d)", chp_vslice);
    run((uint32_t)(sl * 0.4f));
    CHECK(chp_vslice == 20, "quantized: on the next step (%d)", chp_vslice);
    reset_trigs();
    run((uint32_t)(sl * 3.0f));                        /* held: the break runs on from slice 21 */
    seq_trigs = ntrig;
    CHECK(chp_vslice >= 22 && chp_vslice <= 24, "held: the slice runs on (%d)", chp_vslice);
    CHECK(seq_trigs == 0, "held: the sequence is quiet (%d)", seq_trigs);
    chp_live(20, 0, 0);
    run(300);
    CHECK(chp_vslice == chp_mstep, "let go: the sequence is back where it would be (%d, %d)", chp_vslice, chp_mstep);
    /* layer: both */
    setup(165, 165);
    chp_set(P_LIVE, 1);
    chp_set(P_QUANT, 0);
    chp_play(1);
    run((uint32_t)(sl * 1.5f));
    chp_live(30, 1, 15);
    run(50);
    chp_live(30, 0, 0);
    reset_trigs();
    run((uint32_t)(sl * 2.0f));
    CHECK(ntrig >= 2, "layer: the sequence goes on (%d)", ntrig);
    /* stopped: a tap plays its slice, whole */
    setup(165, 165);
    chp_live(7, 1, 15);
    run(20);
    chp_live(7, 0, 0);
    run(200);
    CHECK(chp_vslice == 7, "a tap: the slice (%d)", chp_vslice);
    for (i = 0; i < 6; i++)
        run((uint32_t)(sl * 0.5f));
    CHECK(chp_vslice == CHP_NONE, "... and only that slice");
}

static void t_tape_oct_rev(void)
{
    float sl = step_frames(165);
    float lv;
    setup(165, 165);
    chp_play(1);
    run((uint32_t)(sl * 2.0f));
    chp_perf(PF_TAPE, 1);
    run((uint32_t)(sl * 4.5f));
    run_block(256);
    lv = chp_level;
    CHECK(lv < 0.02f, "tape stop: silent after a beat (%.3f)", (double)lv);
    chp_perf(PF_TAPE, 0);
    run(400);
    CHECK(chp_vslice == chp_mstep, "and back in time (%d, %d)", chp_vslice, chp_mstep);
    /* reverse: what plays runs backwards */
    setup(165, 165);
    chp_play(1);
    run((uint32_t)(sl * 4.5f));
    chp_perf(PF_REV, 1);
    run((uint32_t)(sl * 0.4f));
    {
        uint32_t p0 = chp_vpos;
        run(200);
        CHECK(chp_vpos < p0, "reverse: the position falls (%u -> %u)", p0, chp_vpos);
    }
    chp_perf(PF_REV, 0);
    run(300);
    CHECK(chp_vslice == chp_mstep, "reverse let go: in time (%d, %d)", chp_vslice, chp_mstep);
}

static void t_clock(void)
{
    int i;
    setup(165, 165);
    midi_q[mq_w++ % 256u] = 0x0Fu | 0xFAu << 8;      /* start */
    run(10);
    for (i = 0; i < 96; i++) {                         /* 4 beats at 120 BPM: a tick every 918.75 frames */
        midi_q[mq_w++ % 256u] = 0x0Fu | 0xF8u << 8;
        run_block(918);
    }
    CHECK(chp_ext == 1, "following MIDI clock");
    CHECK(chp_playing == 1, "started by MIDI start");
    CHECK(fabsf(chp_tempo_now - 120.0f) < 1.5f, "tempo measured: %.1f", (double)chp_tempo_now);
    CHECK(chp_mstep == 15 || chp_mstep == 0, "16 steps for 96 ticks (at %d)", chp_mstep);
    midi_q[mq_w++ % 256u] = 0x0Fu | 0xFCu << 8;      /* stop */
    run_block(256);
    CHECK(chp_playing == 0, "stopped by MIDI stop");
    run_block(44100);
    CHECK(chp_ext == 0, "no clock: back to the own tempo");
}

static void t_record(void)
{
    float sl = step_frames(165);
    chp_rec_t r;
    int got = 0, at5 = 0;
    setup(165, 165);
    chp_set(P_QUANT, 1);
    chp_rec(1);
    chp_play(1);
    run((uint32_t)(sl * 4.6f));                        /* late in step 5: lands on step 6 */
    chp_live(12, 1, 15);
    run((uint32_t)(sl * 0.6f));
    chp_live(12, 0, 0);
    run((uint32_t)(sl * 1.0f));
    while (chp_rec_take(&r)) {
        got++;
        if (r.step == 5 && r.st.src == SRC_SLICE + 12)
            at5 = 1;
    }
    CHECK(got >= 1 && at5, "recorded: slice 13 on step 6 (%d records)", got);
    /* rec as type */
    setup(165, 165);
    chp_set(P_RECAS, 1);
    chp_set(P_QUANT, 0);
    chp_rec(1);
    chp_play(1);
    run((uint32_t)(sl * 2.1f));
    chp_live(2, 1, 15);                                /* slice 3: a snare */
    run(100);
    chp_live(2, 0, 0);
    got = 0;
    while (chp_rec_take(&r))
        if (r.st.src == SRC_S)
            got++;
    CHECK(got == 1, "rec as type: a SNARE step");
}

static void t_hold(void)
{
    setup(165, 165);
    chp_play(1);
    run(5000);
    chp_hold(1);
    run(10);
    CHECK(chp_held == 1 && chp_playing == 0 && chp_level == 0.0f, "hold: stopped and silent");
    chp_hold(0);
    run(10);
    CHECK(chp_held == 0, "let go");
}

static void t_gen(void)
{
    gen_cfg_t c;
    chp_pattern_t a, b;
    chp_step_t w[CHP_STEPS];
    int s, i, same, valid = 1, hits;
    setup(165, 165);
    for (s = 0; s < NSTYLES; s++) {
        c.style = (uint8_t)s;
        c.dens = 60;
        c.chaos = 70;
        a.len = b.len = 32;
        gen_write(&a, &c, &brk[0], 99);
        gen_write(&b, &c, &brk[0], 99);
        CHECK(!memcmp(&a, &b, sizeof a), "style %d: the same seed, the same pattern", s);
        valid &= chp_pattern_valid(&a);
        for (i = 0, hits = 0; i < 32; i++)
            hits += a.st[i].src != SRC_NONE;
        CHECK(hits >= 8, "style %d: hits (%d)", s, hits);
        gen_write(&b, &c, &brk[0], 100);
        CHECK(memcmp(&a, &b, sizeof a) != 0, "style %d: another seed, another pattern", s);
    }
    CHECK(valid, "every style writes valid patterns");
    c.style = GS_JUNGLE;
    c.dens = 50;
    c.chaos = 0;
    a.len = 16;
    gen_write(&a, &c, &brk[0], 7);
    CHECK(a.st[0].src == SRC_K && a.st[4].src == SRC_S && a.st[12].src == SRC_S, "jungle: kick on 1, snares on 2 and 4");
    gen_vary(w, &a, &c, &brk[0], 5, 0, 0);
    CHECK(!memcmp(w, a.st, 16 * sizeof w[0]), "AUTO 0: the pattern as it is");
    gen_vary(w, &a, &c, &brk[0], 5, 100, 0);
    for (i = 0, same = 0; i < 16; i++)
        same += !memcmp(&w[i], &a.st[i], sizeof w[i]);
    CHECK(same < 14, "AUTO 100: varied (%d of 16 the same)", same);
    gen_vary(w, &a, &c, &brk[0], 5, 0, 1);
    for (i = 12, same = 0; i < 16; i++)
        same += !memcmp(&w[i], &a.st[i], sizeof w[i]);
    CHECK(same < 4, "a fill: the last beat changes");
    CHECK(!memcmp(w, a.st, 12 * sizeof w[0]), "a fill: the rest stays");
    /* straight: the break's own order */
    CHECK(gen_straight_src(&brk[0], 0) == SRC_SLICE && gen_straight_src(&brk[0], 5) == SRC_SLICE + 5, "straight order");
    /* AUTO in the engine: a varied pass */
    setup(165, 165);
    chp_set(P_AUTO, 100);
    chp_play(1);
    run((uint32_t)(step_frames(165) * 33.0f));
    CHECK(chp_work_on == 1, "AUTO: the engine plays a variation");
}

int main(void)
{
    t_straight();
    t_types();
    t_roll_swing_gate();
    t_repeat();
    t_live();
    t_tape_oct_rev();
    t_clock();
    t_record();
    t_hold();
    t_gen();
    printf("engine: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
