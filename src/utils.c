/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <stdarg.h>

#include "utils.h"
#include "cpu_regs.h"
#include "iodev.h"
#include "smp.h"
#include "types.h"
#include "utils.h"
#include "vsprintf.h"
#include "xnuboot.h"

bool is_mac;

static char ascii(char s)
{
    if (s < 0x20)
        return '.';
    if (s > 0x7E)
        return '.';
    return s;
}

void hexdump(const void *d, size_t len)
{
    u8 *data;
    size_t i, off;
    data = (u8 *)d;
    for (off = 0; off < len; off += 16) {
        printf("%08lx  ", off);
        for (i = 0; i < 16; i++) {
            if ((i + off) >= len)
                printf("   ");
            else
                printf("%02x ", data[off + i]);
        }

        printf(" ");
        for (i = 0; i < 16; i++) {
            if ((i + off) >= len)
                printf(" ");
            else
                printf("%c", ascii(data[off + i]));
        }
        printf("\n");
    }
}

void regdump(u64 addr, size_t len)
{
    u64 i, off;
    for (off = 0; off < len; off += 32) {
        printf("%016lx  ", addr + off);
        for (i = 0; i < 32; i += 4) {
            printf("%08x ", read32(addr + off + i));
        }
        printf("\n");
    }
}

int snprintf(char *buffer, size_t size, const char *fmt, ...)
{
    va_list args;
    int i;

    va_start(args, fmt);
    i = vsnprintf(buffer, size, fmt, args);
    va_end(args);
    return i;
}

int debug_printf(const char *fmt, ...)
{
    va_list args;
    char buffer[512];
    static bool line_start = true;
    int prefix = 0, i;

    /* Each line carries the time since power-on, from the architectural counter. */
    if (line_start) {
        u64 freq = mrs(CNTFRQ_EL0);
        u64 us = freq ? mrs(CNTPCT_EL0) / (freq / 1000000) : 0;
        prefix = snprintf(buffer, sizeof(buffer), "[%3lu.%06lu] ", (unsigned long)(us / 1000000),
                          (unsigned long)(us % 1000000));
    }
    va_start(args, fmt);
    i = vsnprintf(buffer + prefix, sizeof(buffer) - prefix, fmt, args);
    va_end(args);
    i = min(i + prefix, (int)(sizeof(buffer) - 1));
    line_start = i > 0 && buffer[i - 1] == '\n';

    iodev_console_write(buffer, i);

    return i - prefix;
}

void __assert_fail(const char *assertion, const char *file, unsigned int line, const char *function)
{
    printf("Assertion failed: '%s' on %s:%d:%s\n", assertion, file, line, function);
    flush_and_reboot();
}

#ifdef AURA_WINDOWS
bool timer_windows_ready;
u64 timer_windows_offset;

/* Run once on the boot CPU before entering any guest. Probe with a bounded
 * iteration count: the clock being qualified cannot provide its own timeout. */
