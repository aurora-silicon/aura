/* SPDX-License-Identifier: MIT */

#include "psci.h"
#include "smp.h"
#include "types.h"
#include "utils.h"

/*
 * PSCI for an OS the monitor runs at EL1. Apple has no EL3, so the OS reaches
 * the monitor with "method = smc": SMC is UNDEFINED at EL1 unless HCR_EL2.TSC
 * traps it to EL2 (monitor_enter_os sets TSC). The monitor owns the secondary
 * CPUs through the parking pool in smp.c, so CPU_ON is just a release into it.
 */

#define PSCI_0_2_FN(n)   (0x84000000 | (n))
#define PSCI_0_2_FN64(n) (0xc4000000 | (n))

#define PSCI_VERSION           PSCI_0_2_FN(0)
#define PSCI_CPU_SUSPEND       PSCI_0_2_FN(1)
#define PSCI_CPU_SUSPEND64     PSCI_0_2_FN64(1)
#define PSCI_CPU_OFF           PSCI_0_2_FN(2)
#define PSCI_CPU_ON            PSCI_0_2_FN(3)
#define PSCI_CPU_ON64          PSCI_0_2_FN64(3)
#define PSCI_AFFINITY_INFO     PSCI_0_2_FN(4)
#define PSCI_AFFINITY_INFO64   PSCI_0_2_FN64(4)
#define PSCI_MIGRATE_INFO_TYPE PSCI_0_2_FN(6)
#define PSCI_SYSTEM_OFF        PSCI_0_2_FN(8)
#define PSCI_SYSTEM_RESET      PSCI_0_2_FN(9)
#define PSCI_FEATURES          PSCI_0_2_FN(10)

#define SMCCC_VERSION       0x80000000
#define SMCCC_ARCH_FEATURES 0x80000001

#define PSCI_RET_SUCCESS        0
#define PSCI_RET_NOT_SUPPORTED  (-1)
#define PSCI_RET_INVALID_PARAMS (-2)
#define PSCI_RET_ALREADY_ON     (-4)
#define PSCI_RET_INTERNAL_FAIL  (-6)

#define PSCI_AFFINITY_ON  0
#define PSCI_AFFINITY_OFF 1

extern void _deep_wfi_helper(void); /* bare WFI, preserves x18-x30 across it */

static int mpidr_to_cpu(u64 mpidr)
{
    mpidr &= 0xffffff;
    for (int i = 0; i < MAX_CPUS; i++)
        /* The boot CPU carries a valid MPIDR but is not a parking-pool member
         * until it offlines itself, so match it explicitly. */
        if ((smp_is_alive(i) || i == boot_cpu_idx) && (smp_get_mpidr(i) & 0xffffff) == mpidr)
            return i;
    return -1;
}

static bool is_self(u64 mpidr)
{
    return (mpidr & 0xffffff) == (mrs(MPIDR_EL1) & 0xffffff);
}

/*
 * Whether a CPU is currently executing the OS. A secondary runs once it has a
 * dispatched target. The boot CPU runs the OS from the initial handoff and is
 * not a pool member (no flag) until a CPU_OFF parks it, which clears its target
 * and makes it a pool member; so it is on unless it has actually parked.
 */
static bool cpu_is_on(int cpu)
{
    if (cpu == boot_cpu_idx)
        return !smp_is_alive(cpu) || smp_os_started(cpu);
    return smp_os_started(cpu);
}

static s64 psci_cpu_on(u64 mpidr, u64 entry, u64 ctx)
{
    if (is_self(mpidr))
        return PSCI_RET_ALREADY_ON;
    int cpu = mpidr_to_cpu(mpidr);
    if (cpu < 0)
        return PSCI_RET_INVALID_PARAMS;
    if (cpu_is_on(cpu))
        return PSCI_RET_ALREADY_ON;
    /* Release the parked core into the OS at EL1 (monitor_enter_os), with
     * x0 = context_id. smp_call4 returns once the core has taken the target. */
    return smp_call4(cpu, (void *)entry, ctx, 0, 0, 0) == 0 ? PSCI_RET_SUCCESS
                                                            : PSCI_RET_INTERNAL_FAIL;
}

