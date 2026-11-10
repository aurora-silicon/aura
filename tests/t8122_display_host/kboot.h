/* SPDX-License-Identifier: MIT */
/* Host stand-in for src/kboot.h: the boot-args values the display handoff reads. */
#ifndef KBOOT_H
#define KBOOT_H
#include "types.h"
u64 kboot_ram_base(void);
u64 kboot_ram_size(void);
u64 kboot_fb_base(void);
void kboot_loaded_file_part(u64 *base, u64 *size);
int kboot_fdt_path_exact(const void *fdt, const char *path);
#endif
