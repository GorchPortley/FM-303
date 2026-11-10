/* SPDX-License-Identifier: GPL-3.0-only */
/* CHOPPAGE factory breaks: three drum breaks synthesized at boot (no sampled audio ships with the
 * firmware), cut and tagged exactly, so there is something to chop before a break is loaded. */
#pragma once
#include "chp.h"

int chp_factory_count(void);
/* break i into dst (at most max bytes): fills b (len, rate, fmt, beats, name, slices, tags; not off
 * or uid); returns the length, 0 if it does not fit */
uint32_t chp_factory(int i, uint8_t *dst, uint32_t max, chp_break_t *b);
