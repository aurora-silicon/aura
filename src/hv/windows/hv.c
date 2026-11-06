/* SPDX-License-Identifier: MIT */

#include "hv.h"
#include "adt.h"
#include "aic.h"
#include "assert.h"
#include "cpu_regs.h"
#include "display.h"
#include "fb_capture.h"
#include "gxf.h"
#include "malloc.h"
#include "memory.h"
#include "mtp_handoff.h"
#include "pcie.h"
#include "serial_capture.h"
#include "smp.h"
#include "string.h"
#include "usb.h"
#include "utils.h"
#include "xnuboot.h"

#define HV_TICK_RATE      5000
#define HV_SLOW_TICK_RATE 1
/*
 * How often a CPU parked in the EL2 guest-WFI wait re-checks for work.
 *
 * This exists because holding the guest's WFI at EL2 (T8142 has no writable
 * CYC_OVRD, so the wait cannot be left to the guest) changes what can wake the
 * core.  At EL1 a *virtual* interrupt ends a WFI; at EL2 it does not, and the
 * only thing left is a physical one -- which for an idle secondary means its
 * own tick, at HV_SLOW_TICK_RATE.  One hertz.
 *
 * Windows drives its scheduler off a 64 Hz timer, so every timed wait in the
 * guest was being stretched by up to a second.  Measured: idle cores waking
 * ~5.5 times a second, and WinPE progressing at 90 syscalls a second when the
 * same boot had earlier averaged 2200.  It booted, just far too slowly to
 * finish.
 *
 * 1 kHz puts the wakeup floor an order of magnitude below anything Windows asks
 * for.  The cost is bounded and small: this wait is entered from the `handled`
 * fast path in hv_exc_sync(), which does not take the big hypervisor lock, so
 * nine idle cores re-checking at 1 kHz contend with nothing.
 */
#define HV_WFI_WAKE_RATE 1000

DECLARE_SPINLOCK(bhl);

void hv_enter_guest(u64 x0, u64 x1, u64 x2, u64 x3, void *entry);
void hv_exit_guest(void) __attribute__((noreturn));

extern char _hv_vectors_start[0];

u64 hv_tick_interval;
u64 hv_secondary_tick_interval;
u64 hv_wfi_wake_interval;

int hv_pinned_cpu;
int hv_want_cpu;

static bool hv_has_ecv;
static bool hv_should_exit[MAX_CPUS];
bool hv_started_cpus[MAX_CPUS];
u64 hv_cpus_in_guest;

/*
 * T8142's architectural CNTPCT view is redirected/trapped for the guest and
 * is not a dependable EL2 bookkeeping clock. Apple's always-on counter alias
 * is the same source used to emulate guest CNTPCT_EL0 reads.
 */
u64 hv_host_counter(void)
{
    if (chip_id == T8142)
        return mrs(SYS_IMP_APL_CNTVCT_ALIAS_EL0);

    return mrs(CNTPCT_EL0);
}

/*
 * Set for the duration of hv_rendezvous().  hv_cpus_in_guest is cleared ONLY by
 * hv_exc_entry(), and hv_exc_sync()'s fast path -- the one that handles an
 * Apple IMPDEF MSR trap and returns straight to the guest -- deliberately skips
 * it.  A CPU taking those traps back to back therefore stays marked "in guest"
 * no matter how many it services, and a rendezvous requested by any other core
 * can never complete.  Measured on the J414s: CPU 0 runs the pure-AIC software
 * timer reflection, its breadcrumbs read `Sa#sSa#s` (two full fast-path turns,
 * no slow-path entry) while every other CPU ends in `F`, and the HV panics with
 * "Failed to rendezvous, missing CPUs: 0x1".  The fast path consults this flag
 * so it can take the slow round trip exactly when one is outstanding.
 */
u64 hv_rendezvous_pending;
u64 hv_saved_sp[MAX_CPUS];

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

/*
 * Windows ARM64 owns x18 as its KPCR alias while Apple's host ABI reserves x18.
 * m1n1's exception assembly therefore preserves the guest register verbatim.
 * The remaining Apple-specific requirement is the CPU retention policy: mode 2
 * is the clock-gate-only WFI mode, and CYC_OVRD_DISABLE_WFI_RET must be clear.
 * That bit survives some secondary stop/start chains and, when set, hardware can
 * return from WFI with most of the guest register file lost. Enforce both parts
 * of the policy on every CPU before Windows runs.
 */
static void hv_configure_guest_wfi_mode(void)
{
    if (!cpu_features->apple_sysregs_unlocked)
        return;

#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
    const u64 mode = 2;
#else
    const u64 mode = 0;
#endif
    reg_mask(SYS_IMP_APL_CYC_OVRD, CYC_OVRD_WFI_MODE_MASK | CYC_OVRD_DISABLE_WFI_RET,
             CYC_OVRD_WFI_MODE(mode));
    sysop("isb");

    u64 value = mrs(SYS_IMP_APL_CYC_OVRD);
    printf("HV: guest WFI mode %lu retention %s on CPU %d (CYC_OVRD=0x%lx)\n",
           FIELD_GET(CYC_OVRD_WFI_MODE_MASK, value),
           value & CYC_OVRD_DISABLE_WFI_RET ? "disabled" : "enabled", smp_id(), value);
}

static bool hv_shutdown_failed;
static bool hv_windows_initialized;
static bool hv_windows_ready;
static bool hv_windows_entered;
static int hv_windows_exit_reason;
static bool hv_setup_pending[MAX_CPUS];
static u64 hv_windows_flags;

int hv_windows_configure(u64 flags)
{
    if (hv_windows_initialized ||
        flags & ~(HV_WINDOWS_GUEST_COMPAT | HV_WINDOWS_FB_SHADOW | HV_WINDOWS_SERIAL_CAPTURE |
                  HV_WINDOWS_SERIAL_CDC | HV_WINDOWS_MTP))
        return -1;
    if ((flags & HV_WINDOWS_SERIAL_CDC) &&
        ((flags & HV_WINDOWS_SERIAL_CAPTURE) || uartproxy_iodev < IODEV_USB0 ||
         uartproxy_iodev >= IODEV_USB0 + USB_IODEV_COUNT))
        return -1;
    if ((flags & HV_WINDOWS_GUEST_COMPAT) && chip_id != T8142)
        return -1;
    if ((flags & HV_WINDOWS_MTP) && (chip_id != T8142 || board_id != 0x24))
        return -1;
    if ((flags & HV_WINDOWS_SERIAL_CAPTURE) && !hv_windows_serial_available())
        return -1;
    hv_windows_flags = flags;
    return 0;
}

bool hv_windows_guest_compat_enabled(void)
{
    return (hv_windows_flags & HV_WINDOWS_GUEST_COMPAT) != 0;
}

bool hv_windows_serial_capture_enabled(void)
{
    return (hv_windows_flags & HV_WINDOWS_SERIAL_CAPTURE) != 0;
}

bool hv_windows_serial_cdc_enabled(void)
{
    return (hv_windows_flags & HV_WINDOWS_SERIAL_CDC) != 0;
}

