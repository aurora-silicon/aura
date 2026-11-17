/* SPDX-License-Identifier: MIT */

/*
 * G0 GPU admission port and range table for the T8152 (J804) Windows carrier.  See
 * gpu_t8152_admit.h.  The checks follow src/kboot_gpu_t8152.c, which stays the Linux path's
 * own copy; this file reads the ADT only and lifts every address it reports from it.
 */

#include "gpu_t8152_admit.h"
#include "adt.h"
#include "malloc.h"
#include "string.h"
#include "utils.h"

#define T8152_CHIP     0x8152
#define J804_BOARD     0x22
#define UAT_VADDR_BITS 43
#define TLV_SPAN       0x23a /* the patchbay's TLV sequence, as kboot_gpu_t8152.c checks it */
/* Per-boot words outside the TLVs that kboot_gpu_t8152.c zeroes before the digest. */
#define TEXT_ZERO_A     0x224
#define TEXT_ZERO_A_LEN 4
#define TEXT_ZERO_B     0x23c
#define TEXT_ZERO_B_LEN 8
#define MAX_NODE_REGS   1024 /* as hv_t8152.c: J804 error-handler has 120 */
/*
 * GSIVs the Windows arbiter accepts (hv_aic_alias.h), minus the block the carrier keeps for its
 * virtual SPIs and aliases (hv_aic_alias.c: AHCI, USB, ANS all sit in [980, 1000)).
 */
#define GSIV_MIN        32
#define GSIV_END        1020
#define CARRIER_SPI_LO  980
#define CARRIER_SPI_END 1000

static const char *const region_names[GPU_T8152_REGIONS] = {
    "gpu-region",  "gfx-shared-region", "gfx-shared-l2-region", "gfx-handoff",
    "gfx-data",    "gfx1-data",         "gfx-data-shared-ro",   "gfx1-data-shared-ro",
};
static const char *const asc_names[GPU_T8152_ROLES] = {"gfx-asc", "gfx1-asc"};

static bool overlaps(u64 a, u64 n, u64 b, u64 m)
{
    return a < b + m && b < a + n;
}

static bool within(u64 a, u64 n, u64 b, u64 m)
{
    return n && a >= b && a - b <= m && n <= m - (a - b);
}

static bool number(int node, const char *prop, u64 *value)
{
    u32 len;
    const void *p = adt_getprop(adt, node, prop, &len);
    *value = 0;
    if (!p || (len != 4 && len != 8))
        return false;
    memcpy(value, p, len);
    return true;
}

static bool string_is(int node, const char *prop, const char *value)
{
    u32 len;
    const void *p = adt_getprop(adt, node, prop, &len);
    return p && len == strlen(value) + 1 && !memcmp(p, value, len);
}

static int refuse(const char *what)
{
    printf("HV: GPU: admission refused: %s\n", what);
    return -1;
}

