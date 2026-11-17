/* SPDX-License-Identifier: MIT */

#ifndef HV_T8152_H
#define HV_T8152_H

#include "types.h"

/*
 * T8152 (J804, MacBook Pro M6) safety policy for the Windows carrier.
 *
 * On T8152 the carrier refuses to start a guest unless every precondition in
 * hv_t8152_preflight() and hv_t8152_verify() holds, and it says which one did
 * not.  Stage 2 is an allowlist there: the guest gets exactly
 *
 *   - its RAM window (hv_t8152_set_guest_ram), identity mapped, normal memory;
 *   - pages the carrier itself registers with hv_t8152_allow() (the BGRA
 *     framebuffer shadow; no ADT alias is mapped on T8152);
 *   - the AIC, identity mapped as Device-nGnRnE (native AIC passthrough);
 *   - emulated MMIO hooks in the carrier (vUART, vGIC, AIC transition hooks),
 *     never mapped.  Host (proxy) hooks are refused: the host would perform
 *     the access at PA = IPA.
 *
 * A denylist read from the live ADT (QSPI NOR/NVRAM controller, SEP, AOP,
 * ADMAC, i2c, audio, GPIO, WDT, PMGR, SPMI, DART, SART, ANS, SMC, MTP, SPI,
 * DCP, SIO, the proxy's usb-drd0) plus the MTE tag storage and the carve-out
 * above RAM is checked as well, at map time and again before entry, so that a
 * later allowlist edit cannot expose any of it.
 */

/* True once hv_t8152_preflight() has passed on this boot (T8152 only). */
bool hv_t8152_policy_armed(void);

/* hv_init(): every T8152 precondition; prints each failing reason. 0 or -1. */
int hv_t8152_preflight(void);

/*
 * Proxy: the guest RAM window [base, base + size).  host_top is the top of the
 * launcher's own heap, which sits above aura's heap limit; the window must
 * miss [_base, max(heap high water, heap limit, host_top)).  0, or -1 with the
 * reason printed.
 */
int hv_t8152_set_guest_ram(u64 base, u64 size, u64 host_top);
bool hv_t8152_guest_ram(u64 *base, u64 *size);

/* A carrier-owned exception to the RAM allowlist (never to the denylist). */
int hv_t8152_allow(u64 ipa, u64 pa, u64 size, bool read_only, const char *name);

/*
 * hv_map(): may this mapping be made?  Forces Device-nGnRnE on hardware
 * mappings outside DRAM.  Unmapping is always allowed.
 */
bool hv_t8152_map_filter(u64 from, u64 *to, u64 size);

/* hv_write_hcr(): keeps HCR_EL2.TWI set and HCR_EL2.FWB clear while armed. */
u64 hv_t8152_hcr_filter(u64 hcr);

/* hv_start(): walk the whole stage-2 table and the entry state. 0 or -1. */
int hv_t8152_verify(u64 entry, u64 bootargs, u64 secondary_hcr, bool fb_shadow);

#endif
