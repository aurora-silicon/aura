/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#include "hv.h"
#include "adt.h"
#include "assert.h"
#include "cpu_regs.h"
#include "display.h"
#include "gxf.h"
#include "memory.h"
#include "pcie.h"
#include "smp.h"
#include "string.h"
#include "usb.h"
#include "utils.h"

#define HV_TICK_RATE      1000
#define HV_SLOW_TICK_RATE 1

DECLARE_SPINLOCK(bhl);

void hv_enter_guest(u64 x0, u64 x1, u64 x2, u64 x3, void *entry);
void hv_exit_guest(void) __attribute__((noreturn));

extern char _hv_vectors_start[0];

u64 hv_tick_interval;
u64 hv_secondary_tick_interval;

int hv_pinned_cpu;
int hv_want_cpu;

static bool hv_has_ecv;
static bool hv_should_exit[MAX_CPUS];
bool hv_started_cpus[MAX_CPUS];
u64 hv_cpus_in_guest;
u64 hv_saved_sp[MAX_CPUS];

static u64 hv_cpu_state_regs[MAX_CPUS];

static bool hv_cpu_state_access(struct exc_info *ctx, u64 ipa, u64 *val, bool write, int width)
{
    if (1 << width != 4)
        return false;
    if (write)
        return hv_pa_rw(ctx, ipa, val, write, width);

    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        u64 base = hv_cpu_state_regs[cpu];
        if (!base)
            continue;
        if (ipa != base)
            continue;

        /* Clear out the lower byte when the core hasn't been started by the guest yet */
        if (!hv_pa_rw(ctx, ipa, val, write, width))
            return false;
        if (!hv_started_cpus[cpu])
            *val &= ~0xffUL;
        return true;
    }

    return false;
}

static bool hv_cpu_state_setup_hook(void)
{
    int node;

    memset(hv_cpu_state_regs, 0, sizeof(hv_cpu_state_regs));

    node = adt_path_offset(adt, "/cpus");
    if (node < 0)
        return false;

    ADT_FOREACH_CHILD(adt, node)
    {
        u32 cpu;
        u64 reg[2];

        if (ADT_GETPROP(adt, node, "cpu-id", &cpu) != sizeof(cpu))
            return false;
        if (ADT_GETPROP_ARRAY(adt, node, "cpu-impl-reg", reg) != sizeof(reg))
            return false;

        hv_cpu_state_regs[cpu] = reg[0] + 0x100;
        if (hv_map_hook(hv_cpu_state_regs[cpu], hv_cpu_state_access, 8) < 0)
            return false;
    }

    sysop("dsb ishst");
    sysop("tlbi vmalls12e1is");
    sysop("dsb ish");
    sysop("isb");
    return true;
}

struct hv_secondary_info_t {
    uint64_t hcr;
    uint64_t hacr;
    uint64_t vtcr, vttbr;
    uint64_t mdcr;
    uint64_t mdscr;
    uint64_t amx_ctl;
    uint64_t apvmkeylo, apvmkeyhi, apsts;
    uint64_t actlr_el2;
    uint64_t actlr_el1;
    uint64_t cnthctl;
    uint64_t sprr_config;
    uint64_t gxf_config;
    uint64_t agt_cnt_rdir_el1;
    uint64_t agt_cnt_rdir_el12;
};

static struct hv_secondary_info_t hv_secondary_info;
static u64 hv_secondary_regs[MAX_CPUS][4];
static bool hv_shutdown_failed;
static bool hv_setup_pending[MAX_CPUS];

static void hv_enable_sme_zt0(void)
{
    u64 sme = FIELD_GET(ID_AA64PFR1_SME, mrs(SYS_ID_AA64PFR1_EL1));
    if (sme != ID_AA64PFR1_SME2)
        return;

    /* Don't trap SME instructions to EL2 */
    reg_set(SYS_SMCR_EL2, SMCR_EZT0);
}