static s64 psci_affinity_info(u64 mpidr, u64 level)
{
    if (level != 0)
        return PSCI_RET_INVALID_PARAMS;
    if (is_self(mpidr))
        return PSCI_AFFINITY_ON;
    int cpu = mpidr_to_cpu(mpidr);
    if (cpu < 0)
        return PSCI_RET_INVALID_PARAMS;
    return cpu_is_on(cpu) ? PSCI_AFFINITY_ON : PSCI_AFFINITY_OFF;
}

static s64 psci_features(u64 fn)
{
    switch (fn) {
        case PSCI_VERSION:
        case PSCI_CPU_SUSPEND:
        case PSCI_CPU_SUSPEND64:
        case PSCI_CPU_OFF:
        case PSCI_CPU_ON:
        case PSCI_CPU_ON64:
        case PSCI_AFFINITY_INFO:
        case PSCI_AFFINITY_INFO64:
        case PSCI_MIGRATE_INFO_TYPE:
        case PSCI_SYSTEM_OFF:
        case PSCI_SYSTEM_RESET:
        case PSCI_FEATURES:
        case SMCCC_VERSION:
            return PSCI_RET_SUCCESS;
        default:
            return PSCI_RET_NOT_SUPPORTED;
    }
}

bool psci_handle_smc(u64 *regs)
{
    u64 fn = regs[0];

    /* SMCCC: an SMC32 call (bit 30 of the function ID clear) passes its
     * arguments in w1-w6; the upper 32 bits of x1-x6 are UNKNOWN and must be
     * ignored. Only x1-x3 are consumed here. */
    if (!(fn & BIT(30))) {
        regs[1] = (u32)regs[1];
        regs[2] = (u32)regs[2];
        regs[3] = (u32)regs[3];
    }

    switch (fn) {
        case PSCI_VERSION:
            regs[0] = (1 << 16) | 1; /* PSCI 1.1 */
            break;
        case SMCCC_VERSION:
            regs[0] = (1 << 16) | 2; /* SMCCC 1.2 */
            break;
        case PSCI_FEATURES:
            regs[0] = psci_features(regs[1]);
            break;
        case SMCCC_ARCH_FEATURES:
            regs[0] = PSCI_RET_NOT_SUPPORTED;
            break;
        case PSCI_MIGRATE_INFO_TYPE:
            regs[0] = 2; /* Trusted OS not present / migration not required */
            break;
        case PSCI_CPU_ON:
        case PSCI_CPU_ON64:
            regs[0] = psci_cpu_on(regs[1], regs[2], regs[3]);
            break;
        case PSCI_AFFINITY_INFO:
        case PSCI_AFFINITY_INFO64:
            regs[0] = psci_affinity_info(regs[1], regs[2]);
            break;
        case PSCI_CPU_SUSPEND:
        case PSCI_CPU_SUSPEND64:
            /* Retention idle: a pending EL1 interrupt wakes WFI at EL2. The
             * helper preserves the monitor's registers across the wipe. */
            _deep_wfi_helper();
            regs[0] = PSCI_RET_SUCCESS;
            break;
        case PSCI_CPU_OFF:
            /* The caller offlines itself: re-park by its own index, never a
             * reverse MPIDR lookup (the boot CPU is not a pool member and would
             * resolve to -1). */
            smp_reenter_park(smp_id()); /* no return */
            break;
        case PSCI_SYSTEM_OFF:
        case PSCI_SYSTEM_RESET:
            reboot(); /* no poweroff path yet; reset is the safe action */
            break;
        default:
            regs[0] = PSCI_RET_NOT_SUPPORTED;
            break;
    }
    return true;
}
