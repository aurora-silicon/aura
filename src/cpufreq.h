/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#ifndef CPUFREQ_H
#define CPUFREQ_H

#include "types.h"

int cpufreq_init(void);
void cpufreq_fixup(void);
void cpufreq_payload_boost(bool enable);

/*
 * Explicit requests on a cluster with an ADT ladder (index into the chip's cluster table: E first);
 * get reports the command state's ADT frequency.
 */
u64 cpufreq_get_cluster_hz(unsigned int cluster);
int cpufreq_set_cluster_pstate(unsigned int cluster, unsigned int pstate);

#endif
