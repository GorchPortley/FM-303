/* SPDX-License-Identifier: GPL-3.0-only */
/* CHOPPAGE factory breaks: see factory.h. A small drum machine plays each pattern into the sample
 * memory: a kick (a falling sine and a click), a snare (two tones and filtered noise; ghosts are
 * quieter and shorter), hats and a ride (six square waves, as the analog machines make cymbals, high-
 * passed, with noise). It plays twice: the first time to find the peak, the second to write 8-bit
 * samples normalized to it (the noise is the same both times). The patterns are CHOPPAGE's own. */
#include "factory.h"
#include "fastmath.h"

typedef struct {
    const char *name;
    uint16_t rate;
    uint8_t beats;
    float bpm, swing;                    /* swing: where the off sixteenths fall, 0.5 = straight */
    const char *k, *s, *h, *r;           /* a line each, a character a sixteenth:
                                            k x; s x (snare) g (ghost); h x (closed) o (open); r x */
} fb_t;

static const fb_t FB[] = {
    {"RINSE", 22050, 8, 138.0f, 0.5f,
     "x.x.......xx....x.x.......x.....",
     "....x..g.g..x..g....x..g.g..x.g.",
     "..............................o.",
     "x.x.x.x.x.x.x.x.x.x.x.x.x.x.x..."},
    {"TUMBLE", 22050, 4, 102.0f, 0.56f,
     "x..x......x..x..",
     "....x..g...gx..g",
     "xxoxxxxxxxoxxxxx",
     "................"},
    {"RIDER", 16574, 4, 96.0f, 0.5f,
     "x.....x...x.....",
     "....x....g..x...",
     "................",
     "x.xxx.xxx.xxx.xx"},
};
#define NFB (int)(sizeof FB / sizeof FB[0])

int chp_factory_count(void) { return NFB; }

static uint32_t lcg;
static float noise(void)
{
    lcg = lcg * 1664525u + 1013904223u;
    return (float)(int32_t)lcg * (1.0f / 2147483648.0f);
}

static float dec(float tau, float fs) { return fm_expf(-1.0f / (tau * fs)); }
static float onepole(float fc, float fs) { return 1.0f - fm_expf(-FM_TWO_PI * fm_minf(fc, 0.42f * fs) / fs); }

typedef struct {
    float e1, e2, e3, d1, d2, d3, ph, ph2, amp, y1, y2, prev;
    uint8_t on, kind;
} drum_t;

static const float METAL[6] = {205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f};

static float metal(float *ph, float scale, float fs)
{
    float s = 0.0f;
    int i;
    for (i = 0; i < 6; i++) {
        ph[i] += METAL[i] * scale / fs;
        if (ph[i] >= 1.0f)
            ph[i] -= 1.0f;
        s += ph[i] < 0.5f ? 1.0f : -1.0f;
    }
    return s * (1.0f / 6.0f);
}

