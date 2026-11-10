/* SPDX-License-Identifier: GPL-3.0-only */
/* CHOPPAGE on the host: the whole app (UI, project, sample store, SysEx, engine) against a simulated
 * FM-1 driven by a script. Audio goes to a WAV, the screen to PNGs, the lights to text, MIDI out and
 * SysEx replies to logs. The device-only parts (HAL, USB, OTA, flash driver) are replaced by plat.h
 * implemented here; the flash (objects and the sample store) survives a simulated reboot. (The
 * skeleton is FoMni's omni_host, itself from X0X's x0x_host.)
 *
 *   chp_host SCRIPT [OUTDIR]
 *
 * Script, one command per line (# comments):
 *   wait MS                      run the device for MS milliseconds
 *   press BTN | release BTN | tap BTN     (BTN: FX SEL ENV LFO EDIT GLO HOME SAVE ARP SEQ PLAY REC OCT- OCT+)
 *   key K down|up | tapkey K     (K: 0..26 = F3..G5, or w0..w15 white keys, b0..b10 black keys)
 *   hold K MS                    key K down for MS milliseconds, then up
 *   keys K1 K2 ...               tap each key in turn (as tapkey)
 *   turn ENC N [MS]              (ENC: SELECT ALGO PRESET K1 K2 K3 K4; N detents, MS apart, default 80)
 *   spin ENC N                   N detents at once, exactly N (no acceleration)
 *   master N                     MASTER pot 0..4096
 *   midi B0 B1 B2                incoming USB MIDI message (hex bytes)
 *   clockin N MS                 N MIDI clock ticks (F8) in, MS apart
 *   sysex FILE                   the SysEx messages in FILE (.syx) to the device, one by one, each
 *                                as soon as the one before is taken (replies: OUTDIR/sysex_out.txt)
 *   loadwav SLOT FILE [BEATS]    a WAV straight into the break store (as the loaders convert it:
 *                                mono, 22050 Hz, 8-bit), cut and tagged
 *   wav FILE | wavstop           start / stop recording the output
 *   peakreset                    restart the output peak (expect peak_db_min / peak_db_max)
 *   shot FILE.png                save the screen
 *   leds                         print the lit buttons and keys
 *   expect WHAT VALUE            check state: exit 1 on mismatch; VALUE "<N" / ">N" is a bound
 *   reboot                       re-run boot from the simulated flash (persistence test)
 *   wipe                         erase the simulated flash (objects and samples)
 */
#define FM_HOST 1
#define _POSIX_C_SOURCE 199309L         /* clock_gettime under -std=c99 on Linux */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#define __attribute__(x)

/* ------------------------------------------------------------ screen --- */
static uint16_t fb[240 * 240];          /* RGB565, byte-swapped as the panel takes it (gfx.c) */
static void lcd_sync(void) {}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    uint16_t sc = (uint16_t)(((c >> 8) & 0xFFu) | ((c & 0xFFu) << 8));
    for (j = y; j < y + h && j < 240u; j++)
        for (i = x; i < x + w && i < 240u; i++)
            fb[j * 240u + i] = sc;
}
static uint32_t blits;
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint32_t i, j;
    blits++;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            if (x + i < 240u && y + j < 240u)
                fb[(y + j) * 240u + x + i] = px[j * w + i];
}
#include "../firmware/src/gfx.c"

/* ---------------------------------------------------------- platform --- */
#include "../firmware/src/app/plat.h"

static uint32_t now_ms, held_btn, held_keys, master = 2048;
static int32_t enc_acc[NE];
static uint32_t lit_btn, lit_keys;

uint32_t plat_ms(void) { return now_ms; }
uint32_t plat_buttons(void) { return held_btn; }
uint32_t plat_keys(void) { return held_keys; }
int32_t plat_enc(int role)
{
    int32_t v = enc_acc[role];
    enc_acc[role] = 0;
    return v;
}
uint32_t plat_master(void) { return master; }
void plat_leds(uint32_t b, uint32_t k)
{
    lit_btn = b;
    lit_keys = k;
}
static int glow_on = 1, play_red;
void plat_glow(int on) { glow_on = on; }
void plat_play_red(int red) { play_red = red; }

