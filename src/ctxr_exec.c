/* SPDX-License-Identifier: MIT */

#include "ctxr_exec.h"
#include "cpu_regs.h"
#include "exception.h"
#include "fb.h"
#include "malloc.h"
#include "memory.h"
#include "smp.h"
#include "string.h"
#include "utils.h"
#include "xnuboot.h"

/*
 * A boot object with no __TEXT_BOOT_EXEC (kcbz 0) runs without SPTM, but iBoot
 * leaves CTXR windows armed and locked on the secondary CPUs. At EL2 they can
 * then execute only inside those windows, so the OS has to run in one.
 */

bool ctxr_range(enum ctxr_range range, u64 *start, u64 *end)
{
    u64 lower = 0, upper = 0;
    int exc_before = exc_count;

    /* Only EL2 can see them. Not every chip has them, and reading one there faults. */
    if (!in_el2())
        return false;

    exc_guard = GUARD_MARK | GUARD_SILENT;
    switch (range) {
        case CTXR_RANGE_A:
            lower = mrs(SYS_IMP_APL_CTXR_A_LWR_EL1);
            upper = mrs(SYS_IMP_APL_CTXR_A_UPR_EL1);
            break;
        case CTXR_RANGE_B:
            lower = mrs(SYS_IMP_APL_CTXR_B_LWR_EL1);
            upper = mrs(SYS_IMP_APL_CTXR_B_UPR_EL1);
            break;
        case CTXR_RANGE_C:
            lower = mrs(SYS_IMP_APL_CTXR_C_LWR_EL1);
            upper = mrs(SYS_IMP_APL_CTXR_C_UPR_EL1);
            break;
        case CTXR_RANGE_D:
            lower = mrs(SYS_IMP_APL_CTXR_D_LWR_EL1);
            upper = mrs(SYS_IMP_APL_CTXR_D_UPR_EL1);
            break;
        case CTRR_RANGE_C:
            lower = mrs(SYS_IMP_APL_CTRR_C_LWR_EL1);
            upper = mrs(SYS_IMP_APL_CTRR_C_UPR_EL1);
            break;
        case CTRR_RANGE_D:
            lower = mrs(SYS_IMP_APL_CTRR_D_LWR_EL1);
            upper = mrs(SYS_IMP_APL_CTRR_D_UPR_EL1);
            break;
    }
    exc_guard = GUARD_OFF;

    if (exc_count != exc_before || !lower || upper < lower)
        return false;
    *start = lower;
    *end = upper + SZ_4K;
    return true;
}

static const u32 probe_insns[] = {
    0xd2800020, /* mov x0, #1 */
    0xd65f03c0, /* ret */
};

/*
 * A secondary that took the probe gets this long to answer, far more than two instructions
 * or a fault take. smp_call4() has already given it 1000 ms to take it.
 */
#define CTXR_PROBE_TIMEOUT_MS 500

/* The CPU still holding an unanswered probe, or -1. */
static int probe_stuck_cpu = -1;

/*
 * Nothing answered, so the question stays open for a later call. Say so once, and
 * what CTXR_D reads, since an OS started now runs outside any window on this CPU.
 */
static bool unprobed_warned;

static void ctxr_exec_warn_unprobed(const char *why)
{
    if (unprobed_warned)
        return;
    unprobed_warned = true;

    u64 lower, upper;

    if (ctxr_range(CTXR_RANGE_D, &lower, &upper))
        printf("CTXR exec: WARNING: %s, but CTXR_D is armed at [0x%lx, 0x%lx): the OS will run "
               "outside it, on this CPU only\n",
               why, lower, upper);
    else
        printf("CTXR exec: %s and CTXR_D is not armed: treating the machine as not confined\n",
               why);
}

/*
 * Ask a secondary to run code outside the boot object. This is the whole test:
 * it is not limited to the chips where the confinement was first seen (T8140,
 * then T8142), because any Mac whose iBoot entered a kcbz-0 boot object may
 * leave the windows armed, and one that did not (M1 to M3, or a raw boot
 * object) runs the probe and is not confined.
 */
