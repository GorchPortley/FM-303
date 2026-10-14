/* SPDX-License-Identifier: GPL-3.0-only */
/* FM-303 sequencer tests: the pattern model (pitch list + time line), and playback against the
 * TB-303's rules - where the gate falls, what slides, what a tie and a rest do, when a cued
 * pattern takes over, chains, triplets and MIDI out. Exits 1 on a failure. */
#include <stdio.h>
#include <string.h>
#include "../../firmware/src/seq/tb303seq.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* what the sink saw, one event per line: "p<pulse> on <note> a<acc> s<slide>" / "p<pulse> off" */
static char log_[8192];
static int pulse_no;
static void ev(const char *fmt, int a, int b, int c)
{
    char t[64];
    snprintf(t, sizeof t, fmt, pulse_no, a, b, c);
    strncat(log_, t, sizeof log_ - strlen(log_) - 1);
}
static void s_on(void *ctx, int note, int acc, int slide) { (void)ctx; ev("p%d on %d a%d s%d\n", note, acc, slide); }
static void s_off(void *ctx) { (void)ctx; ev("p%d off\n", 0, 0, 0); }
static char mlog[4096];
static void s_midi(void *ctx, uint32_t st, uint32_t d1, uint32_t d2)
{
    char t[32];
    (void)ctx;
    snprintf(t, sizeof t, "%02X %u %u\n", st, d1, d2);
    strncat(mlog, t, sizeof mlog - strlen(mlog) - 1);
}
static const tbs_sink_t SINK = {s_on, s_off, s_midi, 0};

static tb_pattern_t bank[TB_NPAT];
static tbs_t seq;

/* a pattern from a time string ('x' note, '~' tie, '.' rest) and notes (+100: accent, +200: slide) */
static void make(tb_pattern_t *p, const char *time, const int *notes, int n)
{
    int s;
    tb_clear(p);
    for (s = 0; time[s]; s++)
        tb_set_time(p, s, time[s] == 'x' ? TM_NOTE : time[s] == '~' ? TM_TIE : TM_REST);
    p->len = (uint8_t)s;
    for (s = 0; s < n; s++) {
        int v = notes[s];
        p->flag[s] = (uint8_t)((v >= 200 ? TP_SLIDE : 0) | ((v % 200) >= 100 ? TP_ACCENT : 0));
        p->note[s] = (uint8_t)(v % 100);
    }
}

static void run(int pulses)
{
    int i;
    for (i = 0; i < pulses; i++, pulse_no++)
        tbs_pulse(&seq, &SINK);
}

static void reset(void)
{
    log_[0] = mlog[0] = 0;
    pulse_no = 0;
    tbs_init(&seq, bank);
}

