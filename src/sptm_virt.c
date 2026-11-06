/* SPDX-License-Identifier: MIT */

/*
 * Virtualized SPTM: the carve-out and the host's loader entry points.
 */

#include "sptm_virt.h"
#include "adt.h"
#include "heapblock.h"
#include "memory.h"
#include "soc.h"
#include "string.h"
#include "utils.h"
#include "xnuboot.h"

/*
 * aura's top-of-RAM carve-out: its heap (per-CPU NV2 and GL pages, stage-2 tables, the trace ring)
 * and the host's proxy staging heap, which starts 128 MiB above the heap base and runs for about
 * as much again.
 */
#define SPTM_VIRT_AURA_HEAP (512UL << 20)
#define SPTM_VIRT_ALIGN     (2UL << 20) /* an L2 block */

bool sptm_virt_requested(void)
{
    /* Only T8140 boots as a kcbz=0 Mach-O object with GXF unlocked. */
    return chip_id == T8140;
}

int sptm_virt_compute_layout(u64 phys_base, u64 mem_size, u64 ram_base, u64 ram_top, u64 fw_high,
                             u64 aura_size, struct sptm_virt_layout *out)
{
    if (!out)
        return -1;
    memset(out, 0, sizeof(*out));

    if (!mem_size || !aura_size)
        return -1;
    if (ram_top <= ram_base || phys_base < ram_base || phys_base >= ram_top)
        return -1;
    if (mem_size > ram_top - phys_base)
        return -1;

    /* Managed RAM must end before the earliest fixed high-firmware region, or SPTM rejects it. */
    u64 managed_limit = phys_base + mem_size;
    if (fw_high) {
        if (fw_high <= phys_base)
            return -1;
        if (fw_high < managed_limit)
            managed_limit = fw_high;
    }
    managed_limit = ALIGN_DOWN(managed_limit, SPTM_VIRT_ALIGN);
    if (managed_limit <= phys_base)
        return -1;

    /* aura comes off the top, and SPTM is told its window ends below it. */
    aura_size = ALIGN_UP(aura_size, SPTM_VIRT_ALIGN);
    if (aura_size >= managed_limit - phys_base)
        return -1; /* must leave managed RAM for SPTM/XNU below it */
    u64 aura_base = managed_limit - aura_size;
    if (aura_base <= phys_base)
        return -1;

    out->ram_base = ram_base;
    out->ram_top = ram_top;
    out->managed_base = phys_base;
    out->managed_end = aura_base;
    out->aura_base = aura_base;
    out->aura_top = managed_limit;
    out->fw_high = fw_high;
    out->aura_size = aura_size;
    return 0;
}

static u64 sptm_virt_adt_u64(int node, const char *prop)
{
    if (node < 0)
        return 0;
    u32 len = 0;
    const void *p = adt_getprop(adt, node, prop, &len);
    if (!p || len < sizeof(u64))
        return 0;
    u64 v;
    memcpy(&v, p, sizeof(v));
    return v;
}

/* The lowest fixed high-firmware PA SPTM requires outside managed RAM, 0 if none. */
static u64 sptm_virt_fw_high(void)
{
    int sgx = adt_path_offset(adt, "/arm-io/sgx");
    int sapt = adt_path_offset(adt, "/arm-io/sapt");
    const struct {
        int node;
        const char *prop;
    } probes[] = {
        {sapt, "table-address"},         {sgx, "gfx-handoff-base"},
        {sgx, "gfx-shared-region-base"}, {sgx, "gfx-shared-l2-region-base"},
        {sgx, "gpu-region-base"},
    };
    u64 lo = 0;
    for (size_t i = 0; i < ARRAY_SIZE(probes); i++) {
        u64 v = sptm_virt_adt_u64(probes[i].node, probes[i].prop);
        if (v && (!lo || v < lo))
            lo = v;
    }
    return lo;
}

/*
 * Lowest PA SPTM's window may start at: above aura's image, its boot-time heap, iBoot's boot data
 * and the firmware's read-only range, all of which sit at the bottom of what iBoot calls managed
 * RAM. Candidates at or above limit (the carve-out) are ignored.
 */
static u64 sptm_virt_low_floor(u64 limit)
{
    u64 ro_start = 0, ro_end = 0;
    u64 floor = cur_boot_args.phys_base;
    const u64 cand[] = {
        (u64)_end,
        cur_boot_args.top_of_kernel_data,
        (u64)heapblock_high_water(),
        (u64)heapblock_limit(),
        memory_fw_ro_range(&ro_start, &ro_end) > 0 ? ro_end : 0,
    };

    for (size_t i = 0; i < ARRAY_SIZE(cand); i++)
        if (cand[i] < limit && cand[i] > floor)
            floor = cand[i];
    return ALIGN_UP(floor, SZ_32M); /* SPTM maps its window in 32 MiB blocks */
}