bool hv_windows_mtp_enabled(void)
{
    return (hv_windows_flags & HV_WINDOWS_MTP) != 0;
}

bool hv_windows_can_prepare_device(void)
{
    return hv_windows_ready && !hv_windows_entered && !hv_shutdown_failed;
}

void hv_windows_quarantine(void)
{
    hv_windows_ready = false;
    printf("HV: device handoff failed; reload Aura before guest entry\n");
}

static int hv_join_cpus(void)
{
    int ret = 0;
    spin_lock(&bhl);
    for (int i = 0; i < MAX_CPUS; i++)
        __atomic_store_n(&hv_should_exit[i], true, __ATOMIC_RELEASE);
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
    if (!is_mac || !cpu_features->fast_ipi || !aic) {
        printf("HV: this CPU/interrupt controller lacks the Windows backend prerequisites\n");
        return -1;
    }
    if (hv_shutdown_failed && hv_join_cpus())
        return -1;
    /* Device and guest state is single-session; reload Aura before another boot. */
    if (hv_windows_initialized) {
        printf("HV: reload the Windows personality before another session\n");
        return -1;
    }
    hv_windows_initialized = true;
    /* Peripheral handoffs are selected explicitly by the Windows profile. */

    pcie_shutdown();
#if defined(ENABLE_NATIVE_AIC_PASSTHROUGH) && defined(ENABLE_J414S_WINDOWS_USB_HOST_HANDOFF)
    /*
     * The internal PHY host signal does not control connector VBUS.  Put each
     * unused J414s CD3217/TPS6598x policy controller into its source-preferred
     * dual-role configuration while its IRQs are still masked.  The exact
     * proxy-selected controller remains untouched.
     */
    if (platform_is_j414s())
        usb_hpm_handoff_host(uartproxy_iodev);
#endif
    // Make sure we wake up DCP if we put it to sleep, just quiesce it to match ADT
    if (display_is_external && display_start_dcp() >= 0)
        display_shutdown(DCP_QUIESCED);
    // reenable hpm interrupts for the guest for unused iodevs
    usb_hpm_restore_irqs(0);
    if (smp_start_secondaries() < 0)
        return -1;
    smp_set_wfe_mode(true);
    hv_wdt_init();

    hv_pt_init();

    // Configure hypervisor defaults

    //
    // UNKNOWN: do we need to bring TGE back? might have misunderstood why it was there at the
    // start. leaving it off for now.
    //
#ifndef ENABLE_VGIC_MODULE
    hv_write_hcr(HCR_API | // Allow PAuth instructions
                 HCR_APK | // Allow PAuth key registers
                 HCR_TEA | // Trap external aborts
                 HCR_RW |  // AArch64 guest
                 HCR_TSC | // Trap SMC exceptions (only writable on Blizzard/Avalanche cores as the
                           // previous generations used a chicken bit for this.)
                 HCR_AMO | // Trap SError exceptions
                 HCR_IMO | // Trap IRQ exceptions (for now)
                 HCR_FMO | // Trap FIQ exceptions (effectively required for now)
                 HCR_VM);  // Enable stage 2 translation
#elif defined(ENABLE_NATIVE_AIC_PASSTHROUGH)
    //
    // windows-native-aic transform -- primary HCR_EL2 site (secondary cores inherit
    // this exact value verbatim, see hv_secondary_info.hcr below and
    // hv_init_secondary()). See docs/windows-native-aic.md for the full design.
    //
    // Mu and Windows both drive the physical AIC directly, so ordinary IRQs never
    // route through EL2 and no virtual GIC is exposed.  FMO starts set while Mu's
    // VBAR is still zero.  FMO remains set throughout: before ExitBootServices,
    // Mu receives timer ticks through reserved native-AIC software lines; after
    // ExitBootServices, Windows receives a per-CPU HCR.VI doorbell and AIC
    // EVENT(2/3).  No vGIC state is exposed for the Windows-ready timer path.
    //
    // HCR_EL2.FMO stays set: the Apple timer is FIQ-only (there is no IRQ-mode timer
    // delivery on this hardware) and physical FIQs must keep trapping to EL2, because
    // Windows bugchecks (0x2B/0x3D) if a raw FIQ is ever delivered to EL1. m1n1
    // intercepts the timer FIQ, masks the physical source, and reflects it through
    // the Windows-ready synthetic AIC EVENT(2/3) bridge.  A guest timer write is the
    // completion/re-arm edge; the Mu-only reserved software lines are not used for
    // steady-state Windows delivery.
    //
    // HCR_EL2.TID3 is kept: it is unrelated to IRQ/FIQ routing. It still lets m1n1 OR
    // in the "GICv3 CPU interface present" bit on a trapped ID_AA64PFR0_EL1 read
    // (hv_exc.c, SYSREG_ISS(ID_AA64PFR0_EL1) case) for whatever UEFI/HAL GIC probing
    // logic may still run before/alongside the native AIC HAL extension; the vGIC
    // virtual CPU interface itself (ICH_*) is left enabled per-core (see
    // hv_vgicv3_enable_virtual_interrupts() calls below and in hv_init_secondary())
    // but is no longer used for timer delivery, only for the pre-existing, vestigial
    // ICC_SGI1R_EL1 SGI-emulation path -- see docs/windows-native-aic.md.
    //
    hv_write_hcr(HCR_API |  // Allow PAuth instructions
                 HCR_APK |  // Allow PAuth key registers
                 HCR_TEA |  // Trap external aborts
                 HCR_RW |   // AArch64 guest
                 HCR_TSC |  // Trap SMC exceptions (only writable on Blizzard/Avalanche cores as the
                            // previous generations used a chicken bit for this.)
                 HCR_TID3 | // Trap ID group 3 registers (AA64 PFR, MMFR, ISAR, AFR ID registers) -
                            // required to support the vanilla ArmGicDxe UEFI driver.
                 HCR_AMO |  // Trap SError exceptions
                 HCR_FMO |  // Hold timer FIQ until Mu's AIC and timer handlers are ready.
                 HCR_VM);   // Enable stage 2 translation
#else
    hv_write_hcr(HCR_API |  // Allow PAuth instructions
                 HCR_APK |  // Allow PAuth key registers
                 HCR_TEA |  // Trap external aborts
                 HCR_RW |   // AArch64 guest
                 HCR_TSC |  // Trap SMC exceptions (only writable on Blizzard/Avalanche cores as the
                            // previous generations used a chicken bit for this.)
                 HCR_TID3 | // Trap ID group 3 registers (AA64 PFR, MMFR, ISAR, AFR ID registers) -
                            // required to support the vanilla ArmGicDxe UEFI driver.
                 HCR_AMO |  // Trap SError exceptions
                 HCR_IMO |  // Trap IRQ exceptions (for now)
                 HCR_FMO |  // Trap FIQ exceptions (effectively required for now)
                 HCR_VM);   // Enable stage 2 translation
#endif

    /*
     * Hold the guest's WFI at EL2 when the CPU retention policy cannot be set.
     *
     * hv_configure_guest_wfi_mode() below is the normal way to keep the
     * register file across WFI, but it early-returns whenever CYC_OVRD is
     * absent, and on T8142 that register takes an undefined instruction
     * exception on write.  A guest that idles on such a core can return from
     * WFI with its general-purpose registers cleared.
     *
     * That is not survivable for Windows.  ntoskrnl's idle path keeps the PRCB
     * idle-nesting counter address live in x26 across the bare `wfi` inside
     * HalProcessorIdle() and dereferences it in the very next instruction after
     * the call returns, so a cleared x26 is a read of address 0 at
     * DISPATCH_LEVEL -- bugcheck 0xA, reproduced at ntoskrnl RVA 0x2c3d8c on
     * every J813 boot that gets that far.
     *
     * Trapping the instruction moves the wait to EL2, where _hv_entry has
     * already spilled x0-x30 to the exception frame and _hv_return reloads all
     * of them.  Whatever the hardware does to the register file is then
     * invisible to the guest.  Cores that can set the retention policy keep
     * running WFI natively; this costs them nothing.
     */
    if (!cpu_features->apple_sysregs_unlocked) {
        hv_write_hcr(mrs(HCR_EL2) | HCR_TWI);
        printf("HV: trapping guest WFI (no CYC_OVRD on this CPU, HCR_EL2=0x%lx)\n", mrs(HCR_EL2));
    }

    if (chip_id == T8142) {
        /*
         * T8142 does not provide the older Apple PMC register bank used by
         * the imported architectural-PMU redirect.  Keep every architectural
         * PMUv3 access at EL2 and advertise no directly owned counters; the
         * synchronous handler supplies the small, deterministic interface
         * needed by unmodified Mu and Windows binaries.
         */
        u64 mdcr = mrs(MDCR_EL2);
        mdcr &= ~(MDCR_EL2_HPMN | MDCR_EL2_HPME);
        mdcr |= MDCR_EL2_TPMCR | MDCR_EL2_TPM;
        msr(MDCR_EL2, mdcr);
        sysop("isb");
        printf("HV: T8142: architectural PMUv3 trapped to synthetic EL2 backend (MDCR=0x%lx)\n",
               mdcr);
    }

    // No guest vectors initially
    msr(VBAR_EL12, 0);

    // set up a HACR bit (56)
    printf("DEBUG: setting up HACR\n");
    if (cpu_features->apple_sysregs_unlocked) {
        uint64_t hacr_val = mrs(HACR_EL2);
        msr(HACR_EL2, hacr_val | BIT(56));
    }

    //
    // m1n1_windows change: initialize PSCI.
    //
    printf("DEBUG: setting up PSCI\n");
    hv_psci_init();
#ifdef ENABLE_VGIC_MODULE
#ifndef ENABLE_NATIVE_AIC_PASSTHROUGH
    //
    // m1n1_windows change: set up the vGIC
    //

    //
    hv_vgicv3_init();
    init_vgic_irq_queues();
#endif
#endif

    // Compute tick interval
    hv_tick_interval = mrs(CNTFRQ_EL0) / HV_TICK_RATE;
    hv_wfi_wake_interval = mrs(CNTFRQ_EL0) / HV_WFI_WAKE_RATE;

    printf("HV: Host tick timer: %s\n", chip_id == T8142 ? "CNTHP_EL2" : "CNTP_EL0");

    hv_has_ecv = mrs(ID_AA64MMFR0_EL1) & (0xfULL << 60);

    if (chip_id == T8142 && hv_has_ecv) {
        /*
         * T8142 reports ECV in ID_AA64MMFR0_EL1, but CNTPOFF_EL2 is not
         * implemented.  Enabling the ECV aliases consequently leaves Mu's
         * first CNTPCT_EL0 delay loop unable to observe forward progress.
         * Use the architectural direct-counter path until the T8142 offset
         * mechanism is understood and can be enabled safely.
         */
        printf("HV: T8142: ECV advertised without CNTPOFF_EL2; using direct-counter fallback\n");
        hv_has_ecv = false;
    }

    if (hv_has_ecv) {
        printf("HV: ECV enabled\n");
        // VHE uses the EL1 physical-timer view for the host tick.  Do not
        // depend on EL1PTEN being inherited from iBoot across a soft reboot.
        reg_set(CNTHCTL_EL2, CNTHCTL_EL1NVVCT | CNTHCTL_EL1NVPCT | CNTHCTL_EL1TVT |
                                 CNTHCTL_EL1PTEN | CNTHCTL_EL1PCTEN);
        hv_secondary_tick_interval = mrs(CNTFRQ_EL0) / HV_SLOW_TICK_RATE;
    } else {
        printf("HV: No ECV supported\n");
        // Enable the physical timer for EL1.
        if (chip_id == T8142) {
            /*
             * The native EL1 CNTPCT view is frozen on T8142.  Keep physical
             * timer programming available, but trap physical-counter reads so
             * hv_exc can return the advancing EL2 architectural counter.
             */
            /*
             * Trap both the physical counter and the EL1 physical-timer
             * programming interface on T8142.  Leaving EL1PTEN set lets Mu's
             * TimerDxe arm CNTP_CTL_EL0 without passing through
             * hv_timer_reflect_guest_rearm().  m1n1 then never observes the
             * final unmasked ENABLE write, mu_timer_ready remains false, and
             * the first expired CNTP interrupt becomes a permanent EL2 FIQ
             * storm before Mu can execute its next instruction.
             *
             * The trapped CNTP_{CTL,CVAL,TVAL}_EL0 cases below map to the EL02
             * bank and complete the native-AIC software-IRQ handshake; the
             * trapped CNTPCT_EL0 case supplies the advancing Apple counter
             * alias.  No EL1 physical-timer permission is needed here.
             */
            msr(CNTHCTL_EL2, 0);
            printf("HV: T8142: trapping EL1 CNTP timer and counter accesses for reflection\n");
        } else {
            msr(CNTHCTL_EL2, CNTHCTL_EL1PTEN | CNTHCTL_EL1PCTEN);
        }

        hv_secondary_tick_interval = hv_tick_interval;
    }

    hv_configure_guest_wfi_mode();

    sysop("dsb ishst");
    sysop("tlbi alle1is");
    sysop("dsb ish");
    sysop("isb");
    if (!hv_windows_mtp_prepare()) {
        hv_windows_quarantine();
        return -1;
    }
    hv_windows_ready = true;
    return 0;
}

