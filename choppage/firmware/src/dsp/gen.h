/* SPDX-License-Identifier: GPL-3.0-only */
/* CHOPPAGE generator: patterns written in a style, and AUTO's variations. Pure functions of their
 * arguments and the seed (the host tests rely on it), cheap enough for the audio interrupt: the
 * render calls gen_vary() once a pattern pass. */
#pragma once
#include "chp.h"

typedef struct {
    uint8_t style;                       /* GS_* */
    uint8_t dens;                        /* 0..100: how many hits besides the backbone */
    uint8_t chaos;                       /* 0..100: rolls, reverses, pitch jumps, stutters */
} gen_cfg_t;

/* a new pattern in the style, over the pattern's length (kept) */
void gen_write(chp_pattern_t *p, const gen_cfg_t *c, const chp_break_t *b, uint32_t seed);

/* one pass of AUTO: out[0..len) = the pattern p with amount % of its steps varied in the style;
 * fill: the last beat becomes a fill */
void gen_vary(chp_step_t *out, const chp_pattern_t *p, const gen_cfg_t *c, const chp_break_t *b,
              uint32_t seed, int amount, int fill);

/* the straight order: the slice playing at step i of the break (SRC_NONE between slices) */
uint8_t gen_straight_src(const chp_break_t *b, int i);

uint32_t gen_hash(uint32_t a, uint32_t b);
