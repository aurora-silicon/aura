/* SPDX-License-Identifier: MIT */

#ifndef GPU_T8152_UAT_H
#define GPU_T8152_UAT_H

#include "gpu_t8152_admit.h"
#include "types.h"

/*
 * G2: an audit of the GPU's page tables (UAT) from the carrier.
 *
 * The GPU reaches memory through its own page tables, which the guest writes: the UAT regions
 * are mapped RW to the guest, and the tables the probe builds live in the pool, which is ordinary
 * guest RAM.  Stage 2 confines only the guest CPU; nothing confines the GPU's DMA except those
 * tables (owner decision D3).  Validating every table write synchronously would need every table
 * page trapped, the pool included; instead the carrier walks the tables at fixed points and
 * reports, and refuses messages that would let the firmware use a table that maps outside:
 *
 *   - at PROBE, before the guest runs: the baseline (what iBoot and the firmware itself mapped
 *     in their own tables and the shared-L2 table; never what a pool table or a context-table
 *     root holds, which is the guest's and may be stale from an earlier boot);
 *   - before each init-data message (G2): any entry outside the allowed set that is not in the
 *     baseline refuses the message, so the firmware never sees a graph mapped outside;
 *   - before each other application-endpoint message (G2): such an entry in any table the guest
 *     can write refuses the message: the pool, a context-table root, and every table inside an
 *     fw-rw region (PROBE maps all four read-write for the guest, so the page-table region and
 *     the shared-L2 table are guest-writable too, and a guest could place a table there or point
 *     a context root at one).  Only a table outside the pool and every fw-rw region would be the
 *     firmware's alone and reported; the walk reads none today.  So a change the firmware makes
 *     to its own tables after init data also refuses the next message (fail-safe);
 *   - a page reached as a table at two different levels is an outlier (its entries would mean
 *     different things on the two paths and the walk checks only one);
 *   - at teardown: the whole walk again, reported.
 *
 * What this cannot see: a write between an audit and the firmware's next use of the tables, and
 * any use by the GPU that needs no message (the firmware runs on its own once it has init data).
 * The probe's own PTE guard is the first line; this audit is the carrier's check of it.
 *
 * Format (gpu/FACT-LEDGER.md G-01, G-03, public m1n1 hw/uat.py): 16 KiB tables of 2048 u64
 * entries, three levels; a context table of 64 (lower root, upper root) pairs at the gpu-region
 * base; a root or table descriptor is valid in bit 0, bits 1:0 = 3 name a next-level table (or a
 * page at the last level), bits 1:0 = 1 at the middle level a 32 MiB block; output address bits
 * 47:14.  A leaf is allowed when its whole page or block lies in the pool, a UAT region, a DATA
 * region or (with G2-D1 granted) a declared fw-mmio window rounded out to 16 KiB pages.
 * Firmware TEXT, other DRAM and other MMIO are outliers; so are malformed descriptors and tables
 * outside the pool and the page-table regions (which are not followed).
 */

#define GPU_UAT_ENTRIES   2048
#define GPU_UAT_CONTEXTS  64
#define GPU_UAT_MAX_ROOTS 8
#define GPU_UAT_REPORT_MAX 8

enum gpu_uat_class {
    GPU_UC_POOL = 0,
    GPU_UC_UAT = 1,     /* inside a UAT region (fw-rw) */
    GPU_UC_DATA = 2,    /* inside a DATA region (fw-ro) */
    GPU_UC_FW_MMIO = 3, /* inside a declared fw-mmio window */
    GPU_UC_TEXT = 4,    /* outlier: firmware TEXT */
    GPU_UC_DRAM = 5,    /* outlier: other DRAM */
    GPU_UC_MMIO = 6,    /* outlier: anything outside DRAM */
    GPU_UC_BAD = 7,     /* outlier: a malformed descriptor */
    GPU_UC_TABLE = 8,   /* outlier: a table outside the readable table regions (not followed) */
    GPU_UC_COUNT = 9,
};

struct gpu_uat_span {
    u64 base, size;
};

/* What the audit may read and what it allows. */
struct gpu_uat_scope {
    struct gpu_uat_span pool;
    struct gpu_uat_span fw_rw[4], fw_ro[4], text[GPU_T8152_ROLES];
    struct gpu_uat_span ctx_table;  /* the context table (gpu-region) */
    struct gpu_uat_span tables[3];  /* where tables may be read: pool, gfx-shared, shared-l2 */
    u64 dram_base, dram_size;
    const struct gpu_range *fw_mmio; /* declared windows (empty without G2-D1) */
    u32 nfw_mmio;
    u64 roots[GPU_UAT_MAX_ROOTS];    /* extra roots, absolute (the recipe's) */
    u32 nroots;
    u64 *visited;                    /* scratch: table addresses, visited_cap of them */
    u32 visited_cap;
};

/* Reads: a 16 KiB page of tables, made coherent; NULL if it cannot be read. */
struct gpu_uat_ops {
    const u64 *(*page)(u64 pa, void *arg);
    void *arg;
};

/* One outlier: the entry (table page | index) and its descriptor. */
struct gpu_uat_outlier {
    u64 key, desc;
    u8 cls, level;
    bool in_pool; /* a guest-writable table: the pool, a context-table root, any fw-rw region */
};

/*
 * The baseline: every outlier of the PROBE walk, sorted by (entry, descriptor) once recorded so a
 * later audit finds each of its outliers by binary search (the firmware's own tables can hold
 * tens of thousands of leaves outside the allowed set, its TEXT and MMIO among them).  Size it
 * for every entry of every table the walk may read outside the pool (gpu_uat_baseline_cap).
 */
struct gpu_uat_baseline {
    struct gpu_uat_outlier *e;
    u32 n, cap;
    bool overflow;
    bool sorted;
};

/* Entries the baseline needs to hold every outlier a walk of scope s can find outside the pool. */
u32 gpu_uat_baseline_cap(const struct gpu_uat_scope *s);

struct gpu_uat_report {
    u32 roots, tables;
    u32 leaves[GPU_UC_COUNT]; /* per class (outlier classes included) */
    u32 outliers;             /* entries outside the allowed set */
    u32 known;                /* of those, in the baseline (same entry, same descriptor) */
    u32 fresh;                /* outliers not in the baseline */
    u32 fresh_in_pool;        /* of those, in a guest-writable table (in_pool) */
    bool capped;              /* more tables than visited_cap: the walk is incomplete */
    u32 nfirst;
    struct gpu_uat_outlier first[GPU_UAT_REPORT_MAX]; /* the first fresh outliers */
};

const char *gpu_uat_class_name(u32 cls);

/*
 * Walk every valid root of the context table and the extra roots.  With known, outliers found in
 * it are counted as known, the rest as fresh.  With record and without known (the baseline),
 * every outlier of a firmware or shared-L2 table is stored there, counted as known, and the
 * record is sorted at the end (on overflow the rest is not stored, so a later audit counts it as
 * fresh); one in a pool table or behind a context-table root's own descriptor is fresh and never
 * stored.  With neither, every
 * outlier is fresh.  An incomplete walk (table cap) counts as one fresh outlier.  Returns
 * rep->fresh.
 */
u32 gpu_uat_audit(const struct gpu_uat_scope *s, const struct gpu_uat_ops *ops,
                  const struct gpu_uat_baseline *known, struct gpu_uat_baseline *record,
                  struct gpu_uat_report *rep);

#endif
