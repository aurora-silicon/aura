/* SPDX-License-Identifier: MIT */
/* What src/sptm_virt_trace.c needs from aura, for tests/trace_host/trace_host.c. */
#ifndef TRACE_HOST_H
#define TRACE_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

struct exc_info {
    u64 elr;
};

#include "sptm_virt_trace.h"

/* The harness's clock, core and world (a GL1 trap or not), per thread. */
u64 host_now(void);
u32 host_cpu(void);
bool host_gl(void);
#define trace_now() host_now()
#define trace_cpu() host_cpu()
#define trace_gl()  host_gl()

/* Its device: every access the hooks make to it. */
void sptm_virt_mmio_rw(u64 addr, u64 *val, bool write, int width);
void trace_pair(u64 addr, u64 *val, bool write);

#endif
