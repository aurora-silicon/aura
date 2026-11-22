#pragma once
#include <stdio.h>
#include "soc.h"
#include "types.h"
#define UNUSED(x)       (void)(x)
#define ARRAY_SIZE(a)   (sizeof(a) / sizeof((a)[0]))
#define GENMASK(h, l)   (((~0UL) << (l)) & (~0UL >> (63 - (h))))
#define FIELD_GET(m, v) (((v) & (m)) >> __builtin_ctzl(m))
#define FIELD_PREP(m, v) (((u64)(v) << __builtin_ctzl(m)) & (m))
#define SZ_4K           0x1000
#define SZ_16K          0x4000
#define USEC_PER_SEC    1000000
/* The simulation's clock: 1 MHz, advanced by every poll (nvme_sim.c). */
extern u64 sim_now;
#define stub_mrs_CNTPCT_EL0 sim_now
#define stub_mrs_CNTFRQ_EL0 1000000UL
#define mrs(reg) (stub_mrs_##reg)
#define sysop(op) do { } while (0)
typedef struct { int unused; } spinlock_t;
#define DECLARE_SPINLOCK(name) spinlock_t name = {0}
static inline void spin_lock(spinlock_t *l) { (void)l; }
static inline void spin_unlock(spinlock_t *l) { (void)l; }
void memcpy128(void *dst, void *src, size_t size);
void memset64(void *dst, u64 value, size_t size);
/* MMIO: the simulated controller. */
u32 read32(u64 addr);
void write32(u64 addr, u32 value);
static inline u32 set32(u64 a, u32 s) { u32 v = read32(a) | s; write32(a, v); return v; }
static inline u32 clear32(u64 a, u32 c) { u32 v = read32(a) & ~c; write32(a, v); return v; }
static inline u32 mask32(u64 a, u32 c, u32 s) { u32 v = (read32(a) & ~c) | s; write32(a, v); return v; }
static inline void write64_lo_hi(u64 a, u64 v) { write32(a, (u32)v); write32(a + 4, (u32)(v >> 32)); }
static inline void dma_wmb(void) { }
static inline void dma_rmb(void) { }
u64 get_ticks(void);
u64 ticks_to_msecs(u64 ticks);
u64 timeout_calculate(u32 usec);
bool timeout_expired(u64 timeout);
static inline int poll32(u64 addr, u32 mask, u32 target, u32 timeout)
{
    while (--timeout > 0) {
        if ((read32(addr) & mask) == target)
            return 0;
        timeout_expired(0);
    }
    return -1;
}
#define min(a, b) (((a) < (b)) ? (a) : (b))
u64 ticks_to_usecs(u64 ticks);