static int check_sgx(const struct gpu_t8152_env *env, struct gpu_t8152_admission *a)
{
    int chosen = adt_path_offset(adt, "/chosen");
    int defaults = adt_path_offset(adt, "/defaults");
    int trace[8];
    int sgx = adt_path_offset_trace(adt, "/arm-io/sgx", trace);
    u64 width, mode;

    if (env->chip_id != T8152_CHIP)
        return refuse("chip is not T8152");
    if (env->board_id != J804_BOARD)
        return refuse("board is not J804 (0x22)");
    if (!env->profiles || !env->tlvs || !env->tlv_count || !env->receipt || !env->read_text ||
        !env->sha256)
        return refuse("no firmware profile, receipt or TEXT reader for this board");
    if (chosen < 0 || defaults < 0 || sgx < 0)
        return refuse("/chosen, /defaults or /arm-io/sgx is missing");
    if (!number(chosen, "dram-base", &a->dram_base) ||
        !number(chosen, "dram-size", &a->dram_size) || !a->dram_size ||
        a->dram_base > UINT64_MAX - a->dram_size)
        return refuse("/chosen dram-base/dram-size");
    if (!within(env->ram_base, env->ram_top - env->ram_base, a->dram_base, a->dram_size))
        return refuse("boot_args RAM is not inside /chosen DRAM");
    if (!string_is(sgx, "compatible", "gpu,t8152"))
        return refuse("sgx compatible is not gpu,t8152");
    if (!number(defaults, "uat-vaddr-size", &width) || width != UAT_VADDR_BITS)
        return refuse("/defaults uat-vaddr-size is not 43");
    if (!number(sgx, "agx-address-space-mgmt-mode", &mode) || mode != 1)
        return refuse("sgx agx-address-space-mgmt-mode is not 1");
    if (!number(sgx, "rtkit-private-vm-region-base", &a->vm_base) || !a->vm_base)
        return refuse("sgx rtkit-private-vm-region-base");
    if (adt_get_reg(adt, trace, "reg", 0, &a->sgx.base, &a->sgx.size) || !a->sgx.size ||
        (a->sgx.base | a->sgx.size) & (GPU_T8152_PAGE - 1) || a->sgx.base > UINT64_MAX - a->sgx.size)
        return refuse("sgx reg 0 (the register window) is missing or not 16 KiB aligned");
    if (adt_get_reg(adt, trace, "reg", 1, &a->sgx1.base, &a->sgx1.size) ||
        !within(a->sgx1.base, a->sgx1.size, a->sgx.base, a->sgx.size))
        return refuse("sgx reg 1 is missing or not inside the sgx window");
    if (overlaps(a->sgx.base, a->sgx.size, a->dram_base, a->dram_size))
        return refuse("the sgx window overlaps DRAM");

    for (unsigned r = 0; r < GPU_T8152_ROLES; r++) {
        for (unsigned j = 0; j < 2; j++) {
            char key[48];
            u64 value;
            snprintf(key, sizeof(key), "%s-data-shared-rw-%s", r ? "gfx1" : "gfx",
                     j ? "size" : "base");
            if (adt_getprop(adt, sgx, key, NULL) && (!number(sgx, key, &value) || value)) {
                printf("HV: GPU: admission refused: sgx %s is not zero\n", key);
                return -1;
            }
        }
    }

    for (unsigned i = 0; i < GPU_T8152_REGIONS; i++) {
        char key[64];
        struct gpu_t8152_span *g = &a->region[i];
        snprintf(key, sizeof(key), "%s-base", region_names[i]);
        bool ok = number(sgx, key, &g->base);
        snprintf(key, sizeof(key), "%s-size", region_names[i]);
        ok = ok && number(sgx, key, &g->size);
        if (!ok || !g->size || (g->base | g->size) & (GPU_T8152_PAGE - 1) ||
            !within(g->base, g->size, a->dram_base, a->dram_size)) {
            printf("HV: GPU: admission refused: sgx %s is missing, not 16 KiB aligned or "
                   "outside DRAM\n",
                   region_names[i]);
            return -1;
        }
        /* UAT regions sit above boot_args RAM, firmware DATA below it; neither inside. */
        bool above = g->base >= env->ram_top;
        bool below = g->base + g->size <= env->ram_base;
        if (i < GPU_R_DATA ? !above : !below) {
            printf("HV: GPU: admission refused: %s [0x%lx, 0x%lx) is not %s boot_args RAM "
                   "[0x%lx, 0x%lx)\n",
                   region_names[i], g->base, g->base + g->size, i < GPU_R_DATA ? "above" : "below",
                   env->ram_base, env->ram_top);
            return -1;
        }
        for (unsigned j = 0; j < i; j++)
            if (overlaps(g->base, g->size, a->region[j].base, a->region[j].size)) {
                printf("HV: GPU: admission refused: %s overlaps %s\n", region_names[i],
                       region_names[j]);
                return -1;
            }
    }
    if (a->region[GPU_R_PAGETABLES].size < 0x44000 ||
        (a->region[GPU_R_PAGETABLES].size & 0x7fff))
        return refuse("gfx-shared-region size is not what the TLVs split in two");

    u32 len, cnt_len;
    const u8 *perf = adt_getprop(adt, sgx, "perf-states", &len);
    const u8 *sram = adt_getprop(adt, sgx, "perf-states-sram", &cnt_len);
    u64 count = 0;
    if (!perf || !sram || len != cnt_len || !len || len % 8 || len / 8 > GPU_T8152_PERF_MAX ||
        (adt_getprop(adt, sgx, "perf-state-count", NULL) &&
         (!number(sgx, "perf-state-count", &count) || count != len / 8)))
        return refuse("sgx perf-states / perf-states-sram / perf-state-count disagree");
    a->perf_states = perf;
    a->perf_sram = sram;
    a->perf_count = len / 8;
    return 0;
}

