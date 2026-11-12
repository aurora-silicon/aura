/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#ifndef HEAPBLOCK_H
#define HEAPBLOCK_H

#include "types.h"

void heapblock_init(void);

void *heapblock_alloc(size_t size);
void *heapblock_alloc_aligned(size_t size, size_t align);
void heapblock_set_limit(void *limit);
uintptr_t heapblock_high_water(void);
bool heapblock_raise_base(uintptr_t base);

#endif
