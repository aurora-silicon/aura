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
 * for a once-a-second rate.  Nothing here blocks or takes a lock, so the guest
 * can never be held up by the instrumentation.
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
    u64 cntpct;
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

void hv_guest_diag_report(void);

#endif