static bool line_ok(u32 line)
{
    return line >= GSIV_MIN && line < GSIV_END &&
           !(line >= CARRIER_SPI_LO && line < CARRIER_SPI_END);
}

static int check_asc(const struct gpu_t8152_env *env, struct gpu_t8152_admission *a,
                     unsigned r, const struct adt_segment_ranges **segp)
{
    char path[80];
    int trace[8];
    const char *name = asc_names[r];
    u64 mb;
    snprintf(path, sizeof(path), "/arm-io/%s", name);
    int node = adt_path_offset_trace(adt, path, trace);
    if (node < 0 || !string_is(node, "compatible", "iop,ascwrap-v8") ||
        !string_is(node, "msgbox-using-postoffice", "true") ||
        !number(node, "msgbox-mailbox-num", &mb) || mb != r + 1 ||
        !string_is(node, "segment-names", "__TEXT;__DATA;__DATA_SHARED_RO")) {
        printf("HV: GPU: admission refused: /arm-io/%s is missing or not the ascwrap-v8 "
               "post-office shape (mailbox %u)\n",
               name, r + 1);
        return -1;
    }
    a->mailbox_num[r] = (u32)mb;

    struct gpu_t8152_span *w = &a->wrapper[r], *v = &a->vbar[r], mbox;
    if (adt_get_reg(adt, trace, "reg", 0, &w->base, &w->size) ||
        adt_get_reg(adt, trace, "reg", 1, &v->base, &v->size) ||
        adt_get_reg(adt, trace, "reg", 2, &mbox.base, &mbox.size)) {
        printf("HV: GPU: admission refused: /arm-io/%s has fewer than 3 reg entries\n", name);
        return -1;
    }
    if (w->size != GPU_T8152_WRAPPER_SIZE || w->base & (GPU_T8152_PAGE - 1) ||
        !within(w->base, w->size, a->sgx.base, a->sgx.size)) {
        printf("HV: GPU: admission refused: %s wrapper 0x%lx+0x%lx is not a 0x%lx window "
               "inside the sgx window [0x%lx, 0x%lx)\n",
               name, w->base, w->size, GPU_T8152_WRAPPER_SIZE, a->sgx.base,
               a->sgx.base + a->sgx.size);
        return -1;
    }
    if (v->size != GPU_T8152_VBAR_SIZE || v->base != w->base - GPU_T8152_VBAR_BELOW ||
        !within(v->base, v->size, a->sgx.base, a->sgx.size)) {
        printf("HV: GPU: admission refused: %s iop-vbar 0x%lx+0x%lx is not the 8-byte word "
               "0x%lx below its wrapper inside the sgx window\n",
               name, v->base, v->size, GPU_T8152_VBAR_BELOW);
        return -1;
    }
    if (mbox.size != GPU_T8152_MAILBOX_SIZE || mbox.base & (GPU_T8152_PAGE - 1) ||
        overlaps(mbox.base, mbox.size, a->sgx.base, a->sgx.size) ||
        overlaps(mbox.base, mbox.size, a->dram_base, a->dram_size) ||
        (r && (mbox.base != a->mailbox.base || mbox.size != a->mailbox.size))) {
        printf("HV: GPU: admission refused: %s post-office bank 0x%lx+0x%lx is not one shared "
               "16 KiB-aligned 0x%lx bank outside the sgx window and DRAM\n",
               name, mbox.base, mbox.size, GPU_T8152_MAILBOX_SIZE);
        return -1;
    }
    a->mailbox = mbox;
    /* Both GFX windows must sit in the bank's first page (the one the carrier hooks). */
    if ((mb + 1) * GPU_T8152_MBOX_STRIDE > GPU_T8152_PAGE)
        return refuse("a GFX mailbox window is outside the post-office page");

    u32 len;
    const u32 *irq = adt_getprop(adt, node, "interrupts", &len);
    if (!irq || len != 2 * sizeof(u32)) {
        printf("HV: GPU: admission refused: %s interrupts is not two lines\n", name);
        return -1;
    }
    for (unsigned i = 0; i < 2; i++) {
        memcpy(&a->irq[r][i], irq + i, sizeof(u32));
        bool clash = !line_ok(a->irq[r][i]) || (i && a->irq[r][1] == a->irq[r][0]);
        for (unsigned q = 0; q < r; q++)
            clash |= a->irq[r][i] == a->irq[q][0] || a->irq[r][i] == a->irq[q][1];
        if (clash) {
            printf("HV: GPU: admission refused: %s interrupts line %u is outside [%d, %d), "
                   "repeated, or in the carrier's block [%d, %d)\n",
                   name, a->irq[r][i], GSIV_MIN, GSIV_END, CARRIER_SPI_LO, CARRIER_SPI_END);
            return -1;
        }
    }

    const struct adt_segment_ranges *seg = adt_getprop(adt, node, "segment-ranges", &len);
    if (!seg || len != 3 * sizeof(*seg)) {
        printf("HV: GPU: admission refused: %s segment-ranges is not three segments\n", name);
        return -1;
    }
    for (unsigned i = 0; i < 3; i++) {
        struct adt_segment_ranges s;
        memcpy(&s, seg + i, sizeof(s));
        if (s.phys != s.remap || !s.size || (s.phys | s.size) & (GPU_T8152_PAGE - 1) ||
            !within(s.phys, s.size, a->dram_base, a->dram_size) ||
            s.phys + s.size > env->ram_base) {
            printf("HV: GPU: admission refused: %s segment %u 0x%lx+0x%x is not aligned "
                   "firmware memory below boot_args RAM\n",
                   name, i, (u64)s.phys, s.size);
            return -1;
        }
    }
    struct adt_segment_ranges text, data, ro;
    memcpy(&text, seg, sizeof(text));
    memcpy(&data, seg + 1, sizeof(data));
    memcpy(&ro, seg + 2, sizeof(ro));
    const struct gpu_t8152_span *rd = &a->region[GPU_R_DATA + r];
    const struct gpu_t8152_span *rr = &a->region[GPU_R_DATA_RO + r];
    if (data.phys != rd->base || data.size != rd->size || ro.phys != rr->base ||
        ro.size != rr->size) {
        printf("HV: GPU: admission refused: %s segments __DATA/__DATA_SHARED_RO are not the "
               "sgx-named regions\n",
               name);
        return -1;
    }
    if (text.size != env->profiles[r].text_size) {
        printf("HV: GPU: admission refused: %s __TEXT is 0x%x bytes, the profile says 0x%x\n",
               name, text.size, env->profiles[r].text_size);
        return -1;
    }
    for (unsigned i = 0; i < GPU_T8152_REGIONS; i++)
        if (overlaps(text.phys, text.size, a->region[i].base, a->region[i].size)) {
            printf("HV: GPU: admission refused: %s __TEXT overlaps %s\n", name, region_names[i]);
            return -1;
        }
    a->text[r].base = text.phys;
    a->text[r].size = text.size;

    snprintf(path, sizeof(path), "/arm-io/%s/iop-%s-nub", name, r ? "gfx1" : "gfx");
    node = adt_path_offset(adt, path);
    if (node < 0 || !string_is(node, "uuid", env->profiles[r].uuid)) {
        printf("HV: GPU: admission refused: %s firmware uuid is not the profile's\n", name);
        return -1;
    }
    a->uuid[r] = env->profiles[r].uuid;
    *segp = seg;
    return 0;
}

