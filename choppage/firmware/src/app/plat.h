/* SPDX-License-Identifier: GPL-3.0-only */
/* CHOPPAGE platform interface (the shape of FoMni's and X0X's): everything the app needs from the
 * hardware.
 *
 * The device implementation (plat_fm1.c) wraps Felucca's HAL and panel map; the host
 * implementation (host/chp_host.c) is a simulator fed by a script, which is what lets the whole
 * app — UI, sequencer, engine, the break store and its SysEx — run and be tested on a computer.
 * Drawing goes through Felucca's lcd_fill / lcd_blit (lcd.c on the device, a framebuffer on the
 * host) and gfx.c on top. */
#pragma once
#include <stdint.h>

/* big zero-initialised buffers go in the .pool RAM region on the device */
#ifdef FM_HOST
#define FM_POOL
#else
#define FM_POOL __attribute__((section(".pool")))
#endif

/* panel controls, by printed label (Felucca's panel.c map turns these into matrix ids; Felucca
 * calls the second button SCL, the panel prints SEL) */
enum { B_FX, B_SEL, B_ENV, B_LFO, B_EDIT, B_GLO, B_HOME, B_SAVE, B_ARP, B_SEQ, B_PLAY, B_REC,
       B_OCTDN, B_OCTUP, NB };
enum { EN_SELECT, EN_ALGO, EN_PRESET, EN_K1, EN_K2, EN_K3, EN_K4, NE };

#define NKEYS 27                       /* F3 .. G5 */

uint32_t plat_ms(void);
uint32_t plat_buttons(void);           /* held: bit B_* */
uint32_t plat_keys(void);              /* held: bit k = key k (0 = F3) */
int32_t plat_enc(int role);            /* detents since the last call, + = clockwise; EN_* */
uint32_t plat_master(void);            /* MASTER pot, 0..4096 gain Q12 */
void plat_leds(uint32_t buttons, uint32_t keys);   /* lit: bit B_*, bit key */
void plat_glow(int on);                /* the unlit buttons glow dimly (call before plat_leds) */
void plat_play_red(int red);           /* PLAY lights green; red (its other LED) while recording */

/* USB MIDI, 4-byte USB-MIDI event packets (cable 0) */
int plat_midi_in(uint32_t *pkt);       /* 1 = got one (called from the audio ISR) */
void plat_midi_out(uint32_t pkt);      /* queue one (audio ISR or main loop) */

/* SysEx frames for the break store (sysex.c): one received message at a time (the 7-bit bytes
 * between F0 and F7), and a whole F0..F7 message out */
int plat_sx_get(const uint8_t **p, uint32_t *n);   /* 1 = a frame is waiting (not taken) */
void plat_sx_done(void);               /* release it */
int plat_sx_send(const uint8_t *p, uint32_t n);    /* 0 = queued */

/* persistent storage (Felucca storage.c objects: A/B sectors, CRC, torn-write safe) */
int plat_store_load(uint32_t obj, void *dst, uint32_t max);   /* bytes loaded, < 0 = none / bad */
int plat_store_save(uint32_t obj, const void *src, uint32_t len);   /* 0 = ok */
#define PLAT_STORE_MAX 3840u           /* bytes per object */
enum { OBJ_PROJ, OBJ_PANEL, OBJ_PATS, OBJ_KITS, OBJ_NOBJ };
/* flash: OBJ_PROJ 0xFC000, OBJ_PANEL 0xDC000 (Felucca's place: a calibration made under another
 * Felucca firmware still holds), OBJ_PATS 0x97000, OBJ_KITS 0x99000 (2 sectors each) */

/* the sample store: raw flash, 0x9C000 .. 0xDBFFF (64 sectors): a header sector, then the samples.
 * Offsets are from its start; erase works on 4 KiB sectors. Writes silence the audio for a moment
 * (the flash is off while it erases or programs). */
#define PLAT_SAMPLES_LEN (256u * 1024u)
int plat_samples_read(uint32_t off, void *dst, uint32_t n);         /* 0 = ok */
int plat_samples_erase(uint32_t off);                               /* one sector; 0 = ok */
int plat_samples_write(uint32_t off, const void *src, uint32_t n);  /* in 256-byte pages; 0 = ok */
void plat_idle(void);                  /* during a long flash job: watchdog, USB */

/* audio load in % of the render budget, and xruns since boot (device: from the ISR) */
uint32_t plat_cpu_pct(void);
uint32_t plat_xruns(void);