static void hv_set_gxf_vbar(void)
{
    msr(SYS_IMP_APL_VBAR_GL1, _hv_vectors_start);
}

/*
 * Guest framebuffer shadow (J813).
 *
 * iBoot hands this panel over as 30bpp X2R10G10B10: still four bytes per pixel,
 * but ten bits per channel, B at [11:2], G at [21:12], R at [31:22]. Mu reports
 * that faithfully as a PixelBitMask mode (SimpleFbDxe.c), and Windows'
 * BasicDisplay drives only 32-bit BGRA -- so it declines the framebuffer and the
 * installer runs with nowhere to draw.
 *
 * m1n1 solves this on J414s inside display_configure(), by asking the DCP to
 * scan the surface out as BGRA. That path hangs on T8142, whose DCP is
 * iop,ascwrap-v6 (see the comment in src/display.c), so instead of changing what
 * the hardware scans out we change what the guest writes into: the guest gets a
 * real BGRA buffer and we repack it into the scanout from the tick.
 *
 * This is the same conversion Mu does in SimpleFbEncodePixel for its own Blt
 * calls. Mu's copy stops mattering at ExitBootServices, when Windows takes the
 * framebuffer over and writes it directly.
 *
 * The shadow comes from m1n1's heap, which sits in the hole between the guest's
 * RAM-LOW and RAM-HIGH windows: memory the guest will never allocate, but that
 * hv_map_hw() can hand it at its own address.
 */