static int hv_join_cpus(void)
{
    int ret = 0;
    spin_lock(&bhl);
    for (int i = 0; i < MAX_CPUS; i++)
        hv_should_exit[i] = true;
    spin_unlock(&bhl);

    if (hv_wdt_stop()) {
        printf("HV: watchdog CPU did not stop; returning to proxy\n");
        ret = -1;
    }

    for (int i = 0; i < MAX_CPUS; i++) {
        if (i == boot_cpu_idx)
            continue;
        spin_lock(&bhl);
        bool started = hv_started_cpus[i] || hv_setup_pending[i];
        spin_unlock(&bhl);
        if (!started)
            continue;
        printf("HV: Waiting for CPU %d to exit\n", i);
        if (smp_wait(i, NULL)) {
            printf("HV: CPU %d did not exit; returning to proxy\n", i);
            ret = -1;
        } else {
            spin_lock(&bhl);
            hv_started_cpus[i] = false;
            hv_setup_pending[i] = false;
            spin_unlock(&bhl);
        }
    }
    hv_shutdown_failed = ret != 0;
    return ret;
}

int hv_init(void)
{
    /* Keep shared guest state intact until a previous session has joined. */
    if (hv_shutdown_failed && hv_join_cpus())
        return -1;
    pcie_shutdown();
    // Make sure we wake up DCP if we put it to sleep, just quiesce it to match ADT
    if (display_is_external && display_start_dcp() >= 0)
        display_shutdown(DCP_QUIESCED);
    // reenable hpm interrupts for the guest for unused iodevs
    usb_hpm_restore_irqs(0);
    smp_start_secondaries();
    smp_set_wfe_mode(true);
    hv_wdt_init();

    hv_pt_init();

    // Configure hypervisor defaults
    hv_write_hcr(HCR_API | // Allow PAuth instructions
                 HCR_APK | // Allow PAuth key registers
                 HCR_TEA | // Trap external aborts
                 HCR_E2H | // VHE mode (forced)
                 HCR_RW |  // AArch64 guest
                 HCR_AMO | // Trap SError exceptions
                 HCR_VM);  // Enable stage 2 translation

    // No guest vectors initially
    msr(VBAR_EL12, 0);

    // Compute tick interval
    hv_tick_interval = mrs(CNTFRQ_EL0) / HV_TICK_RATE;

    hv_has_ecv = mrs(ID_AA64MMFR0_EL1) & (0xfULL << 60);

    if (hv_has_ecv) {
        printf("HV: ECV enabled\n");
        reg_set(CNTHCTL_EL2,
                CNTHCTL_EL1NVVCT | CNTHCTL_EL1NVPCT | CNTHCTL_EL1TVT | CNTHCTL_EL1PCTEN);
        hv_secondary_tick_interval = mrs(CNTFRQ_EL0) / HV_SLOW_TICK_RATE;
    } else {
        printf("HV: No ECV supported\n");
        // Enable physical timer for EL1
        msr(CNTHCTL_EL2, CNTHCTL_EL1PTEN | CNTHCTL_EL1PCTEN);

        hv_secondary_tick_interval = hv_tick_interval;
    }

    // Set deep WFI back to defaults
    if (cpu_features->apple_sysregs_unlocked)
        reg_mask(SYS_IMP_APL_CYC_OVRD, CYC_OVRD_WFI_MODE_MASK, CYC_OVRD_WFI_MODE(0));

    hv_enable_sme_zt0();

    sysop("dsb ishst");
    sysop("tlbi alle1is");
    sysop("dsb ish");
    sysop("isb");
    return 0;
}

static void hv_set_gxf_vbar(void)
{
    msr(SYS_IMP_APL_VBAR_GL1, _hv_vectors_start);
}

