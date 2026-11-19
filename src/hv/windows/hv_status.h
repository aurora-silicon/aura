/* SPDX-License-Identifier: MIT */
#ifndef HV_WINDOWS_STATUS_H
#define HV_WINDOWS_STATUS_H

#include "types.h"

/*
 * G5b: the carrier's once-a-minute status from the host tick (hv_tick(), which only the tick
 * owner runs: the boot CPU, or --windows-tick-cpu's CPU while it is in the guest).  The pacing is
 * guest-run time from entry, under bhl like the rest of hv_tick(), so a tick handover (tick 6 or
 * 7) keeps one cadence.
 */
#define HV_STATUS_PERIOD_S 60

/* At guest entry: the clock the minutes count from. */
void hv_status_guest_entered(u64 now, u64 freq);
/* From hv_tick(): prints the status when a minute of guest run has passed; true if it did. */
bool hv_status_tick(u64 now, u64 freq);

#endif