/* The TEXT identity, as kboot_gpu_t8152.c's qualify_text() checks it. */
static int check_text(const struct gpu_t8152_env *env, const struct gpu_t8152_admission *a,
                      unsigned r)
{
    const struct gpu_profile *profile = &env->profiles[r];
    const char *name = asc_names[r];
    u8 *copy = malloc(profile->text_size);
    int ret = -1;
    if (!copy)
        return refuse("no memory for the TEXT copy");
    if (!env->read_text(a->text[r].base, copy, profile->text_size)) {
        printf("HV: GPU: admission refused: %s __TEXT is not readable\n", name);
        goto out;
    }
    u32 offset = profile->patchbay;
    for (u32 i = 0; i < env->tlv_count; i++) {
        const struct gpu_tlv *t = &env->tlvs[i];
        u32 length;
        if (offset > profile->text_size || profile->text_size - offset < 8)
            goto malformed;
        memcpy(&length, copy + offset + 4, 4);
        if (memcmp(copy + offset, t->tag, 4) || length != t->size || length > sizeof(u64) ||
            length > profile->text_size - offset - 8)
            goto malformed;
        u64 value = 0, expected = 0;
        bool check = true;
        memcpy(&value, copy + offset + 8, length);
        const struct gpu_t8152_span *pt = &a->region[GPU_R_PAGETABLES];
        if (!memcmp(t->tag, "BPTP", 4))
            expected = pt->base + r * (pt->size / 2);
        else if (!memcmp(t->tag, "SPTP", 4))
            expected = pt->size / 2;
        else if (!memcmp(t->tag, "BlpP", 4))
            expected = a->region[GPU_R_HANDOFF].base;
        else if (!memcmp(t->tag, "S2xG", 4))
            expected = a->region[GPU_R_SHARED_L2].base;
        else if (!memcmp(t->tag, "BVTP", 4))
            expected = a->vm_base;
        else if (!memcmp(t->tag, "dArW", 4))
            expected = a->wrapper[r].base;
        else if (!memcmp(t->tag, "_COS", 4))
            expected = T8152_CHIP;
        else
            check = false;
        if (check && value != expected) {
            printf("HV: GPU: admission refused: %s TLV %.4s 0x%lx disagrees with this boot's ADT "
                   "(0x%lx)\n",
                   name, t->tag, value, expected);
            goto out;
        }
        if (t->exclude)
            memset(copy + offset + 8, 0, length);
        offset += 8 + length;
    }
    if (offset != profile->patchbay + TLV_SPAN)
        goto malformed;
    if (profile->text_size < TEXT_ZERO_B + TEXT_ZERO_B_LEN)
        goto malformed;
    memset(copy + TEXT_ZERO_A, 0, TEXT_ZERO_A_LEN);
    memset(copy + TEXT_ZERO_B, 0, TEXT_ZERO_B_LEN);
    u8 digest[32];
    env->sha256(copy, profile->text_size, digest);
    if (memcmp(digest, profile->digest, sizeof(digest))) {
        printf("HV: GPU: admission refused: %s __TEXT digest is not the profile's\n", name);
        goto out;
    }
    ret = 0;
    goto out;
malformed:
    printf("HV: GPU: admission refused: %s patchbay TLV sequence is malformed\n", name);
out:
    free(copy);
    return ret;
}