int hv_start(void *entry, u64 regs[4])
{
    if (boot_cpu_idx == -1) {
        printf("Boot CPU has not been found, can't start hypervisor\n");
        return -1;
    }
    if (hv_shutdown_failed) {
        hv_join_cpus();
        /* A host may already have started a new watchdog. Cleanup stops it,
         * so do not enter a new guest in this same request. */
        printf("HV: previous session required cleanup; retry guest initialization\n");
        return -1;
    }

    if (!hv_cpu_state_setup_hook()) {
        printf("CPU state hook setup failed, can't start the hypervisor\n");
        return -1;
    }

    memset(hv_should_exit, 0, sizeof(hv_should_exit));
    memset(hv_started_cpus, 0, sizeof(hv_started_cpus));

    hv_started_cpus[boot_cpu_idx] = true;

    msr(VMPIDR_EL2, mrs(MPIDR_EL1));
    msr(VBAR_EL1, _hv_vectors_start);

    if (gxf_enabled())
        gl2_call(hv_set_gxf_vbar, 0, 0, 0, 0);

    hv_secondary_info.hcr = mrs(HCR_EL2);
    hv_secondary_info.hacr = mrs(HACR_EL2);
    hv_secondary_info.vtcr = mrs(VTCR_EL2);
    hv_secondary_info.vttbr = mrs(VTTBR_EL2);
    hv_secondary_info.mdcr = mrs(MDCR_EL2);
    hv_secondary_info.mdscr = mrs(MDSCR_EL1);
    hv_secondary_info.amx_ctl = mrs(SYS_IMP_APL_AMX_CTL_EL2);
    hv_secondary_info.apvmkeylo = mrs(SYS_IMP_APL_APVMKEYLO_EL2);
    hv_secondary_info.apvmkeyhi = mrs(SYS_IMP_APL_APVMKEYHI_EL2);
    hv_secondary_info.apsts = mrs(SYS_IMP_APL_APSTS_EL12);
    hv_secondary_info.actlr_el2 = mrs(ACTLR_EL2);
    if (cpu_features->actlr_el2)
        hv_secondary_info.actlr_el1 = mrs(SYS_ACTLR_EL12);
    else
        hv_secondary_info.actlr_el1 = mrs(SYS_IMP_APL_ACTLR_EL12);
    hv_secondary_info.cnthctl = mrs(CNTHCTL_EL2);
    hv_secondary_info.sprr_config = mrs(SYS_IMP_APL_SPRR_CONFIG_EL1);
    hv_secondary_info.gxf_config = mrs(SYS_IMP_APL_GXF_CONFIG_EL1);
    if (cpu_features->counter_redirect) {
        hv_secondary_info.agt_cnt_rdir_el1 = mrs(SYS_IMP_APL_AGTCNTRDIR_EL1);
        hv_secondary_info.agt_cnt_rdir_el12 = mrs(SYS_IMP_APL_AGTCNTRDIR_EL12);
    }

    hv_arm_tick(false);
    hv_pinned_cpu = -1;
    hv_want_cpu = -1;
    hv_cpus_in_guest = BIT(smp_id());

    hv_enter_guest(regs[0], regs[1], regs[2], regs[3], entry);

    __atomic_and_fetch(&hv_cpus_in_guest, ~BIT(smp_id()), __ATOMIC_ACQUIRE);
    spin_lock(&bhl);
    hv_started_cpus[boot_cpu_idx] = false;
    spin_unlock(&bhl);

    printf("HV: Exiting hypervisor (main CPU)\n");
    // Wait a bit for guest CPUs already in the process of exiting.
    udelay(200000);
    int ret = hv_join_cpus();
    if (!ret)
        printf("HV: All CPUs exited\n");
    return ret;
}

