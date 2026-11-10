/* SPDX-License-Identifier: GPL-3.0-only */
/* CHOPPAGE break analysis: see ana.h.
 *
 * Hits: the break is cut into frames of ~5 ms. Each frame's energy is measured twice, as it is and
 * after a first difference (which favours the crack of a snare and the tick of a hat); a hit is a
 * frame where either jumps up against the frames just before it (a rectified log difference, the
 * "flux"), standing out from the break's average flux by an amount SENS sets. Each hit is then
 * placed on the sample where the sound starts to rise, moved back to a zero crossing.
 *
 * Tags: the first 70 ms of a slice through three filters: below ~120 Hz (a kick's body),
 * 250 Hz .. 2.5 kHz (a snare's body), above ~4 kHz (snare wires, hats, rides). Each slice is judged
 * against the others in the break: a kick has the low band, a snare the middle and the top and a
 * strong attack, a ghost is a snare well below the loudest snare, and a hat is what is left. */
#include "ana.h"
#include "fastmath.h"

const char *const ANA_MODE_NAME[NSLM] = {"Auto", "8", "16", "32", "Hits"};

#define NF 1024                                  /* frames, at most (a long break: longer frames) */
static float odf[NF];                            /* the onset function */
static float fen[NF];                            /* frame energy */
static uint8_t flow[NF];                         /* the frame's flux was mostly low: a kick */

static float smp(const chp_break_t *b, const uint8_t *d, uint32_t i)
{
    uint32_t v = d[i];
    if (b->fmt == FMT_ULAW) {
        int u = (int)(~v & 0xFFu), e = (u >> 4) & 7, m = u & 15, mag = (((m << 3) + 0x84) << e) - 0x84;
        return (float)((u & 0x80) ? -mag : mag) * (1.0f / 32768.0f);
    }
    return (float)(int8_t)v * (1.0f / 128.0f);
}

uint8_t ana_guess_beats(uint32_t len, uint32_t rate)
{
    static const uint8_t B[] = {1, 2, 4, 8, 16, 32};
    float sec = (float)len / (float)(rate ? rate : 22050u), best = 1e9f;
    uint32_t i;
    uint8_t out = 4;
    for (i = 0; i < sizeof B; i++) {
        float bpm = (float)B[i] * 60.0f / sec, d = fm_fabsf(fm_log2f(bpm / 125.0f));
        if (d < best) {
            best = d;
            out = B[i];
        }
    }
    return out;
}

/* where a hit's sound starts, on one envelope (0.5 ms blocks): the first block that climbs 30 % of
 * the way to the peak from the level before it. That level is the quietest block before the peak,
 * or (ceil: a kick on the signal, over a tail that swings) the loudest of the blocks well before it */
static uint32_t rise(const float *env, uint32_t nb, int ceil)
{
    uint32_t j, jp = 0;
    float pk = 0.0f, base = ceil ? 0.0f : 1e9f;
    for (j = 0; j < nb; j++)
        if (env[j] > pk) {
            pk = env[j];
            jp = j;
        }
    for (j = 0; j < jp; j++)
        if (ceil ? j + 3u < jp : 1)
            base = ceil ? fm_maxf(base, env[j]) : fm_minf(base, env[j]);
    if (base > pk)
        base = 0.0f;
    for (j = 0; j < nb; j++)
        if (env[j] >= base + 0.3f * (pk - base) && (env[j] > base || !ceil))
            return j;
    return jp;
}

/* the sample where the hit in frame f begins. On the first difference, where a low tail (a kick
 * ringing on) nearly vanishes; for a kick (low: it was found in the low band) on the signal itself,
 * since a kick has little top. Then back to a zero crossing. */
