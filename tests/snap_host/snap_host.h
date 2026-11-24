/* SPDX-License-Identifier: MIT */
/* What src/sptm_virt_snap.c needs from aura, for tests/snap_host/snap_host.c. */
#ifndef SNAP_HOST_H
#define SNAP_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#define BIT(n)           (1UL << (n))
#define ARRAY_SIZE(a)    (sizeof(a) / sizeof((a)[0]))
#define ALIGN_UP(x, a)   (((x) + (a) - 1) & ~((u64)(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((u64)(a) - 1))
#define min(a, b)        (((a) < (b)) ? (a) : (b))

#include "sptm_virt_snap.h"

#endif
