/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#ifndef __CHAINLOAD_H__
#define __CHAINLOAD_H__

/*
 * A raw image starts with its exception vectors; _start follows at 0x800 and branches over this
 * load header (start.S). run_base is zero in every image file. A loader that has to stage the
 * image where its code may not run sets run_base in its staged copy; the image then copies
 * [base, base + footprint) to run_base before it uses its stack or .bss and continues there,
 * leaving its payload where it was staged.
 */
#define CHAINLOAD_ENTRY_OFFSET      0x800
#define CHAINLOAD_HDR_OFFSET        0x808
#define CHAINLOAD_HDR_MAGIC         0x44414f4c41525541 /* "AURALOAD" */
#define CHAINLOAD_HDR_VERSION       1
#define CHAINLOAD_HDR_F_RUN_BASE    1
#define CHAINLOAD_HDR_F_LIVE_DCP    2 /* consumes the live DCP marker in the ADT */
#define CHAINLOAD_HDR_FOOTPRINT_OFF 0x10
#define CHAINLOAD_HDR_RUN_BASE_OFF  0x18
#define CHAINLOAD_HDR_SIZE          0x20

#ifndef __ASSEMBLER__

#include "types.h"

struct chainload_header {
    u64 magic;
    u32 version;
    u32 flags;
    u64 footprint;
    u64 run_base;
};

int chainload_image(void *base, size_t size, char **vars, size_t var_cnt);
int chainload_load(const char *spec, char **vars, size_t *var_cnt, size_t var_capacity);

void stage1_log_flush(void);
#endif

#endif
