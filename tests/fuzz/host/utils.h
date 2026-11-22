/* SPDX-License-Identifier: MIT */
/*
 * Host stand-in for src/utils.h, for the fuzz targets of tests/fuzz/: aura's log lines are
 * formatted (so their arguments are read) and dropped, system registers come from the target
 * (host_sysreg) and MMIO reads as zero.
 */
#ifndef UTILS_H
#define UTILS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arm_cpu_regs.h"
#include "types.h"

#define ARRAY_SIZE(s) (sizeof(s) / sizeof((s)[0]))
#define min(a, b)     (((a) < (b)) ? (a) : (b))
#define max(a, b)     (((a) > (b)) ? (a) : (b))

int fuzz_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define printf(...) fuzz_log(__VA_ARGS__)

u64 host_sysreg(const char *name);
#define mrs(reg) host_sysreg(#reg)

static inline int in_el2(void)
{
    return 1;
}

u32 read32(u64 addr);
void write32(u64 addr, u32 data);
void udelay(u32 us);

#endif
