/* SPDX-License-Identifier: GPL-3.0-only */
/* FM-303 app: the project (everything that is saved) and the app's entry points. Included by the
 * unity build after gfx.c (drawing helpers in scope) and by the host simulator. */
#pragma once
#include <stdint.h>
#include "plat.h"
#include "../dsp/fm303.h"

#ifndef FM_VERSION                       /* a release build passes its own (tools/build.py --release) */
#define FM_VERSION "DEV"
#endif
#define PROJ_MAGIC 0x33303346u           /* "F303" */
#define PROJ_FORMAT 1u
#define BANK_MAGIC 0x53544150u           /* "PATS" */
#define BANK_FORMAT 1u

/* the settings: one flash object (OBJ_PROJ) */
typedef struct {
    uint32_t magic, format;
    int16_t par[P_NPARAMS];              /* fm303.h P_* */
    uint8_t edit;                        /* the pattern selected: the one PLAY starts and WRITE edits */
    uint8_t group;                       /* the group the white keys pick from (0..3: I..IV) */
    uint8_t oct;                         /* the keyboard's octave: 0..3; 1: the C4 key plays C2 (the 303's C) */
    uint8_t leds;                        /* lights: 0 off, 1 keys and the buttons' glow, 2 keys only */
    uint8_t rsv[16];                     /* room to grow: an older, shorter project loads (zeros here) */
} project_t;

/* the patterns: one flash object (OBJ_PATS), 2568 bytes */
typedef struct {
    uint32_t magic, format;
    tb_pattern_t pat[TB_NPAT];
} bank_t;

extern project_t proj;
extern bank_t bank;

void project_defaults(void);
int project_load(void);                  /* 0 = loaded; else defaults are in place */
int project_save(void);                  /* 0 = saved and verified */
void project_apply(void);                /* every value to the engine (boot, load) */
void bank_defaults(void);                /* the factory patterns in group I, the rest cleared */
int bank_load(void);
int bank_save(void);

void ui_init(void);
void ui_frame(void);
void ui_input_only(void);
int ui_dirty(void);
void ui_say(const char *a, const char *b);
