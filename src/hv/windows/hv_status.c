/* SPDX-License-Identifier: MIT */

#include "hv_status.h"
#include "hv_gpu.h"

static u64 status_entry, status_next;

void hv_status_guest_entered(u64 now, u64 freq)
{
    status_entry = now ? now : 1;
    status_next = status_entry + HV_STATUS_PERIOD_S * freq;
}

bool hv_status_tick(u64 now, u64 freq)
{
    if (!status_entry || !freq || now < status_next)
        return false;
    u64 guest_s = (now - status_entry) / freq;
    /* One line per minute, never a burst after a long stall: skip the minutes that were missed. */
    status_next += HV_STATUS_PERIOD_S * freq;
    if (status_next <= now)
        status_next = now + HV_STATUS_PERIOD_S * freq;
    hv_gpu_sum(guest_s, 0);
    return true;
}
