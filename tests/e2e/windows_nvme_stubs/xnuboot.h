/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "types.h"
struct boot_args_stub {
    u64 phys_base, mem_size;
};
extern struct boot_args_stub cur_boot_args;
