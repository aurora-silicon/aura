/* SPDX-License-Identifier: MIT */

#ifndef SPTM_VIRT_PRIV_H
#define SPTM_VIRT_PRIV_H

/* Shared between the sptm_virt*.c files only. */

#include "smp.h"
#include "sptm_virt.h"
#include "types.h"
#include "utils.h"

/* sptm_virt.c */
extern volatile bool sptm_virt_xnu_live;
void sptm_virt_cpu_enters_xnu(int cpu);

/* sptm_virt_gl.c */
extern volatile bool sptm_virt_peek_active_cpu[MAX_CPUS];
extern volatile bool sptm_virt_peek_faulted_cpu[MAX_CPUS];
bool sptm_virt_peek64(u64 phys, u64 *out);
int sptm_virt_gl_alloc(void);
u64 *sptm_virt_gl2_slot(int cpu, u16 key);
void sptm_virt_amx_host(void);

/* A system-register read that may UNDEF here: the fault is skipped (sptm_virt_peek_fault) and
 * the value reads as SAFE_MRS_FAILED. */
#define SAFE_MRS_FAILED 0xbadUL
#define SAFE_MRS(enc)                                                                              \
    ({                                                                                             \
        int _c = smp_id();                                                                         \
        u64 _v = SAFE_MRS_FAILED;                                                                  \
        sptm_virt_peek_active_cpu[_c] = true;                                                      \
        sysop("isb");                                                                              \
        __asm__ volatile("mrs %0, " enc : "+r"(_v));                                               \
        sysop("isb");                                                                              \
        sptm_virt_peek_active_cpu[_c] = false;                                                     \
        _v;                                                                                        \
    })
/* The same for a write: true if it faulted. */
#define SAFE_MSR(enc, val)                                                                         \
    ({                                                                                             \
        int _c = smp_id();                                                                         \
        u64 _w = (val);                                                                            \
        sptm_virt_peek_faulted_cpu[_c] = false;                                                    \
        sptm_virt_peek_active_cpu[_c] = true;                                                      \
        sysop("isb");                                                                              \
        __asm__ volatile("msr " enc ", %0" ::"r"(_w));                                             \
        sysop("isb");                                                                              \
        sptm_virt_peek_active_cpu[_c] = false;                                                     \
        sptm_virt_peek_faulted_cpu[_c];                                                            \
    })

#endif