int gpu_t8152_admit(const struct gpu_t8152_env *env, struct gpu_t8152_admission *out)
{
    struct gpu_t8152_admission a;
    const struct adt_segment_ranges *seg[GPU_T8152_ROLES];

    memset(&a, 0, sizeof(a));
    memset(out, 0, sizeof(*out));
    if (check_sgx(env, &a))
        return -1;
    for (unsigned r = 0; r < GPU_T8152_ROLES; r++)
        if (check_asc(env, &a, r, &seg[r]))
            return -1;
    if (overlaps(a.text[0].base, a.text[0].size, a.text[1].base, a.text[1].size))
        return refuse("the two __TEXT segments overlap");
    for (unsigned r = 0; r < GPU_T8152_ROLES; r++)
        if (check_text(env, &a, r))
            return -1;
    a.chip_id = env->chip_id;
    a.board_id = env->board_id;
    a.receipt = env->receipt;
    *out = a;
    return 0;
}

/* ---- the class table ---------------------------------------------------------------------- */

const char *gpu_class_name(u32 cls)
{
    static const char *const names[GPU_C_COUNT] = {
        "none", "sgx", "sgx-hole", "asc-ctrl", "asc", "iop-vbar", "mailbox",
        "fw-rw", "fw-ro", "fw-text", "pool", "handoff",
    };
    return cls < GPU_C_COUNT ? names[cls] : "?";
}

