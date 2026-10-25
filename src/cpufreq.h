/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#ifndef CPUFREQ_H
#define CPUFREQ_H

#include "types.h"

int cpufreq_init(void);
void cpufreq_fixup(void);
void cpufreq_payload_boost(bool enable);

/* Explicit firmware requests; get reports the command state's ADT frequency. */
u64 cpufreq_get_cluster_hz(unsigned int cluster);
int cpufreq_set_cluster_pstate(unsigned int cluster, unsigned int pstate);

int cpufreq_t6040_init(void);
u64 cpufreq_t6040_get_hz(unsigned int cluster);
int cpufreq_t6040_set_pstate(unsigned int cluster, unsigned int pstate);

#endif
