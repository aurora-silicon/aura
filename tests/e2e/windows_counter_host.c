/* SPDX-License-Identifier: MIT */
/* Production timing -> USB command polling with independent clock domains. */
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef uint8_t u8;
typedef uint64_t u64;
#define T8142                        0x8142
#define CNTPCT_EL0                   0
#define CNTFRQ_EL0                   1
#define SYS_IMP_APL_CNTVCT_ALIAS_EL0 2
#define CNTVOFF_EL2                  3
#define CNTVCT_EL0                   4
#define sys_reg(...)                 1
#define SYSREG_ISS(x)                (x)
static u32 chip_id = T8142;
static u64 wall, hz = 1000000000, alias_hz = 24000000, raw_epoch, alias_epoch;
static unsigned reads, raw_reads, alias_reads, read_limit = 200000;
static const char *injected;
static unsigned fraction, isb_count;
static void advance_fraction(unsigned units)
{
    fraction += units;
    wall += fraction / 1000000;
    fraction %= 1000000;
}
static void model_sysop(const char *op)
{
    if (!strcmp(op, "isb"))
        isb_count++;
    if (injected && !strcmp(injected, "fast-reads") && !strcmp(op, "isb"))
        advance_fraction(10807704);
}
static char diagnostics[2048];
static int model_printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int count = vsnprintf(diagnostics + strlen(diagnostics),
                          sizeof(diagnostics) - strlen(diagnostics), format, args);
    va_end(args);
    return count;
}
static bool frozen;
static jmp_buf watchdog;
bool timer_windows_ready;
u64 timer_windows_offset;
static u64 model_mrs(int reg)
{
    if (reg == CNTFRQ_EL0)
        return hz;
    if (reg == CNTVOFF_EL2)
        return 1000000;
    if (++reads > read_limit)
        longjmp(watchdog, 1);
    if (injected && !strcmp(injected, "fast-reads"))
        advance_fraction(284468);
    else
        wall += injected && !strcmp(injected, "short-window") ? reads & 1 : 100;
    if (reg == CNTPCT_EL0) {
        raw_reads++;
        if (injected) {
            if ((!strcmp(injected, "first-backward") && raw_reads == 2) ||
                (!strcmp(injected, "poll-backward") && raw_reads == 3) ||
                (!strcmp(injected, "last-backward") && alias_reads == 2))
                return 0;
            if ((!strcmp(injected, "first-wide") && raw_reads == 2) ||
                (!strcmp(injected, "last-wide") && alias_reads == 2))
                return raw_epoch + wall + 20000;
        }
        return raw_epoch + (frozen ? 0 : wall);
    }
    if (reg == CNTVCT_EL0)
        return raw_epoch + wall - 1000000;
    alias_reads++;
    if (injected && !strcmp(injected, "alias-backward") && alias_reads == 2)
        return 0;
    return alias_epoch + wall * alias_hz / 1000000000;
}
#define mrs(reg)              model_mrs(reg)
#define sysop(op)             model_sysop(op)
#define BIT(n)                (1ULL << (n))
#define DWC3_DEPCMDPAR0(ep)   (0x100 + (ep) * 16)
#define DWC3_DEPCMDPAR1(ep)   (0x104 + (ep) * 16)
#define DWC3_DEPCMDPAR2(ep)   (0x108 + (ep) * 16)
#define DWC3_DEPCMD(ep)       (0x10c + (ep) * 16)
#define DWC3_DEPCMD_CMDACT    0x400
#define DWC3_DEPCMD_STATUS(v) (((v) >> 12) & 15)
typedef struct {
    u64 regs;
} dwc3_dev_t;
static unsigned mmio_reads, complete_after;
static void write32(u64 a, u32 v)
{
    (void)a;
    (void)v;
}
static u32 read32(u64 a)
{
    (void)a;
    mmio_reads++;
    return complete_after && mmio_reads >= complete_after ? 0 : DWC3_DEPCMD_CMDACT;
}
#define usb_debug_printf(...) ((void)0)
#define printf                model_printf
#include "production_counter.inc"
#undef printf
int main(int argc, char **argv)
{
    assert(argc == 2);
    bool other = !strcmp(argv[1], "other"), bad_hz = !strcmp(argv[1], "bad-hz"),
         bad_rate = !strcmp(argv[1], "bad-rate");
    const char *expected_reason = NULL;
    const char *faults[] = {"first-backward", "first-wide",     "poll-backward", "last-backward",
                            "last-wide",      "alias-backward", "frozen-init",   "short-window"};
    const char *reasons[] = {"first-bracket",   "first-bracket",  "poll-backward",
                             "last-bracket",    "last-bracket",   "last-bracket",
                             "iteration-limit", "iteration-limit"};
    for (unsigned i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
        if (!strcmp(argv[1], faults[i])) {
            injected = faults[i];
            expected_reason = reasons[i];
            read_limit = 1100000;
            frozen = !strcmp(injected, "frozen-init");
        }
    }
    if (!strcmp(argv[1], "fast-reads")) {
        injected = "fast-reads";
        read_limit = 1100000;
    }
    (void)expected_reason;
    if (bad_hz)
        expected_reason = "frequency";
    if (bad_rate)
        expected_reason = "rate";
    if (other)
        chip_id = 0x8140;
    if (bad_hz)
        hz = 24000000;
    if (bad_rate)
        alias_hz = 48000000;
    if (!strcmp(argv[1], "offset")) {
        raw_epoch = 123456789000ULL;
        alias_epoch = 987654321;
    }
    if (setjmp(watchdog)) {
        puts("{\"stalled\":true}");
        return 0;
    }
#ifdef TIMER_PREPARED
    bool initialized = timer_prepare_windows();
    u64 qualification_ns = wall;
    unsigned qualification_reads = raw_reads + alias_reads;
    if (expected_reason) {
        assert(!initialized && !timer_windows_ready);
        assert(strstr(diagnostics, expected_reason));
        assert(strstr(diagnostics, "freq="));
        if (injected && (!strcmp(injected, "frozen-init") || !strcmp(injected, "short-window")))
            assert(raw_reads == 1000002);
        fprintf(stderr, "%s", diagnostics);
        printf("{\"rejected\":true,\"reason\":\"%s\",\"raw_reads\":%u}\n", expected_reason,
               raw_reads);
        return 0;
    }
    assert(initialized);
    if (injected && !strcmp(injected, "fast-reads")) {
        assert(qualification_ns >= 1000000 && qualification_ns < 1010000);
        assert(qualification_reads < 1000000 && isb_count == qualification_reads);
    }
    injected = NULL;
#endif
#ifndef TIMER_PREPARED
    u64 qualification_ns = 0;
    unsigned qualification_reads = 0;
#endif
    frozen = !other;
#ifndef AURA_WINDOWS
    frozen = false;
#endif
    reads = raw_reads = alias_reads = 0;
    read_limit = 200000;
    u64 before = wall;
    udelay(1000);
    u64 elapsed = wall - before;
    assert(elapsed >= 1000000 && elapsed < 1020000);
    u64 first = get_ticks();
    wall += 1000000000;
    u64 second = get_ticks();
    assert(second - first >= 1000000000 && second - first < 1000001000);
#ifdef TIMER_PREPARED
    u64 actual = hv_host_counter(), expected = raw_epoch + wall;
    assert(actual >= expected && actual - expected <= 1000);
    u64 regs[32] = {0};
    if (!other) {
        assert(guest_counter_read(true, regs));
        expected = raw_epoch + wall;
        assert(regs[4] >= expected && regs[4] - expected <= 1000);
        assert(!guest_counter_read(false, regs));
        actual = hv_guest_timer_counter(false);
        expected = raw_epoch + wall - 1000000;
        assert(actual >= expected && actual - expected <= 1000);
        assert(hv_guest_timer_counter(true) > actual);
    }
    u64 values[] = {0, 1, 2, 3, UINT64_MAX};
    for (unsigned i = 0; i < 5; i++)
        assert(timer_scale_alias(values[i]) == (u64)(((__uint128_t)values[i] * 125) / 3));
#endif
    u64 timeout = timeout_calculate(1000);
    assert(!timeout_expired(timeout));
    wall += 1000000;
    assert(timeout_expired(timeout));
    dwc3_dev_t d = {0};
    before = wall;
    assert(usb_dwc3_ep_command(&d, 2, 1, 0, 0, 0) == -1);
    assert(wall - before >= 990000 && wall - before < 1200000);
    complete_after = mmio_reads + 3;
    before = wall;
    assert(usb_dwc3_ep_command(&d, 2, 1, 0, 0, 0) == 0 && wall - before < 10000);
#ifdef AURA_WINDOWS
#ifdef TIMER_PREPARED
    assert(other ? !alias_reads : !raw_reads);
#endif
#else
    assert(!alias_reads);
#endif
    printf("{\"stalled\":false,\"delay_ns\":%llu,\"raw_reads\":%u,\"alias_reads\":%u,"
           "\"qualification_ns\":%llu,\"qualification_reads\":%u,\"isb_count\":%u}\n",
           (unsigned long long)elapsed, raw_reads, alias_reads,
           (unsigned long long)qualification_ns, qualification_reads, isb_count);
}
