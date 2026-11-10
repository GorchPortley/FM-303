/* SPDX-License-Identifier: GPL-3.0-only */
/* CHOPPAGE project: the settings (OBJ_PROJ), the patterns (OBJ_PATS), the slice tables (OBJ_KITS) and
 * the sample store. Part of the unity build, after app.h; the host simulator includes it too.
 *
 * The sample store is raw flash (plat_samples_*): a header sector, then the samples, mirrored in RAM
 * (mem, in the pool) at boot, since the audio interrupt never reads flash. The header lists the
 * breaks: where each lies, its length, rate, format, beats, name and CRC-32. A break is replaced by
 * pulling the ones after it down (in RAM) and adding the new one at the end; then the flash is
 * rewritten from the first byte that moved: the header first erased, the samples written, the
 * header written last. Power lost halfway leaves no header: the factory breaks come back, and the
 * breaks can be loaded again. With no break loaded, the three factory breaks are synthesized into
 * RAM at boot. */
project_t proj;
pats_t pats;
chp_break_t brk[CHP_NBRK];
uint8_t mem[CHP_MEM] FM_POOL;

uint32_t chp_crc32(const void *p, uint32_t n)        /* zlib's CRC-32 (the loaders compute the same) */
{
    static const uint32_t T[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};
    const uint8_t *b = p;
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *b++;
        c = (c >> 4) ^ T[c & 15u];
        c = (c >> 4) ^ T[c & 15u];
    }
    return ~c;
}

/* ---------------------------------------------------------------- settings --- */
void project_defaults(void)
{
    int i;
    memset(&proj, 0, sizeof proj);
    proj.magic = PROJ_MAGIC;
    proj.format = PROJ_FORMAT;
    for (i = 0; i < P_NPARAMS; i++)
        proj.par[i] = chp_param_info(i)->def;
    proj.leds = 1;
    proj.slm = SLM_AUTO;
    proj.sens = 50;
    proj.snap = 1;
    proj.seed = 0x6A756E67u;                         /* "jung" */
}

static int project_valid(void)
{
    int i;
    if (proj.magic != PROJ_MAGIC || proj.format != PROJ_FORMAT)
        return 0;
    for (i = 0; i < P_NPARAMS; i++) {
        const chp_param_t *p = chp_param_info(i);
        if (proj.par[i] < p->lo || proj.par[i] > p->hi)
            return 0;
    }
    return proj.brk < CHP_NBRK && proj.pat < CHP_NPAT && proj.leds <= 2u && proj.slm < NSLM && proj.sens <= 100u &&
           proj.snap <= 1u;
}

int project_load(void)
{
    int n;
    memset(&proj, 0, sizeof proj);
    n = plat_store_load(OBJ_PROJ, &proj, sizeof proj);
    if (n >= 8 && project_valid())
        return 0;
    project_defaults();
    return -1;
}

int project_save(void) { return plat_store_save(OBJ_PROJ, &proj, sizeof proj); }

void project_apply(void)
{
    int i;
    for (i = 0; i < P_NPARAMS; i++)
        chp_set(i, proj.par[i]);
    chp_seed(proj.seed);
    if (!brk[proj.brk].len) {                        /* the break saved is gone: the first there is */
        for (i = 0; i < CHP_NBRK && !brk[i].len; i++)
            ;
        proj.brk = (uint8_t)(i < CHP_NBRK ? i : 0);
    }
    chp_break(proj.brk);
    chp_pattern(proj.pat);
}

/* ---------------------------------------------------------------- patterns --- */
/* the factory patterns, one token a step:
 *   -  nothing (the slice before plays on)    K S H G  a type     ?  any slice    X  cut
 *   N  slice N (1..32)
 * then, in any order: rN roll (1: 2x, 2: 3x, 3: 4x, 4: 6x, 5: 8x, 6: 12x, 7: 16x), < reverse,
 * ^ / v the roll climbs / falls, p+N / p-N pitch, lN level 0..15, gN gate N/8, %N chance */
