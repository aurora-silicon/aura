/* SPDX-License-Identifier: MIT */
#ifndef HV_WINDOWS_GUEST_DIAG_H
#define HV_WINDOWS_GUEST_DIAG_H

#include "../../smp.h"
#include "../../types.h"

/*
 * Per-vCPU exit counters and a snapshot of where the guest was at its last exit,
 * reported once a second from the host tick (hv_guest_diag_report(), next to the
 * "HV: fb:" line).
 *
 * Written only by the CPU that owns the slot, on its own EL2 entry; read by the
 * reporting CPU without a lock.  Plain increments are enough for that: each field
 * is a naturally aligned u64, so the reader sees an old or a new value, never a
 * torn one, and a count that is off by one in either direction does not matter
 * for a once-a-second rate.  The per-exit counting never blocks, takes a lock or
 * prints.  The report itself is not off the guest's path: hv_tick() calls it
 * (through hv_fb_report()) during a guest FIQ exit on the interruptible CPU, with
 * the big hypervisor lock held, so on a one-CPU guest each 1 Hz line costs that
 * CPU the time of its printf, the same budget as the "HV: fb:" line.
 */
struct hv_guest_diag {
    /* Exits from the guest, by class. */
    u64 exits;
    u64 sysreg;
    u64 dabort;
    u64 dabort_fb;
    u64 wfx;
    u64 hvc;
    u64 smc;
    u64 other_sync;
    u64 fiq;
    u64 fiq_guest_timer;
    u64 irq;
    u64 serror;
    /* vUART MMIO hook (also counted in dabort). */
    u64 vuart_rd;
    u64 vuart_wr;
    u64 vuart_tx;
    u64 vuart_txfull;
    /* Interrupt routing decisions. */
    u64 timer_reflect;
    u64 timer_unrouted_mask;
    u64 virq_exit;
    /* Snapshot at the last exit (hv_get_context()). */
    u64 elr;
    u64 spsr;
    u64 elr_el1;
    u64 esr_el1;
    u64 cntp_ctl;
    u64 cntp_cval;
    u64 cntv_ctl;
    u64 cntv_cval;
    u64 cntvoff;
    u64 cntpct;
    /* Emulated PMCCNTR_EL0 reads (trapped, HVC-redirected or probed) and the last value. */
    u64 pmc_reads;
    u64 pmc_last;
} __attribute__((aligned(64)));

extern struct hv_guest_diag hv_guest_diag[MAX_CPUS];

#define HV_GUEST_DIAG_INC(field) (hv_guest_diag[smp_id()].field++)

/* vUART TX-full edges, recorded by hv_vuart.c's update_irq() and printed by the report. */
struct hv_vuart_diag {
    u64 full_edges;
    u64 full_at;
    u64 recover_edges;
    u64 recover_at;
    bool full;
};

extern struct hv_vuart_diag hv_vuart_diag;

/*
 * PSCI calls by function number (function ID bits [4:0], SMC32 and SMC64 alike),
 * counted by hv_handle_psci_smc() with relaxed atomics, any CPU.  Slot 31 also
 * takes IDs outside the PSCI range (0x84000000 / 0xc4000000 + 0..0x1f).
 */
#define HV_PSCI_CALL_SLOTS 32
extern u32 hv_psci_calls[HV_PSCI_CALL_SLOTS];

void hv_guest_diag_report(void);

#endif
