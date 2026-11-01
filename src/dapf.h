/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#ifndef DAPF_H
#define DAPF_H

#include "types.h"

int dapf_init_all(void);
int dapf_init_fdt(const void *fdt);
int dapf_init(const char *path, int index);

/* A DAPF window, merged to whole 16K pages: [start, end). */
struct dapf_window {
    u64 start;
    u64 end;
};

int dapf_init_t8132_halted(const char *path, int index);
int dapf_t8132_windows(const char *path, int index, struct dapf_window *out, int max);

#endif
