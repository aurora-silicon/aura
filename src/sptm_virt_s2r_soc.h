/* SPDX-License-Identifier: MIT */

#ifndef SPTM_VIRT_S2R_SOC_H
#define SPTM_VIRT_S2R_SOC_H

#include "types.h"

#define S2R_LINK_NODES 6 /* USB controller, DART and PHY of up to two hidden ports */

struct s2r_blk {
    u64 base, size;
};

/* The SoC blocks the sleep path watches, all taken from the ADT. */
struct s2r_soc {
    struct s2r_blk soc_ps, cluster_ctl, pwrgate, cllt, nub_ap, aon, clkgen, soc_misc, aon_pst;
    u64 pms_pmc[2];
    u64 nub_wake_status;
    u64 amcc; /* 0 when the ADT names no AMCC */
    u32 amcc_planes;
    int n_link;
    char link_paths[S2R_LINK_NODES][40];
};

/*
 * Fill *out from the ADT. The debug link is the USB port(s) of the PHYs whose phandles the host
 * hid from the guest. 0, or -1 with *missing naming the first piece the ADT lacks.
 */
int s2r_soc_from_adt(const void *adt, const u32 *link_phys, int n_phys, struct s2r_soc *out,
                     const char **missing);

#endif