static void test_model(void)
{
    tb_pattern_t p;
    tb_step_t r[TB_STEPS];
    int s, n;
    tb_clear(&p);
    CHECK(tb_valid(&p), "a cleared pattern is valid");
    CHECK(p.len == 16 && tb_notes_used(&p) == 16, "cleared: 16 notes");
    for (s = 0; s < 16; s++)
        CHECK(tb_time(&p, s) == TM_NOTE && p.note[s] == TB_NOTE_DEF, "cleared step %d", s);
    /* tb_set_time leaves the neighbours alone */
    tb_set_time(&p, 5, TM_TIE);
    tb_set_time(&p, 6, TM_REST);
    tb_set_time(&p, 15, TM_TIE);
    CHECK(tb_time(&p, 4) == TM_NOTE && tb_time(&p, 5) == TM_TIE && tb_time(&p, 6) == TM_REST &&
          tb_time(&p, 7) == TM_NOTE && tb_time(&p, 15) == TM_TIE, "set_time");
    tb_set_time(&p, 5, TM_NOTE);
    CHECK(tb_time(&p, 5) == TM_NOTE, "set_time back");
    {   /* resolve: pitches are taken in turn by the NOTE steps; ties hold, rests clear */
        static const int N[] = {40, 141, 243, 45};
        make(&p, "~x~.x~x.x", N, 4);
        n = tb_resolve(&p, r);
        CHECK(n == 4, "4 NOTE steps, got %d", n);
        CHECK(r[0].note == TB_NONE, "a tie with nothing before it is silent");
        CHECK(r[1].note == 40 && r[1].pitch == 0, "step 1: pitch 0");
        CHECK(r[2].note == 40 && r[2].time == TM_TIE, "step 2 ties pitch 0");
        CHECK(r[3].note == TB_NONE, "rest");
        CHECK(r[4].note == 41 && (r[4].flag & TP_ACCENT), "step 4: pitch 1, accented");
        CHECK(r[6].note == 43 && (r[6].flag & TP_SLIDE), "step 6: pitch 2, slide");
        CHECK(r[8].note == 45 && r[8].pitch == 3, "step 8: pitch 3");
        CHECK(r[9].note == TB_NONE, "past the end: nothing");
    }
    {   /* factory patterns: valid, and each uses no more pitches than it has */
        int i;
        for (i = 0; i < tb_factory_count(); i++) {
            tb_factory(i, &p);
            CHECK(tb_valid(&p), "factory %d valid", i);
            CHECK(tb_notes_used(&p) >= 1, "factory %d has notes", i);
        }
    }
    {
        char b[8];
        tb_slot_name(0, b);
        CHECK(!strcmp(b, "I 1A"), "slot 0 is %s", b);
        tb_slot_name(TB_PER_GROUP * 3 + 15, b);
        CHECK(!strcmp(b, "IV 8B"), "slot 63 is %s", b);
        tb_note_name(36, b);
        CHECK(!strcmp(b, "C2"), "note 36 is %s", b);
        tb_note_name(61, b);
        CHECK(!strcmp(b, "C#4"), "note 61 is %s", b);
    }
}

static void test_gates(void)
{
    /* plain notes: on at pulse 0 of the step, off at pulse 3 (half of 6) */
    static const int N1[] = {36, 48};
    make(&bank[0], "xx", N1, 2);
    reset();
    tbs_start(&seq, &SINK);
    run(12);
    CHECK(!strcmp(log_, "p0 on 36 a0 s0\np3 off\np6 on 48 a0 s0\np9 off\n"), "plain notes:\n%s", log_);

    /* slide: the gate stays up, the next note is legato; the slid-to note ends normally */
    {
        static const int N[] = {236, 48};
        make(&bank[0], "xx", N, 2);
    }
    reset();
    tbs_start(&seq, &SINK);
    run(12);
    CHECK(!strcmp(log_, "p0 on 36 a0 s0\np6 on 48 a0 s1\np9 off\n"), "slide:\n%s", log_);

    /* tie: the gate stays up through the tied step, falls at its half */
    {
        static const int N[] = {136, 48};
        make(&bank[0], "x~x", N, 2);
    }
    reset();
    tbs_start(&seq, &SINK);
    run(18);
    CHECK(!strcmp(log_, "p0 on 36 a1 s0\np9 off\np12 on 48 a0 s0\np15 off\n"), "tie:\n%s", log_);

    /* slide into a rest: the note is held for its whole step and ends at the rest */
    {
        static const int N[] = {236, 48};
        make(&bank[0], "x.x", N, 2);
    }
    reset();
    tbs_start(&seq, &SINK);
    run(18);
    CHECK(!strcmp(log_, "p0 on 36 a0 s0\np6 off\np12 on 48 a0 s0\np15 off\n"), "slide into rest:\n%s", log_);

    /* slide through a tie into a note: legato after the tie */
    {
        static const int N[] = {236, 48};
        make(&bank[0], "x~x", N, 2);
    }
    reset();
    tbs_start(&seq, &SINK);
    run(18);
    CHECK(!strcmp(log_, "p0 on 36 a0 s0\np12 on 48 a0 s1\np15 off\n"), "slide over a tie:\n%s", log_);

    /* a tie at the top after a rest: silence; the pitch list restarts each pass */
    {
        static const int N[] = {40, 41};
        make(&bank[0], "~x.x.", N, 2);
    }
    reset();
    tbs_start(&seq, &SINK);
    run(60);
    CHECK(!strcmp(log_, "p6 on 40 a0 s0\np9 off\np18 on 41 a0 s0\np21 off\n"
                        "p36 on 40 a0 s0\np39 off\np48 on 41 a0 s0\np51 off\n"), "tie at the top:\n%s", log_);

    /* a tie at the top that follows a sounding last step ties across the loop */
    {
        static const int N[] = {40, 41};
        make(&bank[0], "~x.x", N, 2);
    }
    reset();
    tbs_start(&seq, &SINK);
    run(24 + 12);
    CHECK(!strcmp(log_, "p6 on 40 a0 s0\np9 off\np18 on 41 a0 s0\np27 off\np30 on 40 a0 s0\np33 off\n"),
          "tie across the loop:\n%s", log_);

    /* stop: the gate falls at once */
    {
        static const int N[] = {236};
        make(&bank[0], "x", N, 1);
    }
    reset();
    tbs_start(&seq, &SINK);
    run(2);
    tbs_stop(&seq, &SINK);
    CHECK(!strcmp(log_, "p0 on 36 a0 s0\np2 off\n"), "stop:\n%s", log_);
}