static const char *ctxr_exec_problem(void);

bool ctxr_exec_confined(void)
{
    static int confined = -1;

    if (confined >= 0)
        return confined;

    int cpu;
    for (cpu = 0; cpu < MAX_CPUS; cpu++)
        if (cpu != boot_cpu_idx && smp_is_alive(cpu))
            break;
    if (cpu == MAX_CPUS) {
        u64 lower, upper;

        /*
         * The boot core runs outside CTXR_D now, but a reset brings it back like a secondary
         * (an S2R wake). It can't run the OS there unless the OS is in the window.
         */
        if (ctxr_range(CTXR_RANGE_D, &lower, &upper) && !ctxr_exec_problem()) {
            printf("CTXR exec: no secondary is running; CTXR_D is armed, so a core leaving reset, "
                   "the boot core's too, is taken as confined\n");
            confined = 1;
            return true;
        }
        ctxr_exec_warn_unprobed("no secondary is running to probe");
        return false;
    }

    u32 *probe = memalign(SZ_16K, SZ_16K);
    if (!probe) {
        ctxr_exec_warn_unprobed("no memory to probe a secondary");
        return false;
    }
    memcpy(probe, probe_insns, sizeof(probe_insns));
    dc_cvau_range(probe, sizeof(probe_insns));
    ic_ivau_range(probe, sizeof(probe_insns));
    sysop("dsb ish");
    sysop("isb");

    u64 ret = 0;
    exc_last_esr = 0;
    exc_guard = GUARD_RETURN | GUARD_SILENT;
    int err = smp_call1(cpu, probe, 0);
    if (!err)
        err = smp_wait_timed(cpu, &ret, CTXR_PROBE_TIMEOUT_MS);
    exc_guard = GUARD_OFF;

    if (err) {
        /* Not asked again: that could wait out the same timeout and lose another page. */
        confined = 0;
        /* The secondary may still run the probe later, so its page is not freed. */
        if (err == SMP_TIMEOUT) {
            /*
             * The call is still in the CPU's slot, and kboot never hands off with a call
             * pending, so the OS can't start this boot. ctxr_exec_os_may_start() stops it
             * before any device is handed over.
             */
            probe_stuck_cpu = cpu;
            fb_set_active(true);
            printf("\nCTXR exec: CPU %d did not take or did not answer the probe, and still holds "
                   "it.\n"
                   "  Not starting the OS.\n"
                   "  Hold the power button to shut down, then hold it again to choose another\n"
                   "  system.\n",
                   cpu);
            return false;
        }
        /*
         * A probe that was never sent says nothing about confinement. Treating it as confined
         * would refuse to boot a Mac that may have no window at all (M1 to M3), while booting
         * outside the window at worst leaves the confined secondaries offline.
         */
        printf("CTXR exec: CPU %d did not run the probe (%d); treating the secondaries as "
               "not confined\n",
               cpu, err);
        return false;
    }
    free(probe);

    confined = ret != 1;
    if (confined)
        printf("CTXR exec: CPU %d could not run the heap probe (ESR 0x%lx): secondaries are "
               "confined at EL2\n",
               cpu, exc_last_esr);
    return confined;
}

/*
 * aurora-installer pads the boot object of every chip that boots this way: 16 MiB
 * of __TEXT_EXEC for aura, then a 2 MiB aligned region of 128 MiB.
 *
 * The OS gets that region and no more, whatever the object armed. The part of
 * CTXR_D below it holds the device tree, the trust cache and this image with the
 * reset vector the secondaries are locked to. Any of CTXR_D above it stays reserved:
 * the OS does not pool it for code, and EL0 cannot execute there.
 */
#define CTXR_EXEC_AURA_SLOT   ((u64)16 * SZ_1M)
#define CTXR_EXEC_ALIGN       ((u64)2 * SZ_1M)
#define CTXR_EXEC_WINDOW_SIZE ((u64)128 * SZ_1M)

static bool window_checked;
static const char *window_problem;
static u64 window_base, window_size;

static bool ranges_overlap(u64 start_a, u64 end_a, u64 start_b, u64 end_b)
{
    return start_a < end_b && end_a > start_b;
}

