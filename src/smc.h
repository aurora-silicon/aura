/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#ifndef SMC_H
#define SMC_H

#include "asc.h"
#include "rtkit.h"
#include "types.h"

typedef struct smc_dev smc_dev_t;

int smc_write_u32(smc_dev_t *smc, u32 key, u32 value);
int smc_write_u64(smc_dev_t *smc, u32 key, u64 value);

smc_dev_t *smc_init(void);

/*
 * Release the reset an ADT node's "function-reset" names through the SMC: pKW4(key, lo) is a
 * four-byte write of lo | level, pKW8(key, lo, hi) an eight-byte write of (hi << 32) | lo | level,
 * level 0 then 1, settle_ms after each. 1 when the node has no SMC function-reset, 0 once released,
 * negative on failure.
 */
int smc_adt_function_reset(const char *path, u32 settle_ms);
void smc_shutdown(smc_dev_t *smc);

#endif
