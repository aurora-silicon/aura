/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#ifndef __SMP_H__
#define __SMP_H__

#include "../build/build_cfg.h"
#include "smp_asm.h"
#include "types.h"
#include "utils.h"

#ifdef SMP_HEAP_STACKS
/* Allocated by smp_init() (smp_asm.h) */
extern u8 *secondary_stacks[MAX_CPUS];
extern u8 *secondary_stacks_el3[MAX_EL3_CPUS];
#else
extern u8 secondary_stacks[MAX_CPUS][SECONDARY_STACK_SIZE];
extern u8 secondary_stacks_el3[MAX_EL3_CPUS][SECONDARY_STACK_SIZE];
#endif

void smp_secondary_entry(void);
int smp_secondary_prepare(void);
void smp_secondary_prep_el3(void);

int smp_init(void);
int smp_start_secondaries(void);
int smp_stop_secondaries(bool deep_sleep);

#define smp_call0(i, f)          smp_call4(i, f, 0, 0, 0, 0)
#define smp_call1(i, f, a)       smp_call4(i, f, a, 0, 0, 0)
#define smp_call2(i, f, a, b)    smp_call4(i, f, a, b, 0, 0)
#define smp_call3(i, f, a, b, c) smp_call4(i, f, a, b, c, 0)

int smp_call4(int cpu, void *func, u64 arg0, u64 arg1, u64 arg2, u64 arg3);

#define SMP_TIMEOUT -2

int smp_wait_timed(int cpu, u64 *retval, u32 timeout_ms);
/* A NULL result discards the callback value, never the completion status. */
int smp_wait(int cpu, u64 *retval);
/* Join even a timed-out or busy dispatch; never treat an ACK timeout as cancellation. */
int smp_call_sync0(int cpu, void *func);
bool smp_has_pending_calls(void);

bool smp_is_alive(int cpu);
uint64_t smp_get_mpidr(int cpu);
u64 smp_get_release_addr(int cpu);
void smp_set_wfe_mode(bool new_mode);
void smp_send_ipi(int cpu);

static inline int smp_id(void)
{
    if (in_el3())
        return mrs(TPIDR_EL3);
    else if (in_el2())
        return mrs(TPIDR_EL2);
    else
        return mrs(TPIDR_EL1);
}

extern int boot_cpu_idx;
#endif
