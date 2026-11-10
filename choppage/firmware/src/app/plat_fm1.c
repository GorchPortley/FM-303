/* SPDX-License-Identifier: GPL-3.0-only */
/* CHOPPAGE platform on the FM-1: plat.h over Felucca's HAL, panel map, USB rings and storage.
 * Part of the unity build (main.c), after the HAL, usb.c, storage.c and panel.c. (From FoMni's.) */

uint32_t plat_ms(void) { return fm1_ms; }

uint32_t plat_buttons(void)
{
    uint32_t raw = fm1_in.buttons, out = 0, i;
    for (i = 0; i < NB; i++)
        if (raw & (1u << panel.btn[i]))
            out |= 1u << i;
    return out;
}

uint32_t plat_keys(void) { return fm1_in.notes & ((1u << NKEYS) - 1u); }

int32_t plat_enc(int role)
{
    if (role < 0 || role >= NE)
        return 0;
    return fm1_enc_take(panel.enc[role]) * panel.dir[role];
}

static volatile uint32_t master_q12 = 2048;
uint32_t plat_master(void) { return master_q12; }

/* LEDs: the picture is built off-line and copied one byte per column (Felucca: clearing
 * and relighting would let the 10 kHz scan catch the dark gap and flicker) */
static uint8_t led_pos[FM1_NKEY];
static void led_pos_init(void)
{
    uint32_t id, p, r;
    for (id = 0; id < FM1_NKEY; id++) {
        led_pos[id] = 0xFF;
        for (p = 0; p < FM1_NCOL; p++)
            for (r = 1; r < 5u; r++)
                if (FM1_KEYMAP[r][p] == (int8_t)id)
                    led_pos[id] = (uint8_t)((p << 3) | r);
    }
}
/* Glow: every button not lit is lit dim, on one scan frame in GLOW_DIV (the HAL's fm1_led_dim) */
#define GLOW_DIV 4u
static uint8_t glow_on = 1;
void plat_glow(int on) { glow_on = (uint8_t)(on != 0); }
/* PLAY has two LEDs: its red at the button's own position, a green at the keyless matrix position
 * column 8, row PA5 (found on the device). Green, unless recording. */
static uint8_t play_red;
void plat_play_red(int red) { play_red = (uint8_t)(red != 0); }
void plat_leds(uint32_t buttons, uint32_t keys)
{
    uint8_t nl[FM1_NCOL] = {0}, dl[FM1_NCOL] = {0};
    uint32_t i, c;
    for (i = 0; i < NB + NKEYS; i++) {
        uint32_t id = i < NB ? panel.btn[i] : 14u + (i - NB);
        int on = i < NB ? (buttons >> i) & 1u : (keys >> (i - NB)) & 1u;
        uint8_t q = led_pos[id];
        if (i == B_PLAY && !play_red)            /* PLAY's green LED instead (keyless position col 8, row PA5) */
            q = (uint8_t)((8u << 3) | 1u);
        if (q == 0xFF)
            continue;
        if (on)
            nl[q >> 3] |= (uint8_t)(1u << (q & 7u));
        else if (i < NB && glow_on)
            dl[q >> 3] |= (uint8_t)(1u << (q & 7u));
    }
    for (c = 0; c < FM1_NCOL; c++) {
        fm1_led[c] = nl[c];
        fm1_led_dim[c] = dl[c];
    }
    fm1_led_dim_div = glow_on ? GLOW_DIV : 0u;
}

/* MIDI: Felucca's rings carry 4-byte USB-MIDI event packets, byte 0 = cable / CIN */
int plat_midi_in(uint32_t *pkt)
{
    if (mi_r == mi_w)
        return 0;
    *pkt = midi_in_q[mi_r % MQ];
    mi_r++;
    return 1;
}
void plat_midi_out(uint32_t pkt) { midi_out_event(pkt); }
/* the engine's MIDI (from the render): out as a USB-MIDI packet, cable 0, CIN = the status's high
 * nibble (0xF for real-time); in from the ring USB and the TRS jack fill */
void chp_midi_out(uint32_t st, uint32_t d1, uint32_t d2)
{
    plat_midi_out((st >= 0xF0u ? 0x0Fu : st >> 4) | st << 8 | d1 << 16 | d2 << 24);
}
int chp_midi_in(uint32_t *pkt) { return plat_midi_in(pkt); }

/* SysEx frames: the update entry's (usb.c); sysex.c takes the ones that are its own */
int plat_sx_get(const uint8_t **p, uint32_t *n) { return ota_frame_get(p, n); }
void plat_sx_done(void) { ota_frame_done(); }
int plat_sx_send(const uint8_t *p, uint32_t n) { return ota_wire_send(p, n); }

/* the sample store: raw sectors 0x9C000 .. 0xDBFFF (inside Felucca's data area) */
#define SAMPLES_BASE 0x9C000u
int plat_samples_read(uint32_t off, void *dst, uint32_t n)
{
    if (!flash_ok || off > PLAT_SAMPLES_LEN || n > PLAT_SAMPLES_LEN - off)
        return -8;
    return st_read(SAMPLES_BASE + off, dst, n);
}
int plat_samples_erase(uint32_t off)
{
    if (!flash_ok || (off & 0xFFFu) || off >= PLAT_SAMPLES_LEN)
        return -8;
    fm1_wdt_feed();
    return st_erase(SAMPLES_BASE + off);
}
int plat_samples_write(uint32_t off, const void *src, uint32_t n)
{
    int rc;
    if (!flash_ok || off > PLAT_SAMPLES_LEN || n > PLAT_SAMPLES_LEN - off)
        return -8;
    while (n) {                                  /* pages: a program never crosses 256 bytes */
        uint32_t k = 256u - (off & 255u);
        if (k > n)
            k = n;
        if ((rc = st_prog(SAMPLES_BASE + off, src, k)) != 0)
            return rc;
        off += k;
        src = (const uint8_t *)src + k;
        n -= k;
    }
    return 0;
}
void plat_idle(void)
{
    fm1_wdt_feed();
    usb_retry(fm1_ms);
}

int plat_store_load(uint32_t obj, void *dst, uint32_t max) { return flash_ok ? st_load(obj, dst, max) : -1; }
int plat_store_save(uint32_t obj, const void *src, uint32_t len) { return flash_ok ? st_save(obj, src, len) : -9; }

static volatile uint32_t audio_cpu_pct, audio_xruns;
uint32_t plat_cpu_pct(void) { return audio_cpu_pct; }
uint32_t plat_xruns(void) { return audio_xruns; }