#define MQ 256
static uint32_t min_q[MQ], mi_w, mi_r;
static FILE *midi_log;
static uint32_t midi_out_count, clock_out_count, notes_out_count;
int plat_midi_in(uint32_t *pkt)
{
    if (mi_r == mi_w)
        return 0;
    *pkt = min_q[mi_r++ % MQ];
    return 1;
}
void plat_midi_out(uint32_t pkt)
{
    midi_out_count++;
    if (((pkt >> 8) & 0xFF) == 0xF8) {
        clock_out_count++;
        return;
    }
    if (((pkt >> 8) & 0xF0) == 0x90)
        notes_out_count++;
    if (midi_log)
        fprintf(midi_log, "%u ms: %02X %02X %02X\n", now_ms, (pkt >> 8) & 0xFF, (pkt >> 16) & 0xFF, (pkt >> 24) & 0xFF);
}
void chp_midi_out(uint32_t st, uint32_t d1, uint32_t d2)
{
    plat_midi_out((st >= 0xF0u ? 0x0Fu : st >> 4) | st << 8 | d1 << 16 | d2 << 24);
}
int chp_midi_in(uint32_t *pkt) { return plat_midi_in(pkt); }

/* SysEx: frames from a .syx file, one at a time; replies to a log */
#define SXN 4096
static uint8_t *sx_q[SXN];
static uint32_t sx_qn[SXN], sx_qr, sx_qw;
static FILE *sx_log;
static uint32_t sx_replies, sx_acks_ok, sx_acks_err;
int plat_sx_get(const uint8_t **p, uint32_t *n)
{
    if (sx_qr == sx_qw)
        return 0;
    *p = sx_q[sx_qr % SXN];
    *n = sx_qn[sx_qr % SXN];
    return 1;
}
void plat_sx_done(void)
{
    if (sx_qr != sx_qw) {
        free(sx_q[sx_qr % SXN]);
        sx_qr++;
    }
}
int plat_sx_send(const uint8_t *p, uint32_t n)
{
    uint32_t i;
    sx_replies++;
    if (n > 8 && p[5] == 0x7F) {                    /* an answer: unpack cmd and status */
        uint8_t d[4] = {0};
        uint32_t acc = 0, nb = 0, o = 0;
        for (i = 7; i + 1 < n && o < 4; i++) {
            acc |= (uint32_t)(p[i] & 0x7F) << nb;
            nb += 7;
            if (nb >= 8) {
                d[o++] = (uint8_t)acc;
                acc >>= 8;
                nb -= 8;
            }
        }
        if (d[1] == 0)
            sx_acks_ok++;
        else
            sx_acks_err++;
        if (sx_log)
            fprintf(sx_log, "%u ms: ack cmd %u status %u\n", now_ms, d[0], d[1]);
    } else if (sx_log) {
        fprintf(sx_log, "%u ms: reply cmd %02X, %u bytes:", now_ms, n > 5 ? p[5] : 0, n);
        for (i = 0; i < n && i < 48; i++)
            fprintf(sx_log, " %02X", p[i]);
        fprintf(sx_log, "\n");
    }
    return 0;
}

/* flash: one buffer per object, and the sample store, kept across a simulated reboot */
static uint8_t store[OBJ_NOBJ][PLAT_STORE_MAX];
static int store_len[OBJ_NOBJ] = {-1, -1, -1, -1};
static uint32_t store_writes, sample_erases;
static uint8_t sflash[PLAT_SAMPLES_LEN];
int plat_store_load(uint32_t obj, void *dst, uint32_t max)
{
    int n;
    if (obj >= OBJ_NOBJ || (n = store_len[obj]) < 0)
        return -1;
    if ((uint32_t)n > max)
        n = (int)max;
    memcpy(dst, store[obj], (size_t)n);
    return n;
}
int plat_store_save(uint32_t obj, const void *src, uint32_t len)
{
    if (obj >= OBJ_NOBJ || len > PLAT_STORE_MAX)
        return -1;
    memcpy(store[obj], src, len);
    store_len[obj] = (int)len;
    store_writes++;
    return 0;
}
int plat_samples_read(uint32_t off, void *dst, uint32_t n)
{
    if (off > PLAT_SAMPLES_LEN || n > PLAT_SAMPLES_LEN - off)
        return -8;
    memcpy(dst, sflash + off, n);
    return 0;
}
int plat_samples_erase(uint32_t off)
{
    if ((off & 0xFFFu) || off >= PLAT_SAMPLES_LEN)
        return -8;
    memset(sflash + off, 0xFF, 4096);
    sample_erases++;
    return 0;
}
int plat_samples_write(uint32_t off, const void *src, uint32_t n)
{
    uint32_t i;
    if (off > PLAT_SAMPLES_LEN || n > PLAT_SAMPLES_LEN - off)
        return -8;
    for (i = 0; i < n; i++)
        sflash[off + i] &= ((const uint8_t *)src)[i];   /* NOR: programming only clears bits */
    return 0;
}
void plat_idle(void) {}