static uint32_t refine(const chp_break_t *b, const uint8_t *d, uint32_t f, uint32_t hop, int low)
{
    uint32_t a = f > 1u ? (f - 2u) * hop : 0, e = (f + 2u) * hop, blk = b->rate / 2000u, i, j, at, back, nb, j1, j2;
    float ea[64], eh[64];
    if (blk < 4u)
        blk = 4u;
    if (e > b->len)
        e = b->len;
    nb = (e - a) / blk;
    if (nb > 64u)
        nb = 64u;
    if (nb < 2u)
        return a;
    for (j = 0; j < nb; j++) {
        float m = 0.0f, h = 0.0f;
        for (i = a + j * blk; i < a + (j + 1u) * blk; i++) {
            float x = smp(b, d, i);
            m = fm_maxf(m, fm_fabsf(x));
            h = fm_maxf(h, fm_fabsf(x - (i ? smp(b, d, i - 1u) : 0.0f)));
        }
        ea[j] = m;
        eh[j] = h;
    }
    j1 = rise(ea, nb, 1);
    j2 = rise(eh, nb, 0);
    at = a + (low ? j1 : j2) * blk;
    back = b->rate / 1000u;                      /* 1 ms back to a zero crossing, at most */
    for (i = at; i > 0 && at - i < back; i--)
        if ((smp(b, d, i) >= 0.0f) != (smp(b, d, i - 1u) >= 0.0f))
            return i;
    return at;
}

int ana_onsets(const chp_break_t *b, const uint8_t *d, int sens, uint32_t *out, int max)
{
    uint32_t hop = b->rate / 200u, nf, f, i, mind;
    float mean = 0.0f, var = 0.0f, thr, emax = 0.0f, pl = 0.0f, ph = 0.0f, pk = 0.0f, l1 = 0.0f, l2 = 0.0f;
    float a_lo = 1.0f - fm_expf(-FM_TWO_PI * 150.0f / (float)(b->rate ? b->rate : 22050u));
    int n = 0;
    if (!b->len || !b->rate)
        return 0;
    if (hop < 16u)
        hop = 16u;
    if (b->len / hop >= NF)
        hop = b->len / (NF - 1u) + 1u;
    nf = b->len / hop;
    if (nf < 3u)
        return 0;
    for (f = 0; f < nf; f++) {                   /* energy and flux: all, the top, the bottom (kicks) */
        float el = 0.0f, eh = 0.0f, ek = 0.0f, prev = f ? smp(b, d, f * hop - 1u) : 0.0f, fl, fh, fk;
        for (i = f * hop; i < (f + 1u) * hop; i++) {
            float x = smp(b, d, i), dx = x - prev;
            l1 += a_lo * (x - l1);
            l2 += a_lo * (l1 - l2);
            el += x * x;
            eh += dx * dx;
            ek += l2 * l2;
            prev = x;
        }
        fen[f] = el;
        emax = fm_maxf(emax, el);
        el = fm_log2f(el + 1e-6f);
        eh = fm_log2f(eh + 1e-6f);
        ek = fm_log2f(ek + 1e-6f);
        fl = f ? fm_maxf(0.0f, el - pl) : 0.0f;
        fh = f ? fm_maxf(0.0f, eh - ph) : 0.0f;
        fk = f ? fm_maxf(0.0f, ek - pk) : 0.0f;
        odf[f] = fl * 0.5f + fh + fk;
        flow[f] = (uint8_t)(fk > fh);
        pl = f > 1u ? fm_maxf(el, pl * 0.5f + el * 0.5f) : el;   /* against the recent level */
        ph = f > 1u ? fm_maxf(eh, ph * 0.5f + eh * 0.5f) : eh;
        pk = f > 1u ? fm_maxf(ek, pk * 0.5f + ek * 0.5f) : ek;
        mean += odf[f];
    }
    mean /= (float)nf;
    for (f = 0; f < nf; f++)
        var += (odf[f] - mean) * (odf[f] - mean);
    var /= (float)nf;
    thr = mean + (2.2f - 2.0f * (float)sens * 0.01f) * fm_sqrtf(var);
    mind = (b->rate * 35u / 1000u) / hop + 1u;   /* 35 ms between hits, at least */
    for (f = 1; f + 1u < nf && n < max; f++) {
        uint32_t j, lo = f > 2u ? f - 2u : 0, hi = f + 2u < nf ? f + 2u : nf - 1u, peak = 1;
        if (odf[f] < thr || fen[f] + (f + 1u < nf ? fen[f + 1u] : 0.0f) < emax * 1e-3f)
            continue;
        for (j = lo; j <= hi; j++)
            if (j != f && (odf[j] > odf[f] || (odf[j] == odf[f] && j < f)))
                peak = 0;
        if (!peak)
            continue;
        {
            uint32_t at = refine(b, d, f, hop, flow[f]);
            if (n && at < out[n - 1] + mind * hop)
                continue;
            out[n++] = at;
        }
    }
    return n;
}

