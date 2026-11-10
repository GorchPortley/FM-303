/* SPDX-License-Identifier: GPL-3.0-only */
/* CHOPPAGE break analysis against the factory breaks, whose hits and kinds are known exactly:
 *   - the hits found (ana_onsets) are drum hits, within 10 ms, and most drum hits are found;
 *   - AUTO slicing with snap puts the cuts where the break's own sixteenths are;
 *   - the tags (ana_tag) match the drums in each slice;
 *   - the beats guessed from a length.
 * Writes each factory break as a WAV (build/host/factory_N.wav) to listen to.
 *   ana_test [OUTDIR] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../firmware/src/dsp/chp.h"
#include "../../firmware/src/dsp/ana.h"
#include "../../firmware/src/dsp/factory.h"

/* chp.h helpers live in engine.c; the analysis needs these two */
uint32_t chp_slice_end(const chp_break_t *b, int s) { return s + 1 < b->nsl ? b->start[s + 1] : b->len; }
int chp_slice_at(const chp_break_t *b, uint32_t pos)
{
    int s = 0;
    while (s + 1 < b->nsl && b->start[s + 1] <= pos)
        s++;
    return s;
}

static uint8_t buf[CHP_MEM];
static int fails;

static void wav8(const char *path, const uint8_t *d, uint32_t n, uint32_t rate)
{
    FILE *f = fopen(path, "wb");
    uint32_t v;
    uint16_t h[2];
    uint32_t i;
    if (!f)
        return;
    fwrite("RIFF", 1, 4, f);
    v = 36 + n * 2;
    fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    v = 16;
    fwrite(&v, 4, 1, f);
    h[0] = 1;
    h[1] = 1;
    fwrite(h, 2, 2, f);
    fwrite(&rate, 4, 1, f);
    v = rate * 2;
    fwrite(&v, 4, 1, f);
    h[0] = 2;
    h[1] = 16;
    fwrite(h, 2, 2, f);
    fwrite("data", 1, 4, f);
    v = n * 2;
    fwrite(&v, 4, 1, f);
    for (i = 0; i < n; i++) {
        int16_t s = (int16_t)((int8_t)d[i] * 256);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
}

static void check(int ok, const char *what)
{
    if (!ok) {
        printf("FAIL %s\n", what);
        fails++;
    }
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "build/host";
    static const char *const TN = "-KSHG";
    int i, total_tags = 0, right_tags = 0;
    for (i = 0; i < chp_factory_count(); i++) {
        chp_break_t truth, b;
        uint32_t n, hits[64];
        int nh, s, j, matched = 0, expected = 0, conf[NTAGS][NTAGS];
        char path[256];
        ana_cfg_t c = {SLM_AUTO, 50, 1};
        memset(&truth, 0, sizeof truth);
        memset(conf, 0, sizeof conf);
        n = chp_factory(i, buf, sizeof buf, &truth);
        check(n > 0, "factory break fits");
        snprintf(path, sizeof path, "%s/factory_%d.wav", dir, i + 1);
        wav8(path, buf, n, truth.rate);
        printf("%-7s %u samples at %u Hz, %u beats, %.1f BPM, %d slices\n", truth.name, n, truth.rate, truth.beats,
               (double)((float)truth.beats * 60.0f * (float)truth.rate / (float)n), truth.nsl);
        /* hits: every step with a drum on it is found, within 3 ms */
        nh = ana_onsets(&truth, buf, 50, hits, 64);
        for (s = 0; s < truth.nsl; s++) {
            if (truth.tag[s] == TAG_NONE)
                continue;
            expected++;
            for (j = 0; j < nh; j++) {
                int32_t d = (int32_t)hits[j] - (int32_t)truth.start[s];
                if (d > -(int32_t)truth.rate / 100 && d < (int32_t)truth.rate / 100) {
                    matched++;
                    break;
                }
            }
        }
        printf("  hits: %d found, %d of %d drum steps matched\n", nh, matched, expected);
        /* the detector is careful: a missed hit leaves a grid cut where it was, a false one moves a cut */
        check(matched * 100 >= expected * 60, "60% of the drum steps are found as hits");
        check(matched * 100 >= nh * 85, "85% of the hits found are drum hits");
        /* AUTO slicing with snap: the break's own sixteenths */
        b = truth;
        ana_slice(&b, buf, &c);
        check(b.nsl == truth.nsl, "AUTO cuts sixteenths");
        {
            int close = 0;
            for (s = 0; s < b.nsl && s < truth.nsl; s++) {
                int32_t d = (int32_t)b.start[s] - (int32_t)truth.start[s];
                if (d > -(int32_t)truth.rate * 4 / 1000 && d < (int32_t)truth.rate * 4 / 1000)
                    close++;
            }
            printf("  slices: %d, %d within 4 ms of the steps\n", b.nsl, close);
            check(close * 100 >= truth.nsl * 85, "85% of the cuts on the steps");
        }
        /* tags, on the true slices */
        b = truth;
        ana_tag(&b, buf);
        for (s = 0; s < truth.nsl; s++) {
            conf[truth.tag[s]][b.tag[s]]++;
            if (truth.tag[s] == TAG_NONE)
                continue;
            total_tags++;
            right_tags += b.tag[s] == truth.tag[s];
        }
        printf("  tags truth\\got  -   K   S   H   G\n");
        for (s = 0; s < NTAGS; s++)
            printf("            %c   %3d %3d %3d %3d %3d\n", TN[s], conf[s][0], conf[s][1], conf[s][2], conf[s][3],
                   conf[s][4]);
        check(conf[TAG_K][TAG_K] * 100 >= (conf[TAG_K][0] + conf[TAG_K][1] + conf[TAG_K][2] + conf[TAG_K][3] + conf[TAG_K][4]) * 85,
              "85% of the kicks tagged K");
        check(conf[TAG_S][TAG_S] * 4 >= (conf[TAG_S][0] + conf[TAG_S][1] + conf[TAG_S][2] + conf[TAG_S][3] + conf[TAG_S][4]) * 3,
              "most snares tagged S");
    }
    printf("tags: %d of %d right (%.0f%%)\n", right_tags, total_tags, 100.0 * right_tags / (total_tags ? total_tags : 1));
    check(right_tags * 100 >= total_tags * 90, "90% of the tags right");
    /* beats from a length */
    check(ana_guess_beats(77836, 22050) == 8, "2 bars of 136 BPM: 8 beats");
    check(ana_guess_beats(38918, 22050) == 4, "1 bar of 136 BPM: 4 beats");
    check(ana_guess_beats(51882, 22050) == 4, "1 bar of 102 BPM: 4 beats");
    check(ana_guess_beats(155672, 44100) == 8, "2 bars at 44.1 kHz: 8 beats");
    printf(fails ? "ana: FAILED\n" : "ana: ok\n");
    return fails ? 1 : 0;
}
