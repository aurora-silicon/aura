/* SPDX-License-Identifier: MIT */

#ifndef MCC_H
#define MCC_H

#include "types.h"

struct mcc_carveout {
    u64 base;
    u64 size;
};

extern size_t mcc_carveout_count;
extern struct mcc_carveout mcc_carveouts[];

/* mcc_init() result for an MCC revision with no driver here, or one whose ADT lacks the counts
 * its driver needs (not an error). */
#define MCC_UNSUPPORTED 1
/* mcc_init() result when the ADT has no /arm-io/mcc (A7-A11, T2; not an error). */
#define MCC_ABSENT 2

int mcc_init(void);
int mcc_unmap_carveouts(void);
int mcc_enable_cache(void);
int mcc_plane_info(int mcc, u64 *base, u64 *stride, int *count);

#endif
