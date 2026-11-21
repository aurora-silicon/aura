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

/* An ADT SMC function: pKW4(key, lo) writes four bytes of lo | level, pKW8(key, lo, hi) eight
 * bytes of (hi << 32) | lo | level. */
struct smc_adt_function {
    u32 key;
    u32 lo;
    u32 hi;
    bool wide;
};

/* Read the SMC function in property prop of the ADT node at path: 0 when it is one, 1 when the
 * node or property is missing or is not a pKW4/pKW8 (a GPIO, say). */
int smc_adt_function_get(const char *path, const char *prop, struct smc_adt_function *fn);
int smc_adt_function_write(smc_dev_t *smc, const struct smc_adt_function *fn, u32 level);

/*
 * Release the reset an ADT node's "function-reset" names through the SMC: pKW4(key, lo) is a
 * four-byte write of lo | level, pKW8(key, lo, hi) an eight-byte write of (hi << 32) | lo | level,
 * level 0 then 1, settle_ms after each. 1 when the node has no SMC function-reset, 0 once released,
 * negative on failure.
 */
int smc_adt_function_reset(const char *path, u32 settle_ms);
void smc_shutdown(smc_dev_t *smc);

#endif
