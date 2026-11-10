/* SPDX-License-Identifier: GPL-3.0-only */
/* CHOPPAGE generator: see gen.h.
 *
 * Styles are bars of sixteen steps written in types, so they suit any break that is tagged:
 *   K / S / H / G   kick, snare, hat or ride, ghost snare: always there
 *   k / s / h / g   the same, there or not depending on DENSITY
 *   .               nothing: the slice before plays on
 * STRAIGHT plays the break in its own order and swaps slices for others of the same type (it keeps
 * the groove and changes the hits); TRACKER jumps around the break by slice number, the way a
 * tracker's sample offset does, with stutters. CHAOS adds rolls (often climbing in pitch), reversed
 * snares before the backbeat, pitch jumps and stutters. */
#include "gen.h"

uint32_t gen_hash(uint32_t a, uint32_t b)
{
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u + (a << 6) + (a >> 2));
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}

typedef struct {
    uint32_t s;
} rng_t;
static uint32_t r_next(rng_t *r)
{
    r->s ^= r->s << 13;
    r->s ^= r->s >> 17;
    r->s ^= r->s << 5;
    return r->s;
}
static int r_int(rng_t *r, int n) { return n > 0 ? (int)(r_next(r) % (uint32_t)n) : 0; }
static int r_pct(rng_t *r, int pct) { return r_int(r, 100) < pct; }
static void r_seed(rng_t *r, uint32_t s) { r->s = gen_hash(s, 0x51CEu) | 1u; }

#define NT 4
static const char *const TPL[NSTYLES][NT] = {
    /* STRAIGHT and TRACKER use the break's own order; these are their fallbacks (no break) */
    {"K.h.S.hgk.K.S.h.", "K.h.S.h.h.K.S.hg", "K.hgS.h.k.K.S.hg", "K.h.S..gK.k.S.h."},
    /* JUNGLE: the two-step (kick on 1 and the and-of-2, snare on 2 and 4) and its cousins */
    {"K.h.S.hgk.K.S.hg", "K.h.S..gh.K.S.hg", "K.hgS.h.k.KgS.h.", "K.h.S.hgK.k.S.gh"},
    /* ROLLER: steady, the hats drive */
    {"K.h.S.h.h.K.S.h.", "K.hgS.hgh.K.Sgh.", "K.h.S.h.K.h.S.hg", "K.h.S.hgh.K.S.h."},
    /* HALFTIME: the snare on 3 */
    {"K.h.h.h.S.h.h.hg", "K.h.hgk.S.h.k.hg", "K.hgh.h.S.hgh.k.", "K.h.k.h.S.h.hgh."},
    /* BREAKCORE: everything, all the time */
    {"KSgSKgSKSgKSgKSS", "KkSgKSSgKkSgSKSS", "K.SSK.SKkSKSSSSS", "KSKSgSKSSKSgKSSS"},
    {"K.h.S.hgk.K.S.h.", "K.h.S.h.h.K.S.hg", "K.hgS.h.k.K.S.hg", "K.h.S..gK.k.S.h."},
};

static const uint8_t TAG_LEV[NTAGS] = {15, 15, 15, 12, 9};

static int tag_of_char(char c)
{
    switch (c) {
    case 'K': case 'k': return TAG_K;
    case 'S': case 's': return TAG_S;
    case 'H': case 'h': return TAG_H;
    case 'G': case 'g': return TAG_G;
    default: return TAG_NONE;
    }
}

static int has_break(const chp_break_t *b) { return b && b->len && b->nsl; }

uint8_t gen_straight_src(const chp_break_t *b, int i)
{
    uint32_t steps, sd, t;
    int s;
    if (!has_break(b) || !b->beats)
        return SRC_NONE;
    steps = (uint32_t)b->beats * 4u;
    sd = b->len / steps;                         /* a sixteenth of the break, in samples */
    if (!sd)
        return SRC_NONE;
    t = ((uint32_t)i % steps) * sd;
    s = chp_slice_at(b, t + sd / 2u);            /* the slice nearest the step, if it starts near it */
    if (s + 1 < b->nsl && b->start[s + 1] <= t + sd / 2u)
        s++;
    {
        uint32_t st = b->start[s], d = st > t ? st - t : t - st;
        if (d < sd / 2u)
            return (uint8_t)(SRC_SLICE + s);
        if (s + 1 < b->nsl) {
            st = b->start[s + 1];
            d = st > t ? st - t : t - st;
            if (d < sd / 2u)
                return (uint8_t)(SRC_SLICE + s + 1);
        }
    }
    return SRC_NONE;
}

/* another slice with the same tag as slice s (or s itself) */
static int same_tag(const chp_break_t *b, int s, rng_t *r)
{
    int n = 0, i, k, t = b->tag[s];
    for (i = 0; i < b->nsl; i++)
        n += b->tag[i] == t;
    if (n <= 1)
        return s;
    k = r_int(r, n);
    for (i = 0; i < b->nsl; i++)
        if (b->tag[i] == t && k-- == 0)
            return i;
    return s;
}

