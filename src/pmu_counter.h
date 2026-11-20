/* SPDX-License-Identifier: MIT */

#ifndef PMU_COUNTER_H
#define PMU_COUNTER_H

#include "types.h"

int pmu_clear_panic_counter(void);
int pmu_read_boot_record(u8 *counts, u8 *error_stage);
bool pmu_counter_cleared_by_aura(void);
int pmu_count_failed_boot(u8 *before);

/* The failed-boot count is the low nibble of the counts byte, the panic count the high one. */
#define PMU_FAILED_BOOTS(counts) ((counts) & 0xf)
static inline u8 pmu_counts_add_failed_boot(u8 counts)
{
    return PMU_FAILED_BOOTS(counts) == 0xf ? counts : counts + 1;
}

#endif
