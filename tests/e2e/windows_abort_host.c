/* SPDX-License-Identifier: MIT */
/* Production exit lifecycle; architectural exits and scheduling are modeled. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef int uartproxy_boot_reason_t;
#define MAX_CPUS 12
#define BIT(x) (1ULL << (x))
#define SPSR_M 15
#define FIELD_GET(mask, value) ((value) & (mask))
#define EXC_RET_HANDLED 0
#define EXC_EXIT_GUEST 1
struct exc_info { u64 spsr, elr, far, sp[3], elr_phys, far_phys, sp_phys; void *extra; u64 regs[32]; };
struct uartproxy_msg_start { int reason; u32 code; struct exc_info *info; };
static bool hv_should_exit[MAX_CPUS], hv_started_cpus[MAX_CPUS], hv_setup_pending[MAX_CPUS];
static bool hv_shutdown_failed, exited[MAX_CPUS], time_stealing = true;
static int hv_windows_exit_reason, boot_cpu_idx = 6, current_cpu, bhl, proxy_result;
static int wait_fail = -1, watchdog_fail, waits, finished, mtp_fail, stopped, quarantined;
static u64 hv_cpus_in_guest, stolen_time;
static jmp_buf exit_jump;
static void spin_lock(int *lock) { assert(!*lock); *lock = 1; }
static void spin_unlock(int *lock) { assert(*lock); *lock = 0; }
static int smp_id(void) { return current_cpu; }
static void hv_rendezvous(void) { assert(bhl); printf("rendezvous cpu=%d\n", current_cpu); }
static void hv_wdt_breadcrumb(char c) { (void)c; }
static void hv_wdt_suspend(void) {}
static void hv_wdt_resume(void) {}
static int hv_wdt_stop(void) { printf("watchdog-stop\n"); return watchdog_fail; }
static u64 hv_host_counter(void) { return 100; }
static u64 hv_translate(u64 a, bool b, bool c, void *d) { (void)b; (void)c; (void)d; return a; }
static int uartproxy_run(struct uartproxy_msg_start *msg) { assert(bhl); (void)msg; return proxy_result; }
static void print_regs(u64 *regs, int n) { (void)regs; (void)n; }
static void flush_and_reboot(void) { abort(); }
static void hv_exit_guest(void) __attribute__((noreturn));
static void hv_exit_guest(void) {
    assert(!bhl);
    exited[current_cpu] = true;
    printf("exit cpu=%d reason=%d\n", current_cpu, hv_windows_exit_reason);
    longjmp(exit_jump, 1);
}
static int smp_wait(int cpu, void *unused) {
    (void)unused;
    assert(!bhl && hv_should_exit[cpu]);
    assert(exited[cpu] || hv_setup_pending[cpu]);
    ++waits;
    printf("join cpu=%d pending=%d\n", cpu, hv_setup_pending[cpu]);
    return cpu == wait_fail ? -1 : 0;
}
static void hv_windows_serial_finish(void) { assert(!bhl); ++finished; }
static bool hv_windows_mtp_stop(void) {
    assert(!bhl && !hv_cpus_in_guest);
    for (int i = 0; i < MAX_CPUS; ++i)
        assert(!hv_started_cpus[i] && !hv_setup_pending[i]);
    ++stopped;
    printf("mtp-stop failed=%d\n", mtp_fail);
    return !mtp_fail;
}
static void hv_windows_quarantine(void) { ++quarantined; hv_shutdown_failed = true; }
#include "production_abort.inc"

static void reset_state(void) {
    memset(hv_should_exit, 0, sizeof(hv_should_exit));
    memset(hv_started_cpus, 0, sizeof(hv_started_cpus));
    memset(hv_setup_pending, 0, sizeof(hv_setup_pending));
    memset(exited, 0, sizeof(exited));
    for (int i = 0; i < 9; ++i) hv_started_cpus[i] = true;
    hv_setup_pending[9] = true;
    hv_cpus_in_guest = 0x1ff;
    hv_shutdown_failed = false; hv_windows_exit_reason = 0;
    bhl = waits = finished = watchdog_fail = mtp_fail = stopped = quarantined = 0; wait_fail = -1;
    proxy_result = EXC_EXIT_GUEST;
}
static void proxy_exit(int cpu) {
    current_cpu = cpu;
    hv_cpus_in_guest &= ~BIT(cpu); /* Production exception entry does this. */
    spin_lock(&bhl);
    struct exc_info ctx = {.spsr=4};
    if (!setjmp(exit_jump)) _hv_exc_proxy(&ctx, 2, 0, NULL);
    assert(exited[cpu] && !bhl);
}
static void system_exit(int cpu, bool reset) {
    current_cpu = cpu;
    if (!setjmp(exit_jump)) hv_windows_system_exit(reset);
    assert(exited[cpu] && !bhl);
}
static int drain(void) {
    for (int i = 0; i < MAX_CPUS; ++i) assert(hv_should_exit[i]);
    for (int i = 0; i < 9; ++i) {
        if (exited[i]) continue;
        current_cpu = i;
        hv_cpus_in_guest &= ~BIT(i); /* Production exception entry clears membership. */
        if (!setjmp(exit_jump)) { hv_maybe_exit(); assert(!"peer resumed guest"); }
    }
    current_cpu = boot_cpu_idx;
    int result = finish_boot_cpu();
    assert(waits == 9); /* Eight running APs plus the setup-pending worker. */
    assert(!hv_started_cpus[boot_cpu_idx]);
    return result;
}
int main(int argc, char **argv) {
    assert(argc == 2); reset_state();
    if (!strcmp(argv[1], "unfixed")) {
        proxy_exit(0);
        assert(hv_windows_exit_reason == 0 && !hv_should_exit[boot_cpu_idx]);
        for (int i=1;i<9;i++) assert(!exited[i]);
        puts("REPRODUCED: AP exited while BSP/peers remained runnable; stale success=0");
        return 0;
    }
    if (!strcmp(argv[1], "ap-fault")) proxy_exit(0);
    else if (!strcmp(argv[1], "bsp-fault")) proxy_exit(boot_cpu_idx);
    else if (!strcmp(argv[1], "poweroff")) system_exit(2, false);
    else if (!strcmp(argv[1], "reset")) system_exit(2, true);
    else if (!strcmp(argv[1], "abort-after-poweroff")) { system_exit(2, false); proxy_exit(0); }
    else if (!strcmp(argv[1], "poweroff-after-abort")) { proxy_exit(0); system_exit(2, false); }
    else if (!strcmp(argv[1], "join-failure")) { proxy_exit(0); wait_fail=9; }
    else if (!strcmp(argv[1], "watchdog-failure")) { system_exit(2, false); watchdog_fail=1; }
    else if (!strcmp(argv[1], "mtp-stop-failure")) { system_exit(2, false); mtp_fail=1; }
    else if (!strcmp(argv[1], "handled")) {
        current_cpu=0; proxy_result=EXC_RET_HANDLED; spin_lock(&bhl);
        struct exc_info ctx={.spsr=4}; _hv_exc_proxy(&ctx,2,0,NULL);
        assert(bhl && !hv_windows_exit_reason);
        for(int i=0;i<MAX_CPUS;i++) assert(!hv_should_exit[i] && !exited[i]);
        spin_unlock(&bhl); puts("PASS handled: no exit requested"); return 0;
    } else abort();
    int result=drain();
    int expected=!strcmp(argv[1],"poweroff")?1:!strcmp(argv[1],"reset")?2:-1;
    assert(result==expected);
    assert(finished==(wait_fail<0 && !watchdog_fail));
    assert(stopped==(wait_fail<0 && !watchdog_fail));
    assert(quarantined==mtp_fail);
    if(mtp_fail) assert(hv_shutdown_failed);
    if(wait_fail==9) assert(hv_setup_pending[9] && hv_shutdown_failed);
    else assert(!hv_setup_pending[9]);
    printf("PASS %s result=%d joins=%d\n",argv[1],result,waits);
}
