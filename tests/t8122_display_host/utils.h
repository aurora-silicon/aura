/* SPDX-License-Identifier: MIT */
/* Host stand-in for src/utils.h: the helpers the display handoff uses; MMIO goes to the fake. */
#ifndef UTILS_H
#define UTILS_H
#include "soc.h"
#include "types.h"
#include <stdio.h>
/* The host build prints debug-only lines too, as a DEBUG build does. */
#undef dprintf
#define dprintf(...) printf(__VA_ARGS__)
#define ARRAY_SIZE(s)        (sizeof(s) / sizeof((s)[0]))
#define ALIGN_UP(x, a)       (((x) + ((a)-1)) & ~((a)-1))
#define max(a, b)            (((a) > (b)) ? (a) : (b))
#define BIT(x)               (1UL << (x))
#define GENMASK(msb, lsb)    ((~0UL << (lsb)) & (~0UL >> (63 - (msb))))
#define FIELD_GET(mask, val) (((val) & (mask)) >> __builtin_ctzll(mask))
#define FIELD_PREP(mask, val) (((u64)(val) << __builtin_ctzll(mask)) & (mask))
#define SZ_16K               (16 * 1024)
#define SZ_1M                (1024 * 1024)
#define sysop(x)                                                                                   \
    do {                                                                                           \
    } while (0)
u32 read32(u64 addr);
u64 read64(u64 addr);
void write32(u64 addr, u32 val);
extern u32 chip_id;
#endif
