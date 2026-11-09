/* SPDX-License-Identifier: MIT */
/* Host stand-in for src/utils.h, for tests/kboot_audio_host.c */
#ifndef UTILS_H
#define UTILS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "types.h"

#define ARRAY_SIZE(s) (sizeof(s) / sizeof((s)[0]))

/* MMIO stand-ins: tests/kboot_audio_host.c models the GPIO pins a scenario names */
u32 read32(u64 addr);
void write32(u64 addr, u32 data);
void udelay(u32 us);

#endif