static bool is_gpu_node(const char *name)
{
    return !strcmp(name, "sgx") || !strcmp(name, "gfx-asc") || !strcmp(name, "gfx1-asc");
}

int gpu_t8152_collect_holes(const struct gpu_t8152_admission *a, struct gpu_range *holes,
                            u32 cap)
{
    int armio = adt_path_offset(adt, "/arm-io");
    u32 n = 0;
    u64 lo = a->sgx.base, hi = a->sgx.base + a->sgx.size;
    if (armio < 0)
        return -1;
    int child = armio;
    ADT_FOREACH_CHILD(adt, child)
    {
        const char *name = adt_get_name(adt, child);
        if (!name || is_gpu_node(name))
            continue;
        int path[3] = {armio, child, 0};
        for (int idx = 0; idx < MAX_NODE_REGS; idx++) {
            u64 base, size;
            if (adt_get_reg(adt, path, "reg", idx, &base, &size) < 0)
                break;
            if (!size || base > UINT64_MAX - size || !overlaps(base, size, lo, a->sgx.size))
                continue;
            if (n >= cap) {
                printf("HV: GPU: more than %u foreign ranges inside the sgx window\n", cap);
                return -1;
            }
            u64 s = base < lo ? lo : base, e = base + size > hi ? hi : base + size;
            holes[n++] = (struct gpu_range){s, e - s, GPU_C_SGX_HOLE, 0, 0, name};
        }
    }
    return (int)n;
}

static bool page_hits(u64 page, u64 base, u64 size)
{
    return overlaps(page, GPU_T8152_PAGE, base, size);
}

static void classify(const struct gpu_t8152_admission *a, const struct gpu_range *holes,
                     u32 nholes, u64 page, u32 *cls, u32 *tag, const char **name)
{
    *cls = GPU_C_SGX;
    *tag = 0;
    *name = "sgx";
    for (u32 r = 0; r < GPU_T8152_ROLES; r++)
        if (page_hits(page, a->wrapper[r].base, a->wrapper[r].size)) {
            *cls = GPU_C_ASC;
            *tag = r;
            *name = asc_names[r];
        }
    for (u32 i = 0; i < nholes; i++)
        if (page_hits(page, holes[i].base, holes[i].size)) {
            *cls = GPU_C_SGX_HOLE;
            *tag = 0;
            *name = holes[i].name;
        }
    for (u32 r = 0; r < GPU_T8152_ROLES; r++) {
        /* CPU_CONTROL and CPU_STATUS live in the wrapper's first page. */
        if (page == ALIGN_DOWN(a->wrapper[r].base + GPU_T8152_ASC_CPU_CONTROL, GPU_T8152_PAGE)) {
            *cls = GPU_C_ASC_CTRL;
            *tag = r;
            *name = asc_names[r];
        }
        if (page_hits(page, a->vbar[r].base, a->vbar[r].size)) {
            *cls = GPU_C_IOP_VBAR;
            *tag = r;
            *name = asc_names[r];
        }
    }
}

static int push(struct gpu_range *out, u32 *n, u32 cap, struct gpu_range r)
{
    if (*n >= cap) {
        printf("HV: GPU: the G0 range table does not fit in %u entries\n", cap);
        return -1;
    }
    out[(*n)++] = r;
    return 0;
}

