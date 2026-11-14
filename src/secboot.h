/* SPDX-License-Identifier: MIT */

#ifndef SECBOOT_H
#define SECBOOT_H

#include "../build/build_cfg.h"

#include "types.h"

/* Verdicts, as aura_secboot_verify() returns them */
enum secboot_verdict {
    SECBOOT_VERIFIED = 0,
    SECBOOT_BAD_FORMAT = 1,
    SECBOOT_UNKNOWN_KEY = 2,
    SECBOOT_BAD_SIGNATURE = 3,
    SECBOOT_ROLLBACK = 4,
    SECBOOT_UNSIGNED = 5,
    SECBOOT_MEASURED = 6, /* hashed only: covered by a signature checked elsewhere, or none */
};

/* Record a measurement (SHA-256 of data) for the hand-off record this stage builds. */
void secboot_measure(const char *component, const void *data, size_t size,
                     enum secboot_verdict verdict);

/*
 * Check an aura_sig record at the start of this stage's payloads. Returns 0 to go on (always,
 * unless the build enforces and the payloads are not verified) and sets *next past the record.
 */
int secboot_check_payloads(u8 *p, u8 **next);

/*
 * Where the payload scan goes on from p (p itself, or past refused lines), or NULL to stop. After
 * the bytes a verified record covers, an enforcing stage reads only the lines an earlier aura
 * stage appends (/chosen strings, NVMe hand-over) and skips other text lines.
 */
u8 *secboot_payload_next(u8 *p);

/* Whether a payload kind this stage cannot verify (ESP boot= files) may still run. */
bool secboot_allow_unverified(const char *what);

/*
 * Check the trailer signature of an image this stage chainloads. Returns 0 to go on, with *size
 * cut to the covered bytes when a trailer is present. Adds the measurement var for the next stage.
 */
int secboot_check_chainload(void *image, size_t *size, char **vars, size_t *var_cnt,
                            size_t var_capacity);

/*
 * Whether this stage refuses what it cannot verify: an enforce build, or a Stage 1 whose filled
 * key block asks for enforce (secboot_sign.py fill-keys --enforce).
 */
bool secboot_enforcing(void);

/*
 * Whether a USB host may get the proxy (any window, or the no-payload / failed hand-off
 * fallbacks): never in the secure flavour, and not while enforcing. The secure flavour answers
 * at compile time, so the windows, the proxy and what only the proxy reaches (the hypervisor)
 * are left out of its image.
 */
#ifdef SECBOOT_SECURE_FLAVOUR
static inline bool secboot_host_proxy_allowed(void)
{
    return false;
}
#else
bool secboot_host_proxy_allowed(void);
#endif

/* "dev" or "secure" (SECBOOT_FLAVOUR). */
const char *secboot_flavour(void);

/* Build /chosen/aurora,boot-security in the FDT handed to the next stage. */
void secboot_export_fdt(void *dt);

#endif
