/* SPDX-License-Identifier: MIT */
/* Actual exception lifecycle + dispatcher + census; peripheral services modeled. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define TYPES_H
typedef unsigned long u64;
typedef unsigned u32;
typedef unsigned short u16;
#include "exception.h"

#include "hv/windows/windows_n1.h"
#define BIT(x)                 (1UL << (x))
#define GENMASK(hi, lo)        ((~0UL >> (63 - (hi))) & (~0UL << (lo)))
#define FIELD_GET(mask, value) (((value) & (mask)) >> __builtin_ctzl(mask))
#define ARRAY_SIZE(a)          (sizeof(a) / sizeof((a)[0]))
#include "dispatch-registers.h"
#define PORT                          0x488028000UL
#define ECAM                          0x1cb0000000UL
#define CTRL                          (ECAM + 0x100000)
#define BAR                           0xb80000000UL
#define DART                          0x488000000UL
#define PMCR0_CNT_MASK                0xff
#define PERCPU(x)                     (x)
#define START_EXCEPTION_LOWER         1
#define EXC_SERROR                    2
#define EXC_SYNC                      3
#define ENABLE_NATIVE_AIC_PASSTHROUGH 1
#define T8142                         0x8142
#define HV_NATIVE_AIC_TRACE_BRK       1

volatile enum exc_guard_t exc_guard;
volatile int exc_count;
static int bhl;
static bool n1_prepared = true, inject_fault, lock_held, rendezvous;
static unsigned cpu, boot_cpu_idx, reads, locks, unlocks, proxies, smc_calls, fiq_updates;
static u64 daif, pending, hv_cpus_in_guest, exc_entry_time, stolen_time, exc_entry_pmcr0_cnt;
static struct exc_info hardware;
static struct {
    bool apple_sysregs_unlocked;
} features;
static typeof(features) *cpu_features = &features;
static u32 chip_id = T8142;
static bool t8142_sp_el0_repair_logged[4];
static bool hv_windows_guest_compat_enabled(void)
{
    return false;
}
static bool hv_native_aic_mu_timer_active(void)
{
    return false;
}
static void hv_native_aic_exception_entry(void)
{
}
static void hv_native_aic_trace_context(int kind, struct exc_info *ctx)
{
    (void)kind;
    (void)ctx;
}
static unsigned smp_id(void)
{
    return cpu;
}
static u64 model_mrs(const char *reg)
{
    if (!strcmp(reg, "DAIF"))
        return daif;
    if (!strcmp(reg, "ISR_EL1"))
        return pending;
    if (!strcmp(reg, "SP_EL0"))
        return hardware.sp[0];
    if (!strcmp(reg, "SP_EL1"))
        return hardware.sp[1];
    return 0;
}
static void model_msr(const char *reg, u64 value)
{
    if (!strcmp(reg, "SP_EL0"))
        hardware.sp[0] = value;
    if (!strcmp(reg, "SP_EL1"))
        hardware.sp[1] = value;
}
static void model_sysop(const char *op)
{
    if (!strcmp(op, "msr daifclr, 4")) {
        assert(!pending);
        daif &= ~BIT(8);
    }
}
#define mrs(reg)            model_mrs(#reg)
#define msr(reg, value)     model_msr(#reg, value)
#define sysop(op)           model_sysop(op)
#define reg_set(reg, value) model_msr(#reg, value)
static void spin_lock(int *lock)
{
    assert(lock == &bhl && !lock_held);
    lock_held = true;
    locks++;
}
static void spin_unlock(int *lock)
{
    assert(lock == &bhl && lock_held);
    lock_held = false;
    unlocks++;
}
static void hv_wdt_breadcrumb(char c)
{
    (void)c;
}
static u64 hv_host_counter(void)
{
    return 100;
}
static void hv_windows_update_carrier_readiness(struct exc_info *ctx)
{
    (void)ctx;
    assert(lock_held);
}
static void hv_update_fiq(struct exc_info *ctx)
{
    (void)ctx;
    fiq_updates++;
}
static void hv_maybe_exit(void)
{
    assert(!lock_held);
}
static void hv_set_spsr(u64 value)
{
    hardware.spsr = value;
}
static void hv_set_elr(u64 value)
{
    hardware.elr = value;
}
static u64 hv_get_spsr(void)
{
    return hardware.spsr;
}
static u64 hv_get_elr(void)
{
    return hardware.elr;
}
static u64 hv_get_esr(void)
{
    return hardware.esr;
}
static u64 hv_get_far(void)
{
    return 0;
}
static u64 hv_get_afsr1(void)
{
    return hardware.afsr1;
}
static bool hv_rendezvous_requested(void)
{
    return rendezvous;
}
static void hv_track_t8142_undef_vector(struct exc_info *ctx)
{
    (void)ctx;
}
static bool hv_handle_msr_unlocked(struct exc_info *ctx, u64 iss)
{
    (void)ctx;
    return iss == 1;
}
static bool hv_handle_msr(struct exc_info *ctx, u64 iss)
{
    (void)ctx;
    (void)iss;
    assert(lock_held);
    return true;
}
static bool hv_handle_wfx(struct exc_info *ctx)
{
    (void)ctx;
    return true;
}
static bool hv_handle_smc(struct exc_info *ctx)
{
    smc_calls++;
    return ctx->regs[0] == 0x1111;
}
static bool hv_handle_t8142_undef_trampoline(struct exc_info *ctx)
{
    (void)ctx;
    return false;
}
static bool hv_handle_dabort(struct exc_info *ctx)
{
    (void)ctx;
    assert(lock_held);
    return true;
}
static void hv_exc_proxy(struct exc_info *ctx, int start, int type, void *extra)
{
    (void)ctx;
    (void)start;
    (void)type;
    (void)extra;
    assert(lock_held);
    proxies++;
}
static u32 read32(u64 address)
{
    assert(lock_held && !(daif & BIT(8)) && !pending);
    assert(address == PORT + 0x208);
    reads++;
    if (inject_fault)
        exc_count++;
    return 0x9a001001;
}
static u16 read16(u64 address)
{
    return (u16)read32(address);
}

#include "dispatch-production.h"

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *name = argv[1];
    struct exc_info ctx = {0};
    bool unrelated = !strcmp(name, "unrelated-hvc");
    bool nonzero = !strcmp(name, "nonzero-immediate");
    bool fast_msr = !strcmp(name, "fast-msr"), slow_msr = !strcmp(name, "slow-msr");
    hardware.elr = 0x1004; /* HVC architectural preferred return: Mu's pointer-pop instruction. */
    hardware.spsr = 0x3c5;
    hardware.sp[0] = 0x20000;
    hardware.sp[1] = 0x30000;
    hardware.esr = (u64)(fast_msr || slow_msr ? ESR_EC_MSR : ESR_EC_HVC) << 26;
    if (nonzero || fast_msr)
        hardware.esr |= 1;
    ctx.regs[0] = unrelated ? 0x1111 : HV_WINDOWS_N1_READ_CALL;
    ctx.regs[1] = PORT + 0x208;
    ctx.regs[2] = 1;
    daif = 0x3c0; /* AArch64 exception entry masks SError. */
    if (!strcmp(name, "pending"))
        pending = BIT(8);
    if (!strcmp(name, "wrong-cpu"))
        cpu = 1;
    if (!strcmp(name, "unprepared"))
        n1_prepared = false;
    if (!strcmp(name, "guard-busy"))
        exc_guard = GUARD_RETURN;
    if (!strcmp(name, "invalid-address"))
        ctx.regs[1] += 4;
    if (!strcmp(name, "invalid-version"))
        ctx.regs[2] = 2;
    if (!strcmp(name, "reserved-argument"))
        ctx.regs[3] = 1;
    if (!strcmp(name, "read-fault"))
        inject_fault = true;
    hv_cpus_in_guest = BIT(cpu);
    hv_exc_sync(&ctx);
    printf("%s ack=%#lx status=%ld reads=%u locks=%u unlocks=%u elr=%#lx proxies=%u\n", name,
           ctx.regs[0], (long)ctx.regs[1], reads, locks, unlocks, hardware.elr, proxies);
    fflush(stdout);
    assert(!lock_held && locks == unlocks && hv_cpus_in_guest == BIT(cpu) && fiq_updates == 1);
    assert(hardware.elr == (fast_msr || slow_msr ? 0x1008 : 0x1004));
    assert(hardware.spsr == 0x3c5 && hardware.sp[0] == 0x20000 && hardware.sp[1] == 0x30000);
    if (unrelated || fast_msr) {
        assert(!locks && !reads && !proxies);
        return 0;
    }
    assert(locks == 1);
    if (nonzero) {
        assert(!reads && proxies == 1);
        return 0;
    }
    if (slow_msr) {
        assert(!reads && !proxies);
        return 0;
    }
    assert(!smc_calls && !proxies && ctx.regs[0] == HV_WINDOWS_N1_READ_ACK && ctx.regs[3] == 1);
    int expected = !strcmp(name, "success") ? HV_WINDOWS_N1_READ_OK
                   : inject_fault           ? HV_WINDOWS_N1_READ_FAULT
                   : (!strcmp(name, "invalid-address") || !strcmp(name, "invalid-version") ||
                      !strcmp(name, "reserved-argument"))
                       ? HV_WINDOWS_N1_READ_INVALID
                       : HV_WINDOWS_N1_READ_UNAVAILABLE;
    assert((long)ctx.regs[1] == expected);
    assert(reads == (unsigned)(expected == HV_WINDOWS_N1_READ_OK || inject_fault));
    assert(ctx.regs[2] == (expected == HV_WINDOWS_N1_READ_OK ? 0x9a001001UL : 0));
    return 0;
}
