/* SPDX-License-Identifier: GPL-2.0-only */

/*
 * G2: the private recipe (gpu_t8152_g2.h).  Its values are not kept in this repository: the
 * Makefile copies them from J804_PRIVATE_DATA/gpu/carrier/g18g-j804-gpu-g2.h into
 * build/j804_gpu_g2.h, or writes a header that leaves G2 disabled when that file is missing
 * (--windows-gpu g2 is then refused by the carrier, nothing else changes).
 */

#include "gpu_t8152_g2.h"

#include "../build/j804_gpu_g2.h"

static const struct gpu_g2_recipe recipe = J804_GPU_G2_RECIPE;

const struct gpu_g2_recipe *gpu_t8152_g2_recipe(void)
{
    return &recipe;
}
