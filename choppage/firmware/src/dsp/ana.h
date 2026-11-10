/* SPDX-License-Identifier: GPL-3.0-only */
/* CHOPPAGE break analysis (main loop only): cutting a break into slices, and telling kicks from
 * snares from hats. */
#pragma once
#include "chp.h"

enum { SLM_AUTO, SLM_8, SLM_16, SLM_32, SLM_HITS, NSLM };   /* grids snap to the hits near them */
extern const char *const ANA_MODE_NAME[NSLM];

typedef struct {
    uint8_t mode;                        /* SLM_* */
    uint8_t sens;                        /* 0..100: SLM_HITS, how small a hit counts */
    uint8_t snap;                        /* grids: move each cut to a hit near it */
} ana_cfg_t;

/* the slices of b (b->len, rate, fmt, beats set; d: its samples) */
void ana_slice(chp_break_t *b, const uint8_t *d, const ana_cfg_t *c);
/* a tag for every slice, from its sound: low and loud = KICK, broad and loud = SNARE, a quiet snare
 * = GHOST, high and thin = HAT */
void ana_tag(chp_break_t *b, const uint8_t *d);
/* the beats a loop of len samples most likely holds (its tempo near 88..177 BPM) */
uint8_t ana_guess_beats(uint32_t len, uint32_t rate);
/* the hits (onsets) found, at most max, in samples; returns how many */
int ana_onsets(const chp_break_t *b, const uint8_t *d, int sens, uint32_t *out, int max);
