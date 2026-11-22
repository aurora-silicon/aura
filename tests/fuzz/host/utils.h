/* SPDX-License-Identifier: MIT */
/*
 * Host stand-in for src/utils.h, for the fuzz targets of tests/fuzz/: aura's log lines are
 * formatted (so their arguments are read) and dropped, system registers come from the target
 * (host_sysreg) and so does MMIO (read32() and friends).
 */
#ifndef UTILS_H
#define UTILS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arm_cpu_regs.h"
#include "soc.h"
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

#define dprintf(...)                                                                               \
    do {                                                                                           \
    } while (0)
#define sysop(op)                                                                                  \
    do {                                                                                           \
    } while (0)
#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))

extern u32 board_id, chip_id;

u32 read32(u64 addr);
void write32(u64 addr, u32 data);
u64 read64(u64 addr);
u8 read8(u64 addr);
void udelay(u32 us);

static inline u32 mask32(u64 addr, u32 clear, u32 set)
{
    u32 val = (read32(addr) & ~clear) | set;
    write32(addr, val);
    return val;
}

static inline u32 set32(u64 addr, u32 set)
{
    return mask32(addr, 0, set);
}

static inline u32 clear32(u64 addr, u32 clear)
{
    return mask32(addr, clear, 0);
}

/* As src/utils.h: at most `timeout` reads, a microsecond apart */
static inline int poll32(u64 addr, u32 mask, u32 target, u32 timeout)
{
    while (--timeout > 0) {
        if ((read32(addr) & mask) == target)
            return 0;
        udelay(1);
    }
    return -1;
}

#endif
