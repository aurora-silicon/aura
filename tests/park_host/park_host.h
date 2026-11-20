/* SPDX-License-Identifier: MIT */
/* What src/sptm_virt_park.c needs from aura, for tests/park_host/park_host.c: cores are threads. */
#ifndef PARK_HOST_H
#define PARK_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef int64_t s64;

#define MAX_CPUS  8
#define BIT(n)    (1UL << (n))
#define min(a, b) (((a) < (b)) ? (a) : (b))

/*
 * Every atomic access of the code under test may stall for a while first, so that a window
 * between two of them (a count and a check, an election and a read) is wide enough to hit. A
 * function-like macro doesn't expand inside itself: the inner name is the builtin.
 */
void park_host_race(void);
#ifndef PARK_HOST_HARNESS
#define __atomic_load_n(p, m)        (park_host_race(), __atomic_load_n(p, m))
#define __atomic_store_n(p, v, m)    (park_host_race(), __atomic_store_n(p, v, m))
#define __atomic_fetch_add(p, v, m)  (park_host_race(), __atomic_fetch_add(p, v, m))
#define __atomic_exchange_n(p, v, m) (park_host_race(), __atomic_exchange_n(p, v, m))
#define __atomic_compare_exchange_n(p, e, d, w, s, f)                                              \
    (park_host_race(), __atomic_compare_exchange_n(p, e, d, w, s, f))
#endif

/* System registers: the counter is the host's monotonic clock in ns; the rest are per thread. */
u64 park_host_now(void);
#define host_rd_CNTPCT_EL0() park_host_now()
#define host_rd_CNTFRQ_EL0() 1000000000UL
extern __thread u64 host_reg_ELR_EL2, host_reg_SPSR_EL2, host_reg_ESR_EL2, host_reg_FAR_EL2;
extern __thread u64 host_reg_SYS_IMP_APL_ELR_GL1, host_reg_SYS_IMP_APL_SPSR_GL1,
    host_reg_SYS_IMP_APL_ESR_GL1, host_reg_SYS_IMP_APL_FAR_GL1;
#define host_rd_ELR_EL2()              host_reg_ELR_EL2
#define host_rd_SPSR_EL2()             host_reg_SPSR_EL2
#define host_rd_ESR_EL2()              host_reg_ESR_EL2
#define host_rd_FAR_EL2()              host_reg_FAR_EL2
#define host_rd_SYS_IMP_APL_ELR_GL1()  host_reg_SYS_IMP_APL_ELR_GL1
#define host_rd_SYS_IMP_APL_SPSR_GL1() host_reg_SYS_IMP_APL_SPSR_GL1
#define host_rd_SYS_IMP_APL_ESR_GL1()  host_reg_SYS_IMP_APL_ESR_GL1
#define host_rd_SYS_IMP_APL_FAR_GL1()  host_reg_SYS_IMP_APL_FAR_GL1
#define mrs(r)                         host_rd_##r()
#define msr(r, v)                      (host_reg_##r = (v))

/* wfe waits for an event, sev gives every core one: the ARM event register, per thread. */
void park_host_sysop(const char *op);
#define sysop(op) park_host_sysop(op)

extern __thread int park_host_cpu;
extern __thread bool park_host_gl;
#define smp_id()     park_host_cpu
#define mmu_active() true
#define in_gl12()    park_host_gl
void udelay(u32 us);
int park_host_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#ifndef PARK_HOST_HARNESS
#define printf park_host_printf
#endif

extern volatile bool sptm_virt_guest_live;

struct exc_info {
    u64 elr;
};

typedef int iodev_id_t;
#define USAGE_UARTPROXY 2
extern iodev_id_t uartproxy_iodev;
int iodev_get_usage(iodev_id_t id);
void iodev_handle_events(iodev_id_t id);
ssize_t iodev_can_read(iodev_id_t id);
ssize_t iodev_read(iodev_id_t id, void *buf, size_t n);
bool iodev_can_write(iodev_id_t id);

#define START_HV 3
struct uartproxy_msg_start {
    u32 reason;
    u32 code;
    void *info;
    void *reserved;
};
int uartproxy_run(struct uartproxy_msg_start *start);

/* src/sptm_virt_park.c */
extern volatile int sptm_virt_parking_cpu;
extern u32 sptm_virt_park_wait_ms, sptm_virt_park_poll_ms, sptm_virt_park_gl_grace_ms;
int sptm_virt_park_enter(void);
void sptm_virt_park_exit(int token);
void sptm_virt_park_others(void);
void sptm_virt_park_release(void);
int sptm_virt_park_proxy(struct exc_info *ctx, u32 code);
bool sptm_virt_park_requested(bool gl);
void sptm_virt_park_host(struct exc_info *ctx);
void sptm_virt_park_arrive(void);

#endif