static uint32_t near_hit(const uint32_t *h, int nh, uint32_t g, uint32_t win, int *found)
{
    int i;
    uint32_t best = g, bd = win + 1u;
    *found = 0;
    for (i = 0; i < nh; i++) {
        uint32_t dd = h[i] > g ? h[i] - g : g - h[i];
        if (dd <= win && dd < bd) {
            bd = dd;
            best = h[i];
            *found = 1;
        }
    }
    return best;
}

void ana_slice(chp_break_t *b, const uint8_t *d, const ana_cfg_t *c)
{
    static uint32_t hits[64];
    int nh, n, i;
    uint32_t minlen;
    if (!b->len) {
        b->nsl = 1;
        b->start[0] = 0;
        return;
    }
    minlen = b->rate / 50u + 1u;                 /* 20 ms, the shortest slice */
    nh = ana_onsets(b, d, c->mode == SLM_HITS ? c->sens : 50, hits, 64);
    if (c->mode == SLM_HITS) {
        n = 0;
        b->start[n++] = 0;
        for (i = 0; i < nh && n < CHP_NSL; i++)
            if (hits[i] >= b->start[n - 1] + minlen && hits[i] + minlen < b->len)
                b->start[n++] = hits[i];
        if (n > 1 && b->start[1] < b->rate / 20u)   /* a hit right at the top: that is slice 1 */
            for (i = 1; i < n; i++)
                b->start[i - 1] = i == 1 ? 0 : b->start[i];
        if (n > 1 && b->start[1] == 0)
            n--;
        b->nsl = (uint8_t)n;
        return;
    }
    if (c->mode == SLM_8)
        n = 8;
    else if (c->mode == SLM_16)
        n = 16;
    else if (c->mode == SLM_32)
        n = 32;
    else {                                       /* AUTO: sixteenths; eighths for a long break */
        n = (b->beats ? b->beats : 4) * 4;
        if (n > CHP_NSL)
            n = (b->beats ? b->beats : 4) * 2;
        if (n > CHP_NSL)
            n = CHP_NSL;
    }
    if ((uint32_t)n * minlen > b->len)
        n = (int)(b->len / minlen) > 0 ? (int)(b->len / minlen) : 1;
    b->start[0] = 0;
    for (i = 1; i < n; i++) {
        uint32_t g = b->len / (uint32_t)n * (uint32_t)i + b->len % (uint32_t)n * (uint32_t)i / (uint32_t)n, s = g;
        int found = 0;
        if (c->snap)
            s = near_hit(hits, nh, g, b->len / (uint32_t)n * 35u / 100u, &found);
        if (s < b->start[i - 1] + minlen)
            s = b->start[i - 1] + minlen;
        if (s + minlen >= b->len)
            break;
        b->start[i] = s;
    }
    b->nsl = (uint8_t)i;
}

/* band energies over [a, e): low (< 120 Hz), mid (250 Hz .. 2.5 kHz), high (> 4 kHz), all; peak */
typedef struct {
    float l, m, h, t, pk;
} bands_t;

static void bands(const chp_break_t *b, const uint8_t *d, uint32_t a, uint32_t e, bands_t *o)
{
    float fs = (float)b->rate, l1 = 0, l2 = 0, m1 = 0, m2 = 0, h1 = 0;
    float a_lo = 1.0f - fm_expf(-FM_TWO_PI * 160.0f / fs), a_m1 = 1.0f - fm_expf(-FM_TWO_PI * 250.0f / fs);
    float a_m2 = 1.0f - fm_expf(-FM_TWO_PI * fm_minf(2500.0f, 0.2f * fs) / fs);
    float a_hi = 1.0f - fm_expf(-FM_TWO_PI * fm_minf(4000.0f, 0.3f * fs) / fs);
    uint32_t i, w = b->rate / 200u;              /* 5 ms to settle the filters first */
    o->l = o->m = o->h = o->t = o->pk = 0.0f;
    for (i = a > w ? a - w : 0; i < e && i < b->len; i++) {
        float x = smp(b, d, i);
        l1 += a_lo * (x - l1);
        l2 += a_lo * (l1 - l2);
        m1 += a_m1 * (x - m1);
        m2 += a_m2 * (x - m2);
        h1 += a_hi * (x - h1);
        if (i < a)
            continue;
        o->l += l2 * l2;
        o->m += (m2 - m1) * (m2 - m1);
        o->h += (x - h1) * (x - h1);
        o->t += x * x;
        o->pk = fm_maxf(o->pk, fm_fabsf(x));
    }
}