int gpu_t8152_build_ranges(const struct gpu_t8152_admission *a, const struct gpu_range *holes,
                           u32 nholes, struct gpu_range *out, u32 cap)
{
    u32 n = 0;
    u64 end = a->sgx.base + a->sgx.size;
    for (u64 page = a->sgx.base; page < end; page += GPU_T8152_PAGE) {
        u32 cls, tag;
        const char *name;
        classify(a, holes, nholes, page, &cls, &tag, &name);
        struct gpu_range *last = n ? &out[n - 1] : NULL;
        if (last && last->cls == cls && last->tag == tag && last->base + last->size == page) {
            last->size += GPU_T8152_PAGE;
            continue;
        }
        if (push(out, &n, cap, (struct gpu_range){page, GPU_T8152_PAGE, cls, tag, 0, name}))
            return -1;
    }
    /* The post-office page that holds both GFX windows (the rest of the bank stays denied). */
    if (push(out, &n, cap,
             (struct gpu_range){a->mailbox.base, GPU_T8152_PAGE, GPU_C_MAILBOX, 0, 0,
                                "post-office"}))
        return -1;
    for (u32 i = 0; i < GPU_T8152_REGIONS; i++) {
        bool rw = i < GPU_R_DATA;
        if (push(out, &n, cap,
                 (struct gpu_range){a->region[i].base, a->region[i].size,
                                    rw ? GPU_C_FW_RW : GPU_C_FW_RO, i,
                                    rw ? 0 : GPU_RF_READ_ONLY, region_names[i]}))
            return -1;
    }
    for (u32 r = 0; r < GPU_T8152_ROLES; r++)
        if (push(out, &n, cap,
                 (struct gpu_range){a->text[r].base, a->text[r].size, GPU_C_FW_TEXT, r, 0,
                                    r ? "gfx1-asc __TEXT" : "gfx-asc __TEXT"}))
            return -1;
    for (u32 i = 0; i < n; i++)
        for (u32 j = 0; j < i; j++)
            if (overlaps(out[i].base, out[i].size, out[j].base, out[j].size)) {
                printf("HV: GPU: G0 ranges %s and %s overlap\n", out[i].name, out[j].name);
                return -1;
            }
    return (int)n;
}

int gpu_t8152_add_range(struct gpu_range *table, u32 *count, u32 cap, u64 base, u64 size,
                        u32 cls, const char *name)
{
    if (!size || (base | size) & (GPU_T8152_PAGE - 1) || base > UINT64_MAX - size) {
        printf("HV: GPU: range %s 0x%lx+0x%lx is not 16 KiB aligned\n", name, base, size);
        return -1;
    }
    for (u32 i = 0; i < *count; i++)
        if (overlaps(base, size, table[i].base, table[i].size)) {
            printf("HV: GPU: range %s [0x%lx, 0x%lx) overlaps %s\n", name, base, base + size,
                   table[i].name);
            return -1;
        }
    return push(table, count, cap, (struct gpu_range){base, size, cls, 0, 0, name});
}

const struct gpu_range *gpu_t8152_find(const struct gpu_range *table, u32 count, u64 addr)
{
    for (u32 i = 0; i < count; i++)
        if (addr >= table[i].base && addr - table[i].base < table[i].size)
            return &table[i];
    return NULL;
}

bool gpu_t8152_mailbox_window(const struct gpu_t8152_admission *a, u64 addr, u32 *role)
{
    for (u32 r = 0; r < GPU_T8152_ROLES; r++) {
        u64 w = a->mailbox.base + a->mailbox_num[r] * GPU_T8152_MBOX_STRIDE;
        if (addr >= w && addr - w < GPU_T8152_MBOX_WINDOW) {
            *role = r;
            return true;
        }
    }
    return false;
}

/* ---- the handoff page --------------------------------------------------------------------- */

u32 gpu_t8152_crc32(const void *data, u64 len)
{
    const u8 *p = data;
    u32 crc = 0xffffffffu;
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1));
    }
    return ~crc;
}

static void put32(u8 *page, u32 off, u32 v)
{
    memcpy(page + off, &v, 4);
}

static void put64(u8 *page, u32 off, u64 v)
{
    memcpy(page + off, &v, 8);
}

