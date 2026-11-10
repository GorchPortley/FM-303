/* SPDX-License-Identifier: GPL-3.0-only */
/* CHOPPAGE SysEx: loading breaks from a computer (tools/chp_breaks.py, web/breaks.html). Part of the
 * unity build after project.c; the host simulator includes it too.
 *
 * A message is F0 7D 43 48 50 cmd seq <data> F7: 7D is the manufacturer ID for non-commercial use,
 * 43 48 50 "CHP", cmd and seq 7-bit (seq comes back in the reply, so a retry is recognized), data
 * packed 8 bits to 7, least significant bits first (as the update protocol packs). Numbers are
 * little-endian.
 *
 *   01 HELLO                                  -> 01 version, slots, capacity u32, used u32, factory,
 *                                                then each slot: len u32, rate u16, fmt, beats, slices,
 *                                                name[12]
 *   02 BEGIN slot len u32 rate u16 fmt beats name[12]      a break into the slot (it replaces one there)
 *   03 DATA off u32 bytes (up to 512)         -> the next offset expected
 *   04 END crc32                              -> after the flash is written (a few seconds)
 *   05 DELETE slot                            06 FACTORY (every loaded break goes)
 *   07 SLICES slot n start[n] u32 tag[n]      a slice table worked out on the computer
 *   08 SELECT slot                            the break the patterns play
 * Every command is answered: 7F cmd status [data], status 0 = ok (ST_* in app.h).
 *
 * The audio engine lets go of the sample memory first (chp_hold): BEGIN, DELETE, FACTORY and
 * SLICES wait for it (their frame stays until then, the computer waits for the answer) and the
 * engine stays held until END, or 5 s without a frame. */
#define SX_HDR 6u
#define SX_MAX 560u
static const uint8_t SX_ID[4] = {0x7D, 0x43, 0x48, 0x50};
static uint8_t sx_dec[SX_MAX];
static uint8_t sx_out[2u + SX_HDR + 220u];
static uint8_t sx_holding;                           /* we asked the engine to let go */
static uint8_t sx_xfer;                              /* a break is coming in */
static uint32_t sx_last, sx_rx, sx_len;
uint32_t sx_gen;                                     /* counts changes to the breaks (the UI redraws) */

int sx_loading(void) { return sx_xfer || sx_holding; }
int sx_waiting(void)                                 /* a frame of ours waits (for the engine): keep it */
{
    const uint8_t *p;
    uint32_t n;
    return plat_sx_get(&p, &n) && n >= SX_HDR && memcmp(p, SX_ID, 4) == 0;
}
uint32_t sx_progress(void) { return sx_rx; }
uint32_t sx_total(void) { return sx_len; }

static uint32_t sx_unpack(const uint8_t *in, uint32_t n, uint8_t *out, uint32_t max)
{
    uint32_t acc = 0, nb = 0, o = 0;
    while (n--) {
        acc |= (uint32_t)(*in++ & 0x7Fu) << nb;
        nb += 7u;
        if (nb >= 8u) {
            if (o < max)
                out[o] = (uint8_t)acc;
            o++;
            acc >>= 8;
            nb -= 8u;
        }
    }
    return o;
}