/* one step of a style, at position i of the bar */
static void style_step(chp_step_t *st, int style, const char *tpl, int i, const chp_break_t *b, int dens,
                       rng_t *r)
{
    char c = tpl[i & 15];
    int t = tag_of_char(c);
    chp_step_clear(st);
    if (style == GS_STRAIGHT && has_break(b)) {
        st->src = gen_straight_src(b, i);
        if (SRC_IS_SLICE(st->src))
            st->lev = TAG_LEV[b->tag[st->src - SRC_SLICE]];
        return;
    }
    if (t == TAG_NONE) {                         /* a rest: sometimes a ghost, with density */
        if (r_pct(r, dens / 6)) {
            st->src = SRC_G;
            st->lev = TAG_LEV[TAG_G];
        }
        return;
    }
    if (c >= 'a' && c <= 'z' && !r_pct(r, 25 + dens * 3 / 4))
        return;                                  /* optional, left out */
    st->src = (uint8_t)t;
    st->lev = TAG_LEV[t];
}

static void chaos_step(chp_step_t *p, int i, int len, int style, int chaos, rng_t *r)
{
    chp_step_t *st = &p[i];
    int bi = i & 15, before_snare = bi == 3 || bi == 11 || (style == GS_HALF && bi == 7);
    int roll_odds = style == GS_BREAKCORE ? chaos / 2 : chaos / 5, end = bi >= 12;
    if (before_snare && r_pct(r, chaos / 4)) {   /* a reversed snare, sucked into the backbeat */
        st->src = SRC_S;
        st->fx = FX_REV;
        st->lev = 12;
        return;
    }
    if ((end || before_snare || style == GS_BREAKCORE) && st->src != SRC_NONE && r_pct(r, roll_odds)) {
        int n = style == GS_BREAKCORE ? 1 + r_int(r, 5) : 1 + r_int(r, 3);   /* 2x .. 4x (8x) */
        st->fx = (uint8_t)((st->fx & ~FX_ROLL) | (uint32_t)n);
        if (r_pct(r, style == GS_BREAKCORE ? 60 : 30))
            st->fx |= r_pct(r, 70) ? FX_RAMP_UP : FX_RAMP_DN;
    }
    if (st->src != SRC_NONE && r_pct(r, chaos / 10)) {
        static const int8_t J[] = {12, -12, 7, 5, -5, 24};
        st->pitch = J[r_int(r, style == GS_BREAKCORE ? 6 : 4)];
    }
    if (i > 0 && st->src == SRC_NONE && r_pct(r, chaos / 8)) {   /* a stutter: the step before, again */
        *st = p[i - 1];
        st->fx &= (uint8_t)~(FX_RAMP_UP | FX_RAMP_DN);
    }
    if (style == GS_BREAKCORE && r_pct(r, chaos / 12))
        st->fx |= FX_REV;
    (void)len;
}

static void tracker_bar(chp_step_t *p, int from, int len, const chp_break_t *b, int dens, int chaos, rng_t *r)
{
    int i, ptr = from % (has_break(b) ? b->beats * 4 : 16);
    for (i = from; i < from + 16 && i < len; i++) {
        chp_step_t *st = &p[i];
        uint8_t src;
        chp_step_clear(st);
        if ((i & 3) == 0 && r_pct(r, 20 + chaos / 2)) /* a jump, on the beat: 9xx */
            ptr = r_int(r, 8) * 2;
        src = gen_straight_src(b, ptr);
        if (src == SRC_NONE && (i & 1) == 0 && r_pct(r, dens))
            src = (uint8_t)(SRC_SLICE + r_int(r, b->nsl));
        st->src = src;
        if (i > from && r_pct(r, chaos / 5)) {    /* E9x: retrigger the one before */
            *st = p[i - 1];
            st->fx = (uint8_t)((st->fx & ~FX_ROLL) | (uint32_t)(1 + r_int(r, 3)));
        }
        if (r_pct(r, chaos / 10))
            st->fx |= FX_REV;
        ptr++;
    }
}

void gen_write(chp_pattern_t *p, const gen_cfg_t *c, const chp_break_t *b, uint32_t seed)
{
    rng_t r;
    int len = p->len < 1 ? 16 : p->len, bar, i;
    int style = c->style < NSTYLES ? c->style : GS_JUNGLE;
    r_seed(&r, seed);
    chp_pattern_clear(p, len);
    for (bar = 0; bar * 16 < len; bar++) {
        const char *tpl = TPL[style][r_int(&r, NT)];
        if (style == GS_TRACKER && has_break(b)) {
            tracker_bar(p->st, bar * 16, len, b, c->dens, c->chaos, &r);
            continue;
        }
        for (i = bar * 16; i < bar * 16 + 16 && i < len; i++) {
            style_step(&p->st[i], style, tpl, i, b, c->dens, &r);
            if (style == GS_STRAIGHT && has_break(b) && SRC_IS_SLICE(p->st[i].src) &&
                r_pct(&r, c->chaos / 2)) {       /* the same kind of hit, another one of them */
                int s = same_tag(b, p->st[i].src - SRC_SLICE, &r);
                p->st[i].src = (uint8_t)(SRC_SLICE + s);
            }
        }
    }
    for (i = 0; i < len; i++)
        chaos_step(p->st, i, len, style, c->chaos, &r);
}

