/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#ifndef DAPF_H
#define DAPF_H

#include "types.h"

int dapf_init_all(void);
int dapf_init_fdt(const void *fdt);
int dapf_init(const char *path, int index);
bool dapf_j614s_aop_ready(void);
bool dapf_j614s_isp_ready(void);

/* A DAPF window, merged to whole 16K pages: [start, end). */
struct dapf_window {
    u64 start;
    u64 end;
};

#define DAPF_T8110_MAX_SLICES 16

int dapf_init_t8132_halted(const char *path, int index);
int dapf_t8132_windows(const char *path, int index, struct dapf_window *out, int max);
int dapf_t8132_ranges(const char *path, u32 r0, u32 r4, u32 r20, u64 *ranges, int max);
int dapf_t8110_ranges(const char *path, u32 instance, struct dapf_window *out, int max);

#endif
