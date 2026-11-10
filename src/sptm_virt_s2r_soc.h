/* SPDX-License-Identifier: MIT */

#ifndef SPTM_VIRT_S2R_SOC_H
#define SPTM_VIRT_S2R_SOC_H

#include "types.h"

#define S2R_LINK_NODES      6 /* USB controller, DART and PHY of up to two hidden ports */
#define S2R_LINK_PATH       40
#define S2R_TRACE_BANKS     64
#define S2R_TRACE_MAX_PAGES 512

struct s2r_trace_blk {
    u64 base, size;
    u64 declared_size; /* retained to reject inconsistent shared cluster apertures */
    const char *what;
};

struct s2r_blk {
    u64 base, size;
};

/* The ADT nodes whose PMGR domains keep a CDC debug link alive. */
struct s2r_link {
    int n_link;
    char paths[S2R_LINK_NODES][S2R_LINK_PATH];
};

/* The SoC blocks the sleep path watches, all taken from the ADT. */
struct s2r_soc {
    /* The sleep is the S2R timer's start alone: trace the blocks, no per-core or domain rules. */
    bool timer_only;
    u64 stop_timer; /* its 32-bit start register; 1 starts it */
    int n_trace;
    u32 trace_pages;
    struct s2r_trace_blk trace[S2R_TRACE_BANKS];
    struct s2r_blk soc_ps, cluster_ctl, pwrgate, cllt, nub_ap, aon, clkgen, soc_misc, aon_pst;
    u64 pms_pmc[2];
    u64 nub_wake_status;
    u64 amcc; /* 0 when the ADT names no AMCC */
    u32 amcc_planes;
    char pmu_path[64]; /* primary PMU node below an SPMI controller */
    u32 pmu_slave;
    int n_skipped;
    const char *skipped[10]; /* traced blocks the ADT has no entry for */
};

/*
 * Fill *out from the ADT. 0, or -1 with *missing naming the first sleep piece the ADT lacks.
 * timer_only: the PMGR has no per-core self-disable block, and stop_timer marks the sleep.
 */
int s2r_soc_from_adt(const void *adt, struct s2r_soc *out, const char **missing);

/*
 * Resolve the CDC controller, DART and PHY independently of the sleep blocks. The PHY phandles
 * are the ports the host hid from the guest. This remains usable when s2r_soc_from_adt() fails.
 */
int s2r_link_from_adt(const void *adt, const u32 *phys, int n_phys, struct s2r_link *out,
                      const char **missing);

#endif