/* the last beat of a pass, as a fill */
static void fill_beat(chp_step_t *p, int len, int style, rng_t *r)
{
    int i, from = len >= 4 ? len - 4 : 0, kind = r_int(r, style == GS_BREAKCORE ? 4 : 3);
    for (i = from; i < len; i++) {
        chp_step_t *st = &p[i];
        int k = i - from;
        chp_step_clear(st);
        switch (kind) {
        case 0:                                  /* a snare roll that speeds up and climbs */
            st->src = SRC_S;
            st->fx = (uint8_t)(k < 2 ? 1u : k == 2 ? 3u : 5u);
            if (k >= 2)
                st->fx |= FX_RAMP_UP;
            st->lev = (uint8_t)(10 + k);
            break;
        case 1:                                  /* kick, snare, snare, a reversed snare */
            st->src = k == 0 ? SRC_K : SRC_S;
            if (k == 3)
                st->fx = FX_REV;
            if (k == 2)
                st->fx = 1u;
            break;
        case 2:                                  /* ghosts into a snare, then a stutter */
            st->src = k == 2 ? SRC_S : SRC_G;
            st->lev = k == 2 ? 15 : 10;
            if (k == 3)
                st->fx = 3u;
            break;
        default:                                 /* breakcore: a 16x roll falling an octave */
            st->src = k == 0 ? SRC_K : SRC_S;
            st->fx = (uint8_t)(k == 0 ? 0u : 4u + (uint32_t)(k > 1) + (uint32_t)(k > 2) * 2u);
            if (k == 3)
                st->fx = 7u | FX_RAMP_DN;
            break;
        }
    }
}

void gen_vary(chp_step_t *out, const chp_pattern_t *p, const gen_cfg_t *c, const chp_break_t *b,
              uint32_t seed, int amount, int fill)
{
    rng_t r;
    int len = p->len < 1 ? 1 : p->len > CHP_STEPS ? CHP_STEPS : p->len, i;
    int style = c->style < NSTYLES ? c->style : GS_JUNGLE;
    const char *tpl;
    r_seed(&r, seed);
    tpl = TPL[style][r_int(&r, NT)];
    for (i = 0; i < len; i++)
        out[i] = p->st[i];
    for (i = 0; i < len; i++) {
        chp_step_t *st = &out[i];
        int bi = i & 15, backbone = bi == 0 || bi == 4 || bi == 12, op;
        if (!r_pct(&r, backbone ? amount / 4 : amount))
            continue;
        op = r_int(&r, 100);
        if (op < 35) {                           /* what the style would play here */
            chp_step_t g;
            style_step(&g, style == GS_TRACKER ? GS_STRAIGHT : style, tpl, i, b, c->dens, &r);
            if (g.src != SRC_NONE || !backbone)
                *st = g;
        } else if (op < 55) {                    /* the same kind of hit, another one */
            if (SRC_IS_SLICE(st->src) && has_break(b) && st->src - SRC_SLICE < b->nsl)
                st->src = (uint8_t)(SRC_SLICE + same_tag(b, st->src - SRC_SLICE, &r));
            else if (st->src == SRC_H)
                st->src = SRC_G;
            else if (st->src == SRC_G)
                st->src = SRC_H;
            else if (st->src == SRC_NONE)
                st->src = r_pct(&r, 50) ? SRC_G : SRC_H;
        } else if (op < 70) {                    /* a roll */
            if (st->src != SRC_NONE) {
                int n = 1 + r_int(&r, style == GS_BREAKCORE ? 5 : 3);
                st->fx = (uint8_t)((st->fx & ~FX_ROLL) | (uint32_t)n);
                if (r_pct(&r, c->chaos))
                    st->fx = (uint8_t)((st->fx & ~(FX_RAMP_UP | FX_RAMP_DN)) | (r_pct(&r, 70) ? FX_RAMP_UP : FX_RAMP_DN));
            }
        } else if (op < 80) {                    /* backwards */
            if (st->src != SRC_NONE)
                st->fx ^= FX_REV;
        } else if (op < 90) {                    /* a stutter */
            if (i > 0)
                *st = out[i - 1];
        } else if (op < 95) {                    /* a pitch jump */
            static const int8_t J[] = {12, -12, 7, -5};
            if (st->src != SRC_NONE)
                st->pitch = J[r_int(&r, 4)];
        } else if (!backbone) {                  /* dropped: the slice before runs on */
            st->src = SRC_NONE;
        }
    }
    if (fill)
        fill_beat(out, len, style, &r);
}