static uint32_t sx_pack(const uint8_t *in, uint32_t n, uint8_t *out)
{
    uint32_t acc = 0, nb = 0, o = 0;
    while (n--) {
        acc |= (uint32_t)*in++ << nb;
        nb += 8u;
        while (nb >= 7u) {
            out[o++] = (uint8_t)(acc & 0x7Fu);
            acc >>= 7;
            nb -= 7u;
        }
    }
    if (nb)
        out[o++] = (uint8_t)(acc & 0x7Fu);
    return o;
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void sx_reply(uint32_t seq, uint32_t cmd, const uint8_t *body, uint32_t n)
{
    uint32_t w = 0, i;
    if (n > 190u)
        n = 190u;
    sx_out[w++] = 0xF0;
    for (i = 0; i < 4u; i++)
        sx_out[w++] = SX_ID[i];
    sx_out[w++] = (uint8_t)(cmd & 0x7Fu);
    sx_out[w++] = (uint8_t)(seq & 0x7Fu);
    w += sx_pack(body, n, sx_out + w);
    sx_out[w++] = 0xF7;
    plat_sx_send(sx_out, w);
}

static void sx_ack(uint32_t seq, uint32_t cmd, uint32_t st, const uint8_t *extra, uint32_t n)
{
    uint8_t b[200];
    uint32_t i;
    b[0] = (uint8_t)cmd;
    b[1] = (uint8_t)st;
    for (i = 0; i < n && i + 2u < sizeof b; i++)
        b[2u + i] = extra[i];
    sx_reply(seq, 0x7F, b, 2u + n);
}

static void sx_hello(uint32_t seq)
{
    uint8_t b[11u + CHP_NBRK * 21u];
    uint32_t w = 0, i, k;
    b[w++] = 1;                                      /* protocol version */
    b[w++] = CHP_NBRK;
    wr32(b + w, CHP_MEM);
    w += 4u;
    wr32(b + w, store_used());
    w += 4u;
    b[w++] = (uint8_t)store_is_factory();
    for (i = 0; i < CHP_NBRK; i++) {
        const chp_break_t *k2 = &brk[i];
        wr32(b + w, k2->len);
        b[w + 4u] = (uint8_t)k2->rate;
        b[w + 5u] = (uint8_t)(k2->rate >> 8);
        b[w + 6u] = k2->fmt;
        b[w + 7u] = k2->beats;
        b[w + 8u] = k2->len ? k2->nsl : 0;
        for (k = 0; k < 12u; k++)
            b[w + 9u + k] = (uint8_t)k2->name[k];
        w += 21u;
    }
    sx_reply(seq, 0x01, b, w);
}

static void sx_release(void)
{
    if (sx_holding) {
        chp_hold(0);
        sx_holding = 0;
    }
    sx_xfer = 0;
}

static void sx_changed(void)
{
    sx_gen++;
    project_apply();                                 /* the break selected may be gone */
}

void sx_service(void)
{
    const uint8_t *p;
    uint32_t n, d, cmd, seq, i;
    if (sx_holding && plat_ms() - sx_last > 5000u) { /* the computer went away: let go */
        if (sx_xfer) {
            store_abort();
            sx_changed();
        }
        sx_release();
    }
    if (!plat_sx_get(&p, &n))
        return;
    if (n < SX_HDR || memcmp(p, SX_ID, 4) != 0)
        return;                                      /* not ours: the update entry gets it */
    cmd = p[4];
    seq = p[5];
    sx_last = plat_ms();
    if ((cmd == 2u || cmd == 5u || cmd == 6u || cmd == 7u) && !chp_held) {
        if (!sx_holding) {
            chp_hold(1);
            sx_holding = 1;
        }
        return;                                      /* the frame waits for the engine */
    }
    d = sx_unpack(p + SX_HDR, n - SX_HDR, sx_dec, sizeof sx_dec);
    plat_sx_done();
    if (d > sizeof sx_dec)
        d = 0;
    switch (cmd) {
    case 1:
        sx_hello(seq);
        break;
    case 2: {
        int st;
        char name[13];
        if (d < 21u) {
            sx_ack(seq, cmd, ST_ERR_ARGS, 0, 0);
            break;
        }
        for (i = 0; i < 12u; i++)
            name[i] = (char)sx_dec[9u + i];
        name[12] = 0;
        st = store_begin(sx_dec[0], rd32(sx_dec + 1), rd16(sx_dec + 5), sx_dec[7], sx_dec[8], name);
        sx_xfer = st == ST_OK;
        sx_rx = 0;
        sx_len = st == ST_OK ? rd32(sx_dec + 1) : 0;
        sx_ack(seq, cmd, (uint32_t)st, 0, 0);
        if (st != ST_OK) {
            sx_changed();
            sx_release();
        }
        break;
    }
    case 3: {
        uint8_t nx[4];
        int st = d < 4u ? ST_ERR_ARGS : store_data(rd32(sx_dec), sx_dec + 4, d - 4u);
        if (st == ST_OK && rd32(sx_dec) + d - 4u > sx_rx)
            sx_rx = rd32(sx_dec) + d - 4u;
        wr32(nx, sx_rx);
        sx_ack(seq, cmd, (uint32_t)st, nx, 4);
        break;
    }
    case 4: {
        int st = d < 4u ? ST_ERR_ARGS : store_end(rd32(sx_dec));
        sx_ack(seq, cmd, (uint32_t)st, 0, 0);
        sx_changed();
        sx_release();
        break;
    }
    case 5: {
        int st = d < 1u ? ST_ERR_ARGS : store_delete(sx_dec[0]);
        sx_ack(seq, cmd, (uint32_t)st, 0, 0);
        sx_changed();
        sx_release();
        break;
    }
    case 6: {
        int st = store_factory();
        sx_ack(seq, cmd, (uint32_t)st, 0, 0);
        sx_changed();
        sx_release();
        break;
    }
    case 7: {                                        /* a slice table from the computer */
        uint32_t slot = sx_dec[0], ns = d >= 2u ? sx_dec[1] : 0, ok = d >= 2u + ns * 5u && slot < CHP_NBRK &&
                                                                        ns >= 1u && ns <= CHP_NSL && brk[slot].len;
        chp_break_t *b = ok ? &brk[slot] : 0;
        for (i = 0; ok && i < ns; i++) {
            uint32_t s = rd32(sx_dec + 2u + i * 4u);
            ok = s < b->len && (i ? s > rd32(sx_dec + 2u + (i - 1u) * 4u) : s == 0u) &&
                 sx_dec[2u + ns * 4u + i] < NTAGS;
        }
        if (ok) {
            for (i = 0; i < ns; i++) {
                b->start[i] = rd32(sx_dec + 2u + i * 4u);
                b->tag[i] = sx_dec[2u + ns * 4u + i];
            }
            b->nsl = (uint8_t)ns;
            kits_save();
        }
        sx_ack(seq, cmd, ok ? ST_OK : ST_ERR_ARGS, 0, 0);
        sx_changed();
        sx_release();
        break;
    }
    case 8:
        if (d >= 1u && sx_dec[0] < CHP_NBRK && brk[sx_dec[0]].len) {
            proj.brk = sx_dec[0];
            chp_break(proj.brk);
            sx_gen++;
            sx_ack(seq, cmd, ST_OK, 0, 0);
        } else {
            sx_ack(seq, cmd, ST_ERR_ARGS, 0, 0);
        }
        break;
    default:
        sx_ack(seq, cmd, ST_ERR_ARGS, 0, 0);
        break;
    }
}
