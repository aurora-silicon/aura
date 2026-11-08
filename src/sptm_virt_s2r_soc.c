/* SPDX-License-Identifier: MIT */

/*
 * The SoC blocks the virtualized SPTM's sleep path watches, from the ADT alone.
 *
 * The PMGR's reg list names none of its blocks. Its ps-regs (or ps-groups) give the PS area and
 * the always-on area; each block is the reg entry at its place in one of them. The PMS is the
 * PMGR's 16 MiB entry, the AMCC the mcc entry its amcc-reg-idx names. A chip whose reg list lacks
 * a block that marks the sleep gets no sleep path; one that only the trace reads is skipped.
 */

#include "sptm_virt_s2r_soc.h"
#include "adt.h"
#include "string.h"
#include "utils.h"

#define PMGR_PATH "/arm-io/pmgr"

struct s2r_place {
    const char *what;
    bool aon;      /* in the always-on area, else the PS area */
    bool required; /* else only traced, and skipped when the ADT has no entry there */
    s64 offset;    /* from the area's base */
    u64 size;      /* 0: any entry large enough for the clock pages */
    size_t field;  /* offsetof(struct s2r_soc, ...) */
};

#define PLACE(name, aon, req, off, size)                                                           \
    {"PMGR block " #name, aon, req, off, size, offsetof(struct s2r_soc, name)}

/* Each core's self-disable (soc_misc) and the S2R timer (aon_pst) mark the sleep; the rest is the
 * trace. */
static const struct s2r_place s2r_places[] = {
    PLACE(soc_misc, false, true, 0x30000, 0x14000),
    PLACE(aon_pst, true, true, 0x50000, 0x8000),
    PLACE(soc_ps, false, false, 0, 0x18000),
    PLACE(pwrgate, false, false, 0x24000, 0x4000),
    PLACE(cllt, false, false, 0x68000, 0x4000),
    PLACE(cluster_ctl, false, false, 0x80000, 0x4000),
    PLACE(aon, true, false, 0, 0x80000),
    PLACE(nub_ap, true, false, -0x27c000, 0x1000),
    PLACE(clkgen, false, false, -0x700000, 0),
};

#define S2R_CLKGEN_MIN 0x60000 /* the DCS and mux pages sit below this */
#define S2R_PMS_SIZE   0x1000000
#define S2R_PMC_PAGE0  0x800000
#define S2R_PMC_PAGE1  0x8a8000
#define S2R_NUB_WAKE   0x28
#define S2R_AMCC_ERR   0x25000

static u32 s2r_u32(const void *adt, int node, const char *name, u32 dflt)
{
    u32 v;
    return ADT_GETPROP(adt, node, name, &v) < 0 ? dflt : v;
}

/* The child of `parent` with this phandle, or the child one of whose own children has it. */
static int s2r_by_phandle(const void *adt, int parent, u32 phandle, bool grandchild)
{
    int child = parent;
    if (!phandle)
        return -1;
    ADT_FOREACH_CHILD(adt, child)
    {
        if (!grandchild && s2r_u32(adt, child, "AAPL,phandle", 0) == phandle)
            return child;
        int kid = child;
        if (grandchild)
            ADT_FOREACH_CHILD(adt, kid)
            {
                if (s2r_u32(adt, kid, "AAPL,phandle", 0) == phandle)
                    return child;
            }
    }
    return -1;
}

static void s2r_add_link(struct s2r_soc *out, const void *adt, int node)
{
    if (out->n_link >= S2R_LINK_NODES)
        return;
    char *p = out->link_paths[out->n_link++];
    const char *name = adt_get_name(adt, node);
    size_t n = strlen(name);
    if (n > sizeof(out->link_paths[0]) - sizeof("/arm-io/"))
        n = sizeof(out->link_paths[0]) - sizeof("/arm-io/");
    memcpy(p, "/arm-io/", 8);
    memcpy(p + 8, name, n);
    p[8 + n] = 0;
}

/* The controller, DART and PHY of each hidden USB port, from its PHY's phandle. */
static int s2r_link_from_adt(const void *adt, const u32 *phys, int n_phys, struct s2r_soc *out)
{
    int io = adt_path_offset(adt, "/arm-io");
    if (io < 0)
        return -1;
    for (int i = 0; i < n_phys; i++) {
        int phy = s2r_by_phandle(adt, io, phys[i], false);
        int drd = io;
        if (phy < 0)
            continue;
        ADT_FOREACH_CHILD(adt, drd)
        {
            if (s2r_u32(adt, drd, "atc-phy-parent", 0) != phys[i])
                continue;
            /* iommu-parent is the DART's mapper node, a child of the DART. */
            int dart = s2r_by_phandle(adt, io, s2r_u32(adt, drd, "iommu-parent", 0), true);
            if (dart < 0)
                continue;
            s2r_add_link(out, adt, drd);
            s2r_add_link(out, adt, dart);
            s2r_add_link(out, adt, phy);
        }
    }
    return out->n_link ? 0 : -1;
}

int s2r_soc_from_adt(const void *adt, const u32 *link_phys, int n_phys, struct s2r_soc *out,
                     const char **missing)
{
    int path[8];
    u32 len;

    memset(out, 0, sizeof(*out));
    *missing = "PMGR ps-regs";
    int node = adt_path_offset_trace(adt, PMGR_PATH, path);
    if (node < 0)
        return -1;
    const u32 *ps = adt_getprop(adt, node, "ps-regs", &len);
    if (!ps || !len)
        ps = adt_getprop(adt, node, "ps-groups", &len);
    u32 n_ps = ps ? len / 12 : 0, n_reg = 0;
    if (!n_ps || !adt_getprop(adt, node, "reg", &n_reg))
        return -1;
    n_reg /= 16;

    /* The first group is the PS area, the first other one the always-on area. */
    u64 area[2] = {0, 0}, sz;
    u32 ps_idx = ps[0];
    if (adt_get_reg(adt, path, "reg", ps_idx, &area[0], &sz) < 0)
        return -1;
    for (u32 i = 1; i < n_ps && !area[1]; i++)
        if (ps[3 * i] != ps_idx && adt_get_reg(adt, path, "reg", ps[3 * i], &area[1], &sz) < 0)
            return -1;

    for (size_t p = 0; p < ARRAY_SIZE(s2r_places); p++) {
        const struct s2r_place *pl = &s2r_places[p];
        struct s2r_blk *blk = (void *)((u8 *)out + pl->field);
        u64 want = area[pl->aon] + pl->offset;
        for (u32 i = 0; i < n_reg && area[pl->aon] && !blk->base; i++) {
            u64 a, s;
            if (adt_get_reg(adt, path, "reg", i, &a, &s) < 0 || a != want || i == ps_idx)
                continue;
            if (pl->size ? s == pl->size : s >= S2R_CLKGEN_MIN)
                *blk = (struct s2r_blk){a, s};
        }
        if (blk->base)
            continue;
        *missing = pl->what;
        if (pl->required)
            return -1;
        if (out->n_skipped < (int)ARRAY_SIZE(out->skipped))
            out->skipped[out->n_skipped++] = pl->what;
    }

    /* The PMS's PMC pages: in the PMGR's only 16 MiB entry. */
    for (u32 i = 0, n = 0; i < n_reg; i++) {
        u64 a, s;
        if (adt_get_reg(adt, path, "reg", i, &a, &s) < 0 || s != S2R_PMS_SIZE)
            continue;
        out->pms_pmc[0] = n++ ? 0 : a + S2R_PMC_PAGE0;
        out->pms_pmc[1] = out->pms_pmc[0] ? a + S2R_PMC_PAGE1 : 0;
    }
    if (!out->pms_pmc[0] && out->n_skipped < (int)ARRAY_SIZE(out->skipped))
        out->skipped[out->n_skipped++] = "PMS";
    if (out->nub_ap.base)
        out->nub_wake_status = out->nub_ap.base + S2R_NUB_WAKE;

    /* No amcc-reg-idx: the AMCC quiesce is not available, nothing else needs it. */
    node = adt_path_offset_trace(adt, "/arm-io/mcc", path);
    u32 amcc_idx;
    u64 a, s;
    if (node >= 0 && ADT_GETPROP(adt, node, "amcc-reg-idx", &amcc_idx) >= 0 &&
        adt_get_reg(adt, path, "reg", amcc_idx, &a, &s) >= 0) {
        out->amcc = a + S2R_AMCC_ERR;
        out->amcc_planes =
            s2r_u32(adt, node, "amcc-count", 1) * s2r_u32(adt, node, "dcs-count-per-amcc", 1);
    }

    *missing = "debug-link USB port";
    if (s2r_link_from_adt(adt, link_phys, n_phys, out) < 0)
        return -1;
    *missing = NULL;
    return 0;
}