static u32 *hv_fb_shadow;
static u32 *hv_fb_scanout;
static u32 hv_fb_stride_px;
static u32 hv_fb_height;
static u32 hv_fb_cursor;
static u32 hv_fb_lines;
static u64 hv_fb_report_next;

static bool hv_fb_init(struct boot_args *gba)
{
    if (!(hv_windows_flags & HV_WINDOWS_FB_SHADOW))
        return true;

    /* Accept only the live X2R10G10B10 scanout; never reinterpret guessed memory. */
    if (!gba || (cur_boot_args.video.depth & 0xff) != 30 ||
        gba->video.base != cur_boot_args.video.base ||
        gba->video.stride != cur_boot_args.video.stride ||
        gba->video.width != cur_boot_args.video.width ||
        gba->video.height != cur_boot_args.video.height || (gba->video.depth & 0xff) != 30 ||
        !gba->video.base || !gba->video.width || !gba->video.height || gba->video.width > 16384 ||
        gba->video.height > 16384 || gba->video.stride < gba->video.width * 4 ||
        gba->video.stride > 65536 || (gba->video.stride & 3)) {
        printf("HV: unsupported framebuffer layout for BGRA conversion\n");
        return false;
    }

    u64 size = ALIGN_UP((u64)gba->video.stride * gba->video.height, SZ_16K);
    hv_fb_shadow = memalign(SZ_16K, size);
    if (!hv_fb_shadow) {
        printf("HV: cannot allocate BGRA shadow\n");
        return false;
    }
    dc_civac_range(hv_fb_shadow, size);
    sysop("dsb sy");
    mmu_add_mapping((u64)hv_fb_shadow, (u64)hv_fb_shadow, size, MAIR_IDX_NORMAL_NC, PERM_RW);
    memset(hv_fb_shadow, 0, size);
    if (hv_map_hw((u64)hv_fb_shadow, (u64)hv_fb_shadow, size) < 0) {
        printf("HV: cannot map BGRA shadow; guest entry refused\n");
        /* Keep the allocation reserved: its host mapping is now non-cacheable. */
        hv_fb_shadow = NULL;
        return false;
    }

    hv_fb_scanout = (u32 *)gba->video.base;
    hv_fb_stride_px = gba->video.stride / 4;
    hv_fb_height = gba->video.height;
    gba->video.base = (u64)hv_fb_shadow;
    gba->video.depth = (gba->video.depth & ~0xffUL) | 32;
    fb_capture_set_shadow((u64)hv_fb_shadow, gba->video.width, gba->video.height,
                          gba->video.stride);
    printf("HV: BGRA shadow %p -> X2R10G10B10 scanout %p (%ux%u)\n", hv_fb_shadow, hv_fb_scanout,
           hv_fb_stride_px, hv_fb_height);
    return true;
}

/*
 * Convert `lines` scanlines, continuing from wherever the previous caller
 * stopped.
 *
 * Deliberately not a whole frame. This first ran as one full pass from
 * hv_tick(), which executes with the big hypervisor lock held -- 17 MB of
 * non-cached read plus write, once a second, stalling every other core's EL2
 * entry. Measured cost: guest interrupt throughput fell from bursts of ~120 per
 * tick to a flat 2-3, and the installer's spinner visibly crawled.
 *
 * The callers that matter are cores about to park in WFI, which get here on the
 * fast path in hv_exc_sync() *without* taking that lock, so the work lands on
 * cores that were going to idle anyway. hv_tick() still contributes a slice so a
 * guest that never idles keeps refreshing. The cursor is shared and advanced
 * atomically, so several cores just take different bands.
 */
void hv_fb_convert_slice(u32 lines)
{
    if (!hv_fb_shadow)
        return;

    u32 start = __atomic_fetch_add(&hv_fb_cursor, lines, __ATOMIC_RELAXED) % hv_fb_height;
    u32 end = start + lines;
    if (end > hv_fb_height)
        end = hv_fb_height;

    for (u32 y = start; y < end; y++) {
        const u32 *src = hv_fb_shadow + (size_t)y * hv_fb_stride_px;
        u32 *dst = hv_fb_scanout + (size_t)y * hv_fb_stride_px;

        for (u32 x = 0; x < hv_fb_stride_px; x++) {
            u32 p = src[x];
            dst[x] = ((p & 0xff) << 2) | (((p >> 8) & 0xff) << 12) | (((p >> 16) & 0xff) << 22);
        }
    }

    __atomic_fetch_add(&hv_fb_lines, end - start, __ATOMIC_RELAXED);
}

/*
 * Refresh rate actually achieved, once a second.
 *
 * Worth carrying: what limits this is how often the engines run, not the memory
 * system, and that is not something you can read off the source.  The slice
 * sizes in hv.h are derived from this number.
 */
void hv_fb_report(void)
{
    if (!hv_fb_shadow)
        return;

    u64 now = mrs(CNTPCT_EL0);
    if (now < hv_fb_report_next)
        return;
    hv_fb_report_next = now + mrs(CNTFRQ_EL0);

    u32 lines = __atomic_exchange_n(&hv_fb_lines, 0, __ATOMIC_RELAXED);
    printf("HV: fb: %u lines/s = %u.%02u fps\n", lines, lines / hv_fb_height,
           (lines * 100 / hv_fb_height) % 100);
#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
    /* Same 1 Hz budget; silent until Windows owns the AIC. */
    hv_native_aic_delivery_report();
#endif
}