static uint32_t cpu_pct;
uint32_t plat_cpu_pct(void) { return cpu_pct; }
uint32_t plat_xruns(void) { return 0; }

/* -------------------------------------------------------------- app --- */
#include "../firmware/src/app/app.h"
#include "../firmware/src/app/project.c"
#include "../firmware/src/app/sysex.c"
#include "../firmware/src/app/ui.c"

/* ---------------------------------------------------------- outputs --- */
static void png_write(const char *path)
{
    /* RGB PNG with stored (uncompressed) deflate blocks: no zlib needed */
    static uint8_t raw[240 * (1 + 240 * 3)];
    FILE *f = fopen(path, "wb");
    uint32_t crc_t[256], i, j, k, n = 0, a = 1, b = 0, pos, len = sizeof raw;
    uint8_t hdr[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        return;
    }
    for (i = 0; i < 256; i++) {
        uint32_t c = i;
        for (k = 0; k < 8; k++)
            c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_t[i] = c;
    }
    for (j = 0; j < 240; j++) {
        raw[n++] = 0;
        for (i = 0; i < 240; i++) {
            uint16_t p = fb[j * 240 + i];
            p = (uint16_t)((p >> 8) | (p << 8));
            raw[n++] = (uint8_t)(((p >> 11) & 31) * 255 / 31);
            raw[n++] = (uint8_t)(((p >> 5) & 63) * 255 / 63);
            raw[n++] = (uint8_t)((p & 31) * 255 / 31);
        }
    }
    for (i = 0; i < len; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
#define PUT32(v) do { uint8_t q_[4] = {(uint8_t)((v) >> 24), (uint8_t)((v) >> 16), (uint8_t)((v) >> 8), (uint8_t)(v)}; fwrite(q_, 1, 4, f); } while (0)
    fwrite(hdr, 1, 8, f);
    {
        uint8_t ih[17] = {'I', 'H', 'D', 'R', 0, 0, 0, 240, 0, 0, 0, 240, 8, 2, 0, 0, 0};
        uint32_t c = 0xFFFFFFFFu;
        PUT32(13u);
        fwrite(ih, 1, 17, f);
        for (i = 0; i < 17; i++)
            c = crc_t[(c ^ ih[i]) & 255] ^ (c >> 8);
        PUT32(~c);
    }
    {
        uint32_t nblk = (len + 65534) / 65535, zlen = 2 + len + nblk * 5 + 4, c = 0xFFFFFFFFu;
        uint8_t *z = malloc(zlen + 4), *q = z;
        memcpy(q, "IDAT", 4);
        q += 4;
        *q++ = 0x78;
        *q++ = 0x01;
        for (pos = 0; pos < len;) {
            uint32_t m = len - pos > 65535 ? 65535 : len - pos;
            *q++ = pos + m >= len ? 1 : 0;
            *q++ = (uint8_t)m;
            *q++ = (uint8_t)(m >> 8);
            *q++ = (uint8_t)~m;
            *q++ = (uint8_t)(~m >> 8);
            memcpy(q, raw + pos, m);
            q += m;
            pos += m;
        }
        *q++ = (uint8_t)(b >> 8);
        *q++ = (uint8_t)b;
        *q++ = (uint8_t)(a >> 8);
        *q++ = (uint8_t)a;
        PUT32(zlen);
        fwrite(z, 1, zlen + 4, f);
        for (i = 0; i < zlen + 4; i++)
            c = crc_t[(c ^ z[i]) & 255] ^ (c >> 8);
        PUT32(~c);
        free(z);
    }
    {
        uint8_t ie[4] = {'I', 'E', 'N', 'D'};
        uint32_t c = 0xFFFFFFFFu;
        PUT32(0u);
        fwrite(ie, 1, 4, f);
        for (i = 0; i < 4; i++)
            c = crc_t[(c ^ ie[i]) & 255] ^ (c >> 8);
        PUT32(~c);
    }
    fclose(f);
}

static FILE *wav;
static uint32_t wav_frames;
static float peak_out;
static void wav_open(const char *path)
{
    static const uint8_t h[44] = {0};
    wav = fopen(path, "wb");
    if (wav)
        fwrite(h, 1, 44, wav);
    wav_frames = 0;
}
static void wav_close(void)
{
    uint32_t bytes = wav_frames * 4, v;
    if (!wav)
        return;
    fseek(wav, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, wav);
    v = 36 + bytes;
    fwrite(&v, 4, 1, wav);
    fwrite("WAVEfmt ", 1, 8, wav);
    v = 16;
    fwrite(&v, 4, 1, wav);
    {
        uint16_t fmt[2] = {1, 2};
        uint32_t r[2] = {44100, 44100 * 4};
        uint16_t al[2] = {4, 16};
        fwrite(fmt, 2, 2, wav);
        fwrite(r, 4, 2, wav);
        fwrite(al, 2, 2, wav);
    }
    fwrite("data", 1, 4, wav);
    fwrite(&bytes, 4, 1, wav);
    fclose(wav);
    wav = 0;
}

/* --------------------------------------------------------- the device --- */
static double audio_due_ms, render_ns_total, render_budget_ns_total;
static uint32_t ui_due;
static double now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

static void boot(void)
{
    project_load();
    pats_load();
    store_boot();
    chp_init(pats.pat, brk, mem);
    project_apply();
    lcd_fill(0, 0, 240, 240, 0);
    ui_init();
}

static void run_ms(uint32_t ms)
{
    static int32_t blk[512];
    uint32_t i, k;
    for (i = 0; i < ms; i++) {
        now_ms++;
        while (audio_due_ms <= (double)now_ms) {      /* 256-frame blocks, as the device renders them */
            double t0 = now_ns(), dt;
            chp_render(blk, 256, master);
#ifdef FM_WEB
            web_audio(blk, 256);
#endif
            dt = now_ns() - t0;
            render_ns_total += dt;
            render_budget_ns_total += 256.0 * 1e9 / 44100.0;
            cpu_pct = (uint32_t)(100.0 * dt / (256.0 * 1e9 / 44100.0));
            for (k = 0; k < 256; k++) {
                float l = (float)blk[2 * k] / 8388608.0f, r = (float)blk[2 * k + 1] / 8388608.0f;
                int16_t s[2];
                if (fabsf(l) > peak_out)
                    peak_out = fabsf(l);
                if (fabsf(r) > peak_out)
                    peak_out = fabsf(r);
                s[0] = (int16_t)(l * 32767.0f);
                s[1] = (int16_t)(r * 32767.0f);
                if (wav) {
                    fwrite(s, 2, 2, wav);
                    wav_frames++;
                }
            }
            audio_due_ms += 256.0 * 1000.0 / 44100.0;
        }
        sx_service();                                  /* the main loop's order on the device */
        if (sx_qr != sx_qw && !sx_waiting())
            plat_sx_done();                            /* not ours: the update entry would drop it */
        if (now_ms >= ui_due) {
            ui_due = now_ms + 16;
            ui_frame();
        } else {
            ui_input_only();
        }
    }
}

/* a WAV (8/16/24-bit PCM or 32-bit float, any channels and rate) into slot, as the loaders do it:
 * mono, resampled to 22050 Hz (linear, after a lowpass), peak-normalized, 8-bit */
static int load_wav(int slot, const char *path, int beats)
{
    FILE *f = fopen(path, "rb");
    uint8_t h[12], *raw = 0;
    uint32_t rate = 0, ch = 1, bits = 16, fmt = 1, len = 0, n, i, out_n;
    float *x, *y, peak = 0.0f;
    int rc;
    if (!f || fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
        printf("loadwav: %s is not a WAV\n", path);
        if (f)
            fclose(f);
        return -1;
    }
    for (;;) {                                         /* the chunks: fmt and data */
        uint8_t c[8];
        uint32_t sz;
        if (fread(c, 1, 8, f) != 8)
            break;
        sz = c[4] | c[5] << 8 | c[6] << 16 | (uint32_t)c[7] << 24;
        if (!memcmp(c, "fmt ", 4)) {
            uint8_t fm[40] = {0};
            if (fread(fm, 1, sz < 40 ? sz : 40, f) == 0)
                break;
            if (sz > 40)
                fseek(f, (long)(sz - 40), SEEK_CUR);
            fmt = fm[0] | fm[1] << 8;
            ch = fm[2] | fm[3] << 8;
            rate = fm[4] | fm[5] << 8 | fm[6] << 16 | (uint32_t)fm[7] << 24;
            bits = fm[14] | fm[15] << 8;
            if (fmt == 0xFFFE && sz >= 26)
                fmt = fm[24] | fm[25] << 8;
        } else if (!memcmp(c, "data", 4)) {
            raw = malloc(sz);
            len = (uint32_t)fread(raw, 1, sz, f);
            break;
        } else {
            fseek(f, (long)(sz + (sz & 1)), SEEK_CUR);
        }
    }
    fclose(f);
    if (!raw || !rate || !ch || (bits != 8 && bits != 16 && bits != 24 && bits != 32)) {
        printf("loadwav: %s: unsupported format\n", path);
        free(raw);
        return -1;
    }
    n = len / (ch * bits / 8);
    x = calloc(n + 1, sizeof *x);
    for (i = 0; i < n; i++) {
        uint32_t c;
        float s = 0.0f;
        for (c = 0; c < ch; c++) {
            const uint8_t *p = raw + (i * ch + c) * (bits / 8);
            float v;
            if (bits == 8)
                v = ((float)p[0] - 128.0f) / 128.0f;
            else if (bits == 16)
                v = (float)(int16_t)(p[0] | p[1] << 8) / 32768.0f;
            else if (bits == 24)
                v = (float)((int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 8) / 8388608.0f;
            else if (fmt == 3) {
                float fv;
                memcpy(&fv, p, 4);
                v = fv;
            } else
                v = (float)(int32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24) / 2147483648.0f;
            s += v;
        }
        x[i] = s / (float)ch;
    }
    free(raw);
    if (rate > 22050) {                                /* a lowpass before going down: two one-poles */
        float a = 1.0f - expf(-2.0f * 3.14159265f * 9500.0f / (float)rate), s1 = 0, s2 = 0;
        for (i = 0; i < n; i++) {
            s1 += a * (x[i] - s1);
            s2 += a * (s1 - s2);
            x[i] = s2;
        }
    }
    out_n = (uint32_t)((double)n * 22050.0 / rate);
    y = calloc(out_n + 1, sizeof *y);
    for (i = 0; i < out_n; i++) {
        double t = (double)i * rate / 22050.0;
        uint32_t k = (uint32_t)t;
        float fr = (float)(t - k);
        y[i] = x[k] + fr * (x[k + 1 < n ? k + 1 : k] - x[k]);
        if (fabsf(y[i]) > peak)
            peak = fabsf(y[i]);
    }
    free(x);
    raw = malloc(out_n);
    for (i = 0; i < out_n; i++) {
        float v = y[i] / (peak > 1e-6f ? peak : 1.0f) * 127.0f;
        int q = (int)lrintf(v);
        raw[i] = (uint8_t)(int8_t)(q < -127 ? -127 : q > 127 ? 127 : q);
    }
    free(y);
    {
        const char *b = strrchr(path, '/');
        char name[13] = {0};
        b = b ? b + 1 : path;
        for (i = 0; i < 11 && b[i] && b[i] != '.'; i++)
            name[i] = (char)(b[i] >= 'a' && b[i] <= 'z' ? b[i] - 32 : b[i]);
        chp_hold(1);
        run_ms(12);
        rc = store_begin(slot, out_n, 22050, FMT_PCM8, beats, name);
        if (rc == ST_OK)
            rc = store_data(0, raw, out_n);
        if (rc == ST_OK)
            rc = store_end(chp_crc32(raw, out_n));
        chp_hold(0);
        sx_gen++;
        project_apply();
    }
    free(raw);
    printf("  loadwav %s -> slot %d: %u samples, %s\n", path, slot + 1, out_n, rc == ST_OK ? "ok" : "FAILED");
    return rc;
}

static int sysex_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    long sz;
    uint8_t *d;
    uint32_t i, start = 0, frames = 0, guard;
    if (!f) {
        printf("sysex: cannot read %s\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    d = malloc((size_t)sz);
    if (fread(d, 1, (size_t)sz, f) != (size_t)sz)
        sz = 0;
    fclose(f);
    for (i = 0; i < (uint32_t)sz; i++) {
        if (d[i] == 0xF0)
            start = i + 1;
        else if (d[i] == 0xF7 && start) {             /* one frame, delivered when the one before is gone */
            uint32_t n = i - start;
            for (guard = 0; sx_qr != sx_qw && guard < 30000; guard++)
                run_ms(1);
            sx_q[sx_qw % SXN] = malloc(n ? n : 1);
            memcpy(sx_q[sx_qw % SXN], d + start, n);
            sx_qn[sx_qw % SXN] = n;
            sx_qw++;
            frames++;
            start = 0;
        }
    }
    for (guard = 0; sx_qr != sx_qw && guard < 30000; guard++)
        run_ms(1);
    run_ms(2);
    free(d);
    printf("  sysex %s: %u messages\n", path, frames);
    return 0;
}

/* ---------------------------------------------------------- script --- */
static const char *const BTN_N[NB] = {"FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ",
                                      "PLAY", "REC", "OCT-", "OCT+"};
static const char *const ENC_N[NE] = {"SELECT", "ALGO", "PRESET", "K1", "K2", "K3", "K4"};

static int find_name(const char *const *t, int n, const char *s)
{
    int i;
    for (i = 0; i < n; i++)
        if (!strcmp(t[i], s))
            return i;
    return -1;
}

static int key_of(const char *s)
{
    if (s[0] == 'w')
        return WHITE_K[atoi(s + 1) & 15];
    if (s[0] == 'b')
        return BLACK_K[atoi(s + 1) % 11];
    return atoi(s);
}

static int expect(const char *what, const char *val)
{
    int got;
    const chp_pattern_t *ep = &pats.pat[proj.pat];
    const chp_break_t *eb = &brk[proj.brk];
    if (!strcmp(what, "playing"))
        got = chp_playing;
    else if (!strcmp(what, "view"))
        got = ui.view;
    else if (!strcmp(what, "page"))
        got = ui.page[ui.view];
    else if (!strcmp(what, "pat"))                    /* the pattern selected, 0..15 */
        got = proj.pat;
    else if (!strcmp(what, "brk"))
        got = proj.brk;
    else if (!strcmp(what, "cur"))                    /* the pattern playing */
        got = chp_cur;
    else if (!strcmp(what, "cue"))
        got = chp_cue;
    else if (!strcmp(what, "step"))                   /* the step heard */
        got = chp_step;
    else if (!strcmp(what, "mstep"))                  /* the step underneath */
        got = chp_mstep;
    else if (!strcmp(what, "bars"))
        got = (int)chp_bars;
    else if (!strcmp(what, "ext"))
        got = chp_ext;
    else if (!strcmp(what, "rec"))
        got = chp_recording;
    else if (!strcmp(what, "perf"))                   /* perform fx in effect, bit mask */
        got = chp_perf_on & 0x7FF;
    else if (!strcmp(what, "repeat"))
        got = (chp_perf_on & 0x8000) != 0;
    else if (!strcmp(what, "vslice"))                 /* the slice heard */
        got = chp_vslice;
    else if (!strcmp(what, "vbrk"))
        got = chp_vbrk;
    else if (!strcmp(what, "trig"))
        got = chp_trig;
    else if (!strcmp(what, "work"))                   /* AUTO is varying this bar */
        got = chp_work_on;
    else if (!strcmp(what, "nbreaks"))
        got = store_count();
    else if (!strcmp(what, "used"))
        got = (int)store_used();
    else if (!strcmp(what, "factory"))
        got = store_is_factory();
    else if (!strcmp(what, "nsl"))                    /* the break's slices */
        got = eb->nsl;
    else if (!strcmp(what, "beats"))
        got = eb->beats;
    else if (!strcmp(what, "rate"))
        got = eb->rate;
    else if (!strcmp(what, "len"))                    /* the pattern's length */
        got = ep->len;
    else if (!strcmp(what, "cursor"))
        got = ui.cur;
    else if (!strcmp(what, "scur"))
        got = ui.scur;
    else if (!strcmp(what, "kpage"))
        got = ui.kpage;
    else if (!strcmp(what, "dirty"))
        got = ui_dirty();
    else if (!strcmp(what, "store_writes"))
        got = (int)store_writes;
    else if (!strcmp(what, "sample_erases"))
        got = (int)sample_erases;
    else if (!strcmp(what, "clocks_out"))
        got = (int)clock_out_count;
    else if (!strcmp(what, "notes_out"))
        got = (int)notes_out_count;
    else if (!strcmp(what, "sx_ok"))
        got = (int)sx_acks_ok;
    else if (!strcmp(what, "sx_err"))
        got = (int)sx_acks_err;
    else if (!strcmp(what, "sx_replies"))
        got = (int)sx_replies;
    else if (!strcmp(what, "held"))
        got = chp_held;
    else if (!strncmp(what, "par", 3))                /* parN: parameter N (chp.h P_*) */
        got = proj.par[atoi(what + 3)];
    else if (!strncmp(what, "src", 3))                /* srcN: step N's source */
        got = ep->st[atoi(what + 3) & 31].src;
    else if (!strncmp(what, "fx", 2))                 /* fxN: step N's fx */
        got = ep->st[atoi(what + 2) & 31].fx;
    else if (!strncmp(what, "tag", 3))                /* tagN: slice N's tag */
        got = eb->tag[atoi(what + 3) & 31];
    else if (!strncmp(what, "blen", 4))               /* blenN: break N's length */
        got = (int)brk[atoi(what + 4) & 7].len;
    else if (!strncmp(what, "patlen", 6))             /* patlenN: pattern N's length */
        got = pats.pat[atoi(what + 6) % CHP_NPAT].len;
    else if (!strncmp(what, "patsrc", 6))             /* patsrcN: pattern N's first step */
        got = pats.pat[atoi(what + 6) % CHP_NPAT].st[0].src;
    else if (!strcmp(what, "peak_db_max") || !strcmp(what, "peak_db_min")) {
        double db = 20.0 * log10(peak_out > 1e-9f ? peak_out : 1e-9f);
        int bad = what[8] == 'm' && what[9] == 'a' ? db > atof(val) : db < atof(val);
        printf("%s peak %.1f dBFS (%s %s)\n", bad ? "FAIL" : "  ok", db, what + 8, val);
        return bad;
    } else {
        printf("FAIL unknown expect %s\n", what);
        return 1;
    }
    if (val[0] == '<' || val[0] == '>') {
        int lim = (int)strtol(val + 1, 0, 0);
        if (val[0] == '<' ? got >= lim : got <= lim) {
            printf("FAIL expect %s %s, got %d\n", what, val, got);
            return 1;
        }
        printf("  ok %s == %d (%s)\n", what, got, val);
        return 0;
    }
    if (got != (int)strtol(val, 0, 0)) {
        printf("FAIL expect %s == %s, got %d\n", what, val, got);
        return 1;
    }
    printf("  ok %s == %d\n", what, got);
    return 0;
}

#ifndef FM_WEB
int main(int argc, char **argv)
{
    FILE *sc;
    char line[512], out[400];
    const char *dir = argc > 2 ? argv[2] : ".";
    int lineno = 0, fails = 0;
    if (argc < 2) {
        fprintf(stderr, "usage: chp_host SCRIPT [OUTDIR]\n");
        return 2;
    }
    sc = fopen(argv[1], "r");
    if (!sc) {
        perror(argv[1]);
        return 2;
    }
    snprintf(out, sizeof out, "%s/midi_out.txt", dir);
    midi_log = fopen(out, "w");
    snprintf(out, sizeof out, "%s/sysex_out.txt", dir);
    sx_log = fopen(out, "w");
    memset(sflash, 0xFF, sizeof sflash);
    boot();
    while (fgets(line, sizeof line, sc)) {
        char cmd[32] = {0}, a[256] = {0}, b[256] = {0}, c[64] = {0};
        lineno++;
        if (line[0] == '#' || sscanf(line, "%31s %255s %255s %63s", cmd, a, b, c) < 1)
            continue;
        if (!strcmp(cmd, "wait"))
            run_ms((uint32_t)atoi(a));
        else if (!strcmp(cmd, "press") || !strcmp(cmd, "release") || !strcmp(cmd, "tap")) {
            int i = find_name(BTN_N, NB, a);
            if (i < 0) {
                printf("line %d: no button %s\n", lineno, a);
                return 2;
            }
            if (cmd[0] != 'r')
                held_btn |= 1u << i;
            if (cmd[0] == 't')
                run_ms(40);
            if (cmd[0] != 'p')
                held_btn &= ~(1u << i);
            run_ms(cmd[0] == 't' ? 40 : 2);
        } else if (!strcmp(cmd, "key") || !strcmp(cmd, "tapkey")) {
            int k = key_of(a);
            if (cmd[0] == 't') {
                held_keys |= 1u << k;
                run_ms(60);
                held_keys &= ~(1u << k);
                run_ms(30);
            } else {
                if (!strcmp(b, "down"))
                    held_keys |= 1u << k;
                else
                    held_keys &= ~(1u << k);
                run_ms(2);
            }
        } else if (!strcmp(cmd, "hold")) {
            int k = key_of(a);
            held_keys |= 1u << k;
            run_ms((uint32_t)atoi(b));
            held_keys &= ~(1u << k);
            run_ms(2);
        } else if (!strcmp(cmd, "keys")) {           /* keys K1 K2 ...: each tapped in turn */
            char *tok = strtok(line + 5, " \t\r\n");
            for (; tok; tok = strtok(0, " \t\r\n")) {
                int k = key_of(tok);
                held_keys |= 1u << k;
                run_ms(60);
                held_keys &= ~(1u << k);
                run_ms(30);
            }
        } else if (!strcmp(cmd, "turn")) {
            int e = find_name(ENC_N, NE, a), n = atoi(b), s = n > 0 ? 1 : -1;
            uint32_t gap = c[0] ? (uint32_t)atoi(c) : 80u;
            if (e < 0) {
                printf("line %d: no encoder %s\n", lineno, a);
                return 2;
            }
            for (; n; n -= s) {
                enc_acc[e] += s;
                run_ms(gap);
            }
        } else if (!strcmp(cmd, "spin")) {
            int e = find_name(ENC_N, NE, a);
            if (e < 0) {
                printf("line %d: no encoder %s\n", lineno, a);
                return 2;
            }
            enc_acc[e] += atoi(b);
            accel_off = 1;
            run_ms(1);
            accel_off = 0;
        } else if (!strcmp(cmd, "master"))
            master = (uint32_t)atoi(a);
        else if (!strcmp(cmd, "midi")) {
            uint32_t s = (uint32_t)strtoul(a, 0, 16), d1 = (uint32_t)strtoul(b, 0, 16), d2 = (uint32_t)strtoul(c, 0, 16);
            uint32_t cin = s >= 0xF0 ? 0x0F : s >> 4;
            min_q[mi_w++ % MQ] = cin | s << 8 | d1 << 16 | d2 << 24;
        } else if (!strcmp(cmd, "clockin")) {
            int n = atoi(a), gap = atoi(b);
            for (; n > 0; n--) {
                min_q[mi_w++ % MQ] = 0x0Fu | 0xF8u << 8;
                run_ms((uint32_t)(gap > 0 ? gap : 1));
            }
        } else if (!strcmp(cmd, "sysex"))
            sysex_file(a);
        else if (!strcmp(cmd, "loadwav")) {
            if (load_wav(atoi(a) - 1, b, c[0] ? atoi(c) : 0) != ST_OK)
                fails++;
        } else if (!strcmp(cmd, "wav")) {
            snprintf(out, sizeof out, "%s/%s", dir, a);
            wav_open(out);
        } else if (!strcmp(cmd, "wavstop"))
            wav_close();
        else if (!strcmp(cmd, "peakreset"))
            peak_out = 0.0f;
        else if (!strcmp(cmd, "shot")) {
            snprintf(out, sizeof out, "%s/%s", dir, a);
            png_write(out);
        } else if (!strcmp(cmd, "leds")) {
            int i;
            printf("  leds: buttons");
            for (i = 0; i < NB; i++)
                if (lit_btn >> i & 1u)
                    printf(" %s", BTN_N[i]);
            printf(" | white ");
            for (i = 0; i < 16; i++)
                printf("%c", (lit_keys >> WHITE_K[i] & 1u) ? '#' : '.');
            printf(" black ");
            for (i = 0; i < 11; i++)
                printf("%c", (lit_keys >> BLACK_K[i] & 1u) ? '#' : '.');
            printf("\n");
        } else if (!strcmp(cmd, "expect"))
            fails += expect(a, b);
        else if (!strcmp(cmd, "reboot")) {
            memset(&proj, 0, sizeof proj);
            memset(&pats, 0, sizeof pats);
            memset(brk, 0, sizeof brk);
            memset(mem, 0, sizeof mem);
            audio_due_ms = now_ms;
            boot();
        } else if (!strcmp(cmd, "wipe")) {
            memset(sflash, 0xFF, sizeof sflash);
            memset(store_len, 0xFF, sizeof store_len);
        } else if (!strcmp(cmd, "echo"))
            printf("%s", line + 5);
        else {
            printf("line %d: unknown command %s\n", lineno, cmd);
            return 2;
        }
    }
    wav_close();
    if (midi_log)
        fclose(midi_log);
    if (sx_log)
        fclose(sx_log);
    printf("host: %u ms simulated, %u blits, render %.1f%% of real time (host CPU)%s\n", now_ms, blits,
           render_budget_ns_total > 0 ? 100.0 * render_ns_total / render_budget_ns_total : 0.0,
           fails ? ", EXPECTATIONS FAILED" : "");
    return fails ? 1 : 0;
}
#endif /* FM_WEB */
