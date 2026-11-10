/* SPDX-License-Identifier: GPL-3.0-only */
/* CHOPPAGE app: what is saved (the project, the patterns, the slice tables, the sample store) and
 * the app's entry points. Included by the unity build after gfx.c (drawing helpers in scope) and by
 * the host simulator. */
#pragma once
#include <stdint.h>
#include "plat.h"
#include "../dsp/chp.h"
#include "../dsp/ana.h"
#include "../dsp/gen.h"
#include "../dsp/factory.h"

#ifndef FM_VERSION                       /* a release build passes its own (tools/build.py --release) */
#define FM_VERSION "DEV"
#endif
#define PROJ_MAGIC 0x50484350u           /* "PCHP" */
#define PROJ_FORMAT 1u
#define PATS_MAGIC 0x54415043u           /* "CPAT" */
#define PATS_FORMAT 1u
#define KITS_MAGIC 0x54494B43u           /* "CKIT" */
#define KITS_FORMAT 1u
#define STORE_MAGIC 0x53504843u          /* "CHPS" */
#define STORE_FORMAT 1u

/* the settings: one flash object (OBJ_PROJ) */
typedef struct {
    uint32_t magic, format;
    int16_t par[P_NPARAMS];              /* chp.h P_* */
    uint8_t brk;                         /* the break the patterns play */
    uint8_t pat;                         /* the pattern selected */
    uint8_t leds;                        /* lights: 0 off, 1 keys and the buttons' glow, 2 keys only */
    uint8_t slm, sens, snap;             /* how a new break is cut: ana.h SLM_*, sensitivity, snap */
    uint8_t rsv[2];
    uint32_t seed;                       /* the types' and the generator's dice */
    uint8_t rsv2[16];                    /* room to grow: an older, shorter project loads (zeros here) */
} project_t;

/* the patterns: one flash object (OBJ_PATS), 3144 bytes */
typedef struct {
    uint32_t magic, format;
    chp_pattern_t pat[CHP_NPAT];
} pats_t;

/* a break's slice table, kept apart from its samples (OBJ_KITS): editing it saves 1.5 KB, not 250 */
typedef struct {
    uint32_t uid;                        /* the samples it belongs to (their CRC-32) */
    uint8_t beats, nsl, rsv[2];
    char name[12];
    uint32_t start[CHP_NSL];
    uint8_t tag[CHP_NSL];
} kit_t;
typedef struct {
    uint32_t magic, format;
    kit_t k[CHP_NBRK];
} kits_t;

extern project_t proj;
extern pats_t pats;
extern chp_break_t brk[CHP_NBRK];
extern uint8_t mem[CHP_MEM];

void project_defaults(void);
int project_load(void);                  /* 0 = loaded; else defaults are in place */
int project_save(void);                  /* 0 = saved and verified */
void project_apply(void);                /* every value to the engine (boot, load) */
void pats_defaults(void);                /* the factory patterns */
void pat_factory(int i, chp_pattern_t *p);
int pats_load(void);
int pats_save(void);
int kits_save(void);

/* the sample store */
int store_boot(void);                    /* the breaks from flash (else the factory ones); how many */
uint32_t store_used(void);
int store_count(void);
int store_is_factory(void);              /* nothing loaded yet: the breaks are the synthesized ones */
int store_begin(int slot, uint32_t len, uint32_t rate, int fmt, int beats, const char *name);
int store_data(uint32_t off, const uint8_t *p, uint32_t n);
int store_end(uint32_t crc);             /* 0 = the break is in, cut, tagged and saved */
void store_abort(void);
int store_delete(int slot);
int store_factory(void);                 /* forget every loaded break: the factory ones again */
void brk_reslice(int b);                 /* with the project's cut settings, then tags */
uint32_t chp_crc32(const void *p, uint32_t n);
enum { ST_OK, ST_ERR_ARGS, ST_ERR_SPACE, ST_ERR_STATE, ST_ERR_CRC, ST_ERR_FLASH };

/* SysEx: the break store from a computer (sysex.c) */
void sx_service(void);                   /* main loop: one frame at a time */
int sx_loading(void);                       /* a transfer runs: the screen says so */
int sx_waiting(void);                    /* a frame of ours is waiting: the update entry leaves it */
extern uint32_t sx_gen;                  /* counts changes to the breaks */
uint32_t sx_progress(void);              /* bytes received of the break coming in */
uint32_t sx_total(void);

void ui_init(void);
void ui_frame(void);
void ui_input_only(void);
int ui_dirty(void);
void ui_say(const char *a, const char *b);
