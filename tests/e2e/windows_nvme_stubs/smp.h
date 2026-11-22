/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "types.h"
#define MAX_CPUS 16
extern int stub_cpu;
static inline int smp_id(void)
{
    return stub_cpu;
}