bool timer_prepare_windows(void)
{
    if (chip_id != T8142)
        return true;
    if (__atomic_load_n(&timer_windows_ready, __ATOMIC_ACQUIRE))
        return true;
    u64 frequency = mrs(CNTFRQ_EL0);
    if (frequency != 1000000000) {
        printf("timer: reject frequency freq=%lu expected=1000000000\n", frequency);
        return false;
    }
    /* Serialize every calibration read. On T8142 the plain read loop can
     * exhaust the iteration cap in less than the required millisecond. */
    sysop("isb");
    u64 first = mrs(CNTPCT_EL0);
    sysop("isb");
    u64 alias_first = mrs(SYS_IMP_APL_CNTVCT_ALIAS_EL0);
    sysop("isb");
    u64 after = mrs(CNTPCT_EL0);
    if (after < first || after - first > 10000) {
        printf("timer: reject first-bracket freq=%lu raw=%lu..%lu alias=%lu width=%lu\n", frequency,
               first, after, alias_first, after - first);
        return false;
    }
    u64 start = first + (after - first) / 2;
    u64 now = after;
    unsigned i;
    for (i = 0; i < 1000000; i++) {
        sysop("isb");
        now = mrs(CNTPCT_EL0);
        if (now < after) {
            printf("timer: reject poll-backward freq=%lu start=%lu now=%lu iteration=%u\n",
                   frequency, after, now, i);
            return false;
        }
        if (now - after >= 1000000)
            break;
    }
    if (i == 1000000) {
        printf("timer: reject iteration-limit freq=%lu start=%lu now=%lu elapsed=%lu "
               "required=1000000 iterations=%u\n",
               frequency, after, now, now - after, i);
        return false;
    }
    sysop("isb");
    first = mrs(CNTPCT_EL0);
    sysop("isb");
    u64 alias_last = mrs(SYS_IMP_APL_CNTVCT_ALIAS_EL0);
    sysop("isb");
    after = mrs(CNTPCT_EL0);
    if (after < first || after - first > 10000 || alias_last < alias_first) {
        printf("timer: reject last-bracket freq=%lu raw=%lu..%lu width=%lu "
               "alias=%lu..%lu iterations=%u\n",
               frequency, first, after, after - first, alias_first, alias_last, i);
        return false;
    }
    u64 elapsed = first + (after - first) / 2 - start;
    u64 alias_elapsed = timer_scale_alias(alias_last - alias_first);
    u64 error = elapsed > alias_elapsed ? elapsed - alias_elapsed : alias_elapsed - elapsed;
    if (error > elapsed / 100 + 20000) {
        printf("timer: reject rate freq=%lu elapsed=%lu alias_delta=%lu scaled=%lu "
               "error=%lu tolerance=%lu iterations=%u\n",
               frequency, elapsed, alias_last - alias_first, alias_elapsed, error,
               elapsed / 100 + 20000, i);
        return false;
    }
    /* Anchor to the upper bracket, not an assumed power-on epoch. The first
     * alias read is no earlier than this observed architectural counter. */
    timer_windows_offset = after - timer_scale_alias(alias_last);
    __atomic_store_n(&timer_windows_ready, true, __ATOMIC_RELEASE);
    return true;
}

#endif

void udelay(u32 d)
{
    u64 delay = ((u64)d) * mrs(CNTFRQ_EL0) / 1000000;
    u64 val = get_ticks();
    while ((get_ticks() - val) < delay)
        ;
    sysop("isb");
}

u64 ticks_to_msecs(u64 ticks)
{
    // NOTE: only accurate if freq is even kHz
    return ticks / (mrs(CNTFRQ_EL0) / 1000);
}

u64 ticks_to_usecs(u64 ticks)
{
    // NOTE: only accurate if freq is even MHz
    return ticks / (mrs(CNTFRQ_EL0) / 1000000);
}

u64 timeout_calculate(u32 usec)
{
    u64 delay = ((u64)usec) * mrs(CNTFRQ_EL0) / 1000000;
    return get_ticks() + delay;
}

bool timeout_expired(u64 timeout)
{
    bool expired = get_ticks() > timeout;
    sysop("isb");
    return expired;
}

void flush_and_reboot(void)
{
    iodev_console_flush();
    reboot();
}

void spin_init(spinlock_t *lock)
{
    lock->lock = -1;
    lock->count = 0;
}

void spin_lock(spinlock_t *lock)
{
    s64 tmp;
    s64 me = smp_id();
    if (__atomic_load_n(&lock->lock, __ATOMIC_ACQUIRE) == me) {
        lock->count++;
        return;
    }

    __asm__ volatile("1:\n"
                     "mov\t%0, -1\n"
                     "2:\n"
                     "\tldaxr\t%0, %1\n"
                     "\tcmn\t%0, 1\n"
                     "\tbeq\t3f\n"
                     "\tldxr\t%0, %1\n"
                     "\tcmn\t%0, 1\n"
                     "\tbeq\t2b\n"
                     "\twfe\n"
                     "\tb\t1b\n"
                     "3:"
                     "\tstxr\t%w0, %2, %1\n"
                     "\tcbnz\t%w0, 2b\n"
                     : "=&r"(tmp), "+m"(lock->lock)
                     : "r"(me)
                     : "cc", "memory");

    assert(__atomic_load_n(&lock->lock, __ATOMIC_RELAXED) == me);
    lock->count++;
}

