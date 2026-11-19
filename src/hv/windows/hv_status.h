/* SPDX-License-Identifier: MIT */
#ifndef HV_WINDOWS_STATUS_H
#define HV_WINDOWS_STATUS_H

#include "types.h"

/*
 * G5b: the carrier's once-a-minute status block from the host tick (hv_tick(), which only the
 * tick owner runs: the boot CPU, or --windows-tick-cpu's CPU while it is in the guest).  The
 * pacing is guest-run time from entry, under bhl like the rest of hv_tick(), so a tick handover
 * (tick 6 or 7) keeps one cadence.  The block (G5B-BUILD 3.2):
 *
 *   HV: status t=<guest s> soc=<SoC s> tick cpu <t> boot cpu <b>
 *   HV: status exits/min: c0=<n> c1=<n> ...            (live CPUs only)
 *   HV: status ahci <commands> input <ok>/<bad>; console dropped <bytes> stalls <n> longest
 *       <us> us now <us> us write max <us> us; vuart dropped <bytes>
 *   HV: GPU SUM ...                                     (only while a G1/G2 session forwards)
 *
 * Every line is one printf: one console write each.  The lower-case "HV: status" and the
 * "console dropped" / "vuart dropped" words are what g5watch's condrop rule reads.
 */
#define HV_STATUS_PERIOD_S 60
#define HV_STATUS_CPUS     12

struct hv_status_snapshot {
    u64 guest_s, soc_s;
    int tick_cpu, boot_cpu;
    u64 exits[HV_STATUS_CPUS]; /* in the last minute; ~0 for a CPU that is not live */
    u64 ahci_commands;
    u32 input_ok, input_bad;
    u64 con_dropped, con_stalls, con_longest_stall_us, con_stalled_now_us, con_longest_call_us;
    u64 vuart_overruns;
};

/* At guest entry: the clock the minutes count from. */
void hv_status_guest_entered(u64 now, u64 freq);
/* From hv_tick(): true (and the guest seconds) when a minute of guest run has passed. */
bool hv_status_due(u64 now, u64 freq, u64 *guest_s);
/* Prints the block (and GPU SUM). */
void hv_status_print(const struct hv_status_snapshot *s);

#endif