static void hv_init_secondary(struct hv_secondary_info_t *info)
{
    if (cpu_features->apple_sysregs_unlocked)
        gxf_init();

    msr(VMPIDR_EL2, mrs(MPIDR_EL1));
    msr(VBAR_EL1, _hv_vectors_start);

    msr(HCR_EL2, info->hcr);
    msr(HACR_EL2, info->hacr);
    msr(VTCR_EL2, info->vtcr);
    msr(VTTBR_EL2, info->vttbr);
    msr(MDCR_EL2, info->mdcr);
    msr(MDSCR_EL1, info->mdscr);

    if (cpu_features->apple_sysregs_unlocked) {
        msr(SYS_IMP_APL_AMX_CTL_EL2, info->amx_ctl);
        msr(SYS_IMP_APL_APVMKEYLO_EL2, info->apvmkeylo);
        msr(SYS_IMP_APL_APVMKEYHI_EL2, info->apvmkeyhi);
        msr(SYS_IMP_APL_APSTS_EL12, info->apsts);
    }
    msr(ACTLR_EL2, info->actlr_el2);
    if (cpu_features->actlr_el2)
        msr(SYS_ACTLR_EL12, info->actlr_el1);
    else
        msr(SYS_IMP_APL_ACTLR_EL12, info->actlr_el1);
    if (cpu_features->apple_sysregs_unlocked) {
        msr(SYS_IMP_APL_SPRR_CONFIG_EL1, info->sprr_config);
        /* GXF_CONFIG is UNDEF unless SPRR is enabled */
        if (info->sprr_config & SPRR_CONFIG_EN)
            msr(SYS_IMP_APL_GXF_CONFIG_EL1, info->gxf_config);
    }
    if (cpu_features->counter_redirect) {
        msr(SYS_IMP_APL_AGTCNTRDIR_EL1, info->agt_cnt_rdir_el1);
        msr(SYS_IMP_APL_AGTCNTRDIR_EL12, info->agt_cnt_rdir_el12);
    }

    if (cpu_features->apple_sysregs_unlocked)
        reg_mask(SYS_IMP_APL_CYC_OVRD, CYC_OVRD_WFI_MODE_MASK, CYC_OVRD_WFI_MODE(0));

    hv_enable_sme_zt0();

    // For M3 and up, CNTHCTL_EL2 must be written after the counter redirection
    sysop("isb");
    msr(CNTHCTL_EL2, info->cnthctl);

    if (gxf_enabled())
        gl2_call(hv_set_gxf_vbar, 0, 0, 0, 0);

    hv_arm_tick(true);
}

static void hv_enter_secondary(void *entry, u64 regs[4])
{
    hv_enter_guest(regs[0], regs[1], regs[2], regs[3], entry);

    spin_lock(&bhl);

    printf("HV: Exiting from CPU %d\n", smp_id());

    __atomic_and_fetch(&hv_cpus_in_guest, ~BIT(smp_id()), __ATOMIC_ACQUIRE);

    hv_started_cpus[smp_id()] = false;
    spin_unlock(&bhl);
}

int hv_start_secondary(int cpu, void *entry, u64 regs[4])
{
    if (cpu < 0 || cpu >= MAX_CPUS || cpu == boot_cpu_idx || !regs || hv_shutdown_failed)
        return -1;

    printf("HV: Initializing secondary %d\n", cpu);
    iodev_console_flush();

    hv_setup_pending[cpu] = true;
    int ret = mmu_init_secondary(cpu);
    if (ret)
        goto setup_failed;
    iodev_console_flush();
    ret = smp_call4(cpu, hv_init_secondary, (u64)&hv_secondary_info, 0, 0, 0);
    if (!ret)
        ret = smp_wait(cpu, NULL);
    if (ret)
        goto setup_failed;
    hv_setup_pending[cpu] = false;
    iodev_console_flush();

    printf("HV: Entering guest secondary %d at %p\n", cpu, entry);
    memcpy(hv_secondary_regs[cpu], regs, sizeof(hv_secondary_regs[cpu]));
    hv_started_cpus[cpu] = true;
    __atomic_or_fetch(&hv_cpus_in_guest, BIT(cpu), __ATOMIC_ACQUIRE);

    iodev_console_flush();
    /* An acknowledgement timeout can still have a late guest entry. Retain
     * the guest CPU state so exit waits for it instead of declaring it gone. */
    ret = smp_call4(cpu, hv_enter_secondary, (u64)entry, (u64)hv_secondary_regs[cpu], 0, 0);
    if (ret == SMP_TIMEOUT)
        hv_shutdown_failed = true;
    else if (ret) {
        hv_started_cpus[cpu] = false;
        __atomic_and_fetch(&hv_cpus_in_guest, ~BIT(cpu), __ATOMIC_ACQUIRE);
    }
    return ret;

setup_failed:
    if (ret == SMP_TIMEOUT)
        hv_shutdown_failed = true;
    else
        hv_setup_pending[cpu] = false;
    return ret;
}

