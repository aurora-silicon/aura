/* SPDX-License-Identifier: MIT */

/*
 * G1: the GPU PSD power-on recipe (gpu_t8152_power.h).  Its values are not kept in this
 * repository: the Makefile copies them from J804_PRIVATE_DATA/aura/g18g-j804-gpu-power.h into
 * build/j804_gpu_power.h, or writes a header that leaves power-on disabled when that file is
 * missing (--windows-gpu probe is then refused by the carrier, nothing else changes).
 */

#include "gpu_t8152_power.h"

#include "../build/j804_gpu_power.h"

static const struct gpu_psd_recipe recipe = J804_GPU_PSD_RECIPE;

const struct gpu_psd_recipe *gpu_t8152_psd_recipe(void)
{
    return &recipe;
}