int hv_start(void *entry, u64 regs[4])
{
    if (boot_cpu_idx == -1) {
        printf("Boot CPU has not been found, can't start hypervisor\n");
        return -1;
    }

    if (hv_shutdown_failed || !hv_windows_ready || hv_windows_entered) {
        printf("HV: Windows session is not ready\n");
        return -1;
    }

    /*
     * regs[0] is the guest's boot_args pointer, written by the proxyclient just
     * before this call and not yet read by anything. Last chance to redirect the
     * framebuffer before the guest sees it.
     */
    hv_windows_entered = true;
    if (!hv_windows_mtp_map_guest() || !hv_fb_init((struct boot_args *)regs[0])) {
        hv_windows_mtp_stop();
        hv_windows_quarantine();
        return -1;
    }

    memset(hv_should_exit, 0, sizeof(hv_should_exit));
    memset(hv_started_cpus, 0, sizeof(hv_started_cpus));

    hv_started_cpus[boot_cpu_idx] = true;

    msr(VBAR_EL1, _hv_vectors_start);

    if (gxf_enabled())
        gl2_call(hv_set_gxf_vbar, 0, 0, 0, 0);

#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
    /*
     * Windows needs the GICD/GICR MMIO records advertised by Mu only while it
     * classifies the startup controller.  Initialize those register hooks after
     * the host's broad mappings are complete, but leave HCR.IMO clear and never
     * enable ICH/LRs: this is a topology carrier, not an interrupt-delivery path.
     */
#ifdef ENABLE_VGIC_MODULE
    /*
     * The carrier was deferred on T8142 because an earlier implementation reset
     * the machine before Mu executed its first instruction, and the deferral was
     * written to last only "after firmware and bootmgfw handoff are
     * independently proven".  Both are now proven: firmware reaches
     * ExitBootServices and the boot manager hands off to a kernel that runs.
     *
     * Keeping it deferred is no longer neutral.  Mu advertises GICD and GICR
     * records in its MADT unconditionally, so with no carrier the kernel
     * classifies its startup controller by reading GICD_PIDR2 at dist_base +
     * 0xFFE8 and takes an unmapped-IPA fault on a region nothing backs.  This is
     * still only a topology carrier: HCR.IMO stays clear and no list registers
     * are enabled, so it answers the classification and delivers nothing.
     */
    printf("HV: windows-native-aic: initializing virtual IRQ queues\n");
    hv_vgicv3_init();
    printf("HV: windows-native-aic: vGIC MMIO carrier ready\n");
    init_vgic_irq_queues();
    printf("HV: windows-native-aic: virtual IRQ queues ready\n");
#endif

    /*
     * Same reasoning as the hooks below: the host's broad mappings are done, so
     * a mapping added here survives into the guest instead of being overwritten
     * by a later pt_update.
     */

    /*
     * Host MMIO mappings are complete before hv_start(), so these hooks persist.
     *
     * T8142 used to skip this and defer the hooks "during Mu/NVMe bring-up",
     * because there was no CSRT and therefore no way for Windows to drive the
     * AIC itself -- installing the hooks would have handed interrupts to an
     * EL1 that could not service them.  T8142FamilyPkg now publishes a real
     * AIC3 CSRT and the HAL extension binds to it, so the reason is gone and
     * this SoC takes the same path as every other one.
     */
    hv_native_aic_transition_init();
#endif

    //
    // windows-native-aic: this is the "secondary CPU path" half of the HCR_EL2 update
    // in hv_init() above.  APs started by Windows apply the current pure-AIC phase
    // policy in hv_init_secondary() rather than inheriting a stale Mu-era FIQ state.
    //
    hv_secondary_info.hcr = mrs(HCR_EL2);
    if (cpu_features->apple_sysregs_unlocked)
        hv_secondary_info.hacr = mrs(HACR_EL2);
    hv_secondary_info.vtcr = mrs(VTCR_EL2);
    hv_secondary_info.vttbr = mrs(VTTBR_EL2);
    hv_secondary_info.mdcr = mrs(MDCR_EL2);
    hv_secondary_info.mdscr = mrs(MDSCR_EL1);
    if (!cpu_features->apple_sysregs_unlocked) {
        printf("HV: skipping unavailable AMX/AP-key state\n");
        hv_secondary_info.amx_ctl = 0;
        hv_secondary_info.apvmkeylo = 0;
        hv_secondary_info.apvmkeyhi = 0;
        hv_secondary_info.apsts = 0;
    } else {
        hv_secondary_info.amx_ctl = mrs(SYS_IMP_APL_AMX_CTL_EL2);
        hv_secondary_info.apvmkeylo = mrs(SYS_IMP_APL_APVMKEYLO_EL2);
        hv_secondary_info.apvmkeyhi = mrs(SYS_IMP_APL_APVMKEYHI_EL2);
        hv_secondary_info.apsts = mrs(SYS_IMP_APL_APSTS_EL12);
    }
    hv_secondary_info.actlr_el2 = mrs(ACTLR_EL2);
    if (cpu_features->actlr_el2)
        hv_secondary_info.actlr_el1 = mrs(SYS_ACTLR_EL12);
    else
        hv_secondary_info.actlr_el1 = mrs(SYS_IMP_APL_ACTLR_EL12);
    hv_secondary_info.cnthctl = mrs(CNTHCTL_EL2);
    if (!cpu_features->apple_sysregs_unlocked) {
        printf("HV: skipping unavailable SPRR/GXF state\n");
        hv_secondary_info.sprr_config = 0;
        hv_secondary_info.gxf_config = 0;
    } else {
        hv_secondary_info.sprr_config = mrs(SYS_IMP_APL_SPRR_CONFIG_EL1);
        if (hv_secondary_info.sprr_config & SPRR_CONFIG_EN)
            hv_secondary_info.gxf_config = mrs(SYS_IMP_APL_GXF_CONFIG_EL1);
    }

#if defined(ENABLE_VGIC_MODULE) && !defined(ENABLE_NATIVE_AIC_PASSTHROUGH)
    hv_vgicv3_enable_virtual_interrupts();
    hv_vgicv3_init_list_registers();
#endif

    if (cpu_features->counter_redirect) {
        /*
         * The J813 launcher supplies Mu as a raw FD, so run_guest.py does not
         * apply its Mach-O counter-redirect setup.  Mu still expects the
         * architectural counter view rather than Apple's scaled redirect.
         * Program both the host and guest aliases before the first EL1 read.
         */
        msr(SYS_IMP_APL_AGTCNTRDIR_EL1, AGTCNTRDIR_DISABLE | AGTCNTRDIR_EL0_TRAP_CTL);
        msr(SYS_IMP_APL_AGTCNTRDIR_EL12, AGTCNTRDIR_DISABLE | AGTCNTRDIR_EL0_TRAP_CTL);
        sysop("isb");
        printf("HV: T8142: architectural counter redirect selected for raw Mu guest\n");
    }

    if (cpu_features->counter_redirect) {
        hv_secondary_info.agt_cnt_rdir_el1 = mrs(SYS_IMP_APL_AGTCNTRDIR_EL1);
        hv_secondary_info.agt_cnt_rdir_el12 = mrs(SYS_IMP_APL_AGTCNTRDIR_EL12);
    }

    printf("HV: Timer state CNTHCTL_EL2=0x%lx HCR_EL2=0x%lx\n", mrs(CNTHCTL_EL2), mrs(HCR_EL2));
    if (chip_id == T8142) {
        hv_tick_interval = mrs(CNTFRQ_EL0) / HV_SLOW_TICK_RATE;
        /*
         * The installed USB-proxy path can leave uart0 clock-gated on J813, so
         * hv_tick() suppresses virtual-UART/AIC polling until Mu's TimerDxe
         * readiness edge. The remaining tick body is safe and load-bearing:
         * it services the USB proxy and gives EL2 a bounded rendezvous point
         * even when firmware spins without taking another exception.
         */
        printf("HV: T8142: arming safe primary EL2 liveness tick\n");
        hv_arm_tick(false);
    } else {
        printf("HV: Arming primary host tick\n");
        hv_arm_tick(false);
    }
    hv_pinned_cpu = -1;
    hv_want_cpu = -1;
    hv_cpus_in_guest = BIT(smp_id());

    u64 adt_base;
    if (chip_id == T8103 || chip_id == T8112)
        adt_base = ADT_EL2_36_BIT;
    else
        adt_base = ADT_EL2_42_BIT;

    // map the address of the (EL2) ADT to a fixed location so EL1 can patch it
    if (hv_map_hw(adt_base, (u64)adt, ALIGN_UP(cur_boot_args.devtree_size, SZ_16K)) < 0) {
        printf("HV: cannot map the Mu ADT alias\n");
        hv_windows_mtp_stop();
        hv_windows_quarantine();
        return -1;
    }

    /*
     * TEE ACPI Profile 4.6.3 requires a TPM's Error, Cancel and Start bits to
     * be clear when firmware hands control to the OS. No-op unless a CRB was
     * mapped, so this is unconditional rather than gated on a flag nobody
     * would remember to set.
     */

    printf("HV: Entering guest at %p (x0=0x%lx)\n", entry, regs[0]);
    hv_enter_guest(regs[0], regs[1], regs[2], regs[3], entry);

    __atomic_and_fetch(&hv_cpus_in_guest, ~BIT(smp_id()), __ATOMIC_ACQUIRE);
    spin_lock(&bhl);

    hv_started_cpus[boot_cpu_idx] = false;
    spin_unlock(&bhl);
    int result = hv_join_cpus();
    if (!result) {
        if (!hv_windows_mtp_stop()) {
            hv_windows_quarantine();
            result = -1;
        }
        hv_windows_serial_finish();
        printf("HV: All CPUs exited\n");
    }
    return result ? result : __atomic_load_n(&hv_windows_exit_reason, __ATOMIC_ACQUIRE);
}