static const char *const FACTORY[] = {
    /* 1  the break, straight (32 sixteenths: two bars of a two-bar break) */
    "1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32",
    /* 2  two-step, in types: any tagged break */
    "K - H - S - H Gl9 - - K H S - H Gl9",
    /* 3  the classic chop: a bar straight, then the first half again and the snares rolled */
    "1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 1 2 3 4 5 6 7 8 5 6 13 14 13r1 13r2^ 15 16<",
    /* 4  roller */
    "K - H - S - H - H - K - S - H - K - H Gl9 S - H - H - K Gl9 S - H Gr1",
    /* 5  halftime */
    "K - H - H - H Gl9 S - H - H - K -",
    /* 6  breakcore */
    "K Sr1 G S K G Sr2 K S G K Sr1 G K Sr3^ Sr5^",
    /* 7  ragga: a reversed snare into each backbeat */
    "K - H S<l11 S - H - K - H S<l11 S - Hr1 Gl9",
    /* 8  tracker: jumps and stutters by slice number */
    "1 1 3 4 1 1 3 4 9 10 9 10 13 13 13r2 16<",
    /* 9  amen-style edit in types, two bars */
    "K - K - S - H S - S K K S - H S K - K - S - H S - S K - Sr1 Sr2 Sr3^ Sr5^",
    /* 10 stepper: a kick on every beat, snares on 2 and 4 */
    "K - H - S - H - K - H - S - H G",
    /* 11 sparse: hits and space for the echo */
    "K - - - S - - - - - K - S - - -",
    /* 12 gabber-ish: kicks, pitched down, with a ramped roll */
    "Kp-5 - Kp-5 - Sp-5 - Kp-5 - Kp-5 - Kp-5 - Sp-5 - Kr3vp-5 -",
};
#define NFACTORY (int)(sizeof FACTORY / sizeof FACTORY[0])

static int num(const char **s)
{
    int v = 0, neg = 0;
    if (**s == '-' || **s == '+')
        neg = *(*s)++ == '-';
    while (**s >= '0' && **s <= '9')
        v = v * 10 + *(*s)++ - '0';
    return neg ? -v : v;
}

void pat_factory(int i, chp_pattern_t *p)
{
    const char *s;
    int n = 0;
    if (i < 0 || i >= NFACTORY) {                    /* past the factory ones: the break, a bar */
        int k;
        chp_pattern_clear(p, 16);
        for (k = 0; k < 16; k++)
            p->st[k].src = (uint8_t)(SRC_SLICE + k);
        return;
    }
    chp_pattern_clear(p, CHP_STEPS);
    for (s = FACTORY[i]; *s && n < CHP_STEPS;) {
        chp_step_t *st = &p->st[n];
        while (*s == ' ')
            s++;
        if (!*s)
            break;
        if (*s >= '0' && *s <= '9') {
            int v = num(&s);
            st->src = (uint8_t)(v >= 1 && v <= CHP_NSL ? SRC_SLICE + v - 1 : SRC_NONE);
        } else {
            switch (*s++) {
            case 'K': st->src = SRC_K; break;
            case 'S': st->src = SRC_S; break;
            case 'H': st->src = SRC_H; break;
            case 'G': st->src = SRC_G; st->lev = 10; break;
            case '?': st->src = SRC_ANY; break;
            case 'X': st->src = SRC_CUT; break;
            default: st->src = SRC_NONE; break;
            }
        }
        while (*s && *s != ' ') {
            char c = *s++;
            int v;
            switch (c) {
            case 'r': st->fx = (uint8_t)((st->fx & ~FX_ROLL) | ((uint32_t)num(&s) & FX_ROLL)); break;
            case '<': st->fx |= FX_REV; break;
            case '^': st->fx |= FX_RAMP_UP; break;
            case 'v': st->fx |= FX_RAMP_DN; break;
            case 'p': v = num(&s); st->pitch = (int8_t)(v < -24 ? -24 : v > 24 ? 24 : v); break;
            case 'l': v = num(&s); st->lev = (uint8_t)(v < 0 ? 0 : v > 15 ? 15 : v); break;
            case 'g': v = num(&s); st->gate = (uint8_t)(v < 0 ? 0 : v > 8 ? 8 : v); break;
            case '%': v = num(&s); st->prob = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v); break;
            default: break;
            }
        }
        n++;
    }
    p->len = (uint8_t)(n ? n : 1);
}

