/* SPDX-License-Identifier: MIT */
/* The boot-args prefix referenced by the unmodelled T6050 display path. */
#ifndef XNUBOOT_H
#define XNUBOOT_H
#include "types.h"

struct boot_video {
    u64 base;
    u64 display;
    u64 stride;
    u64 width;
    u64 height;
    u64 depth;
};

struct boot_args {
    u16 revision;
    u16 version;
    u64 virt_base;
    u64 phys_base;
    u64 mem_size;
    u64 top_of_kernel_data;
    struct boot_video video;
};

extern struct boot_args cur_boot_args;
#endif
