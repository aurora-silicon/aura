/*
 * Modeled boundary for the EL2 accounting replay. Production functions are
 * appended verbatim by run.py; only clocks, registers, locks and costs are
 * modeled here. Each CPU has its own clock, so one CPU's stays never move
 * another CPU's time; get_ticks() reads the clock of the CPU "running" now.
 */
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned long u64;
typedef long s64;
typedef uint32_t u32;
#include "tick_deadline.h"
#include "hv_el2_acct.h"

#define MAX_CPUS 10
#define T8142 0x8142
#define T8152 0x8152
#define HV_SCANOUT_FAST_ROWS 32u
#define HV_FATAL_TRAP_FIQ 1
#define HV_FATAL_TRAP_FIQ_FAST 2
#define ENABLE_NATIVE_AIC_PASSTHROUGH 1
#define ALIGNED(x) __attribute__((aligned(x)))
#define BIT(x) (1UL << (x))
#define UNUSED(x) ((void)(x))
#define CNTx_CTL_ENABLE 1
#define CNTx_CTL_IMASK 2
#define CNTx_CTL_ISTATUS 4
#define HCR_VI 128
#define HCR_VF 64
#define ESR_ISS 0
#define START_HV 1
#define HV_VTIMER 3
#define START_EXCEPTION_LOWER 4
#define EXC_FIQ 5
#define HV_CPU_SWITCH 6
#define PMCR0_IMODE_MASK 7
#define PMCR0_IACT 8
#define PMCR0_IMODE_FIQ 2
#define PMCR0_CNT_MASK 0xff
#define UPMCR0_IMODE_T8020 0
#define UPMCR0_IMODE_FIQ 3
#define UPMSR_IACT 8
#define IPI_SR_PENDING 1
#define FIELD_GET(field, value) (value)
#define HV_NATIVE_AIC_HOT_TRACE(...) ((void)0)
#define PERCPU(field) (pcpu[cpu].field)

enum { SYS_CNTHP_TVAL_EL2, SYS_CNTHP_CTL_EL2, CNTP_TVAL_EL0, CNTP_CTL_EL0, CNTV_CTL_EL0,
       ISR_EL1, HCR_EL2, SYS_IMP_APL_PMCR0, SYS_IMP_APL_UPMCR0, SYS_IMP_APL_UPMSR,
       SYS_IMP_APL_IPI_SR_EL1, CNTFRQ_EL0, CNTVOFF_EL2, SP_EL0, SP_EL1, REGISTER_COUNT };

struct exc_info { u64 regs[32]; u64 spsr, elr, esr, far, afsr1, sp[3]; };
struct { u32 ipi_pending, ipi_queued, pmc_pending; u64 exc_entry_pmcr0_cnt; } pcpu[MAX_CPUS];
struct features { bool apple_sysregs_unlocked, fast_ipi; } features;
static struct features *cpu_features = &features;

static unsigned cpu, chip_id = T8142;
static int boot_cpu_idx = 6, hv_pinned_cpu = -1, hv_want_cpu = -1;
static u64 hv_tick_interval, hv_secondary_tick_interval, hv_wfi_wake_interval;
static struct hv_tick_deadline hv_periodic_deadlines[MAX_CPUS];
static u64 registers[MAX_CPUS][REGISTER_COUNT], expires[MAX_CPUS];
static u64 clock_now[MAX_CPUS], frequency = 1000000000;
static u64 hv_cpus_in_guest, stolen_time, exc_entry_time;
static u64 wfi_trap_count[MAX_CPUS];
static bool wfi_trap_logged = true, t8142_sp_el0_repair_logged[MAX_CPUS];
static int bhl;

/* Modeled costs, in counter ticks (ns at 1 GHz). */
static u64 cost_context = 40, cost_update_fiq = 200, cost_fb_line = 130000, cost_tick = 5000;
static u64 pending_at[MAX_CPUS] = {[0 ... MAX_CPUS - 1] = ~0UL}; /* a physical interrupt arrives here */
static u64 fb_lines_converted;
static u32 hv_fb_shadow_model[1], *hv_fb_shadow = hv_fb_shadow_model;
static u64 lock_contention;              /* next acquisition waits this long */
static u64 truth_lock_wait[MAX_CPUS];    /* what the lock model made each CPU wait */
static u64 truth_locks[MAX_CPUS], truth_lock_taken[MAX_CPUS], truth_held[MAX_CPUS];
static void (*during_wfi)(void);         /* runs while the current CPU sits in WFI */
static void (*after_report)(void);       /* runs right after the housekeeping report */

static unsigned smp_id(void) { return cpu; }
static bool smp_is_alive(unsigned n) { return n < MAX_CPUS && (int)n != boot_cpu_idx; }
static u64 get_ticks(void) { return clock_now[cpu]; }