void pats_defaults(void)
{
    int i;
    pats.magic = PATS_MAGIC;
    pats.format = PATS_FORMAT;
    for (i = 0; i < CHP_NPAT; i++)
        pat_factory(i, &pats.pat[i]);
}

int pats_load(void)
{
    int n, i;
    n = plat_store_load(OBJ_PATS, &pats, sizeof pats);
    if (n == (int)sizeof pats && pats.magic == PATS_MAGIC && pats.format == PATS_FORMAT) {
        for (i = 0; i < CHP_NPAT; i++)
            if (!chp_pattern_valid(&pats.pat[i]))
                pat_factory(i, &pats.pat[i]);    /* one bad pattern does not cost the others */
        return 0;
    }
    pats_defaults();
    return -1;
}

int pats_save(void) { return plat_store_save(OBJ_PATS, &pats, sizeof pats); }

/* ------------------------------------------------------------ slice tables --- */
static kits_t kits;

int kits_save(void)
{
    int i;
    memset(&kits, 0, sizeof kits);
    kits.magic = KITS_MAGIC;
    kits.format = KITS_FORMAT;
    for (i = 0; i < CHP_NBRK; i++) {
        kit_t *k = &kits.k[i];
        const chp_break_t *b = &brk[i];
        if (!b->len)
            continue;
        k->uid = b->uid;
        k->beats = b->beats;
        k->nsl = b->nsl;
        memcpy(k->name, b->name, sizeof k->name);
        memcpy(k->start, b->start, sizeof k->start);
        memcpy(k->tag, b->tag, sizeof k->tag);
    }
    return plat_store_save(OBJ_KITS, &kits, sizeof kits);
}

static int kit_valid(const kit_t *k, const chp_break_t *b)
{
    int s;
    if (k->uid != b->uid || k->nsl < 1 || k->nsl > CHP_NSL || k->beats < 1 || k->beats > 64 || k->start[0] != 0)
        return 0;
    for (s = 0; s < k->nsl; s++)
        if (k->start[s] >= b->len || (s && k->start[s] <= k->start[s - 1]) || k->tag[s] >= NTAGS)
            return 0;
    return 1;
}

static void kits_apply(void)                         /* the edits saved for each break, if it is still the same */
{
    int i, n = plat_store_load(OBJ_KITS, &kits, sizeof kits);
    if (n != (int)sizeof kits || kits.magic != KITS_MAGIC || kits.format != KITS_FORMAT)
        return;
    for (i = 0; i < CHP_NBRK; i++) {
        const kit_t *k = &kits.k[i];
        chp_break_t *b = &brk[i];
        if (!b->len || !kit_valid(k, b))
            continue;
        b->beats = k->beats;
        b->nsl = k->nsl;
        memcpy(b->name, k->name, sizeof b->name);
        memcpy(b->start, k->start, sizeof b->start);
        memcpy(b->tag, k->tag, sizeof b->tag);
    }
}

void brk_reslice(int i)
{
    ana_cfg_t c;
    chp_break_t *b = &brk[i];
    if (i < 0 || i >= CHP_NBRK || !b->len)
        return;
    c.mode = proj.slm;
    c.sens = proj.sens;
    c.snap = proj.snap;
    ana_slice(b, mem + b->off, &c);
    ana_tag(b, mem + b->off);
}

/* ----------------------------------------------------------- sample store --- */
typedef struct {
    uint32_t off, len, uid;
    uint16_t rate;
    uint8_t fmt, beats;
    char name[12];
} store_ent_t;
typedef struct {
    uint32_t magic, format, used, rsv;
    store_ent_t e[CHP_NBRK];
    uint32_t crc;                                    /* of everything above */
} store_hdr_t;

#define SEC 4096u
#define DATA0 SEC                                    /* the samples start a sector in */
static uint32_t used;                                /* bytes of mem in use */
static uint8_t factory_only;                         /* the breaks are the synthesized ones */
static int8_t rx_slot = -1;                          /* a break coming in */
static uint32_t rx_lo;                               /* the lowest byte of mem changed since the last save */