u32 gpu_t8152_handoff_build(void *dst, u64 page_size, const struct gpu_t8152_admission *a,
                            const struct gpu_range *table, u32 count,
                            const struct gpu_t8152_handoff_in *in)
{
    u8 *page = dst;
    u32 roff = GPU_HANDOFF_HEADER;
    u64 poff = roff + (u64)count * GPU_HANDOFF_RANGE;
    u64 total = poff + (u64)a->perf_count * GPU_HANDOFF_PERF;
    if (total > page_size || total > GPU_HANDOFF_SIZE)
        return 0;
    memset(page, 0, total);
    put32(page, GPU_HO_MAGIC, GPU_HANDOFF_MAGIC);
    u16 v16 = GPU_HANDOFF_VERSION;
    memcpy(page + GPU_HO_VERSION, &v16, 2);
    v16 = GPU_HANDOFF_HEADER;
    memcpy(page + GPU_HO_HEADER, &v16, 2);
    put32(page, GPU_HO_TOTAL, (u32)total);
    put32(page, GPU_HO_STATE, in->state);
    put32(page, GPU_HO_CHIP, a->chip_id);
    put32(page, GPU_HO_BOARD, a->board_id);
    put32(page, GPU_HO_RANGE_COUNT, count);
    put32(page, GPU_HO_RANGE_OFFSET, roff);
    put32(page, GPU_HO_RANGE_STRIDE, GPU_HANDOFF_RANGE);
    put32(page, GPU_HO_PERF_COUNT, a->perf_count);
    put32(page, GPU_HO_PERF_OFFSET, (u32)poff);
    put32(page, GPU_HO_PERF_STRIDE, GPU_HANDOFF_PERF);
    for (u32 r = 0; r < GPU_T8152_ROLES; r++) {
        put32(page, GPU_HO_GSIV + 8 * r, a->irq[r][0]);
        put32(page, GPU_HO_GSIV + 8 * r + 4, a->irq[r][1]);
        put32(page, GPU_HO_MAILBOX_NUM + 4 * r, a->mailbox_num[r]);
    }
    put64(page, GPU_HO_POOL_BASE, in->pool_base);
    put64(page, GPU_HO_POOL_SIZE, in->pool_size);
    put64(page, GPU_HO_HANDOFF_IPA, in->handoff_ipa);
    put64(page, GPU_HO_VM_BASE, a->vm_base);
    put64(page, GPU_HO_DRAM_BASE, a->dram_base);
    put64(page, GPU_HO_DRAM_SIZE, a->dram_size);
    if (a->receipt)
        strncpy((char *)page + GPU_HO_RECEIPT, a->receipt, 63);
    for (u32 i = 0; i < GPU_HANDOFF_ID_WORDS; i++)
        put32(page, GPU_HO_GPU_ID + 4 * i, in->gpu_id[i]);
    for (u32 r = 0; r < GPU_T8152_ROLES; r++) {
        put32(page, GPU_HO_CPU_CONTROL + 4 * r, in->cpu_control[r]);
        put32(page, GPU_HO_CPU_STATUS + 4 * r, in->cpu_status[r]);
    }
    for (u32 i = 0; i < count; i++) {
        u32 o = roff + i * GPU_HANDOFF_RANGE;
        put64(page, o, table[i].base);
        put64(page, o + 8, table[i].size);
        put32(page, o + 16, table[i].cls);
        put32(page, o + 20, table[i].tag);
        put32(page, o + 24, table[i].flags);
    }
    for (u32 i = 0; i < a->perf_count; i++) {
        u32 o = (u32)poff + i * GPU_HANDOFF_PERF, f, mv, sram_mv;
        memcpy(&f, a->perf_states + 8 * i, 4);
        memcpy(&mv, a->perf_states + 8 * i + 4, 4);
        memcpy(&sram_mv, a->perf_sram + 8 * i + 4, 4);
        put32(page, o, f);
        put32(page, o + 4, mv);
        put32(page, o + 8, sram_mv);
    }
    put32(page, GPU_HO_CRC, gpu_t8152_crc32(page, total));
    return (u32)total;
}
