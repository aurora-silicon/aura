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
 * DCP, SIO, every usb-drd and atc-phy node; D4 lifts only a usb-drd1/3 xHCI
 * window, and adds its USB DART tables) plus the MTE tag storage and the carve-out
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

/*
 * D1: the AHCI disk store [start, end), directly above the guest RAM window.  It must be
 * inside boot_args RAM, clear of every carve-out, CTXR range, aura, its heaps, the live ADT
 * and the scanout (the guest RAM rules), and it joins the denylist so that no stage-2 map can
 * reach it (map filter and hv_t8152_verify).  Once per session.  0, or -1 with the reason.
 */
int hv_t8152_reserve_store(u64 start, u64 end);

/*
 * D4: the usb-drd<port> (1 or 3) xHCI window [xhci, xhci + 0x8000) as an identity Device-nGnRnE
 * passthrough.  Only the window build_denylist() split off that port's ADT reg 0; the rest of
 * the controller, atc-phy*, dart-usb* and the TLBI aperture stay denied.  0, or -1 with the
 * reason.  hv_t8152_usb_revoke() denies it again (after the caller unmapped it).
 */
int hv_t8152_usb_allow(int port, u64 xhci);
void hv_t8152_usb_revoke(int port);

/*
 * D4: carrier memory (the USB DART tables) that must stay out of every stage-2 map: inside
 * aura's heap span, clear of guest RAM and the store; denied for the rest of the session.
 */
int hv_t8152_reserve_carrier(u64 start, u64 end, const char *name);

/*
 * G0 (hv_gpu.c): one admitted GPU firmware region as an identity exception outside boot_args
 * RAM (inside DRAM [dram_lo, dram_hi), clear of CTXR/CTRR, the denylist and guest RAM), and the
 * firmware __TEXT segments added to the denylist.  Registered only: neither maps anything.
 */
int hv_t8152_gpu_fw_allow(u64 pa, u64 size, bool read_only, u64 dram_lo, u64 dram_hi,
                          const char *name);
int hv_t8152_gpu_deny(u64 start, u64 end, const char *name);
unsigned int hv_t8152_allow_mark(void);
void hv_t8152_allow_rollback(unsigned int mark, const char *why);

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

/*
 * The guest's ACTLR_EL1, as EL2 saves and restores it (hv_start, hv_init_secondary, the
 * ACTLR_EL1 trap).  Only the architectural ACTLR_EL12 (S3_5_C1_C0_1; with HCR_EL2.E2H = 1
 * it names the EL1 register, Arm ARM) is used: on T8152 a write to the implementation-
 * defined alias is UNDEFINED at EL2 on every core type, so that alias is never accessed,
 * not even read.  The write is skipped when the register already holds the value, since a
 * write of ACTLR_EL12 itself has not been measured on T8152 yet.
 */
u64 hv_t8152_guest_actlr_read(void);
void hv_t8152_guest_actlr_write(u64 value);

#endif
