/* Modeled register/clock/I/O boundary; production functions are appended verbatim. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "tick_deadline.h"
typedef uint64_t u64;
typedef uint32_t u32;
#define MAX_CPUS 10
#define T8142 0x8142
#define ENABLE_NATIVE_AIC_PASSTHROUGH 1
#define CNTx_CTL_ENABLE 1
#define CNTx_CTL_IMASK 2
#define CNTx_CTL_ISTATUS 4
#define HCR_VI 128
#define HCR_VF 64
#define ESR_ISS 0
#define HV_FB_SLICE_IDLE 1
#define HV_FB_SLICE_TICK 2
#define START_HV 1
#define HV_USER_INTERRUPT 2
#define HV_VTIMER 3
#define START_EXCEPTION_LOWER 4
#define EXC_FIQ 5
#define HV_CPU_SWITCH 6
#define PMCR0_IMODE_MASK 7
#define PMCR0_IACT 8
#define PMCR0_IMODE_FIQ 2
#define UPMCR0_IMODE_T8020 0
#define UPMCR0_IMODE_FIQ 3
#define UPMSR_IACT 8
#define IPI_SR_PENDING 1
#define FIELD_GET(field, value) (value)
#define HV_NATIVE_AIC_HOT_TRACE(...) ((void)0)
#define PERCPU(field) (pcpu[cpu].field)
enum { SYS_CNTHP_TVAL_EL2, SYS_CNTHP_CTL_EL2, CNTP_TVAL_EL0, CNTP_CTL_EL0,
       CNTV_CTL_EL0, ISR_EL1, HCR_EL2, SYS_IMP_APL_PMCR0, SYS_IMP_APL_UPMCR0,
       SYS_IMP_APL_UPMSR, SYS_IMP_APL_IPI_SR_EL1, REGISTER_COUNT };
struct exc_info { u64 esr, elr; };
struct { bool pmc_pending; unsigned ipi_queued; } pcpu[MAX_CPUS];
struct features { bool apple_sysregs_unlocked, fast_ipi; } features;
static struct features *cpu_features = &features;
static unsigned cpu, chip_id = T8142;
static int boot_cpu_idx, hv_pinned_cpu = -1, hv_want_cpu = -1, uartproxy_iodev;
static u64 hv_tick_interval = 1000000000, hv_secondary_tick_interval = 1000000000;
static u64 hv_wfi_wake_interval = 1000000, clock_now;
static struct hv_tick_deadline hv_periodic_deadlines[MAX_CPUS];
static u64 registers[MAX_CPUS][REGISTER_COUNT], expires[MAX_CPUS][2];
static unsigned full_ticks, serial_flushes, io_polls, vuart_polls, user_interrupts;
static unsigned guest_updates, ipi_retries, doorbells, wfi_calls;
static u64 wfi_trap_count[MAX_CPUS];
static u64 el0_sync_pass, el0_sync_emul, el1_sync_pass, el1_sync_emul;
static u32 gic_cpuif_ack_count[MAX_CPUS], gic_cpuif_eoi_count[MAX_CPUS];
#define HV_GIC_INTID_WORDS 32
static struct { u32 active[HV_GIC_INTID_WORDS]; } gic_cpuif[MAX_CPUS];
static bool smp_is_alive(unsigned n) { return n < MAX_CPUS; }
static FILE *health_log;
static int report_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    if (health_log) {
        if (!strncmp(format, "HV:", 3))
            fprintf(health_log, "TTY> [%12.6f] ", (double)clock_now / 1000000000.0);
        int result = vfprintf(health_log, format, args);
        va_end(args);
        return result;
    }
    int result = vprintf(format, args);
    va_end(args);
    return result;
}
#define printf report_printf
static unsigned frame_idle, frame_tick, scans, vector_repairs, maybe_exits, rendezvous_entries;
static bool wfi_trap_logged, mu_ready, windows_ready, host_input, guest_tag, rendezvous;
static u64 scan_delay;
static unsigned smp_id(void) { return cpu; }
static u64 get_ticks(void) { return clock_now; }
static u64 read_reg(unsigned reg) {
    u64 v = registers[cpu][reg];
    if (reg == SYS_CNTHP_CTL_EL2 || reg == CNTP_CTL_EL0) {
        unsigned idx = reg == SYS_CNTHP_CTL_EL2 ? 0 : 1;
        if ((v & CNTx_CTL_ENABLE) && (int64_t)(clock_now - expires[cpu][idx]) >= 0)
            v |= CNTx_CTL_ISTATUS;
    }
    return v;
}
static void write_reg(unsigned reg, u64 value) {
    registers[cpu][reg] = value;
    if (reg == SYS_CNTHP_TVAL_EL2 || reg == CNTP_TVAL_EL0) {
        assert(value > 0);
        expires[cpu][reg == SYS_CNTHP_TVAL_EL2 ? 0 : 1] = clock_now + value;
    }
}
#define mrs(reg) read_reg(reg)
#define msr(reg, value) write_reg(reg, value)
#define reg_clr(reg, value) write_reg(reg, read_reg(reg) & ~(u64)(value))
#define sysop(op) ((void)0)
static void hv_wdt_pet(void) { ++full_ticks; }
static void hv_windows_serial_flush(void) { ++serial_flushes; }
static void hv_fb_convert_slice(unsigned slice) { if (slice == HV_FB_SLICE_IDLE) ++frame_idle; else ++frame_tick; }
static void hv_fb_report(void) {}
static void hv_track_t8142_undef_vector(struct exc_info *ctx) { (void)ctx; }
static void hv_verify_t8142_undef_vectors(void) { ++vector_repairs; }
static void hv_scan_t8142_guest_modules(void) { ++scans; clock_now += scan_delay; }
static void hv_report_t8142_process(struct exc_info *ctx) { (void)ctx; }
void hv_report_t8142_gic_activity(void);
static void hv_windows_mtp_poll(void) {}
static void hv_windows_mtp_report(void) {}
static void hv_check_t8142_bugcheck(void) {}
static void hv_dump_t8142_bugcheck_stack(struct exc_info *ctx) { (void)ctx; }
static void hv_recover_t8142_sysreg_undef(struct exc_info *ctx) { (void)ctx; }
static void iodev_handle_events(int dev) { (void)dev; ++io_polls; }
static bool iodev_can_read(int dev) { (void)dev; return host_input; }
static void iodev_console_flush(void) {}
static void hv_exc_proxy(struct exc_info *ctx, int start, int reason, void *arg) {
    (void)ctx; (void)start; (void)arg; if (reason == HV_USER_INTERRUPT) ++user_interrupts;
}
static bool hv_native_aic_mu_timer_active(void) { return mu_ready; }
static bool hv_native_aic_windows_active(void) { return windows_ready; }
static bool hv_native_aic_windows_ready(void) { return windows_ready; }
static void hv_vuart_poll(void) { ++vuart_polls; }
static void cpu_wfi_stateless(void) {
    ++wfi_calls; clock_now = expires[cpu][chip_id == T8142 ? 0 : 1];
}
static void hv_native_aic_exception_entry(void) {}
static void hv_maybe_exit(void) { ++maybe_exits; }
static bool hv_guest_ipi_take_tag(void) { bool v=guest_tag; guest_tag=false; return v; }
static void hv_native_aic_doorbell_sync(void) { ++doorbells; }
static void hv_guest_ipi_retry_tick(void) { ++ipi_retries; }
static bool hv_rendezvous_requested(void) { return rendezvous; }
static void hv_get_context(struct exc_info *ctx) { (void)ctx; }
static void hv_update_fiq(struct exc_info *ctx) { (void)ctx; ++guest_updates; }
static void hv_wdt_breadcrumb(char c) { (void)c; }
static void hv_windows_update_carrier_readiness(struct exc_info *ctx) { (void)ctx; }
static void hv_exc_entry(void) { ++rendezvous_entries; }
static void hv_maybe_switch_cpu(struct exc_info *ctx,int a,int b,void *p) { (void)ctx;(void)a;(void)b;(void)p; }
static void hv_exc_exit(struct exc_info *ctx) { hv_update_fiq(ctx); }
/* EL2 accounting is exercised by tests/windows_el2_acct; values match hv_el2_acct.h. */
#define HV_EL2_TICK 0
#define HV_EL2_WFI 5
static unsigned el2_causes[8], el2_reports;
static void hv_el2_cause(unsigned cause) { ++el2_causes[cause]; }
static void hv_report_t8142_el2_acct(void) { ++el2_reports; }
