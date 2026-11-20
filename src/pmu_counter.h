/* SPDX-License-Identifier: MIT */

#ifndef PMU_COUNTER_H
#define PMU_COUNTER_H

#include "types.h"

int pmu_clear_panic_counter(void);
/* The primary PMU's SPMI bus node, its SPMI address and the failed-boot count's register. */
int pmu_counter_locate(char *path, size_t path_len, u8 *addr, u16 *reg);
int pmu_read_boot_record(u8 *counts, u8 *error_stage);

#endif