static int sptm_virt_layout(struct sptm_virt_layout *out)
{
    u64 ram_base = ALIGN_DOWN(cur_boot_args.phys_base, BIT(32));
    int rc = sptm_virt_compute_layout(cur_boot_args.phys_base, cur_boot_args.mem_size, ram_base,
                                      ram_base + mem_size_actual, sptm_virt_fw_high(),
                                      SPTM_VIRT_AURA_HEAP, out);
    if (rc < 0)
        return rc;
    u64 floor = sptm_virt_low_floor(out->managed_end);
    if (floor > out->managed_base) {
        if (floor + (64UL << 20) >= out->managed_end)
            return -1; /* nothing sensible left to manage */
        out->managed_base = floor;
    }
    return 0;
}

/* heapblock_init: aura's heap lives in the carve-out, never in SPTM's window. */
bool sptm_virt_heap_window(u64 *base, u64 *limit)
{
    struct sptm_virt_layout l;

    if (!sptm_virt_requested() || sptm_virt_layout(&l) < 0 || l.aura_top <= l.aura_base)
        return false;
    *base = l.aura_base;
    *limit = l.aura_top;
    return true;
}

__attribute__((used, retain)) int sptm_virt_place(struct sptm_virt_placement *out)
{
    struct sptm_virt_layout l;

    if (!out)
        return -1;
    memset(out, 0, sizeof(*out));
    if (sptm_virt_layout(&l) < 0)
        return -1;
    out->managed_base = l.managed_base;
    out->managed_end = l.managed_end;
    return 0;
}

/*
 * The handoff SPTM's __start reads from x0 is an XNU boot_args (revision 2, version 2): virtBase
 * +8, physBase +0x10, memSize +0x18, topOfKernelData +0x20. The host builds it
 * (proxyclient/aura/sptm_virt_plan.py); this only refuses one that is not a boot_args or that would
 * let SPTM manage aura's memory.
 */
struct sptm_virt_boot_args {
    u16 revision, version;
    u32 pad;
    u64 virt_base, phys_base, mem_size, top_of_kernel_data;
};

static int sptm_virt_check_handoff(const struct sptm_virt_layout *l, u64 handoff)
{
    const volatile struct sptm_virt_boot_args *ba = (const void *)handoff;

    if (handoff & (SZ_16K - 1)) {
        printf("SPTM-virt: load refused, handoff 0x%lx is not 16 KiB aligned\n", handoff);
        return -1;
    }
    dc_civac_range((void *)handoff, sizeof(*ba));
    u16 rev = ba->revision, ver = ba->version;
    u64 pbase = ba->phys_base, msize = ba->mem_size, top = ba->top_of_kernel_data;
    printf("SPTM-virt: handoff boot_args rev %u ver %u virtBase 0x%lx physBase 0x%lx memSize 0x%lx "
           "top 0x%lx\n",
           rev, ver, (u64)ba->virt_base, pbase, msize, top);
    if (rev != 2 || ver != 2) {
        printf("SPTM-virt: load refused, handoff is not a revision-2/version-2 boot_args\n");
        return -1;
    }
    if (pbase < l->managed_base || pbase >= l->managed_end || msize > l->managed_end - pbase ||
        top > l->managed_end) {
        printf("SPTM-virt: load refused, boot_args managed window [0x%lx, 0x%lx) is not inside "
               "SPTM's window [0x%lx, 0x%lx)\n",
               pbase, pbase + msize, l->managed_base, l->managed_end);
        return -1;
    }
    u64 wend = pbase + msize;
    u64 ranges[][2] = {{(u64)_base, (u64)_end},
                       {(u64)heapblock_high_water(), (u64)heapblock_limit()}};
    for (size_t i = 0; i < ARRAY_SIZE(ranges); i++) {
        if (ranges[i][1] > ranges[i][0] && ranges[i][0] < wend && ranges[i][1] > pbase) {
            printf("SPTM-virt: load refused, SPTM's window [0x%lx, 0x%lx) overlaps aura [0x%lx, "
                   "0x%lx)\n",
                   pbase, wend, ranges[i][0], ranges[i][1]);
            return -1;
        }
    }
    return 0;
}

/* The guest starts with its MMU off, so whatever the host wrote must reach the point of coherency.
 */
__attribute__((used, retain)) void sptm_virt_flush(u64 pa, u64 size)
{
    dc_civac_range((void *)pa, size);
    sysop("dsb sy");
}

__attribute__((used, retain)) int sptm_virt_load(u64 sptm_entry, u64 handoff)
{
    struct sptm_virt_layout l;

    if (sptm_virt_layout(&l) < 0) {
        printf("SPTM-virt: load refused, no valid carve-out layout\n");
        return -1;
    }
    if (sptm_entry < l.managed_base || sptm_entry >= l.managed_end) {
        printf("SPTM-virt: load refused, entry 0x%lx outside managed RAM [0x%lx, 0x%lx)\n",
               sptm_entry, l.managed_base, l.managed_end);
        return -1;
    }
    if (handoff < l.managed_base || handoff >= l.managed_end) {
        printf("SPTM-virt: load refused, handoff 0x%lx outside managed RAM\n", handoff);
        return -1;
    }
    if (sptm_virt_check_handoff(&l, handoff) < 0)
        return -1;
    printf("SPTM-virt: images recorded, entry 0x%lx handoff 0x%lx; ready to enter\n", sptm_entry,
           handoff);
    return 0;
}
