/* SPDX-License-Identifier: MIT */
/* What src/sptm_virt_gfxrec.c needs from aura, for tests/gfxrec_host/gfxrec_host.c. */
#ifndef GFXREC_HOST_H
#define GFXREC_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#include "sptm_virt_gfxrec.h"

#endif
