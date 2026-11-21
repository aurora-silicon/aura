/* SPDX-License-Identifier: MIT */
/*
 * Host-test stand-in for src/utils.h. The test copies the production ATC file
 * next to this header. Every register access aborts: the handoff edits only DT.
 */
#ifndef UTILS_H
#define UTILS_H

#include <stdio.h>
#include <stdlib.h>

#include "soc.h"
#include "types.h"

#define printf(...) debug_printf(__VA_ARGS__)

#define ARRAY_SIZE(s) (sizeof(s) / sizeof((s)[0]))

#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) > (b)) ? (a) : (b))

void host_mmio(u64 addr);

static inline u32 read32(u64 addr)
{
    host_mmio(addr);
    return 0;
}

static inline u64 read64(u64 addr)
{
    host_mmio(addr);
    return 0;
}

static inline void write32(u64 addr, u32 v)
{
    (void)v;
    host_mmio(addr);
}

extern u32 board_id, chip_id;
extern bool host_fail_malloc;
void *host_malloc(size_t size);
#define malloc host_malloc

int debug_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