static u64 read_reg(unsigned reg)
{
    if (reg == CNTFRQ_EL0)
        return frequency;
    if (reg == ISR_EL1 && clock_now[cpu] >= pending_at[cpu])
        return registers[cpu][reg] | 0x80;
    u64 v = registers[cpu][reg];
    if (reg == SYS_CNTHP_CTL_EL2 && (v & CNTx_CTL_ENABLE) && (s64)(clock_now[cpu] - expires[cpu]) >= 0)
        v |= CNTx_CTL_ISTATUS;
    return v;
}
static void write_reg(unsigned reg, u64 value)
{
    registers[cpu][reg] = value;
    if (reg == SYS_CNTHP_TVAL_EL2) {
        assert(value > 0);
        expires[cpu] = clock_now[cpu] + value;
    }
}
#define mrs(reg) read_reg(reg)
#define msr(reg, value) write_reg(reg, value)
#define reg_clr(reg, value) write_reg(reg, read_reg(reg) & ~(u64)(value))
#define reg_set(reg, value) write_reg(reg, read_reg(reg) | (u64)(value))
#define sysop(op) ((void)0)

/* Captured console: one entry per printf call. */
static char console[1 << 20];
static size_t console_len;
static unsigned printf_calls;
static int model_printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int n = vsnprintf(console + console_len, sizeof(console) - console_len, format, args);
    va_end(args);
    assert(n >= 0 && console_len + n < sizeof(console));
    console_len += n;
    printf_calls++;
    return n;
}
#define printf model_printf

static void spin_lock(int *lock)
{
    assert(lock == &bhl && *lock == 0);
    clock_now[cpu] += lock_contention;
    truth_lock_wait[cpu] += lock_contention;
    truth_locks[cpu]++;
    lock_contention = 0;
    truth_lock_taken[cpu] = clock_now[cpu];
    *lock = 1;
}
static void spin_unlock(int *lock)
{
    assert(lock == &bhl && *lock == 1);
    truth_held[cpu] += clock_now[cpu] - truth_lock_taken[cpu];
    *lock = 0;
}

static void hv_wdt_breadcrumb(char c) { (void)c; }
static void hv_native_aic_exception_entry(void) {}
static void hv_maybe_exit(void) {}
static bool hv_guest_ipi_take_tag(void) { return false; }
static bool hv_native_aic_windows_ready(void) { return false; }
static void hv_native_aic_doorbell_sync(void) {}
static void hv_guest_ipi_retry_tick(void) {}
static bool hv_rendezvous_requested(void) { return false; }
static void hv_get_context(struct exc_info *ctx) { (void)ctx; clock_now[cpu] += cost_context; }
static void hv_update_fiq(struct exc_info *ctx) { (void)ctx; clock_now[cpu] += cost_update_fiq; }
static void hv_windows_update_carrier_readiness(struct exc_info *ctx) { (void)ctx; }
static void hv_exc_proxy(struct exc_info *ctx, int start, int reason, void *arg)
{
    (void)ctx; (void)start; (void)reason; (void)arg;
}
static void hv_maybe_switch_cpu(struct exc_info *ctx, int a, int b, void *p)
{
    (void)ctx; (void)a; (void)b; (void)p;
}
static bool hv_windows_guest_compat_enabled(void) { return true; }
static bool hv_native_aic_mu_timer_active(void) { return false; }
static void hv_set_spsr(u64 v) { (void)v; }
static void hv_set_elr(u64 v) { (void)v; }
static void hv_windows_mtp_poll(void) {}
static void hv_input_idle_poll(void) {}
static bool hv_scanout_work(u32 rows) { (void)rows; return false; }
static bool hv_scanout_work_idle(void) { return false; }
static void hv_guest_diag_note_fiq(void) {}
static int hv_tick_owner(void) { return boot_cpu_idx; }
static void hv_fatal_note_trap(int kind, u64 esr, u64 elr, u64 spsr, u64 far)
{
    (void)kind; (void)esr; (void)elr; (void)spsr; (void)far;
}
static bool hv_fast_ipi(void) { return cpu_features->fast_ipi; }
static void hv_fb_convert_slice(unsigned lines) { clock_now[cpu] += lines * cost_fb_line; fb_lines_converted += lines; }
/* Pacing is exercised by tests/windows_fb_path; here every idle step is due. */
static bool hv_fb_idle_due(void) { return true; }
static void cpu_wfi_stateless(void)
{
    if (during_wfi)
        during_wfi();
    if (clock_now[cpu] >= pending_at[cpu])
        return; /* a pending interrupt ends WFI at once */
    assert((s64)(expires[cpu] - clock_now[cpu]) >= 0);
    clock_now[cpu] = expires[cpu];
}
u64 hv_host_counter(void);
void hv_report_t8142_el2_acct(void);
/* Production hv_tick's housekeeping is modeled by its cost, its report and its deadline completion. */
static void hv_tick(struct exc_info *ctx)
{
    (void)ctx;
    clock_now[cpu] += cost_tick;
    hv_report_t8142_el2_acct();
    if (after_report)
        after_report();
    hv_tick_deadline_complete(&hv_periodic_deadlines[cpu], clock_now[cpu]);
}