/*
 * Whether [start, end) holds something kboot keeps from the OS: the firmware RO range
 * where aura knows one (T8140), the windows A to C, or what the secondaries can't write.
 */
static bool ctxr_exec_holds_ro_range(u64 start, u64 end)
{
    /* Windows A to C, then CTRR_C and CTRR_D, which kboot reads only with locked sysregs. */
    static const enum ctxr_range ranges[] = {CTXR_RANGE_A, CTXR_RANGE_B, CTXR_RANGE_C, CTRR_RANGE_C,
                                             CTRR_RANGE_D};
    size_t count = cpu_features->apple_sysregs_unlocked ? 3 : ARRAY_SIZE(ranges);
    u64 ro_start, ro_end;
    int status = memory_fw_ro_range(&ro_start, &ro_end);

    if (status < 0 || (status && ranges_overlap(ro_start, ro_end, start, end)))
        return true;
    for (size_t i = 0; i < count; i++)
        if (ctxr_range(ranges[i], &ro_start, &ro_end) &&
            ranges_overlap(ro_start, ro_end, start, end))
            return true;
    return false;
}

/* Returns why there is no window to use, or NULL having set it. */
static const char *ctxr_exec_find_window(u64 *base, u64 *size)
{
    u64 lower = 0, armed_end = 0;
    u64 start = ALIGN_UP((u64)_base + CTXR_EXEC_AURA_SLOT, CTXR_EXEC_ALIGN);
    u64 end = start + CTXR_EXEC_WINDOW_SIZE;

    if (ctxr_range(CTXR_RANGE_D, &lower, &armed_end))
        armed_end = ALIGN_DOWN(armed_end, SZ_16K);
    printf("CTXR exec: armed [0x%lx, 0x%lx)\n", lower, armed_end);
    if (armed_end <= lower)
        return "CTXR_D reads as empty";
    if ((u64)_base < lower || (u64)_end > start)
        return "aura is not inside CTXR_D and its slot";
    if (armed_end < end)
        return "this Stage 1 did not arm a large enough window";
    if (ctxr_exec_holds_ro_range(start, end))
        return "a read-only firmware range is inside it";
    /* A chainloaded image ends with SEPFW and boot_args, so this also clears its payload. */
    if (boot_args_addr < end && boot_args_addr + SZ_16K > start)
        return "boot_args is inside it";
    /* The heap holds the device tree and the initrd the OS is about to read. */
    if (cur_boot_args.top_of_kernel_data < end)
        return "the heap starts inside it";

    printf("CTXR exec: OS window [0x%lx, 0x%lx); 0x%lx armed bytes above it stay reserved\n", start,
           end, armed_end - end);
    *base = start;
    *size = end - start;
    return NULL;
}

/* Decided once a boot. Returns why there is no window to use, or NULL. */
static const char *ctxr_exec_problem(void)
{
    if (!window_checked) {
        window_checked = true;
        window_problem = ctxr_exec_find_window(&window_base, &window_size);
    }
    return window_problem;
}

bool ctxr_exec_window(u64 *base, u64 *size)
{
    if (!ctxr_exec_confined() || ctxr_exec_problem())
        return false;
    if (base)
        *base = window_base;
    if (size)
        *size = window_size;
    return true;
}

bool ctxr_exec_os_may_start(void)
{
    bool confined = ctxr_exec_confined();

    /* ctxr_exec_confined() said why on the panel. */
    if (probe_stuck_cpu >= 0)
        return false;
    if (!confined || !ctxr_exec_problem())
        return true;

    /* Shown on the panel: this is where a user without a USB host ends up. */
    fb_set_active(true);
    printf("\nCTXR exec: the secondary CPUs can only run code in a window, and there is none for\n"
           "the OS: %s.\n"
           "  Not starting the OS. Enroll Stage 1 again through the Recovery step of the Aurora\n"
           "  installer, then boot this system again.\n"
           "  Hold the power button to shut down, then hold it again to choose another system.\n",
           ctxr_exec_problem());
    return false;
}
