/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "types.h"
bool hv_t8152_guest_ram(u64 *base, u64 *size);
int hv_t8152_reserve_store(u64 start, u64 end);
int hv_t8152_reserve_carrier(u64 start, u64 end, const char *name);