static float play(const fb_t *f, uint32_t n_total, int pass_peak, uint8_t *dst, float norm, chp_break_t *b)
{
    float fs = (float)f->rate, peak = 0.0f;
    uint32_t steps = (uint32_t)f->beats * 4u, sd = n_total / steps, n, next = 0, si = 0;
    float swing_d = (f->swing - 0.5f) * 2.0f * (float)sd;
    drum_t K = {0}, S = {0}, H = {0}, R = {0};
    float hph[6] = {0}, rph[6] = {0};
    float a_hp_s = onepole(1200.0f, fs), a_lp_s = onepole(9000.0f, fs), a_hp_h = onepole(6500.0f, fs),
          a_hp_r = onepole(3000.0f, fs);
    lcg = 0x5EEDu + (uint32_t)f->rate;
    for (n = 0; n < n_total; n++) {
        float x = 0.0f;
        while (si < steps && n >= next) {        /* the hits of step si */
            char ck = f->k[si], cs = f->s[si], ch = f->h[si], cr = f->r[si];
            float acc = (si & 1u) ? 0.8f : 1.0f;
            if (b) {
                b->start[si] = next;
                b->tag[si] = ck == 'x' ? TAG_K : cs == 'x' ? TAG_S : cs == 'g' ? TAG_G :
                             (ch != '.' || cr != '.') ? TAG_H : TAG_NONE;
            }
            if (ck == 'x') {
                K.on = 1;
                K.e1 = 1.0f;                     /* body */
                K.e2 = 1.0f;                     /* click */
                K.e3 = 1.0f;                     /* pitch */
                K.d1 = dec(0.16f, fs);
                K.d2 = dec(0.0012f, fs);
                K.d3 = dec(0.028f, fs);
                K.ph = 0.0f;
                K.amp = 1.0f;
            }
            if (cs == 'x' || cs == 'g') {
                S.on = 1;
                S.kind = cs == 'g';
                S.e1 = S.e2 = 1.0f;
                S.d1 = dec(S.kind ? 0.03f : 0.05f, fs);
                S.d2 = dec(S.kind ? 0.05f : 0.11f, fs);
                S.amp = S.kind ? 0.28f : 0.9f;
                S.ph = S.ph2 = 0.0f;
            }
            if (ch == 'x' || ch == 'o') {        /* closed and open are one voice: a close chokes */
                H.on = 1;
                H.e1 = 1.0f;
                H.d1 = dec(ch == 'o' ? 0.24f : 0.028f, fs);
                H.amp = 0.75f * acc;
            }
            if (cr == 'x') {
                R.on = 1;
                R.e1 = 1.0f;                     /* wash */
                R.e2 = 1.0f;                     /* stick */
                R.d1 = dec(0.8f, fs);
                R.d2 = dec(0.012f, fs);
                R.amp = 0.55f * acc;
            }
            si++;
            next = si * sd + ((si & 1u) ? (uint32_t)swing_d : 0u);
        }
        if (K.on) {
            float fq = 46.0f + 130.0f * K.e3;
            K.ph += fq / fs;
            if (K.ph >= 1.0f)
                K.ph -= 1.0f;
            x += (fm_sinf(FM_TWO_PI * K.ph) * K.e1 + noise() * K.e2 * 0.5f) * K.amp;
            K.e1 *= K.d1;
            K.e2 *= K.d2;
            K.e3 *= K.d3;
            if (K.e1 < 1e-4f)
                K.on = 0;
        }
        if (S.on) {
            float nz = noise(), hp;
            S.ph += 185.0f / fs;
            S.ph2 += 330.0f / fs;
            if (S.ph >= 1.0f)
                S.ph -= 1.0f;
            if (S.ph2 >= 1.0f)
                S.ph2 -= 1.0f;
            S.y1 += a_hp_s * (nz - S.y1);        /* highpass: the noise less its lowpass */
            hp = nz - S.y1;
            S.y2 += a_lp_s * (hp - S.y2);
            x += ((fm_sinf(FM_TWO_PI * S.ph) + 0.6f * fm_sinf(FM_TWO_PI * S.ph2)) * 0.5f * S.e1 + S.y2 * 1.3f * S.e2) *
                 S.amp;
            S.e1 *= S.d1;
            S.e2 *= S.d2;
            if (S.e2 < 1e-4f)
                S.on = 0;
        }
        if (H.on) {
            float m = metal(hph, 1.9f, fs) * 0.6f + noise() * 0.5f;
            H.y1 += a_hp_h * (m - H.y1);
            x += (m - H.y1) * H.e1 * H.amp;
            H.e1 *= H.d1;
            if (H.e1 < 1e-4f)
                H.on = 0;
        }
        if (R.on) {
            float m = metal(rph, 1.25f, fs) * 0.8f + noise() * 0.25f;
            R.y1 += a_hp_r * (m - R.y1);
            x += (m - R.y1) * (0.4f * R.e1 + 1.6f * R.e2) * R.amp;
            R.e1 *= R.d1;
            R.e2 *= R.d2;
            if (R.e1 < 1e-4f)
                R.on = 0;
        }
        x = fm_tanhf(x * 1.15f);                 /* a little glue */
        if (pass_peak) {
            peak = fm_maxf(peak, fm_fabsf(x));
        } else {
            float v = x * norm * 126.0f;
            int32_t q = (int32_t)(v < 0.0f ? v - 0.5f : v + 0.5f);
            dst[n] = (uint8_t)(int8_t)(q < -127 ? -127 : q > 127 ? 127 : q);
        }
    }
    return peak;
}

uint32_t chp_factory(int i, uint8_t *dst, uint32_t max, chp_break_t *b)
{
    const fb_t *f;
    uint32_t n, k;
    float peak;
    if (i < 0 || i >= NFB)
        return 0;
    f = &FB[i];
    n = (uint32_t)((float)f->beats * 60.0f / f->bpm * (float)f->rate);
    if (n > max)
        return 0;
    peak = play(f, n, 1, 0, 0.0f, 0);
    play(f, n, 0, dst, peak > 0.0f ? 1.0f / peak : 1.0f, b);
    for (k = 0; k < sizeof b->name; k++)
        b->name[k] = 0;
    for (k = 0; f->name[k] && k < sizeof b->name; k++)
        b->name[k] = f->name[k];
    b->len = n;
    b->rate = f->rate;
    b->fmt = FMT_PCM8;
    b->beats = f->beats;
    b->nsl = (uint8_t)(f->beats * 4u);
    b->rsv[0] = b->rsv[1] = b->rsv[2] = 0;
    return n;
}
