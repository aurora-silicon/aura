/* SPDX-License-Identifier: MIT */
#ifndef WINDOWS_COMPAT_H
#define WINDOWS_COMPAT_H
#include "types.h"
int smp_cpu_count(void);
int smp_get_id(u64 mpidr);
void aic_set_mask(int irq, bool active);
void aic_set_affinity(int irq, int cpu);
void aic_set_enabled(bool enabled);
bool aic_is_enabled(void);
void cpu_wfi_stateless(void);
#define MPIDR_AFF0(mpidr)       ((mpidr) & 0xff)
#define MPIDR_AFF1(mpidr)       (((mpidr) >> 8) & 0xff)
#define MPIDR_AFF2(mpidr)       (((mpidr) >> 16) & 0xff)
#define MPIDR_AFF3(mpidr)       (((mpidr) >> 32) & 0xff)
#define AIC23_GLOBAL_CFG        0x0014
#define AIC23_GLOBAL_CFG_ENABLE 1
#define ADT_EL2_36_BIT          0xf30000000UL
#define ADT_EL2_42_BIT          0x5300000000UL
#endif
