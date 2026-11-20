/* SPDX-License-Identifier: MIT */

#ifndef PMU_COUNTER_H
#define PMU_COUNTER_H

#include "types.h"

int pmu_clear_panic_counter(void);
int pmu_read_boot_record(u8 *counts, u8 *error_stage);

/* The failed-boot count is the low nibble of iBoot's counts byte, the panic count the high one. */
#define PMU_FAILED_BOOTS(counts) ((counts) & 0xf)

/* The chain's failed-boot count, handed from stage to stage as aura.failed-boots= */
void pmu_snapshot_failed_boots(void);
void pmu_take_failed_boots_var(const char *val);
bool pmu_failed_boots_passed(u8 *count);
const char *pmu_failed_boots_var(void);

#endif
