/* SPDX-License-Identifier: MIT */

#include "hv_status.h"
#include "hv_gpu.h"
#include "utils.h"

static u64 status_entry, status_next;

void hv_status_guest_entered(u64 now, u64 freq)
{
    status_entry = now ? now : 1;
    status_next = status_entry + HV_STATUS_PERIOD_S * freq;
}

bool hv_status_due(u64 now, u64 freq, u64 *guest_s)
{
    if (!status_entry || !freq || now < status_next)
        return false;
    *guest_s = (now - status_entry) / freq;
    /* One block per minute, never a burst after a long stall: skip the minutes that were missed. */
    status_next += HV_STATUS_PERIOD_S * freq;
    if (status_next <= now)
        status_next = now + HV_STATUS_PERIOD_S * freq;
    return true;
}

void hv_status_print(const struct hv_status_snapshot *s)
{
    char exits[HV_STATUS_CPUS * 16 + 1];
    size_t at = 0;
    exits[0] = 0;
    for (int cpu = 0; cpu < HV_STATUS_CPUS && at < sizeof(exits); cpu++)
        if (s->exits[cpu] != ~0UL)
            at += snprintf(exits + at, sizeof(exits) - at, " c%d=%lu", cpu, s->exits[cpu]);
    printf("HV: status t=%lu soc=%lu tick cpu %d boot cpu %d\n", s->guest_s, s->soc_s, s->tick_cpu,
           s->boot_cpu);
    printf("HV: status exits/min:%s\n", exits[0] ? exits : " none");
    printf("HV: status ahci %lu input %u/%u; console dropped %lu stalls %lu longest %lu us now %lu "
           "us write max %lu us; vuart dropped %lu\n",
           s->ahci_commands, s->input_ok, s->input_bad, s->con_dropped, s->con_stalls,
           s->con_longest_stall_us, s->con_stalled_now_us, s->con_longest_call_us,
           s->vuart_overruns);
    hv_gpu_sum(s->guest_s, s->con_dropped);
}
