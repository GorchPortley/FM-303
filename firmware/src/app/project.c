/* SPDX-License-Identifier: GPL-3.0-only */
/* FM-303 project: the settings (OBJ_PROJ) and the pattern bank (OBJ_PATS), two flash objects. One of
 * another format, or none, gives the defaults. Part of the unity build, after app.h. */
project_t proj;
bank_t bank;

void project_defaults(void)
{
    int i;
    memset(&proj, 0, sizeof proj);
    proj.magic = PROJ_MAGIC;
    proj.format = PROJ_FORMAT;
    for (i = 0; i < P_NPARAMS; i++)
        proj.par[i] = fm_param_info(i)->def;
    proj.oct = 1;
    proj.leds = 1;
}

static int project_valid(void)
{
    int i;
    if (proj.magic != PROJ_MAGIC || proj.format != PROJ_FORMAT)
        return 0;
    for (i = 0; i < P_NPARAMS; i++) {
        const fm_param_t *p = fm_param_info(i);
        if (proj.par[i] < p->lo || proj.par[i] > p->hi)
            return 0;
    }
    return proj.edit < TB_NPAT && proj.group < TB_GROUPS && proj.oct <= 3u && proj.leds <= 2u;
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
        fm_set(i, proj.par[i]);
    fm_select(proj.edit, proj.edit);
}

void bank_defaults(void)
{
    int i;
    bank.magic = BANK_MAGIC;
    bank.format = BANK_FORMAT;
    for (i = 0; i < TB_NPAT; i++) {
        if (i < tb_factory_count())
            tb_factory(i, &bank.pat[i]);
        else
            tb_clear(&bank.pat[i]);
    }
}

int bank_load(void)
{
    int n, i;
    n = plat_store_load(OBJ_PATS, &bank, sizeof bank);
    if (n == (int)sizeof bank && bank.magic == BANK_MAGIC && bank.format == BANK_FORMAT) {
        for (i = 0; i < TB_NPAT; i++)
            if (!tb_valid(&bank.pat[i]))
                tb_clear(&bank.pat[i]);           /* one bad pattern does not cost the others */
        return 0;
    }
    bank_defaults();
    return -1;
}

int bank_save(void) { return plat_store_save(OBJ_PATS, &bank, sizeof bank); }