static void test_patterns(void)
{
    static const int A[] = {30}, B[] = {50}, C[] = {60};
    int i;
    make(&bank[1], "x", A, 1);
    make(&bank[2], "x", B, 1);
    make(&bank[3], "x", C, 1);
    /* stopped: a selection takes effect at once */
    reset();
    tbs_select(&seq, 2, 2);
    tbs_start(&seq, &SINK);
    run(6);
    CHECK(!strcmp(log_, "p0 on 50 a0 s0\np3 off\n"), "select while stopped:\n%s", log_);
    /* playing: the cue waits for the end of the pattern */
    bank[2].len = 2;
    tb_set_time(&bank[2], 1, TM_NOTE);
    reset();
    tbs_select(&seq, 2, 2);
    tbs_start(&seq, &SINK);
    run(3);
    tbs_select(&seq, 1, 1);
    CHECK(seq.cue_lo == 1, "cued");
    run(9);
    CHECK(seq.cur == 1 && seq.cue_lo == TB_NONE, "switched at the end (cur %d)", seq.cur);
    CHECK(!strcmp(log_, "p0 on 50 a0 s0\np3 off\np6 on 36 a0 s0\np9 off\n"), "cue:\n%s", log_);
    /* a chain plays lo..hi in turn */
    bank[2].len = 1;
    reset();
    tbs_select(&seq, 1, 3);
    tbs_start(&seq, &SINK);
    log_[0] = 0;
    for (i = 0; i < 4; i++) {
        int c = seq.cur;
        run(6);
        CHECK(c == 1 + i % 3, "chain step %d plays %d", i, c);
    }
    CHECK(seq.loops == 4, "loops %u", seq.loops);
    /* triplets: 8 pulses a step, the gate falls at 4 */
    {
        static const int N[] = {36, 38};
        make(&bank[0], "xx", N, 2);
        bank[0].triplet = 1;
    }
    reset();
    tbs_start(&seq, &SINK);
    run(16);
    CHECK(!strcmp(log_, "p0 on 36 a0 s0\np4 off\np8 on 38 a0 s0\np12 off\n"), "triplets:\n%s", log_);
    /* transpose applies as notes start */
    bank[0].triplet = 0;
    reset();
    seq.transpose = 5;
    tbs_start(&seq, &SINK);
    run(6);
    CHECK(!strcmp(log_, "p0 on 41 a0 s0\np3 off\n"), "transpose:\n%s", log_);
}

static void test_midi(void)
{
    static const int N[] = {236, 148, 50};
    make(&bank[0], "xxx", N, 3);
    reset();
    seq.midi_ch = 1;
    tbs_start(&seq, &SINK);
    run(18);
    /* legato: the new note on before the old note off; accent = velocity 127 */
    CHECK(!strcmp(mlog, "91 36 96\n91 48 127\n81 36 0\n81 48 0\n91 50 96\n81 50 0\n"), "midi:\n%s", mlog);
}

int main(void)
{
    test_model();
    test_gates();
    test_patterns();
    test_midi();
    printf(fails ? "seq_test: %d FAILED\n" : "seq_test: all ok\n", fails);
    return fails != 0;
}