void ana_tag(chp_break_t *b, const uint8_t *d)
{
    static bands_t lg[CHP_NSL];                  /* the first 70 ms */
    static float rl[CHP_NSL], rm[CHP_NSL], rh[CHP_NSL];   /* how each band rises at the start */
    float maxl = 0.0f, maxm = 0.0f, maxpk = 0.0f, snpk = 0.0f;
    int s;
    if (!b->len || !b->rate)
        return;
    for (s = 0; s < b->nsl; s++) {
        uint32_t a = b->start[s], e = chp_slice_end(b, s), w = b->rate * 70u / 1000u, w0 = b->rate * 12u / 1000u;
        uint32_t wl = b->rate * 40u / 1000u;     /* the low band needs longer to show a rise */
        bands_t pre, post, lpre, lpost;
        if (e > a + w)
            e = a + w;
        bands(b, d, a, e, &lg[s]);
        bands(b, d, a, a + w0, &post);
        bands(b, d, a, a + wl, &lpost);
        if (a >= w0)
            bands(b, d, a - w0, a, &pre);
        else
            pre.l = pre.m = pre.h = pre.t = pre.pk = 0.0f;
        if (a >= wl)
            bands(b, d, a - wl, a, &lpre);
        else
            lpre.l = 0.0f;
        rl[s] = lpost.l / (lpre.l + 1e-7f + 0.02f * lpost.l);
        rm[s] = post.m / (pre.m + 1e-7f + 0.02f * post.m);
        rh[s] = post.h / (pre.h + 1e-7f + 0.02f * post.h);
        maxl = fm_maxf(maxl, lg[s].l);
        maxpk = fm_maxf(maxpk, lg[s].pk);
    }
#ifdef ANA_DEBUG
    for (s = 0; s < b->nsl; s++)
        printf("  s%2d l %.4f m %.4f h %.4f t %.4f pk %.3f  rl %.1f rm %.1f rh %.1f  lr %.2f mr %.2f\n", s, (double)lg[s].l,
               (double)lg[s].m, (double)lg[s].h, (double)lg[s].t, (double)lg[s].pk, (double)rl[s], (double)rm[s],
               (double)rh[s], (double)(lg[s].l / (lg[s].t + 1e-9f)), (double)(lg[s].m / (lg[s].t + 1e-9f)));
#endif
    for (s = 0; s < b->nsl; s++) {               /* kicks: the low band jumps and is big, or a hit */
        float lr = lg[s].l / (lg[s].t + 1e-9f), hr = lg[s].h / (lg[s].t + 1e-9f);   /* that is all low */
        int hit = rl[s] > 2.0f || rm[s] > 1.8f || rh[s] > 1.8f;
        b->tag[s] = lg[s].l > 0.25f * maxl && ((rl[s] > 2.0f && lr > 0.2f) || (hit && rl[s] > 1.1f && lr > 0.4f && hr < 0.02f))
                        ? TAG_K : TAG_NONE;
        if (b->tag[s] == TAG_NONE && (rm[s] > 1.8f || rh[s] > 1.8f))
            maxm = fm_maxf(maxm, lg[s].m);
    }
    for (s = 0; s < b->nsl; s++) {               /* snares: the body jumps, and the top with it */
        float mr = lg[s].m / (lg[s].t + 1e-9f);
        if (b->tag[s] != TAG_NONE)
            continue;
        if (rm[s] > 1.8f && mr > 0.08f && lg[s].m > 0.2f * maxm) {
            b->tag[s] = TAG_S;
            snpk = fm_maxf(snpk, lg[s].pk);
        } else if (rm[s] > 1.5f && mr > 0.06f && lg[s].m > 0.05f * maxm) {
            b->tag[s] = TAG_G;
        } else if (rh[s] > 1.3f && lg[s].pk > 0.02f * maxpk) {
            b->tag[s] = TAG_H;                   /* the top jumps alone: a hat, a ride */
        }
    }
    for (s = 0; s < b->nsl; s++) {
        if (b->tag[s] == TAG_S && lg[s].pk < 0.6f * snpk)
            b->tag[s] = TAG_G;                   /* well under the loudest snare: a ghost */
        else if (b->tag[s] == TAG_G && lg[s].pk >= 0.8f * snpk && snpk > 0.0f)
            b->tag[s] = TAG_S;
    }
}
