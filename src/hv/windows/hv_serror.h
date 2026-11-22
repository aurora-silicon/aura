/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HV_WINDOWS_SERROR_H
#define HV_WINDOWS_SERROR_H

/*
 * T8152 guest SError attribution (i0 session 2).
 *
 * An SError carries no address: ESR_EL2 for EC 0x2f has at most a class (and on these cores
 * ISS 0, none at all), and FAR_EL2/HPFAR_EL2 are not written.  Apple's own error status
 * registers are locked from EL2 on T8152.  So the only way to tie a guest SError to a stage-2
 * passthrough or identity region is circumstantial: which addresses the guest was working
 * with when it was taken.  hv_serror_attribute() lists
 *
 *  - the guest PC and the loads/stores in the eight instructions before it and the one at it,
 *    with the address each one names (base register plus offset, from the registers as they
 *    are now: approximate once a register was reused);
 *  - every general register and SP_EL0/SP_EL1 whose value, as a guest VA, translates to a
 *    non-RAM IPA, or which itself is the IPA of a passthrough, identity or denied region;
 *
 * each with what stage 2 and the T8152 policy say that IPA is.  A device access that reaches
 * hardware (the xHCI or AIC passthrough, a mapped identity exception) can raise an SError; an
 * emulated page or an unmapped IPA traps synchronously first, so it is not a candidate.
 *
 * The guest stage-1 walk is done in software from TTBR0/1_EL12 and TCR_EL12 (4, 16 or 64 KiB
 * granules), reading descriptors only from the guest RAM window (identity mapped, normal
 * memory).  No AT instruction is used: D3 run 1 took an EL2 SError at an AT on a guest
 * address.  Nothing outside guest RAM is ever read.
 */

#include "types.h"

/* What an IPA is, for the attribution (stage 2 now, plus the T8152 policy lists). */
enum hv_serror_ipa_kind {
    HV_SERR_IPA_RAM = 0,       /* guest RAM */
    HV_SERR_IPA_PASSTHROUGH,   /* hardware leaf on an MMIO allowlist range (AIC, xHCI) */
    HV_SERR_IPA_EXCEPTION,     /* hardware leaf on a carrier exception (fb shadow, ...) */
    HV_SERR_IPA_REGISTERED,    /* a registered identity exception with no stage-2 leaf */
    HV_SERR_IPA_EMULATED,      /* a carrier hook (AHCI, input, vUART, vGIC, GPU logging) */
    HV_SERR_IPA_UNMAPPED,      /* no leaf: a stage-2 abort, not an SError */
    HV_SERR_IPA_DENIED,        /* on the denylist (never mapped) */
    HV_SERR_IPA_OTHER_HW,      /* a hardware leaf no list explains (should not exist) */
    HV_SERR_IPA_KINDS
};

const char *hv_serror_kind_name(int kind);
/* Kinds whose access reaches real hardware or memory outside guest RAM: SError candidates. */
bool hv_serror_kind_candidate(int kind);

/*
 * The guest stage-1 regime and a reader for its descriptors.  The reader returns false for an
 * address it will not read (outside guest RAM); the walk then stops.
 */
struct hv_serror_s1 {
    u64 sctlr, tcr, ttbr0, ttbr1;
};
typedef bool(hv_serror_rd64_fn)(void *ctx, u64 ipa, u64 *out);

#define HV_SERR_S1_OK         0
#define HV_SERR_S1_OFF        1  /* stage 1 disabled: IPA = VA */
#define HV_SERR_S1_FAULT      -1 /* invalid descriptor, or walks disabled for that half */
#define HV_SERR_S1_RANGE      -2 /* VA outside both halves */
#define HV_SERR_S1_GRANULE    -3 /* reserved granule encoding */
#define HV_SERR_S1_UNREADABLE -4 /* a table outside what the reader reads */

int hv_serror_s1_walk(const struct hv_serror_s1 *s1, u64 va, hv_serror_rd64_fn *rd, void *ctx,
                      u64 *ipa);

/*
 * One A64 instruction, if it is in the loads-and-stores group: the address it names.
 * `executed`: the instruction already ran, so a writeback form has updated its base register
 * (pre-index: the base is the address; post-index: base - offset).  Returns false for any
 * other instruction.  `dir`: 1 store, 0 load, -1 not decoded.  `approx` is set when the
 * address depends on register values that may have changed since.
 */
struct hv_serror_mem {
    unsigned int rn;
    int dir;
    u64 addr;
    bool pc_relative;
};
bool hv_serror_decode_mem(u32 insn, u64 pc, const u64 *regs, u64 sp, bool executed,
                          struct hv_serror_mem *out);

/*
 * The report part, from hv_windows_guest_serror_report() (hv_fatal.c), after the records and
 * the registers.  `sp` = {SP_EL0, SP_EL1}.  T8152 only.
 */
void hv_serror_attribute(const u64 *regs, const u64 *sp, u64 esr, u64 elr, u64 spsr);

#endif