static void hv_init_secondary(struct hv_secondary_info_t *info)
{
    if (cpu_features->apple_sysregs_unlocked)
        gxf_init();

    msr(VBAR_EL1, _hv_vectors_start);

    //
    // windows-native-aic: secondary-CPU HCR_EL2 site. info->hcr is the value hv_init()
    // computed on the boot CPU (see the primary HCR_EL2 comment there) captured by
    // hv_start() above; this core gets the identical IMO-clear/FMO-set configuration,
    // not a re-derived one, so there is nothing native-AIC-specific to add here beyond
    // this note.
    //
    msr(HCR_EL2, info->hcr);
    if (cpu_features->apple_sysregs_unlocked)
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
        if (info->sprr_config & SPRR_CONFIG_EN)
            msr(SYS_IMP_APL_GXF_CONFIG_EL1, info->gxf_config);
    }

#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
    /* APs started by Windows inherit the post-EBS FIQ bridge policy. */
    hv_native_aic_enter_cpu();
#elif defined(ENABLE_VGIC_MODULE)
    hv_vgicv3_enable_virtual_interrupts();
    hv_vgicv3_init_list_registers();
#endif

    hv_configure_guest_wfi_mode();

    // For M3 and up, CNTHCTL_EL2 must be written after the counter redirection
    if (cpu_features->counter_redirect) {
        msr(SYS_IMP_APL_AGTCNTRDIR_EL1, info->agt_cnt_rdir_el1);
        msr(SYS_IMP_APL_AGTCNTRDIR_EL12, info->agt_cnt_rdir_el12);
    }
    sysop("isb");
    msr(CNTHCTL_EL2, info->cnthctl);

    if (gxf_enabled())
        gl2_call(hv_set_gxf_vbar, 0, 0, 0, 0);

    if (chip_id == T8142)
        printf("HV: T8142: deferring secondary EL2 host tick during Mu/NVMe bring-up\n");
    else
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

/* May also run while the exception proxy holds bhl. Do not acquire it here.
 * Failure wins over every voluntary exit, including a racing SYSTEM_OFF. */