void spin_unlock(spinlock_t *lock)
{
    s64 me = smp_id();
    assert(__atomic_load_n(&lock->lock, __ATOMIC_RELAXED) == me);
    assert(lock->count > 0);
    if (!--lock->count)
        __atomic_store_n(&lock->lock, -1L, __ATOMIC_RELEASE);
}

bool is_heap(void *addr)
{
    u64 p = (u64)addr;
    u64 top_of_kernel_data = (u64)cur_boot_args.top_of_kernel_data;
    u64 top_of_ram = cur_boot_args.mem_size + cur_boot_args.phys_base;

    return p > top_of_kernel_data && p < top_of_ram;
}

bool supports_arch_retention(void)
{
    return mrs(AIDR_EL1) & AIDR_EL1_ARCH_RETENTION;
}

bool supports_gxf(void)
{
    return (mrs(AIDR_EL1) & AIDR_EL1_GXF) && cpu_features->mmu_sprr;
}

bool supports_pan(void)
{
    return (mrs(ID_AA64MMFR1_EL1) >> 20) & 0xf;
}

// TODO: update mapping?
u64 top_of_memory_alloc(size_t size)
{
    static bool guard_page_inserted = false;
    cur_boot_args.mem_size -= ALIGN_UP(size, SZ_16K);
    u64 ret = cur_boot_args.phys_base + cur_boot_args.mem_size;

    if (!guard_page_inserted) {
        cur_boot_args.mem_size -= SZ_16K;
        guard_page_inserted = true;
    } else {
        // If the guard page was already there, move it down and allocate
        // above it -- this is accomplished by simply shifting the allocated
        // region by one page up.
        ret += SZ_16K;
    }

    return ret;
}

extern void _deep_wfi_helper(void);
void deep_wfi(void)
{
    u64 cyc_ovrd;
    const bool apple_sysregs_unlocked = cpu_features->apple_sysregs_unlocked;

    if (!supports_arch_retention()) {
        // A7 - A11 does not support state retention across deep WFI
        // i.e. CPU always ends up at rvbar after deep WFI
        sysop("wfi");
        return;
    }

    if (apple_sysregs_unlocked) {
        cyc_ovrd = mrs(SYS_IMP_APL_CYC_OVRD);
        msr(SYS_IMP_APL_CYC_OVRD, cyc_ovrd | CYC_OVRD_WFI_MODE(3));
    }

    _deep_wfi_helper();

    if (apple_sysregs_unlocked)
        msr(SYS_IMP_APL_CYC_OVRD, cyc_ovrd);
}

void cpu_sleep(bool deep)
{
    if (deep) {
        switch (cpu_features->sleep_mode) {
            case SLEEP_GLOBAL:
                reg_mask(SYS_IMP_APL_ACC_OVRD, ACC_OVRD_DIS_L2_FLUSH_ACC_SLEEP_MASK,
                         ACC_OVRD_PWR_DN_SRM(3) | ACC_OVRD_DIS_L2_FLUSH_ACC_SLEEP(2) |
                             ACC_OVRD_TRAIN_DOWN_LINK(3) | ACC_OVRD_POWER_DOWN_CPM(3) |
                             ACC_OVRD_DISABLE_PIO_ON_WFI_CPU | ACC_OVRD_DEEP_SLEEP);
                break;

            case SLEEP_LEGACY:
                reg_set(SYS_IMP_APL_ACC_CFG, ACC_CFG_DEEP_SLEEP);
                break;

            default:
                break;
        }
    } else {
        if (cpu_features->apple_sysregs_unlocked) {
            reg_mask(SYS_IMP_APL_CYC_OVRD, CYC_OVRD_FIQ_MODE_MASK | CYC_OVRD_IRQ_MODE_MASK,
                     CYC_OVRD_FIQ_MODE(2) | CYC_OVRD_IRQ_MODE(2));
        }
    }

    if (cpu_features->apple_sysregs_unlocked) {
        // disable wfi retention mode to allow deepest sleep states
        reg_mask(SYS_IMP_APL_CYC_OVRD, CYC_OVRD_WFI_MODE_MASK,
                 CYC_OVRD_WFI_MODE(3) | CYC_OVRD_DISABLE_WFI_RET);
    }

    // enter deep sleep
    while (1) {
        sysop("isb");
        sysop("wfi");
    }
}