uint32_t store_used(void) { return used; }
int store_is_factory(void) { return factory_only; }
int store_count(void)
{
    int i, n = 0;
    for (i = 0; i < CHP_NBRK; i++)
        n += brk[i].len != 0;
    return n;
}

static void brk_clear(chp_break_t *b) { memset(b, 0, sizeof *b); }

static void load_factory(void)
{
    int i, s = 0;
    used = 0;
    for (i = 0; i < CHP_NBRK; i++)
        brk_clear(&brk[i]);
    for (i = 0; i < chp_factory_count() && s < CHP_NBRK; i++) {
        chp_break_t *b = &brk[s];
        uint32_t n = chp_factory(i, mem + used, CHP_MEM - used, b);
        if (!n)
            continue;
        b->off = used;
        b->uid = chp_crc32(mem + used, n);
        used += n;
        s++;
    }
    factory_only = 1;
}

static int hdr_read(store_hdr_t *h)
{
    int i;
    if (plat_samples_read(0, h, sizeof *h) || h->magic != STORE_MAGIC || h->format != STORE_FORMAT ||
        h->crc != chp_crc32(h, sizeof *h - 4u) || h->used > CHP_MEM)
        return -1;
    for (i = 0; i < CHP_NBRK; i++)
        if (h->e[i].len && (h->e[i].off > h->used || h->e[i].len > h->used - h->e[i].off))
            return -1;
    return 0;
}

int store_boot(void)
{
    store_hdr_t h;
    int i, n = 0;
    rx_slot = -1;
    if (hdr_read(&h) == 0 && h.used && plat_samples_read(DATA0, mem, h.used) == 0) {
        used = h.used;
        for (i = 0; i < CHP_NBRK; i++) {
            chp_break_t *b = &brk[i];
            const store_ent_t *e = &h.e[i];
            brk_clear(b);
            if (!e->len || e->rate < 1000u || e->fmt >= NFMT || chp_crc32(mem + e->off, e->len) != e->uid)
                continue;                            /* empty, or damaged: left out */
            b->off = e->off;
            b->len = e->len;
            b->uid = e->uid;
            b->rate = e->rate;
            b->fmt = e->fmt;
            b->beats = e->beats ? e->beats : ana_guess_beats(e->len, e->rate);
            memcpy(b->name, e->name, sizeof b->name);
            brk_reslice(i);
            n++;
        }
        factory_only = 0;
    }
    if (!n)
        load_factory();
    kits_apply();
    rx_lo = CHP_MEM;
    return store_count();
}

/* rewrite the flash from mem byte `from` on: header erased, sectors, header */
static int store_save(uint32_t from)
{
    store_hdr_t h;
    uint32_t s, e;
    int i;
    memset(&h, 0, sizeof h);
    h.magic = STORE_MAGIC;
    h.format = STORE_FORMAT;
    h.used = used;
    for (i = 0; i < CHP_NBRK; i++) {
        const chp_break_t *b = &brk[i];
        if (!b->len)
            continue;
        h.e[i].off = b->off;
        h.e[i].len = b->len;
        h.e[i].uid = b->uid;
        h.e[i].rate = b->rate;
        h.e[i].fmt = b->fmt;
        h.e[i].beats = b->beats;
        memcpy(h.e[i].name, b->name, sizeof h.e[i].name);
    }
    h.crc = chp_crc32(&h, sizeof h - 4u);
    if (plat_samples_erase(0))
        return ST_ERR_FLASH;
    e = (used + SEC - 1u) / SEC * SEC;
    for (s = from / SEC * SEC; s < e; s += SEC) {
        uint32_t k;
        plat_idle();
        if (plat_samples_erase(DATA0 + s))
            return ST_ERR_FLASH;
        for (k = 0; k < SEC && s + k < used; k += 256u)
            if (plat_samples_write(DATA0 + s + k, mem + s + k, s + k + 256u <= used ? 256u : used - s - k))
                return ST_ERR_FLASH;
    }
    plat_idle();
    if (plat_samples_write(0, &h, sizeof h))
        return ST_ERR_FLASH;
    {   /* read the header back: a part that will not write must not report success */
        store_hdr_t chk;
        if (hdr_read(&chk) || chk.crc != h.crc)
            return ST_ERR_FLASH;
    }
    factory_only = 0;
    return ST_OK;
}