static void hv_windows_request_exit(int reason)
{
    if (reason < 0) {
        __atomic_store_n(&hv_windows_exit_reason, reason, __ATOMIC_RELEASE);
    } else {
        int expected = 0;
        __atomic_compare_exchange_n(&hv_windows_exit_reason, &expected, reason, false,
                                    __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    }
    for (int cpu = 0; cpu < MAX_CPUS; cpu++)
        __atomic_store_n(&hv_should_exit[cpu], true, __ATOMIC_RELEASE);
}

/* Request first, then let the exception proxy rendezvous/unlock/exit.
 * The boot CPU also observes this flag and performs the normal join path. */
void hv_windows_abort_guest(void)
{
    hv_windows_request_exit(-1);
}

/* Called from the unlocked synchronous SMC path. End the virtual machine;
 * the physical host remains in the proxy for an explicit subsequent boot. */
void hv_windows_system_exit(bool reset)
{
    hv_windows_request_exit(reset ? 2 : 1);
    __atomic_and_fetch(&hv_cpus_in_guest, ~BIT(smp_id()), __ATOMIC_ACQUIRE);
    printf("HV: Windows requested SYSTEM_%s; draining guest CPUs\n", reset ? "RESET" : "OFF");
    hv_exit_guest();
}

void hv_exit_cpu(int cpu)
{
    if (cpu == -1)
        cpu = smp_id();

    printf("HV: Requesting exit of CPU#%d from the guest\n", cpu);
    if (cpu >= 0 && cpu < MAX_CPUS)
        __atomic_store_n(&hv_should_exit[cpu], true, __ATOMIC_RELEASE);
}

/*
 * Rendezvous budget.
 *
 * The old budget was 1,000,000 iterations of a bare acquire load.  That is not
 * a duration: once every other CPU is parked on bhl the line stays clean in
 * this CPU's cache and the loop retires in a couple of cycles per iteration,
 * so the real budget was somewhere around 0.3-1 ms.  Nothing about the system
 * guarantees a collection inside that window:
 *
 *  - The only lever on a CPU that is running guest code is the IPI, and on
 *    this SoC that is an Apple Fast IPI delivered as an FIQ.  It is latched in
 *    IPI_SR_EL1 so it cannot be lost, but it is not taken while the target is
 *    already at EL2 (exception entry masks DAIF and hv_exc_entry() only clears
 *    the SError bit).  Every microsecond the target spends inside EL2 is a
 *    microsecond the requester spends spinning.
 *  - Several EL2 entries return to the guest without ever calling
 *    hv_exc_entry(), so they hold the CPU's hv_cpus_in_guest bit for their
 *    whole duration -- and some of them printf() to the console, which at UART
 *    speeds is milliseconds per line.  See the enumeration above
 *    hv_rendezvous_quiesce() in hv_exc.c.
 *  - The self-collection fallback cannot help: hv_exc_fiq()'s fast path re-arms
 *    a non-interruptible CPU with hv_secondary_tick_interval, which is one full
 *    second when ECV is available (HV_SLOW_TICK_RATE in this file).  That is
 *    three orders of magnitude past the old budget.
 *
 * So make the budget an actual duration, re-arm the IPI at a fixed cadence
 * inside it rather than exactly once, and -- most importantly -- do not kill
 * the machine when it expires.
 *
 * Measured on the J414s: roughly half of all Windows boots were lost to "HV:
 * Failed to rendezvous".  Before 575ca6c9 it always named CPU 0; afterwards it
 * named a different CPU each time (0x40/CPU 6 in one capture, whose breadcrumb
 * trail `57*89+xs` shows it had *completed* a slow-path MMIO emulation and
 * returned to the guest).  The rendezvous is a time-coherence measure for the
 * proxy -- it exists so every CPU re-enters the guest with the same
 * CNTVOFF_EL2/stolen_time -- not a safety property.  A CPU that misses one
 * picks up the current stolen_time at its next real hv_exc_exit() anyway.
 * Trading a transient timebase skew on one core against destroying the boot is
 * not a close call.
 *
 * Escalate only on evidence of a genuine hang: a CPU that is wedged (a stalled
 * MMIO access, a core that never took the FIQ) will miss every rendezvous, so
 * count consecutive misses and panic once the count is unambiguous.  That is
 * ~1.3s of accumulated stall, which no transient can produce.
 */
#define HV_RENDEZVOUS_TIMEOUT_US    20000
#define HV_RENDEZVOUS_IPI_PERIOD_US 250
#define HV_RENDEZVOUS_MAX_MISSES    64

/*
 * Every hv_rendezvous() caller holds bhl (_hv_exc_proxy(), and hv_switch_cpu()
 * from the proxy request handler, which runs inside uartproxy_run()), so this
 * needs no atomics.
 */
static u32 hv_rendezvous_misses;

static u64 hv_usecs_to_ticks(u32 usecs)
{
    return (mrs(CNTFRQ_EL0) * (u64)usecs) / 1000000;
}

void hv_rendezvous(void)
{
    if (!__atomic_load_n(&hv_cpus_in_guest, __ATOMIC_ACQUIRE))
        return;

    /*
     * Publish BEFORE the IPIs.  A CPU already inside one of the fast paths will
     * not take the IPI until it next opens an FIQ window, and if it is
     * servicing a dense stream of IMPDEF MSR traps that window may never come.
     * The flag lets that CPU notice the rendezvous from inside the fast path
     * itself rather than depending on interrupt delivery.
     */
    __atomic_store_n(&hv_rendezvous_pending, 1, __ATOMIC_RELEASE);

    u64 ipi_period = hv_usecs_to_ticks(HV_RENDEZVOUS_IPI_PERIOD_US);
    u64 now = hv_host_counter();
    u64 deadline = now + hv_usecs_to_ticks(HV_RENDEZVOUS_TIMEOUT_US);
    u64 next_ipi = now; /* fire the first round immediately */
    u64 missing;

    for (;;) {
        missing = __atomic_load_n(&hv_cpus_in_guest, __ATOMIC_ACQUIRE);
        if (!missing) {
            __atomic_store_n(&hv_rendezvous_pending, 0, __ATOMIC_RELEASE);
            hv_rendezvous_misses = 0;
            return;
        }

        now = hv_host_counter();

        if ((s64)(now - next_ipi) >= 0) {
            /*
             * Re-IPI on a cadence, not just once.  A CPU that was at EL2 with
             * FIQs masked when the first IPI landed does take it on its next
             * eret, so the retry is usually redundant -- but it costs nothing
             * here and it is the only recovery if an edge is ever dropped.
             * Poke only the CPUs still marked in-guest: one that has already
             * left is parked on bhl or legitimately back in the guest, and a
             * spurious IPI there is a cost paid by the guest.
             */
            for (int i = 0; i < MAX_CPUS; i++) {
                if (i != smp_id() && hv_started_cpus[i] && (missing & BIT(i))) {
                    smp_send_ipi(i);
                }
            }
            next_ipi = now + ipi_period;
        }

        if ((s64)(now - deadline) >= 0)
            break;
    }

    __atomic_store_n(&hv_rendezvous_pending, 0, __ATOMIC_RELEASE);

    if (++hv_rendezvous_misses >= HV_RENDEZVOUS_MAX_MISSES)
        hv_panic("HV: %u consecutive failed rendezvous, missing CPUs: 0x%lx (current: %d)\n",
                 hv_rendezvous_misses, missing, smp_id());

    /*
     * Loud, but not fatal.  The missing CPU keeps running the guest with a
     * stale CNTVOFF_EL2 until its next hv_exc_exit(); the proxy transaction we
     * are about to run proceeds without it.
     */
    printf("HV: rendezvous incomplete after %d us (miss %u of %d), missing CPUs: 0x%lx "
           "(current: %d); continuing\n",
           HV_RENDEZVOUS_TIMEOUT_US, hv_rendezvous_misses, HV_RENDEZVOUS_MAX_MISSES, missing,
           smp_id());
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

/*
 * Bound a guest-WFI wait held at EL2; see HV_WFI_WAKE_RATE.
 *
 * Uses the same timer the host tick does, so whichever fires is taken as an
 * ordinary tick and re-arms the normal schedule on its way out -- there is no
 * separate state to restore, and an early tick is harmless.
 */
void hv_arm_wfi_wake(void)
{
    if (chip_id == T8142) {
        msr(SYS_CNTHP_TVAL_EL2, hv_wfi_wake_interval);
        msr(SYS_CNTHP_CTL_EL2, CNTx_CTL_ENABLE);
    } else {
        msr(CNTP_TVAL_EL0, hv_wfi_wake_interval);
        msr(CNTP_CTL_EL0, CNTx_CTL_ENABLE);
    }
}

void hv_arm_tick(bool secondary)
{
    u64 interval = secondary ? hv_secondary_tick_interval : hv_tick_interval;

    /*
     * T8142 faults on direct CNTP_*_EL0 accesses from the VHE host context.
     * Keep CNTP_*_EL02 exclusively for the guest and use the EL2 physical
     * timer for m1n1's own periodic tick on this generation.
     */
    if (chip_id == T8142) {
        msr(SYS_CNTHP_TVAL_EL2, interval);
        msr(SYS_CNTHP_CTL_EL2, CNTx_CTL_ENABLE);
    } else {
        msr(CNTP_TVAL_EL0, interval);
        msr(CNTP_CTL_EL0, CNTx_CTL_ENABLE);
    }
}

bool hv_mask_pending_tick(void)
{
    u64 ctl = chip_id == T8142 ? mrs(SYS_CNTHP_CTL_EL2) : mrs(CNTP_CTL_EL0);

    if (ctl != (CNTx_CTL_ISTATUS | CNTx_CTL_ENABLE))
        return false;

    ctl = CNTx_CTL_ISTATUS | CNTx_CTL_IMASK | CNTx_CTL_ENABLE;
    if (chip_id == T8142)
        msr(SYS_CNTHP_CTL_EL2, ctl);
    else
        msr(CNTP_CTL_EL0, ctl);

    return true;
}

void hv_maybe_exit(void)
{
    if (__atomic_load_n(&hv_should_exit[smp_id()], __ATOMIC_ACQUIRE)) {
        hv_exit_guest();
    }
}

#ifdef ENABLE_GUEST_PC_SAMPLER
/*
 * See ENABLE_GUEST_PC_SAMPLER in config.h.  Driven from the EL2 host tick, so
 * it reports guest progress even when the guest has stopped producing output
 * of its own -- which is the normal state once an EFI application takes the
 * console.
 *
 * The per-sample PC delta is the useful part: a guest making progress moves by
 * large and irregular amounts, while one wedged in a spin loop stays inside a
 * span of a few dozen bytes.  SPSR and VBAR_EL1 then say which kind of stall
 * it is:
 *   - VBAR_EL1 moving off the firmware's vector table proves the guest has
 *     installed its own exception handlers, so firmware-side assists no longer
 *     see the guest's traps at all.
 *   - SPSR.I separates a guest sitting with interrupts masked (typically
 *     inside an exception handler) from one spinning with them enabled
 *     (typically waiting on an interrupt that is never delivered).
 */
static void hv_sample_guest_pc(struct exc_info *ctx)
{
    static u32 ticks;
    static u64 seq;
    static u64 prev_pc;

    /*
     * The host tick rate is chip-dependent -- T8142 deliberately runs the
     * primary tick at HV_SLOW_TICK_RATE, not HV_TICK_RATE -- so derive the
     * reporting interval from the interval actually armed.  Keying off the
     * compile-time constant instead silently stretches the report period by
     * the ratio between the two rates.
     */
    u32 per_report = 1;
    if (hv_tick_interval)
        per_report = (u32)(mrs(CNTFRQ_EL0) / hv_tick_interval);
    if (per_report < 1)
        per_report = 1;

    if (++ticks < per_report)
        return;
    ticks = 0;

    u64 pc = ctx->elr;
    s64 delta = (s64)(pc - prev_pc);

    u64 n = seq++;

    printf("HV: guest[%ld] pc=0x%lx d=%s0x%lx spsr=0x%lx EL%ld%s vbar_el1=0x%lx "
           "sp_el1=0x%lx lr=0x%lx\n",
           n, pc, delta < 0 ? "-" : "+", delta < 0 ? (u64)-delta : (u64)delta, ctx->spsr,
           (ctx->spsr >> 2) & 3, (ctx->spsr & BIT(7)) ? " I-masked" : "", mrs(VBAR_EL12),
           ctx->sp[1], ctx->regs[30]);

    /*
     * The guest's own EL1 fault state.  A guest stopped inside its exception
     * path leaves the triage registers standing, and the ESR exception class
     * names the failure outright rather than leaving it to inference.
     *
     * These MUST use the _EL12 aliases.  m1n1 runs at EL2 with HCR_EL2.E2H
     * set, and under E2H the plain _EL1 register names are redirected to their
     * _EL2 counterparts when named from EL2 -- so mrs(ESR_EL1) here would
     * silently report ESR_EL2, i.e. m1n1's own last trap, not the guest's
     * fault.  Same reason hv.c writes the guest's vectors via VBAR_EL12.
     *
     * x1/x2/x3 are printed alongside because a guest fault-capture stub has
     * typically just read ELR/SPSR/ESR into them, so they corroborate the
     * sysregs independently.
     */
    printf("HV: guest[%ld]   el1 esr=0x%lx ec=0x%lx elr=0x%lx far=0x%lx spsr_el1=0x%lx "
           "x1=0x%lx x2=0x%lx x3=0x%lx\n",
           n, mrs(ESR_EL12), (mrs(ESR_EL12) >> 26) & 0x3f, mrs(ELR_EL12), mrs(FAR_EL12),
           mrs(SPSR_EL12), ctx->regs[1], ctx->regs[2], ctx->regs[3]);

    /*
     * For an undefined instruction, the opcode itself is the whole story: it
     * names the register the hardware refused.  Fetch it rather than inferring
     * from where the guest stopped.
     */
    if (mrs(ESR_EL12) == 0x02000000) {
        u64 fault_pa = hv_translate(mrs(ELR_EL12), false, false, NULL);
        if (fault_pa)
            printf("HV: guest[%ld]   undef insn 0x%08x at 0x%lx\n", n, read32(fault_pa),
                   mrs(ELR_EL12));
    }

    prev_pc = pc;
}
#endif

void hv_tick(struct exc_info *ctx)
{
    hv_wdt_pet();
    hv_windows_serial_flush();
    /*
     * A floor, not the main path: this runs with the big hypervisor lock held,
     * so it converts only a band. Idle cores parking in WFI do the bulk of the
     * work, unlocked. See hv_fb_convert_slice().
     */
    hv_fb_convert_slice(HV_FB_SLICE_TICK);
    /* Same budget rationale as the slice above: bounded work on an exit that
     * was happening anyway. One tile row is 40 tiles of 64x64 on this panel. */
    hv_fb_report();
#ifdef ENABLE_GUEST_PC_SAMPLER
    hv_sample_guest_pc(ctx);
#endif
    /*
     * The host tick is the only place EL2 regains control from a guest that
     * has wedged itself, so it is where an absent-system-register hang has to
     * be unwedged.  Costs one guest-PC read per tick when nothing is stuck.
     */
    hv_track_t8142_undef_vector(ctx);
    /*
     * A redirect can be invalidated without VBAR_EL1 ever changing -- a guest
     * that rebuilds its page tables can leave the vector VA resolving to a
     * different physical page than the one we patched -- and tracking keyed on
     * VBAR alone cannot see that.  Re-assert the redirects here, where the cost
     * is per tick rather than per trap.
     */
    hv_verify_t8142_undef_vectors();
    hv_scan_t8142_guest_modules();
    hv_report_t8142_process(ctx);
    hv_report_t8142_gic_activity();
    /* Bounded, non-allocating mailbox fallback for a guest that never idles.
     * The primary servicer runs in the unlocked WFI path. */
    hv_windows_mtp_poll();
    hv_windows_mtp_report();
    hv_check_t8142_bugcheck();
    hv_recover_t8142_sysreg_undef(ctx);
    iodev_handle_events(uartproxy_iodev);
    if (iodev_can_read(uartproxy_iodev)) {
        printf("HV: User interrupt\n");
        iodev_console_flush();
        if (hv_pinned_cpu == -1 || hv_pinned_cpu == smp_id())
            hv_exc_proxy(ctx, START_HV, HV_USER_INTERRUPT, NULL);
    }
#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
    /*
     * J813 can enter the hypervisor with uart0 clock-gated and before Mu has
     * configured AIC2. Keep the early T8142 host tick useful for watchdog and
     * USB-proxy polling, but do not drive the virtual UART's AIC software line
     * until TimerDxe proves that Mu's native interrupt handler is live.
     */
    if (chip_id != T8142 || hv_native_aic_mu_timer_active() || hv_native_aic_windows_active())
#endif
        hv_vuart_poll();
}