void hv_exit_cpu(int cpu)
{
    if (cpu == -1)
        cpu = smp_id();

    printf("HV: Requesting exit of CPU#%d from the guest\n", cpu);
    hv_should_exit[cpu] = true;
}

void hv_rendezvous(void)
{
    int timeout = 1000000;

    if (!__atomic_load_n(&hv_cpus_in_guest, __ATOMIC_ACQUIRE))
        return;

    /* IPI all CPUs. This might result in spurious IPIs to the guest... */
    for (int i = 0; i < MAX_CPUS; i++) {
        if (i != smp_id() && hv_started_cpus[i]) {
            smp_send_ipi(i);
        }
    }

    while (timeout--) {
        if (!__atomic_load_n(&hv_cpus_in_guest, __ATOMIC_ACQUIRE))
            return;
    }

    hv_panic("HV: Failed to rendezvous, missing CPUs: 0x%lx (current: %d)\n",
             __atomic_load_n(&hv_cpus_in_guest, __ATOMIC_ACQUIRE), smp_id());
}

bool hv_switch_cpu(int cpu)
{
    if (cpu > MAX_CPUS || cpu < 0 || !hv_started_cpus[cpu]) {
        printf("HV: CPU #%d is inactive or invalid\n", cpu);
        return false;
    }
    printf("HV: switching to CPU #%d\n", cpu);
    hv_want_cpu = cpu;
    hv_rendezvous();
    return true;
}

void hv_pin_cpu(int cpu)
{
    hv_pinned_cpu = cpu;
}

void hv_write_hcr(u64 val)
{
    if (gxf_enabled() && !in_gl12())
        gl2_call(hv_write_hcr, val, 0, 0, 0);
    else
        msr(HCR_EL2, val);
}

u64 hv_get_spsr(void)
{
    if (in_gl12())
        return mrs(SYS_IMP_APL_SPSR_GL1);
    else
        return mrs(SPSR_EL2);
}

void hv_set_spsr(u64 val)
{
    if (in_gl12())
        return msr(SYS_IMP_APL_SPSR_GL1, val);
    else
        return msr(SPSR_EL2, val);
}

u64 hv_get_esr(void)
{
    if (in_gl12())
        return mrs(SYS_IMP_APL_ESR_GL1);
    else
        return mrs(ESR_EL2);
}

u64 hv_get_far(void)
{
    if (in_gl12())
        return mrs(SYS_IMP_APL_FAR_GL1);
    else
        return mrs(FAR_EL2);
}

u64 hv_get_afsr1(void)
{
    if (in_gl12())
        return mrs(SYS_IMP_APL_AFSR1_GL1);
    else
        return mrs(AFSR1_EL2);
}

u64 hv_get_elr(void)
{
    if (in_gl12())
        return mrs(SYS_IMP_APL_ELR_GL1);
    else
        return mrs(ELR_EL2);
}

void hv_set_elr(u64 val)
{
    if (in_gl12())
        return msr(SYS_IMP_APL_ELR_GL1, val);
    else
        return msr(ELR_EL2, val);
}

void hv_arm_tick(bool secondary)
{
    if (secondary)
        msr(CNTP_TVAL_EL0, hv_secondary_tick_interval);
    else
        msr(CNTP_TVAL_EL0, hv_tick_interval);
    msr(CNTP_CTL_EL0, CNTx_CTL_ENABLE);
}

void hv_maybe_exit(void)
{
    if (hv_should_exit[smp_id()]) {
        hv_exit_guest();
    }
}

void hv_tick(struct exc_info *ctx)
{
    hv_wdt_pet();
    iodev_handle_events(uartproxy_iodev);
    if (iodev_can_read(uartproxy_iodev)) {
        printf("HV: User interrupt\n");
        iodev_console_flush();
        if (hv_pinned_cpu == -1 || hv_pinned_cpu == smp_id())
            hv_exc_proxy(ctx, START_HV, HV_USER_INTERRUPT, NULL);
    }
    hv_vuart_poll();
}