/* take break i out of mem: the ones after it move down */
static void remove_slot(int i)
{
    chp_break_t *b = &brk[i];
    uint32_t off = b->off, len = b->len;
    int k;
    if (!len)
        return;
    memmove(mem + off, mem + off + len, used - off - len);
    used -= len;
    for (k = 0; k < CHP_NBRK; k++)
        if (brk[k].len && brk[k].off > off)
            brk[k].off -= len;
    brk_clear(b);
    if (off < rx_lo)
        rx_lo = off;
}

int store_begin(int slot, uint32_t len, uint32_t rate, int fmt, int beats, const char *name)
{
    chp_break_t *b;
    uint32_t k;
    if (slot < 0 || slot >= CHP_NBRK || !len || rate < 1000u || rate > 48000u || fmt < 0 || fmt >= NFMT ||
        beats < 0 || beats > 64)
        return ST_ERR_ARGS;
    if (rx_slot >= 0)
        store_abort();
    if (factory_only) {                              /* the first load: the factory breaks go */
        for (k = 0; k < CHP_NBRK; k++)
            brk_clear(&brk[k]);
        used = 0;
        rx_lo = 0;
        factory_only = 0;
    }
    if (len > CHP_MEM - used + brk[slot].len)
        return ST_ERR_SPACE;
    remove_slot(slot);
    b = &brk[slot];
    b->off = used;
    b->len = len;
    b->rate = (uint16_t)rate;
    b->fmt = (uint8_t)fmt;
    b->beats = (uint8_t)(beats ? beats : ana_guess_beats(len, rate));
    for (k = 0; k < sizeof b->name; k++)
        b->name[k] = name && k < 11u ? name[k] : 0;
    for (k = 0; k < sizeof b->name && b->name[k]; k++)
        if (b->name[k] < 32 || b->name[k] > 126)
            b->name[k] = '?';
    if (b->off < rx_lo)
        rx_lo = b->off;
    used += len;
    rx_slot = (int8_t)slot;
    return ST_OK;
}

int store_data(uint32_t off, const uint8_t *p, uint32_t n)
{
    const chp_break_t *b;
    if (rx_slot < 0)
        return ST_ERR_STATE;
    b = &brk[rx_slot];
    if (off > b->len || n > b->len - off)
        return ST_ERR_ARGS;
    memcpy(mem + b->off + off, p, n);
    return ST_OK;
}

void store_abort(void)
{
    if (rx_slot >= 0)
        remove_slot(rx_slot);
    rx_slot = -1;
}

int store_end(uint32_t crc)
{
    chp_break_t *b;
    int rc;
    if (rx_slot < 0)
        return ST_ERR_STATE;
    b = &brk[rx_slot];
    if (chp_crc32(mem + b->off, b->len) != crc) {
        store_abort();
        return ST_ERR_CRC;
    }
    b->uid = crc;
    brk_reslice(rx_slot);
    rx_slot = -1;
    rc = store_save(rx_lo);
    rx_lo = CHP_MEM;
    if (rc == ST_OK)
        kits_save();
    return rc;
}

int store_delete(int slot)
{
    int rc;
    if (slot < 0 || slot >= CHP_NBRK)
        return ST_ERR_ARGS;
    if (!brk[slot].len)
        return ST_OK;
    if (factory_only) {                              /* a factory break: it goes until the next boot */
        remove_slot(slot);
        return ST_OK;
    }
    remove_slot(slot);
    rc = store_save(rx_lo);
    rx_lo = CHP_MEM;
    if (rc == ST_OK)
        kits_save();
    if (!store_count())
        load_factory();
    return rc;
}

int store_factory(void)
{
    if (plat_samples_erase(0))                       /* no header: the factory breaks, now and at boot */
        return ST_ERR_FLASH;
    rx_slot = -1;
    load_factory();
    rx_lo = CHP_MEM;
    return ST_OK;
}
