/* SPDX-License-Identifier: MIT */

#include "hv.h"
#include "adt.h"
#include "aic.h"
#include "aic_regs.h"
#include "assert.h"
#include "cpu_regs.h"
#include "exception.h"
#include "hv_aic_alias.h"
#include "hv_vgic.h"
#include "smp.h"
#include "string.h"
#include "uart.h"
#include "uartproxy.h"
#include "utils.h"
#include "windows_n1.h"

#define TIME_ACCOUNTING
//
// m1n1_windows change: when the vGIC is running in the guest - timer interrupts by virtue of coming
// from the generic timer are still going to come as FIQs to EL2 - we'll need to divert those to the
// guest as *IRQs* (to prevent Windows from crashing as it treats FIQs as errors).
//
extern bool vgic_inited;
extern spinlock_t bhl;

#define PERCPU(x)      pcpu[mrs(TPIDR_EL2)].x
#define PERCPU_N(x, y) pcpu[x].y

#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
/*
 * T8142 can return a Mu SP_EL0 value with its upper 32 bits cleared even
 * though SP_EL1 remains in the firmware's high VA window.  The first SP0
 * exception entry then underflows into unmapped low memory while saving x0/x1.
 *
 * Keep the workaround outside hv_pcpu_data: that structure is a debugger ABI.
 * It is deliberately restricted to Mu's pre-Windows timer phase; a legitimate
 * low userspace stack after ExitBootServices must never be canonicalized.
 */
static bool t8142_sp_el0_repair_logged[MAX_CPUS];
static bool t8142_sysreg_assist_logged;
#endif

/*
 * Guest Fast-IPI transport state.  DELIVERABLE is also used by the legacy
 * vGIC path, so these common state bits must remain available when native AIC
 * passthrough is compiled out.
 *
 * Under native AIC, the physical Fast-IPI latch is only a wake edge; it is
 * not proof that Windows reached KiIpiServiceRoutine. EVENT accept moves a
 * generation to INFLIGHT, and the HAL's controller EOI writes IPI_SR_EL1 to
 * commit it. DELIVERABLE may coexist with INFLIGHT when a newer send arrives.
 */
#define HV_GUEST_IPI_DELIVERABLE      BIT(0)
#define HV_GUEST_IPI_INFLIGHT         BIT(1)
#define HV_GUEST_IPI_RETRY_ARMED      BIT(2)
#define HV_GUEST_IPI_GENERATION       GENMASK(9, 3)
#define HV_GUEST_IPI_INFLIGHT_CARRIER BIT(10)
#define HV_GUEST_IPI_EVENT_TOKEN      GENMASK(10, 3)
#define HV_GUEST_IPI_RETRY_COUNT      GENMASK(14, 11)
#define HV_GUEST_IPI_START_TIME       GENMASK(30, 15)
#define HV_GUEST_IPI_QUEUED_CARRIER   BIT(31)
#define HV_GUEST_IPI_OUTSTANDING      (HV_GUEST_IPI_DELIVERABLE | HV_GUEST_IPI_INFLIGHT)
#define HV_GUEST_IPI_RETRY_COUNT_MAX  MASK(4)
#define HV_GUEST_IPI_CLOCK_SHIFT      18
#define HV_GUEST_IPI_CLOCK_MASK       MASK(16)

#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
//
/*
 * windows-native-aic timer reflector state.
 *
 * These two ranges are Mu-only compatibility lines.  Before ExitBootServices,
 * Mu's AppleAic driver receives timer ticks through AIC_SW_SET on one reserved
 * line per local CNTP/CNTV source.  The lines are also cleared during phase
 * transitions and stale EVENT reads so that a Mu token cannot escape into the
 * Windows HAL.
 *
 * They are deliberately NOT the Windows-ready timer transport.  Once Windows
 * owns AIC2, hv_update_fiq() keeps FMO set, masks the physical timer FIQ, and
 * raises a per-CPU HCR.IMO|HCR.VI doorbell.  The AIC EVENT hook then supplies
 * the processor-local source value 2 (CNTP) or 3 (CNTV).  Windows-ready code
 * does not call aic_set_sw() and does not depend on an AIC2 target register.
 *
 * A J414s live AIC2 target/SW_SET sweep did not produce a usable per-CPU
 * mapping: the target nibble is opaque routing metadata, not an OS CPU selector.
 * Do not add aic_set_affinity() or guessed AIC2 target programming here without
 * new hardware evidence.  Keep the ranges because Mu and the transition
 * cleanup still need stable, implementation-private line numbers.
 */
//
#define HV_TIMER_SWIRQ_BASE            (aic->nr_irq - (2 * MAX_CPUS))
#define HV_TIMER_P_SWIRQ(cpu)          (HV_TIMER_SWIRQ_BASE + (cpu))
#define HV_TIMER_V_SWIRQ(cpu)          (HV_TIMER_SWIRQ_BASE + MAX_CPUS + (cpu))
#define HV_TIMER_REFLECT_CALL_MAGIC    0x4e54414943ULL /* "NTAIC" */
#define HV_SYSREG_ASSIST_CALL_MAGIC    0x4e54535247ULL /* "NTSRG" */
#define HV_TIMER_REFLECT_CALL_REPOST_P 0x100
#define HV_TIMER_REFLECT_CALL_REPOST_V 0x101
/*
 * The AIC3 handover.  AIC2 announces "Windows owns the controller now" by
 * writing CONFIG |= ENABLE, which hv_aic.c watches for; AIC3 has no global
 * CONFIG register at all, so the HAL extension says so explicitly instead.
 */
#define HV_TIMER_REFLECT_CALL_CONTROLLER_READY 0x102
/*
 * Step reporting from the HAL extension, step code in x2.
 *
 * Its gNtasiTrace[] can only be read by walking the loaded-module list to find
 * the image, and that list does not exist during HAL init phase 0 -- exactly
 * where a failure to register lands.  Measured on J813: a boot that bugchecked
 * 0x5C there reported the extension "is not loaded", which was the reader
 * failing on an empty list rather than a fact about the extension.  This needs
 * no symbols and works at any stage.
 */
#define HV_TIMER_REFLECT_CALL_PROGRESS         0x103
#define NTASI_STEP_LOCAL_UNIT_READY            35
#define HV_TIMER_REFLECT_REPOST_STALE          0
#define HV_TIMER_REFLECT_REPOSTED              1
#define HV_TIMER_REFLECT_REPOST_ALREADY_UNREAD 2
#define HV_TIMER_REFLECT_READY_IGNORED         0
#define HV_TIMER_REFLECT_READY_ACCEPTED        1
#define HV_TIMER_REFLECT_READY_ALREADY         2
#define HV_TIMER_CTL_ENABLE                    BIT(0)
#define HV_TIMER_CTL_IMASK                     BIT(1)
/*
 * Timer IDs published by the T8142 Mu GTDT.  Apple delivers the underlying
 * sources as per-CPU FIQs, so these GSIVs are firmware-chosen compatibility
 * numbers rather than the conventional Arm GIC PPIs 30/27.  They must stay in
 * lockstep with T8142FamilyPkg.dsc.inc or Windows will acknowledge a line with
 * no clock ISR attached.
 */
#define HV_GIC_TIMER_P_INTID 17
#define HV_GIC_TIMER_V_INTID 18

/*
 * Windows ARM64 uses KPCR+0x24d8 (KPRCB.PanicStackBase) for synchronous
 * kernel exceptions and KPCR+0x24e0 (KPRCB.InterruptStackBase) in
 * KxSwitchStackAndPlayInterrupt. During AP startup TPIDR_EL1 becomes non-zero
 * before both stacks are guaranteed writable. Treating TPIDR_EL1 alone as
 * proof that the AP can receive a carrier SGI lets Windows take an exception
 * or interrupt on an uninitialised stack; the resulting nested trap corrupts
 * the PRCB and is later reported as CRITICAL_STRUCTURE_CORRUPTION (0x109).
 *
 * Keep this offset next to the carrier workaround rather than pretending it
 * is architectural.  The checks are read-only and fail closed: both ends of
 * the space consumed by KiKernelStackException must translate writable before
 * m1n1 observes x18 or drains an SGI to the AP.
 */
#define HV_WINDOWS_PANIC_STACK_SLOT_OFFSET     0x24d8
#define HV_WINDOWS_INTERRUPT_STACK_SLOT_OFFSET 0x24e0
#define HV_WINDOWS_PANIC_STACK_RESERVE         0x700
#endif

struct hv_pcpu_data {
    u32 ipi_queued;
    u32 ipi_pending;
    u32 pmc_pending;
    u64 pmc_irq_mode;
    u64 exc_entry_pmcr0_cnt;
    u64 mdscr;
    u64 guest_pmuserenr;
#ifdef ENABLE_VGIC_MODULE
    /* ICC_PMR_EL1 has no hardware backing on Apple cores. */
    u64 vgic_pmr;
    virq_queue_t irq_queue;
    virq_queue_t sgi_queue;
    virq_queue_t timer_queue;
    /* One pending bit per architectural SGI INTID (0..15). */
    u32 sgi_queued_mask;
    u32 sgi_coalesced;
#endif
#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
    //
    // windows-native-aic timer-FIQ reflector state, one instance per physical timer
    // source (P = CNTP, the non-secure EL1 physical timer; V = CNTV, the virtual
    // timer), per CPU. Only ever read/written by the owning core's own EL2 code (in
    // hv_update_fiq(), called from that core's own FIQ/sync/SError exit paths), same
    // as ipi_pending/pmc_pending above, so plain (non-atomic) read-modify-write is
    // sufficient -- there is no cross-core writer.
    //
    // *_fiq_count is a monotonic reflection-generation counter.  It increments
    // only when EL2 first observes and masks a source while no reflection is
    // pending; repeated observations of the same asserted timer condition are
    // intentionally coalesced and are not counted as separate guest events.
    // It is diagnostic state, not a delivered tick count.
    //
    // *_reflection_pending is true from the moment EL2 masks a new source until
    // the guest's timer-write completion edge.  EVENT consumption clears only
    // *_event_unread; it does not re-enable the physical source.  This keeps a
    // still-asserted timer from retrapping FIQ before Windows has programmed its
    // next deadline.  In Mu phase the same state gates redundant AIC_SW_SET;
    // Windows-ready phase uses the synthetic EVENT bridge instead.
    //
    u64 timer_p_fiq_count;
    u64 timer_v_fiq_count;
    bool timer_p_reflection_pending;
    bool timer_v_reflection_pending;
    bool timer_p_event_unread;
    bool timer_v_event_unread;
    bool native_doorbell_posted;
    bool carrier_timer_ready;
    bool carrier_stack_defer_logged;
    bool carrier_stack_ready;
    bool carrier_vi_logged;
    bool carrier_irq_active;
    /*
     * Set once this CPU has completed a full carrier IAR->EOI cycle.
     * That proves VBAR, KPCR, stack switching and PMR discipline all work.
     * After it, the exception-stack gate must not veto delivery: Windows
     * parks PanicStackBase/InterruptStackBase at 0 while a dispatch is in
     * flight -- which is exactly when an AP self-requests its software
     * interrupt via ICC_SGI1R_EL1. Re-checking the slots then deadlocks the
     * AP forever: NT will not restore them until the interrupt it is waiting
     * for is delivered. Observed on J414s CPU 4 (first Avalanche core),
     * 2026-07-27. This closes a carrier-progress hole; it is distinct from
     * the later NT scheduler 0xA caused by an invalid HAL LocalUnitId.
     */
    bool carrier_delivery_proven;
    u32 carrier_active_intid;
    u32 carrier_iar_count;
    u32 carrier_eoi_count;
    /*
     * Monotonic, read-only diagnostic counters for the guest Fast-IPI path.
     * Keep these at the tail so the established offsets of ipi_queued,
     * ipi_pending, native_doorbell_posted and the carrier counters do not
     * change. The 0x800-byte aligned structure has exactly 16 tail bytes.
     */
    u32 guest_ipi_send_count;
    u32 guest_ipi_tag_take_count;
    u32 guest_ipi_event_emit_count;
    u32 guest_ipi_commit_count;
#endif
} ALIGNED(64);

#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
static_assert(sizeof(struct hv_pcpu_data) == 0x800, "native-AIC pcpu debug ABI changed");
#endif

struct hv_pcpu_data pcpu[MAX_CPUS];

#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
/*
 * A native level IRQ can reach EL2 during the short interval in which a
 * synthetic timer/IPI wake has HCR.IMO asserted.  Keep this outside
 * hv_pcpu_data: that structure's exact 0x800-byte layout is a debugger ABI.
 * While set, doorbell synchronization must not restore IMO until EL1 reads
 * AIC EVENT and thereby auto-masks the real source. If the physical IRQ input
 * remains asserted for a bounded interval, EL2 reads EVENT exactly once,
 * retains the raw token, and replays it before any newer physical or synthetic
 * token. m1n1's private tick stays armed so an idle AP reaches that fallback.
 */
#define HV_NATIVE_AIC_ISR_IRQ_PENDING       BIT(7)
#define HV_NATIVE_AIC_REARM_CAPTURE_DIVISOR 4

static bool native_irq_rearm_deferred[MAX_CPUS];
static u32 native_irq_bounce_count[MAX_CPUS];
static u32 native_irq_entry_epoch[MAX_CPUS];
static u32 native_irq_rearm_entry_epoch[MAX_CPUS];
static u32 native_irq_rearm_start_time[MAX_CPUS];
static u32 native_irq_rearm_still_asserted_count[MAX_CPUS];
static u32 native_irq_rearm_release_count[MAX_CPUS];
/*
 * AIC3 announces local-unit readiness before InitializeIoUnit. Keep the last
 * progress marker per CPU so the otherwise-identical CONTROLLER_READY SMCs can
 * be separated without changing the already-deployed v5 HAL extension:
 *
 *   step 35 + CONTROLLER_READY  -> keep the startup carrier
 *   step 23 + CONTROLLER_READY  -> InitializeIoUnit is live; retire carrier
 *
 * These stay outside hv_pcpu_data, whose 0x800-byte layout is a debugger ABI.
 */
static u32 native_aic_last_progress[MAX_CPUS];
static bool native_aic_local_unit_ready[MAX_CPUS];

static bool hv_native_aic_carrier_local_ready(void)
{
    u32 cpu = smp_id();

    return cpu < MAX_CPUS && native_aic_local_unit_ready[cpu] && hv_native_aic_windows_active() &&
           !hv_native_aic_windows_ready();
}
static u32 native_irq_rearm_capture_count[MAX_CPUS];
static u32 native_irq_rearm_empty_capture_count[MAX_CPUS];
static u32 native_irq_rearm_replay_count[MAX_CPUS];
static volatile u32 native_irq_rearm_last_captured_event[MAX_CPUS];
static bool native_irq_rearm_replay_valid[MAX_CPUS];
static u32 native_irq_rearm_replay_event[MAX_CPUS];

/*
 * Timer reflection latency evidence lives outside hv_pcpu_data so the stable
 * debugger ABI remains exactly 0x800 bytes.  Every field has one CPU-local
 * writer.  The host reads this structure only after stopping the guest.
 *
 * Source 0 is CNTP and source 1 is CNTV.  Host-counter deltas measure time
 * spent in the reflector independent of CNTVOFF_EL2.  The deadline fields use
 * the same architectural counter domain as the corresponding guest compare
 * register, so a late physical FIQ is distinguishable from a delayed EVENT or
 * a Windows handler that accepted EVENT but did not reprogram its timer.
 */
struct hv_native_aic_timer_diag {
    u64 last_programmed_cval[2];
    u64 last_fiq_counter[2];
    u64 last_fiq_guest_counter[2];
    u64 last_event_counter[2];
    u64 last_rearm_counter[2];
    u64 max_deadline_lateness[2];
    u64 max_fiq_to_event[2];
    u64 max_event_to_rearm[2];
    u64 event_count[2];
    u64 ipi_bypass_count[2];
    u64 max_ipi_bypass_age[2];
    u64 repost_count[2];
    u64 repost_already_unread_count[2];
    u64 repost_stale_count[2];
};

/*
 * Locked Apple system registers cannot service the timer reflection path.
 * Use the architectural timer mask unless this CPU exposes the legacy bank.
 */
static inline void vm_tmr_fiq_set(u64 bits)
{
    if (cpu_features->apple_sysregs_unlocked) {
        reg_set(SYS_IMP_APL_VM_TMR_FIQ_ENA_EL2, bits);
        return;
    }

    if (bits & VM_TMR_FIQ_ENA_ENA_P)
        reg_clr(SYS_CNTP_CTL_EL02, CNTx_CTL_IMASK);
    if (bits & VM_TMR_FIQ_ENA_ENA_V)
        reg_clr(SYS_CNTV_CTL_EL02, CNTx_CTL_IMASK);
}

static inline void vm_tmr_fiq_clr(u64 bits)
{
    if (cpu_features->apple_sysregs_unlocked) {
        reg_clr(SYS_IMP_APL_VM_TMR_FIQ_ENA_EL2, bits);
        return;
    }

    if (bits & VM_TMR_FIQ_ENA_ENA_P)
        reg_set(SYS_CNTP_CTL_EL02, CNTx_CTL_IMASK);
    if (bits & VM_TMR_FIQ_ENA_ENA_V)
        reg_set(SYS_CNTV_CTL_EL02, CNTx_CTL_IMASK);
}

static inline bool hv_timer_firing(u64 ctl)
{
    return (ctl & (CNTx_CTL_ISTATUS | CNTx_CTL_ENABLE)) == (CNTx_CTL_ISTATUS | CNTx_CTL_ENABLE);
}

static struct hv_native_aic_timer_diag native_aic_timer_diag[MAX_CPUS];

static unsigned int hv_timer_diag_source(bool physical)
{
    return physical ? 0 : 1;
}

static void hv_timer_diag_note_fiq(bool physical)
{
    unsigned int source = hv_timer_diag_source(physical);
    struct hv_native_aic_timer_diag *diag = &native_aic_timer_diag[smp_id()];
    u64 guest_counter = physical ? hv_host_counter() : mrs(CNTVCT_EL0);
    u64 cval = physical ? mrs(CNTP_CVAL_EL02) : mrs(CNTV_CVAL_EL02);
    u64 lateness = (s64)(guest_counter - cval) > 0 ? guest_counter - cval : 0;

    diag->last_fiq_counter[source] = hv_host_counter();
    diag->last_fiq_guest_counter[source] = guest_counter;
    diag->last_programmed_cval[source] = cval;
    if (lateness > diag->max_deadline_lateness[source])
        diag->max_deadline_lateness[source] = lateness;
}

static void hv_timer_diag_note_event(bool physical)
{
    unsigned int source = hv_timer_diag_source(physical);
    struct hv_native_aic_timer_diag *diag = &native_aic_timer_diag[smp_id()];
    u64 now = hv_host_counter();
    u64 latency = now - diag->last_fiq_counter[source];

    diag->last_event_counter[source] = now;
    diag->event_count[source]++;
    if (latency > diag->max_fiq_to_event[source])
        diag->max_fiq_to_event[source] = latency;
}

static void hv_timer_diag_note_rearm(bool physical)
{
    unsigned int source = hv_timer_diag_source(physical);
    struct hv_native_aic_timer_diag *diag = &native_aic_timer_diag[smp_id()];
    u64 now = hv_host_counter();

    if (diag->last_event_counter[source] != 0 &&
        diag->last_rearm_counter[source] < diag->last_event_counter[source]) {
        u64 latency = now - diag->last_event_counter[source];

        if (latency > diag->max_event_to_rearm[source])
            diag->max_event_to_rearm[source] = latency;
    }
    diag->last_rearm_counter[source] = now;
    diag->last_programmed_cval[source] = physical ? mrs(CNTP_CVAL_EL02) : mrs(CNTV_CVAL_EL02);
}

static void hv_timer_diag_note_ipi_bypass(void)
{
    struct hv_native_aic_timer_diag *diag = &native_aic_timer_diag[smp_id()];
    u64 now = hv_host_counter();

    for (unsigned int source = 0; source < 2; source++) {
        bool unread = source == 0 ? PERCPU(timer_p_event_unread) : PERCPU(timer_v_event_unread);

        if (!unread)
            continue;
        u64 age = now - diag->last_fiq_counter[source];
        diag->ipi_bypass_count[source]++;
        if (age > diag->max_ipi_bypass_age[source])
            diag->max_ipi_bypass_age[source] = age;
    }
}

static bool hv_carrier_irq_pending(void);
static void hv_native_aic_doorbell_sync(void);

static void hv_native_aic_exception_entry(void)
{
    native_irq_entry_epoch[smp_id()]++;
}
#endif

void hv_exit_guest(void) __attribute__((noreturn));

static u64 stolen_time = 0;
static u64 exc_entry_time;
extern u64 hv_cpus_in_guest;
extern u64 hv_rendezvous_pending;
extern int hv_pinned_cpu;
extern int hv_want_cpu;

static bool time_stealing = true;

void init_vgic_irq_queues(void)
{
#ifdef ENABLE_VGIC_MODULE
    for (int i = 0; i < MAX_CPUS; i++) {
        virq_queue_init(&PERCPU_N(i, irq_queue));
        virq_queue_init(&PERCPU_N(i, sgi_queue));
        virq_queue_init(&PERCPU_N(i, timer_queue));
        __atomic_store_n(&PERCPU_N(i, sgi_queued_mask), 0, __ATOMIC_RELAXED);
        __atomic_store_n(&PERCPU_N(i, sgi_coalesced), 0, __ATOMIC_RELAXED);
        PERCPU_N(i, guest_pmuserenr) = 0;
    }
#endif
}

#ifdef ENABLE_VGIC_MODULE
/*
 * A GIC SGI is a pending state, not an edge counter.  Windows can write the
 * same ICC_SGI1R target repeatedly while that SGI is already queued.  Keeping
 * every write in a FIFO replays stale IPIs after EOI and can corrupt scheduler
 * state during processor startup.  Keep at most one queued instance per
 * target/INTID; once the target removes it from the queue, a new write can
 * become the active+pending state in hv_vgic3_inject_irq().
 */
static bool hv_sgi_queue_push(int cpu, const virq_t *pending)
{
    if (cpu < 0 || cpu >= MAX_CPUS || pending->vintid >= 16)
        return false;

    u32 bit = (u32)BIT(pending->vintid);
    if (__atomic_fetch_or(&PERCPU_N(cpu, sgi_queued_mask), bit, __ATOMIC_ACQ_REL) & bit) {
        __atomic_fetch_add(&PERCPU_N(cpu, sgi_coalesced), 1, __ATOMIC_RELAXED);
        return false;
    }

    if (virq_queue_push(&PERCPU_N(cpu, sgi_queue), pending))
        return true;

    __atomic_fetch_and(&PERCPU_N(cpu, sgi_queued_mask), ~bit, __ATOMIC_RELEASE);
    return false;
}

#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
/*
 * Windows starts issuing Apple Fast-IPIs while some processors are still in
 * the short GIC startup-carrier phase.  Do not consume those sends into the
 * native EVENT transaction before the first Windows AIC2 CONFIG enable: the
 * readiness bit is global, HCR.VI is CPU-local, and an AP can otherwise take
 * the transport FIQ just before readiness changes, return with no doorbell,
 * mask FIQs in KiInitializeKernel, and never enter EL2 again to arm it.
 *
 * Keep the request as carrier SGI 0 during that window.  If CONFIG becomes
 * ready before the target handles it, hv_carrier_migrate_sgis_to_native()
 * converts the queued SGI into the native transactional EVENT.  If not, the
 * existing carrier path delivers it directly.  Either ordering therefore
 * leaves a CPU-local HCR.VI armed before the target returns to EL1.
 */
static bool hv_guest_ipi_queue_pre_config_carrier(int cpu)
{
    if (!hv_native_aic_windows_active() || hv_native_aic_windows_ready())
        return false;

    virq_t pending = {
        .vintid = 0,
        .priority = hv_vgic3_get_priority_cpu(cpu, 0),
        .active = false,
        .pending = true,
        .hw_status = false,
        .hw_irq = 0,
    };

    if (hv_sgi_queue_push(cpu, &pending))
        smp_send_ipi(cpu);
    return true;
}
#endif

static bool hv_sgi_queue_pop(virq_t *pending)
{
    if (!virq_queue_pop(&PERCPU(sgi_queue), pending))
        return false;

    if (pending->vintid < 16) {
        u32 bit = (u32)BIT(pending->vintid);
        __atomic_fetch_and(&PERCPU(sgi_queued_mask), ~bit, __ATOMIC_RELEASE);
    }
    return true;
}
#endif

#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
//
// windows-native-aic: initialize per-CPU timer reflection state and clear any
// stale Mu-only AIC software events.  The reserved lines are not configured as
// Windows timer affinity targets here: J414s AIC2 target/SW_SET probing did not
// establish a valid per-CPU encoding, and the Windows-ready path does not use
// these lines.  Must run after aic_init() so the implementation-private range
// derived from aic->nr_irq is valid.
//
void hv_timer_reflect_init(void)
{
    memset(native_aic_timer_diag, 0, sizeof(native_aic_timer_diag));
    memset(native_aic_last_progress, 0, sizeof(native_aic_last_progress));
    memset(native_aic_local_unit_ready, 0, sizeof(native_aic_local_unit_ready));
    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        PERCPU_N(cpu, ipi_queued) = 0;
        PERCPU_N(cpu, ipi_pending) = 0;
        PERCPU_N(cpu, timer_p_reflection_pending) = false;
        PERCPU_N(cpu, timer_v_reflection_pending) = false;
        PERCPU_N(cpu, timer_p_event_unread) = false;
        PERCPU_N(cpu, timer_v_event_unread) = false;
        PERCPU_N(cpu, native_doorbell_posted) = false;
        PERCPU_N(cpu, carrier_timer_ready) = false;
        PERCPU_N(cpu, carrier_stack_defer_logged) = false;
        PERCPU_N(cpu, carrier_stack_ready) = false;
        PERCPU_N(cpu, carrier_vi_logged) = false;
        PERCPU_N(cpu, carrier_irq_active) = false;
        PERCPU_N(cpu, carrier_delivery_proven) = false;
        PERCPU_N(cpu, carrier_active_intid) = 0x3ff;
        PERCPU_N(cpu, carrier_iar_count) = 0;
        PERCPU_N(cpu, carrier_eoi_count) = 0;
        PERCPU_N(cpu, guest_ipi_send_count) = 0;
        PERCPU_N(cpu, guest_ipi_tag_take_count) = 0;
        PERCPU_N(cpu, guest_ipi_event_emit_count) = 0;
        PERCPU_N(cpu, guest_ipi_commit_count) = 0;
        native_irq_rearm_deferred[cpu] = false;
        native_irq_bounce_count[cpu] = 0;
        native_irq_entry_epoch[cpu] = 0;
        native_irq_rearm_entry_epoch[cpu] = 0;
        native_irq_rearm_start_time[cpu] = 0;
        native_irq_rearm_still_asserted_count[cpu] = 0;
        native_irq_rearm_release_count[cpu] = 0;
        native_irq_rearm_capture_count[cpu] = 0;
        native_irq_rearm_empty_capture_count[cpu] = 0;
        native_irq_rearm_replay_count[cpu] = 0;
        native_irq_rearm_last_captured_event[cpu] = 0;
        native_irq_rearm_replay_valid[cpu] = false;
        native_irq_rearm_replay_event[cpu] = 0;
        aic_set_sw(HV_TIMER_P_SWIRQ(cpu), false);
        aic_set_sw(HV_TIMER_V_SWIRQ(cpu), false);
    }
    vm_tmr_fiq_clr(VM_TMR_FIQ_ENA_ENA_P | VM_TMR_FIQ_ENA_ENA_V);
    printf("HV: windows-native-aic: holding timer FIQ until Mu AIC is ready\n");
}

void hv_timer_native_enable(void)
{
    PERCPU(timer_p_reflection_pending) = false;
    PERCPU(timer_v_reflection_pending) = false;
    PERCPU(timer_p_event_unread) = false;
    PERCPU(timer_v_event_unread) = false;
    PERCPU(native_doorbell_posted) = false;
    vm_tmr_fiq_set(VM_TMR_FIQ_ENA_ENA_P | VM_TMR_FIQ_ENA_ENA_V);
}

void hv_timer_reflect_enable(void)
{
    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        aic_set_sw(HV_TIMER_P_SWIRQ(cpu), false);
        aic_set_sw(HV_TIMER_V_SWIRQ(cpu), false);
        PERCPU_N(cpu, carrier_timer_ready) = false;
        PERCPU_N(cpu, carrier_stack_ready) = false;
        PERCPU_N(cpu, carrier_delivery_proven) = false;
        PERCPU_N(cpu, carrier_vi_logged) = false;
        PERCPU_N(cpu, native_doorbell_posted) = false;
    }
    PERCPU(timer_p_reflection_pending) = false;
    PERCPU(timer_v_reflection_pending) = false;
    PERCPU(timer_p_event_unread) = false;
    PERCPU(timer_v_event_unread) = false;
    PERCPU(native_doorbell_posted) = false;
    vm_tmr_fiq_set(VM_TMR_FIQ_ENA_ENA_P | VM_TMR_FIQ_ENA_ENA_V);
    printf("HV: windows-native-aic: enabled FIQ-to-AIC-EVENT timer bridge\n");
}

void hv_timer_reflect_hold(void)
{
    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        aic_set_sw(HV_TIMER_P_SWIRQ(cpu), false);
        aic_set_sw(HV_TIMER_V_SWIRQ(cpu), false);
        PERCPU_N(cpu, timer_p_reflection_pending) = false;
        PERCPU_N(cpu, timer_v_reflection_pending) = false;
        PERCPU_N(cpu, timer_p_event_unread) = false;
        PERCPU_N(cpu, timer_v_event_unread) = false;
        PERCPU_N(cpu, native_doorbell_posted) = false;
        PERCPU_N(cpu, carrier_timer_ready) = false;
    }
    hv_native_aic_apply_hcr_route(HV_NATIVE_AIC_HCR_CLEAR_VI);
    vm_tmr_fiq_clr(VM_TMR_FIQ_ENA_ENA_P | VM_TMR_FIQ_ENA_ENA_V);
    printf("HV: windows-native-aic: holding timer bridge until Windows enables AIC2\n");
}

void hv_carrier_retire_active_sgis(void)
{
#if defined(ENABLE_VGIC_MODULE) && defined(ENABLE_NATIVE_AIC_PASSTHROUGH)
    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        u32 intid = __atomic_load_n(&pcpu[cpu].carrier_active_intid, __ATOMIC_ACQUIRE);

        /*
         * CONFIG changes the interrupt-controller callbacks globally.  An AP
         * can already have accepted a startup-carrier SGI when the BSP makes
         * that change; its ISR then completes through the native AIC callback
         * and no GIC EOIR is issued.  Leaving the software carrier active in
         * that case suppresses HCR.VI forever on the AP.  The SGI has already
         * reached Windows, so retire only accepted SGIs here.  Other carrier
         * interrupt classes remain fail-closed for diagnosis.
         */
        if (__atomic_load_n(&pcpu[cpu].carrier_irq_active, __ATOMIC_ACQUIRE) && intid < 16) {
            __atomic_store_n(&pcpu[cpu].carrier_active_intid, 0x3ff, __ATOMIC_RELEASE);
            __atomic_store_n(&pcpu[cpu].carrier_irq_active, false, __ATOMIC_RELEASE);
            hv_native_aic_trace_record(HV_NATIVE_AIC_TRACE_CARRIER_RETIRE, cpu, intid);
        }
    }
#endif
}

static bool hv_guest_ipi_doorbell_pending(void)
{
    u32 state = PERCPU(ipi_pending);

    return ((state & HV_GUEST_IPI_DELIVERABLE) && !(state & HV_GUEST_IPI_INFLIGHT)) ||
           (state & HV_GUEST_IPI_RETRY_ARMED);
}

static u32 hv_guest_ipi_clock_now(void)
{
    return (u32)(hv_host_counter() >> HV_GUEST_IPI_CLOCK_SHIFT) & HV_GUEST_IPI_CLOCK_MASK;
}

static u32 hv_guest_ipi_stamp(u32 state)
{
    state &= ~HV_GUEST_IPI_START_TIME;
    state |= FIELD_PREP(HV_GUEST_IPI_START_TIME, hv_guest_ipi_clock_now());
    return state;
}

static bool hv_guest_ipi_begin_delivery(u32 *event_token)
{
    u32 state = PERCPU(ipi_pending);
    u32 next_generation;
    bool carrier;

    if (event_token == NULL)
        return false;

    if ((state & HV_GUEST_IPI_INFLIGHT) && (state & HV_GUEST_IPI_RETRY_ARMED)) {
        /* Re-emit the same uncommitted generation. */
        state &= ~HV_GUEST_IPI_RETRY_ARMED;
    } else if ((state & HV_GUEST_IPI_DELIVERABLE) && !(state & HV_GUEST_IPI_INFLIGHT)) {
        /*
         * Begin a new transaction. Bits 30:24 of the synthetic EVENT carry a
         * seven-bit generation, while bit 31 records that this wake came from
         * a GIC-carrier SGI migrated across CONFIG. The origin bit lets the
         * HAL distinguish real class-0 carrier work from an ordinary
         * redundant Fast-IPI edge whose class bitmap is already empty.
         */
        carrier = (state & HV_GUEST_IPI_QUEUED_CARRIER) != 0;
        next_generation = (FIELD_GET(HV_GUEST_IPI_GENERATION, state) + 1) & MASK(7);
        state &=
            ~(HV_GUEST_IPI_DELIVERABLE | HV_GUEST_IPI_QUEUED_CARRIER | HV_GUEST_IPI_RETRY_ARMED |
              HV_GUEST_IPI_RETRY_COUNT | HV_GUEST_IPI_START_TIME | HV_GUEST_IPI_EVENT_TOKEN);
        state |= HV_GUEST_IPI_INFLIGHT | FIELD_PREP(HV_GUEST_IPI_GENERATION, next_generation);
        if (carrier)
            state |= HV_GUEST_IPI_INFLIGHT_CARRIER;
    } else {
        return false;
    }

    PERCPU(ipi_pending) = hv_guest_ipi_stamp(state);
    PERCPU(guest_ipi_event_emit_count)++;
    *event_token = FIELD_GET(HV_GUEST_IPI_EVENT_TOKEN, state);
    HV_NATIVE_AIC_HOT_TRACE(HV_NATIVE_AIC_TRACE_IPI_BEGIN, state, *event_token);
    return true;
}

static bool hv_guest_ipi_commit_delivery(void)
{
    u32 state = PERCPU(ipi_pending);

    if (!(state & HV_GUEST_IPI_INFLIGHT))
        return false;

    /* Preserve a newer DELIVERABLE generation across this EOI commit. */
    state &= ~(HV_GUEST_IPI_INFLIGHT | HV_GUEST_IPI_INFLIGHT_CARRIER | HV_GUEST_IPI_RETRY_ARMED |
               HV_GUEST_IPI_RETRY_COUNT | HV_GUEST_IPI_START_TIME);
    PERCPU(ipi_pending) = state;
    PERCPU(guest_ipi_commit_count)++;
    HV_NATIVE_AIC_HOT_TRACE(HV_NATIVE_AIC_TRACE_IPI_COMMIT, state, 0);
    return true;
}

static void hv_guest_ipi_retry_tick(void)
{
    u32 state;
    u32 retries;
    u32 now;
    u32 started;
    u32 elapsed;
    u32 delay;

    if (!hv_native_aic_windows_ready())
        return;

    state = PERCPU(ipi_pending);
    if (!(state & HV_GUEST_IPI_INFLIGHT) || (state & HV_GUEST_IPI_RETRY_ARMED))
        return;

    now = hv_guest_ipi_clock_now();
    started = FIELD_GET(HV_GUEST_IPI_START_TIME, state);
    elapsed = (now - started) & HV_GUEST_IPI_CLOCK_MASK;
    delay = (u32)((mrs(CNTFRQ_EL0) / 4) >> HV_GUEST_IPI_CLOCK_SHIFT);
    if (delay == 0)
        delay = 1;
    if (elapsed < delay)
        return;

    /*
     * A non-interruptible AP receives m1n1's one-Hz slow tick even while EL1
     * is parked in WFI.  The boot CPU ticks at 5 kHz, so use the architectural
     * counter rather than tick count and wait at least 250 ms between attempts.
     * The stored 17-bit coarse timestamp wraps after tens of minutes on J414s;
     * unsigned modular subtraction is unambiguous for this sub-second delay.
     * Retry frequency is bounded, but the transport is never abandoned while
     * NT still owes an EOI. Saturate the diagnostic count rather than recreating
     * the original permanent INFLIGHT-with-no-doorbell hang after a fixed cap.
     */
    retries = FIELD_GET(HV_GUEST_IPI_RETRY_COUNT, state);
    if (retries < HV_GUEST_IPI_RETRY_COUNT_MAX)
        retries++;
    state &= ~HV_GUEST_IPI_RETRY_COUNT;
    state |= FIELD_PREP(HV_GUEST_IPI_RETRY_COUNT, retries) | HV_GUEST_IPI_RETRY_ARMED;
    PERCPU(ipi_pending) = hv_guest_ipi_stamp(state);
}

/*
 * A real AIC IRQ can enter EL2 during the short HCR.IMO window used for a
 * synthetic timer/IPI doorbell.  The direct handoff clears IMO and leaves the
 * destructive EVENT read to Windows.  Usually that read clears the deferred
 * guard immediately, but another CPU can mask or the device can deassert the
 * source before this CPU reaches EVENT.  Waiting only for that read then
 * suppresses every later Fast-IPI forever.
 *
 * Release only from a later top-level EL2 entry and only after ISR_EL1.I says
 * the physical IRQ input is quiescent.  If a new source races the check, the
 * restored IMO route takes it back through the same bounded handoff.  This is
 * the UNREAD_REAL quiescence rule from the reviewed capture/replay model,
 * without putting physical EVENT reads or a replay FIFO in the direct path.
 */
static void hv_native_aic_progress_deferred_irq(void)
{
    u32 cpu = smp_id();
    u32 now;
    u32 elapsed;
    u32 delay;
    u32 raw_event;
    u32 type;
    u32 die;
    u32 irq;
    u32 flat_irq;

    if (!native_irq_rearm_deferred[cpu] ||
        native_irq_rearm_entry_epoch[cpu] == native_irq_entry_epoch[cpu])
        return;

    sysop("isb");
    if (mrs(ISR_EL1) & HV_NATIVE_AIC_ISR_IRQ_PENDING) {
        native_irq_rearm_still_asserted_count[cpu]++;
        /*
         * The direct path gets a full quarter second to reach Windows EVENT.
         * If IRQ remains asserted after that, retaining UNREAD_REAL forever
         * suppresses every later timer and Fast-IPI doorbell. Capture only in
         * this cold fallback, and only when the one-token replay slot is free.
         *
         * The coarse counter is the same wrap-safe clock used by the IPI retry
         * transaction. An idle AP enters EL2 at least once per second, so this
         * remains bounded even when Windows parks the target in WFI.
         */
        now = hv_guest_ipi_clock_now();
        elapsed = (now - native_irq_rearm_start_time[cpu]) & HV_GUEST_IPI_CLOCK_MASK;
        delay = (u32)((mrs(CNTFRQ_EL0) / HV_NATIVE_AIC_REARM_CAPTURE_DIVISOR) >>
                      HV_GUEST_IPI_CLOCK_SHIFT);
        if (delay == 0)
            delay = 1;
        if (elapsed < delay || native_irq_rearm_replay_valid[cpu])
            return;

        /*
         * EVENT is destructive and auto-masks a hardware source. Retain every
         * nonzero raw token before allowing the synthetic doorbell to overtake
         * it. A zero read proves no token was consumed; releasing the stale
         * guard is then safe. If a physical source races the route change it
         * simply re-enters this IRQ path and acquires a fresh guard.
         */
        raw_event = aic_ack();
        sysop("dsb sy");
        sysop("isb");
        native_irq_rearm_capture_count[cpu]++;
        native_irq_rearm_last_captured_event[cpu] = raw_event;
        native_irq_rearm_deferred[cpu] = false;
        native_irq_rearm_start_time[cpu] = 0;
        native_irq_rearm_release_count[cpu]++;

        if (raw_event == 0) {
            native_irq_rearm_empty_capture_count[cpu]++;
            HV_NATIVE_AIC_HOT_TRACE(HV_NATIVE_AIC_TRACE_IRQ_REARM, elapsed, 0);
            return;
        }

        type = FIELD_GET(AIC_EVENT_TYPE, raw_event);
        die = FIELD_GET(AIC_EVENT_DIE, raw_event);
        irq = FIELD_GET(AIC_EVENT_NUM, raw_event);
        flat_irq = die * aic->max_irq + irq;
        if (type == AIC_EVENT_TYPE_HW && flat_irq >= HV_TIMER_SWIRQ_BASE &&
            flat_irq < HV_TIMER_SWIRQ_BASE + (2 * MAX_CPUS)) {
            aic_set_sw(flat_irq, false);
            aic_set_mask(flat_irq, true);
            HV_NATIVE_AIC_HOT_TRACE(HV_NATIVE_AIC_TRACE_EVENT_RESERVED, flat_irq, raw_event);
            return;
        }

        native_irq_rearm_replay_event[cpu] = raw_event;
        native_irq_rearm_replay_valid[cpu] = true;
        HV_NATIVE_AIC_HOT_TRACE(HV_NATIVE_AIC_TRACE_IRQ_REARM, elapsed, raw_event);
        return;
    }

    native_irq_rearm_deferred[cpu] = false;
    native_irq_rearm_start_time[cpu] = 0;
    native_irq_rearm_release_count[cpu]++;
    HV_NATIVE_AIC_HOT_TRACE(HV_NATIVE_AIC_TRACE_IRQ_REARM, native_irq_rearm_entry_epoch[cpu],
                            native_irq_entry_epoch[cpu]);
}

/*
 * Guest EVENT reads, i.e. "Windows' interrupt handler actually ran".  The one
 * number that separates "the timer bridge is delivering" from "EL2 is posting
 * doorbells nobody collects"; see hv_native_aic_delivery_report().
 */
static u32 native_event_read_count[MAX_CPUS];

/*
 * One line per second once Windows owns the AIC, answering the only question
 * that matters after the handover: does an interrupt actually reach the guest?
 *
 * fiq   - the Apple timer FIQ arrived at EL2 and was coalesced
 * evrd  - the guest read the AIC EVENT aperture, i.e. its ISR ran
 * IMO/VI - what EL2 is asking for right now
 * gI    - guest PSTATE.I at the last trap; a guest spinning with interrupts
 *         masked cannot take VI no matter how correct the routing is, which is
 *         the difference between "not delivered" and "not delivarable".
 */
void hv_native_aic_delivery_report(void)
{
    u32 fiq = 0, evrd = 0, unread = 0;
    u64 hcr = mrs(HCR_EL2);
    u64 spsr = mrs(SPSR_EL2);

    if (!hv_native_aic_windows_ready())
        return;

    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        fiq += PERCPU_N(cpu, timer_p_fiq_count) + PERCPU_N(cpu, timer_v_fiq_count);
        evrd += native_event_read_count[cpu];
        unread += PERCPU_N(cpu, timer_p_event_unread) || PERCPU_N(cpu, timer_v_event_unread);
    }

    /*
     * Keep reporting IGRPEN1 as phase evidence, not as a controller-owner
     * verdict. J813 proved that Windows can clear it and still acknowledge the
     * transition clock through ICC_IAR1_EL1 until InitializeIoUnit installs the
     * native EVENT callback.
     */
    printf("HV: native-aic: fiq=%u evrd=%u unread=%u IMO=%d VI=%d gI=%d gEL=%d "
           "igrpen1=%d elr=0x%lx\n",
           fiq, evrd, unread, !!(hcr & HCR_IMO), !!(hcr & HCR_VI), !!(spsr & BIT(7)),
           (int)((spsr >> 2) & 3), !!hv_vgic3_get_igrpen1(), (unsigned long)mrs(ELR_EL2));
}

static void hv_native_aic_doorbell_sync(void)
{
    bool pending;
    bool carrier_active = hv_native_aic_windows_ready() && PERCPU(carrier_irq_active);

    if (!hv_native_aic_windows_ready())
        return;

    hv_native_aic_progress_deferred_irq();
    pending = hv_guest_ipi_doorbell_pending() || PERCPU(timer_p_event_unread) ||
              PERCPU(timer_v_event_unread) || native_irq_rearm_replay_valid[smp_id()];
    if (native_irq_rearm_deferred[smp_id()]) {
        PERCPU(native_doorbell_posted) = false;
        hv_native_aic_apply_hcr_route(HV_NATIVE_AIC_HCR_PASSTHROUGH);
        return;
    }

    if (carrier_active) {
        PERCPU(native_doorbell_posted) = false;
        hv_native_aic_apply_hcr_route(HV_NATIVE_AIC_HCR_PASSTHROUGH);
        return;
    }

    if (pending) {
        PERCPU(native_doorbell_posted) = true;
        hv_native_aic_apply_hcr_route(HV_NATIVE_AIC_HCR_SYNTHETIC_DOORBELL);
    } else {
        PERCPU(native_doorbell_posted) = false;
        hv_native_aic_apply_hcr_route(HV_NATIVE_AIC_HCR_PASSTHROUGH);
    }
}
#endif

static void _hv_exc_proxy(struct exc_info *ctx, uartproxy_boot_reason_t reason, u32 type,
                          void *extra)
{
    int from_el = FIELD_GET(SPSR_M, ctx->spsr) >> 2;

    hv_wdt_breadcrumb('P');

    /*
     * Get all the CPUs into the HV before running the proxy, to make sure they all exit to
     * the guest with a consistent time offset.
     */
    if (time_stealing)
        hv_rendezvous();

    u64 entry_time = hv_host_counter();

    ctx->elr_phys = hv_translate(ctx->elr, false, false, NULL);
    ctx->far_phys = hv_translate(ctx->far, false, false, NULL);
    ctx->sp_phys = hv_translate(from_el == 0 ? ctx->sp[0] : ctx->sp[1], false, false, NULL);
    ctx->extra = extra;

    struct uartproxy_msg_start start = {
        .reason = reason,
        .code = type,
        .info = ctx,
    };

    hv_wdt_suspend();
    int ret = uartproxy_run(&start);
    hv_wdt_resume();

    switch (ret) {
        case EXC_RET_HANDLED:
            hv_wdt_breadcrumb('p');
            if (time_stealing) {
                u64 lost = hv_host_counter() - entry_time;
                stolen_time += lost;
            }
            break;
        case EXC_EXIT_GUEST:
            hv_rendezvous();
            spin_unlock(&bhl);
            hv_exit_guest(); // does not return
        default:
            printf("Guest exception not handled, rebooting.\n");
            print_regs(ctx->regs, 0);
            flush_and_reboot(); // does not return
    }
}

static void hv_maybe_switch_cpu(struct exc_info *ctx, uartproxy_boot_reason_t reason, u32 type,
                                void *extra)
{
    while (hv_want_cpu != -1) {
        if (hv_want_cpu == smp_id()) {
            hv_want_cpu = -1;
            _hv_exc_proxy(ctx, reason, type, extra);
        } else {
            // Unlock the HV so the target CPU can get into the proxy
            spin_unlock(&bhl);
            while (hv_want_cpu != -1)
                sysop("dmb sy");
            spin_lock(&bhl);
        }
    }
}

void hv_exc_proxy(struct exc_info *ctx, uartproxy_boot_reason_t reason, u32 type, void *extra)
{
    /*
     * Wait while another CPU is pinned or being switched to.
     * If a CPU switch is requested, handle it before actually handling the
     * exception. We still tell the host the real reason code, though.
     */
    while ((hv_pinned_cpu != -1 && hv_pinned_cpu != smp_id()) || hv_want_cpu != -1) {
        if (hv_want_cpu == smp_id()) {
            hv_want_cpu = -1;
            _hv_exc_proxy(ctx, reason, type, extra);
        } else {
            // Unlock the HV so the target CPU can get into the proxy
            spin_unlock(&bhl);
            while ((hv_pinned_cpu != -1 && hv_pinned_cpu != smp_id()) || hv_want_cpu != -1)
                sysop("dmb sy");
            spin_lock(&bhl);
        }
    }

    /* Handle the actual exception */
    _hv_exc_proxy(ctx, reason, type, extra);

    /*
     * If as part of handling this exception we want to switch CPUs, handle it without returning
     * to the guest.
     */
    hv_maybe_switch_cpu(ctx, reason, type, extra);
}

void hv_set_time_stealing(bool enabled, bool reset)
{
    time_stealing = enabled;
    if (reset)
        stolen_time = 0;
}

void hv_add_time(s64 time)
{
    stolen_time -= (u64)time;
}

static void hv_windows_update_carrier_readiness(struct exc_info *ctx)
{
#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
    if (ctx == NULL || !hv_native_aic_windows_active() || hv_native_aic_windows_ready() ||
        (FIELD_GET(SPSR_M, ctx->spsr) >> 2) != 1 || ctx->regs[18] == 0 ||
        PERCPU(carrier_stack_ready))
        return;

    /*
     * This gate exists only for the pre-AIC startup carrier.  Windows owns x18
     * architecturally and may legally use it as scratch after saving it.  The
     * 26200 exception-vector hardening stubs do exactly that: they save x18,
     * count it down to zero, execute SB, and reload it.  A bare x18 == 0 is
     * therefore not evidence of register loss and must never cause EL2 to
     * rewrite guest state.
     *
     * TPIDR_EL1 may carry Windows-private low-bit tags while the established
     * KPCR alias is page aligned.  Admit startup delivery only when Windows
     * itself presents the exact tag-stripped alias in x18.  If the register is
     * absent or temporarily in use, the queued carrier remains pending.
     */
    u64 guest_pcr = mrs(TPIDR_EL1) & ~0xfffULL;
    if (guest_pcr == 0 || ctx->regs[18] != guest_pcr)
        return;

    /*
     * Do not cache this translation. Windows changes AP page tables during
     * bring-up and can tear the temporary KPCR mapping down after a startup
     * timeout. A once-valid PanicStackBase is therefore not proof that a later
     * carrier exception is safe.
     */
    u64 stack_slot =
        hv_translate(guest_pcr + HV_WINDOWS_PANIC_STACK_SLOT_OFFSET, false, false, NULL);
    u64 panic_stack = stack_slot ? read64(stack_slot) : 0;
    u64 interrupt_stack_slot =
        hv_translate(guest_pcr + HV_WINDOWS_INTERRUPT_STACK_SLOT_OFFSET, false, false, NULL);
    u64 interrupt_stack = interrupt_stack_slot ? read64(interrupt_stack_slot) : 0;
    bool panic_stack_ready =
        panic_stack >= HV_WINDOWS_PANIC_STACK_RESERVE &&
        hv_translate(panic_stack - 8, false, true, NULL) != 0 &&
        hv_translate(panic_stack - HV_WINDOWS_PANIC_STACK_RESERVE, false, true, NULL) != 0;
    bool interrupt_stack_ready =
        interrupt_stack >= HV_WINDOWS_PANIC_STACK_RESERVE &&
        hv_translate(interrupt_stack - 16, false, true, NULL) != 0 &&
        hv_translate(interrupt_stack - HV_WINDOWS_PANIC_STACK_RESERVE, false, true, NULL) != 0;
    bool stack_ready = panic_stack_ready && interrupt_stack_ready;
    if (!stack_ready) {
        /*
         * This runs from exception/interrupt paths.  Do not serialize the
         * guest behind UART while Windows is rebuilding a per-CPU mapping;
         * the old transition log could itself create an interrupt storm.
         * Record the first defer and every ready->lost edge in the bounded
         * binary trace instead.
         */
        if (PERCPU(carrier_stack_ready) || !PERCPU(carrier_stack_defer_logged)) {
            hv_native_aic_trace_record(HV_NATIVE_AIC_TRACE_STACK, panic_stack, interrupt_stack);
            PERCPU(carrier_stack_defer_logged) = true;
        }
        PERCPU(carrier_stack_ready) = false;
        return;
    }

    if (!PERCPU(carrier_stack_ready)) {
        hv_native_aic_trace_record(HV_NATIVE_AIC_TRACE_STACK, panic_stack, interrupt_stack);
        PERCPU(carrier_stack_ready) = true;
    }

#else
    (void)ctx;
#endif
}

enum hv_carrier_queue {
    HV_CARRIER_QUEUE_NONE,
    HV_CARRIER_QUEUE_SGI,
    HV_CARRIER_QUEUE_TIMER,
    HV_CARRIER_QUEUE_IRQ,
};

static enum hv_carrier_queue hv_carrier_select_pending(virq_t *selected)
{
#if defined(ENABLE_VGIC_MODULE) && defined(ENABLE_NATIVE_AIC_PASSTHROUGH)
    enum hv_carrier_queue best_queue = HV_CARRIER_QUEUE_NONE;
    virq_t candidate;

#define CONSIDER_CARRIER_QUEUE(queue, which)                                                       \
    do {                                                                                           \
        if (virq_queue_peek((queue), &candidate) &&                                                \
            (best_queue == HV_CARRIER_QUEUE_NONE || candidate.priority < selected->priority)) {    \
            *selected = candidate;                                                                 \
            best_queue = (which);                                                                  \
        }                                                                                          \
    } while (0)

    CONSIDER_CARRIER_QUEUE(&PERCPU(sgi_queue), HV_CARRIER_QUEUE_SGI);
    CONSIDER_CARRIER_QUEUE(&PERCPU(timer_queue), HV_CARRIER_QUEUE_TIMER);
    CONSIDER_CARRIER_QUEUE(&PERCPU(irq_queue), HV_CARRIER_QUEUE_IRQ);
#undef CONSIDER_CARRIER_QUEUE
    return best_queue;
#else
    (void)selected;
    return HV_CARRIER_QUEUE_NONE;
#endif
}

static u32 hv_carrier_do_iar1(void)
{
#if defined(ENABLE_VGIC_MODULE) && defined(ENABLE_NATIVE_AIC_PASSTHROUGH)
    if (PERCPU(carrier_irq_active))
        return 0x3ff;

    virq_t selected = {.priority = 0xff};
    enum hv_carrier_queue queue = hv_carrier_select_pending(&selected);
    bool popped = false;
    switch (queue) {
        case HV_CARRIER_QUEUE_SGI:
            popped = hv_sgi_queue_pop(&selected);
            break;
        case HV_CARRIER_QUEUE_TIMER:
            popped = virq_queue_pop(&PERCPU(timer_queue), &selected);
            break;
        case HV_CARRIER_QUEUE_IRQ:
            popped = virq_queue_pop(&PERCPU(irq_queue), &selected);
            break;
        default:
            break;
    }
    if (!popped)
        return 0x3ff;

    PERCPU(carrier_irq_active) = true;
    PERCPU(carrier_active_intid) = selected.vintid;
    return selected.vintid;
#else
    return 0x3ff;
#endif
}

static void hv_carrier_do_eoir1(u32 intid)
{
#if defined(ENABLE_VGIC_MODULE) && defined(ENABLE_NATIVE_AIC_PASSTHROUGH)
    if (PERCPU(carrier_irq_active) && PERCPU(carrier_active_intid) == intid) {
        PERCPU(carrier_irq_active) = false;
        PERCPU(carrier_active_intid) = 0x3ff;
        PERCPU(carrier_delivery_proven) = true;
    }
#else
    (void)intid;
#endif
}

static bool hv_guest_ipi_take_tag(void)
{
    if (!__atomic_exchange_n(&PERCPU(ipi_queued), false, __ATOMIC_ACQUIRE))
        return false;

    /* Do not overwrite an older INFLIGHT transaction. */
    PERCPU(ipi_pending) |= HV_GUEST_IPI_DELIVERABLE;
#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
    PERCPU(guest_ipi_tag_take_count)++;
#endif
    return true;
}

/*
 * A Windows GIC-carrier SGI can race the global AIC2 CONFIG write.  The old
 * post-CONFIG FIQ path popped such an SGI into an ICH LR after the local CPU
 * interface had been disabled.  That made every hypervisor pending field
 * read zero while NT's per-source IPI node remained queued forever.
 *
 * Once native AIC is ready, an architectural SGI no longer needs its GIC
 * INTID: Windows' own per-processor IPI queue is authoritative.  Preserve one
 * level wake for any queued SGI and let the native EVENT hook return the
 * Apple Fast-IPI token.  Coalescing is correct because one KiIpiInterrupt
 * drains the target's NT queue.
 */
static void hv_carrier_migrate_sgis_to_native(void)
{
#if defined(ENABLE_VGIC_MODULE) && defined(ENABLE_NATIVE_AIC_PASSTHROUGH)
    if (!hv_native_aic_windows_ready())
        return;

    virq_t pending;
    u32 migrated = 0;
    while (hv_sgi_queue_pop(&pending))
        migrated++;

    if (migrated != 0) {
        PERCPU(ipi_pending) |= HV_GUEST_IPI_DELIVERABLE | HV_GUEST_IPI_QUEUED_CARRIER;
        hv_native_aic_trace_record(HV_NATIVE_AIC_TRACE_CARRIER_MIGRATE, migrated,
                                   PERCPU(ipi_pending));
    }
#endif
}

static bool hv_carrier_irq_pending(void)
{
#if defined(ENABLE_VGIC_MODULE) && defined(ENABLE_NATIVE_AIC_PASSTHROUGH)
    if (!hv_native_aic_windows_active() || hv_native_aic_windows_ready() ||
        (!hv_vgic3_get_igrpen1() && !hv_native_aic_carrier_local_ready()) ||
        (!PERCPU(carrier_stack_ready) && !PERCPU(carrier_delivery_proven)))
        return false;

    /*
     * J414s' Blizzard cores assert a virtual IRQ for a pending ICH LR, but the
     * first Avalanche core can leave the same LR pending indefinitely. Keep
     * the short-lived carrier's pending/active state in software and drive the
     * IRQ line with HCR.VI while a deliverable Group-1 entry is queued.
     *
     * HCR.VI bypasses the virtual CPU interface's priority filter, so mirror
     * the relevant VMCR checks here. A second interrupt is not deliverable
     * until EOIR clears the software active state.
     */
    if (PERCPU(carrier_irq_active))
        return false;

    u8 pmr = (mrs(ICH_VMCR_EL2) >> 24) & 0xff;
    virq_t selected = {.priority = 0xff};
    if (hv_carrier_select_pending(&selected) != HV_CARRIER_QUEUE_NONE) {
        /*
         * After AIC3 LocalUnit is ready, Windows deliberately parks the GIC
         * PMR at zero but continues acknowledging the compatibility clock via
         * ICC_IAR1_EL1 until IoUnit installs the native EVENT callback. HCR.VI
         * bypasses VMCR priority in hardware; allow that bypass only inside
         * the measured step-35 transition window.
         */
        if (hv_native_aic_carrier_local_ready() || selected.priority < pmr)
            return true;
    }
#endif
    return false;
}

/*
 * GTDT NonSecurePL1TimerGSIV / VirtualTimerGSIV as published by
 * T8142FamilyPkg/AcpiTables/GTDT.aslc (PcdArmArchTimerIntrNum /
 * PcdArmArchTimerVirtIntrNum).  The numbers are arbitrary -- Apple has no GIC
 * and wires the timers to FIQ -- but they are what the guest connected its
 * clock ISR to, so they are what has to be acknowledged back to it.
 */
#define HV_VGIC_TIMER_P_INTID 17
#define HV_VGIC_TIMER_V_INTID 18

/*
 * T8142's emulated GICv3 CPU interface.  Defined with the rest of that model
 * further down; declared here because hv_update_fiq() is the timer's producer.
 */
static bool hv_gic_cpuif_active(void);
static void hv_gic_cpuif_set_pending(u32 intid);
static void hv_gic_cpuif_timer_pend(bool physical);
static void hv_gic_cpuif_send_sgi(u64 val);
static void hv_gic_cpuif_sync_vi(void);

static void hv_update_fiq(struct exc_info *ctx)
{
    u64 hcr = mrs(HCR_EL2);
    bool fiq_pending = false;

    hv_windows_update_carrier_readiness(ctx);

#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
    if (hv_native_aic_windows_active() && hv_native_aic_windows_ready()) {
        hv_carrier_migrate_sgis_to_native();
        /* CONFIG is global, but HCR is per-CPU. Retire the startup carrier on
         * every AP at its first post-handoff EL2 entry.  An AP already between
         * carrier IAR and EOIR must retain TALL1 until the EOIR trap clears its
         * software-active state. */
        if (!PERCPU(carrier_irq_active) && ((hcr & HCR_IMO) || mrs(ICH_HCR_EL2) != 0)) {
            hv_native_aic_enter_cpu();
            hcr = mrs(HCR_EL2);
        }
        /*
         * Windows owns AIC directly, but Apple wires the architectural timers
         * to FIQ.  Coalesce an asserted timer, suppress its physical FIQ, and
         * assert HCR.VI.  The HAL then takes an ordinary IRQ and reads the AIC
         * EVENT hook, which returns the native Apple source value 2 or 3.
         */
        if (hv_timer_firing(mrs(CNTP_CTL_EL02))) {
            fiq_pending = true;
            vm_tmr_fiq_clr(VM_TMR_FIQ_ENA_ENA_P);
            if (!PERCPU(timer_p_reflection_pending)) {
                hv_timer_diag_note_fiq(true);
                PERCPU(timer_p_fiq_count)++;
                PERCPU(timer_p_reflection_pending) = true;
                PERCPU(timer_p_event_unread) = true;
                HV_NATIVE_AIC_HOT_TRACE(HV_NATIVE_AIC_TRACE_TIMER_FIQ, 0,
                                        PERCPU(timer_p_fiq_count));
            }
        } else if (!PERCPU(timer_p_reflection_pending)) {
            vm_tmr_fiq_set(VM_TMR_FIQ_ENA_ENA_P);
            PERCPU(timer_p_event_unread) = false;
        } else {
            /* Keep the reflected source masked until EVENT or rearm. */
            vm_tmr_fiq_clr(VM_TMR_FIQ_ENA_ENA_P);
        }

        if (hv_timer_firing(mrs(CNTV_CTL_EL02))) {
            fiq_pending = true;
            vm_tmr_fiq_clr(VM_TMR_FIQ_ENA_ENA_V);
            if (!PERCPU(timer_v_reflection_pending)) {
                hv_timer_diag_note_fiq(false);
                PERCPU(timer_v_fiq_count)++;
                PERCPU(timer_v_reflection_pending) = true;
                PERCPU(timer_v_event_unread) = true;
                HV_NATIVE_AIC_HOT_TRACE(HV_NATIVE_AIC_TRACE_TIMER_FIQ, 1,
                                        PERCPU(timer_v_fiq_count));
            }
        } else if (!PERCPU(timer_v_reflection_pending)) {
            vm_tmr_fiq_set(VM_TMR_FIQ_ENA_ENA_V);
            PERCPU(timer_v_event_unread) = false;
        } else {
            /* Keep the reflected source masked until EVENT or rearm. */
            vm_tmr_fiq_clr(VM_TMR_FIQ_ENA_ENA_V);
        }
    } else if (hv_native_aic_windows_active() && !hv_gic_cpuif_active()) {
        /*
         * Windows calibrates its architectural clock before InitializeIoUnit.
         * Prefer the already-live emulated GIC CPU interface below when Group
         * 1 is enabled: it owns the guest's PMR/IAR/EOI state and is the path
         * that carried the clock before the AIC3 HAL extension became active.
         * This fallback is only for a core that never brought that interface
         * up. Step 35 is the measured, narrow override for that interval;
         * stack readiness, PMR, and the single-active-token rule are still
         * enforced below.
         */
        PERCPU(timer_p_event_unread) = false;
        PERCPU(timer_v_event_unread) = false;

        /*
         * J813's exception hardening uses x18 as scratch at every trapped
         * timer write, so that write cannot establish carrier readiness. Once
         * LocalUnit has completed and the independent KPCR/exception-stack
         * validator succeeds, the BSP has the stronger proof we actually
         * need. Keep it latched until the IoUnit handoff.
         */
        if (hv_native_aic_carrier_local_ready() && smp_id() == boot_cpu_idx &&
            PERCPU(carrier_stack_ready))
            PERCPU(carrier_timer_ready) = true;

        if (!PERCPU(carrier_timer_ready) ||
            (!hv_vgic3_get_igrpen1() && !hv_native_aic_carrier_local_ready()) || ctx == NULL ||
            ctx->regs[18] == 0) {
            vm_tmr_fiq_clr(VM_TMR_FIQ_ENA_ENA_P | VM_TMR_FIQ_ENA_ENA_V);
            PERCPU(timer_p_reflection_pending) = false;
            PERCPU(timer_v_reflection_pending) = false;
        } else {
            if (hv_timer_firing(mrs(CNTP_CTL_EL02))) {
                fiq_pending = true;
                vm_tmr_fiq_clr(VM_TMR_FIQ_ENA_ENA_P);
                if (!PERCPU(timer_p_reflection_pending) && !PERCPU(carrier_irq_active)) {
                    PERCPU(timer_p_fiq_count)++;
                    PERCPU(timer_p_reflection_pending) = true;
                    virq_t pending = {
                        .vintid = HV_GIC_TIMER_P_INTID,
                        .priority = hv_vgic3_get_priority(HV_GIC_TIMER_P_INTID),
                        .pending = true,
                    };
                    virq_queue_push(&PERCPU(timer_queue), &pending);
                }
            } else {
                PERCPU(timer_p_reflection_pending) = false;
                vm_tmr_fiq_set(VM_TMR_FIQ_ENA_ENA_P);
            }

            if (hv_timer_firing(mrs(CNTV_CTL_EL02))) {
                fiq_pending = true;
                vm_tmr_fiq_clr(VM_TMR_FIQ_ENA_ENA_V);
                if (!PERCPU(timer_v_reflection_pending) && !PERCPU(carrier_irq_active)) {
                    PERCPU(timer_v_fiq_count)++;
                    PERCPU(timer_v_reflection_pending) = true;
                    virq_t pending = {
                        .vintid = HV_GIC_TIMER_V_INTID,
                        .priority = hv_vgic3_get_priority(HV_GIC_TIMER_V_INTID),
                        .pending = true,
                    };
                    virq_queue_push(&PERCPU(timer_queue), &pending);
                }
            } else {
                PERCPU(timer_v_reflection_pending) = false;
                vm_tmr_fiq_set(VM_TMR_FIQ_ENA_ENA_V);
            }
        }
    } else if (hv_gic_cpuif_active()) {
        /*
         * Unmodified Windows on T8142.
         *
         * The MADT and GTDT this platform publishes describe a GICv3, so NT
         * arms the architectural timer and then waits for GSIV 17 or 18 to
         * arrive through the interrupt controller.  Apple wires the timers to
         * a per-core FIQ instead, and this SoC never reaches the AIC handoff
         * that drives the native bridge: the CONFIG/EVENT hooks are
         * deliberately left unmapped during Mu/NVMe bring-up, so
         * windows_aic_phase never flips and hv_native_aic_mu_timer_active()
         * stays true for the whole of the guest's life.
         *
         * The consequence was measured: every expiry kept being reflected into
         * Mu's reserved real-AIC software IRQ, which nothing reads once Mu has
         * exited.  The HAL spins three seconds in clockint.c waiting for a
         * clock that cannot arrive, then bugchecks 0x5C with
         * STATUS_UNSUCCESSFUL.  This branch must therefore be tested before
         * the Mu one, not after it.
         *
         * Mask the Apple FIQ source and mark the matching GTDT PPI pending;
         * hv_gic_cpuif_sync_vi() raises HCR.VI and the guest acknowledges
         * through ICC_IAR1_EL1 exactly as it would on real GICv3 hardware.
         */
        if (hv_timer_firing(mrs(CNTP_CTL_EL02))) {
            vm_tmr_fiq_clr(VM_TMR_FIQ_ENA_ENA_P);
            hv_gic_cpuif_timer_pend(true);
        } else {
            vm_tmr_fiq_set(VM_TMR_FIQ_ENA_ENA_P);
        }

        if (hv_timer_firing(mrs(CNTV_CTL_EL02))) {
            vm_tmr_fiq_clr(VM_TMR_FIQ_ENA_ENA_V);
            hv_gic_cpuif_timer_pend(false);
        } else {
            vm_tmr_fiq_set(VM_TMR_FIQ_ENA_ENA_V);
        }
    } else if (hv_native_aic_mu_timer_active()) {
        /*
         * Mu's native FIQ exception return corrupts its exception frame on
         * J414s.  Keep FIQ at EL2 and post the timer through Mu's reserved
         * real-AIC software IRQ ABI instead.  Ordinary device IRQs remain
         * physical AIC pass-through and no GIC state is exposed.
         */
        if (hv_timer_firing(mrs(CNTP_CTL_EL02))) {
            fiq_pending = true;
            vm_tmr_fiq_clr(VM_TMR_FIQ_ENA_ENA_P);
            if (!PERCPU(timer_p_reflection_pending)) {
                PERCPU(timer_p_fiq_count)++;
                PERCPU(timer_p_reflection_pending) = true;
                aic_set_sw(HV_TIMER_P_SWIRQ(smp_id()), true);
                HV_NATIVE_AIC_HOT_TRACE(HV_NATIVE_AIC_TRACE_TIMER_FIQ, 0,
                                        PERCPU(timer_p_fiq_count));
            }
        } else if (!PERCPU(timer_p_reflection_pending)) {
            vm_tmr_fiq_set(VM_TMR_FIQ_ENA_ENA_P);
        }

        if (hv_timer_firing(mrs(CNTV_CTL_EL02))) {
            fiq_pending = true;
            vm_tmr_fiq_clr(VM_TMR_FIQ_ENA_ENA_V);
            if (!PERCPU(timer_v_reflection_pending)) {
                PERCPU(timer_v_fiq_count)++;
                PERCPU(timer_v_reflection_pending) = true;
                aic_set_sw(HV_TIMER_V_SWIRQ(smp_id()), true);
                HV_NATIVE_AIC_HOT_TRACE(HV_NATIVE_AIC_TRACE_TIMER_FIQ, 1,
                                        PERCPU(timer_v_fiq_count));
            }
        } else if (!PERCPU(timer_v_reflection_pending)) {
            vm_tmr_fiq_set(VM_TMR_FIQ_ENA_ENA_V);
        }
    }
#else
    if (hv_timer_firing(mrs(CNTP_CTL_EL02))) {
        fiq_pending = true;
        vm_tmr_fiq_clr(VM_TMR_FIQ_ENA_ENA_P);

        // TODO: proper injection
#ifdef ENABLE_VGIC_MODULE
        if (hv_vgic3_get_free_lr() != -1) {
            hv_vgic3_inject_irq(17,                        // vintid
                                hv_vgic3_get_priority(17), // priority
                                false,                     // active
                                true,                      // pending
                                false,                     // hw_status
                                0                          // hw_irq
            );
        } else {
            virq_t pending = {
                .vintid = 17,
                .priority = 0x20,
                .active = false,
                .pending = true,
                .hw_status = false,
                .hw_irq = 0,
            };
            virq_queue_push(&PERCPU(timer_queue), &pending);
        }
#endif
    } else {
        vm_tmr_fiq_set(VM_TMR_FIQ_ENA_ENA_P);
    }

    if (hv_timer_firing(mrs(CNTV_CTL_EL02))) {
        fiq_pending = true;
        vm_tmr_fiq_clr(VM_TMR_FIQ_ENA_ENA_V);

        // TODO: proper injection
#ifdef ENABLE_VGIC_MODULE
        if (hv_vgic3_get_free_lr() != -1) {
            hv_vgic3_inject_irq(18,                        // vintid
                                hv_vgic3_get_priority(18), // priority
                                false,                     // active
                                true,                      // pending
                                false,                     // hw_status
                                0                          // hw_irq
            );
        } else {
            virq_t pending = {
                .vintid = 18,
                .priority = 0x20,
                .active = false,
                .pending = true,
                .hw_status = false,
                .hw_irq = 0,
            };
            virq_queue_push(&PERCPU(timer_queue), &pending);
        }
#endif
    } else {
        vm_tmr_fiq_set(VM_TMR_FIQ_ENA_ENA_V);
    }
#endif

    fiq_pending |= (PERCPU(ipi_pending) & HV_GUEST_IPI_OUTSTANDING) || PERCPU(pmc_pending);
#if defined(ENABLE_NATIVE_AIC_PASSTHROUGH) && defined(ENABLE_VGIC_MODULE)
    (void)fiq_pending;
#endif

    sysop("isb");
#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
    bool carrier_pending = hv_carrier_irq_pending();
    if (carrier_pending && !PERCPU(carrier_vi_logged)) {
        hv_native_aic_trace_record(HV_NATIVE_AIC_TRACE_CARRIER_VI, ctx ? ctx->elr : 0,
                                   ctx ? ctx->spsr : 0);
        PERCPU(carrier_vi_logged) = true;
    }
    hcr = mrs(HCR_EL2);
    if (hv_native_aic_windows_ready()) {
        /*
         * One policy owner decides the complete Windows-ready IRQ route.
         * In particular, it retains passthrough while a real AIC source is
         * awaiting EVENT acceptance even when an IPI or timer is also pending.
         */
        hv_native_aic_doorbell_sync();
    } else if (carrier_pending) {
        if (!(hcr & HCR_VI))
            hv_native_aic_apply_hcr_route(HV_NATIVE_AIC_HCR_STARTUP_PENDING);
    } else if (hv_gic_cpuif_active()) {
        /*
         * The emulated CPU interface is the sole owner of HCR.VI while it is
         * live.  Without this the unconditional clear below would strip the
         * line on the very next EL2 exit -- the guest would be interrupted
         * only if it happened to be at EL1 with IRQs unmasked at that instant,
         * which is exactly the race that makes a lost tick look like a hang.
         */
        hv_gic_cpuif_sync_vi();
    } else if (hcr & HCR_VI) {
        hv_native_aic_apply_hcr_route(HV_NATIVE_AIC_HCR_CLEAR_VI);
    }
#elif !defined(ENABLE_VGIC_MODULE)
    if ((hcr & HCR_VF) && !fiq_pending) {
        hv_write_hcr(hcr & ~HCR_VF);
    } else if (!(hcr & HCR_VF) && fiq_pending) {
        hv_write_hcr(hcr | HCR_VF);
    }
#endif
}

#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
bool hv_native_aic_event_replay(u64 *event)
{
    u32 cpu = smp_id();

    if (!hv_native_aic_windows_ready() || event == NULL || !native_irq_rearm_replay_valid[cpu])
        return false;

    /*
     * This token was already consumed from the physical EVENT aperture and is
     * therefore older than any still-asserted hardware source. Pop it before
     * the caller performs hv_pa_rw(), preserving exact controller ordering.
     */
    *event = native_irq_rearm_replay_event[cpu];
    native_irq_rearm_replay_event[cpu] = 0;
    native_irq_rearm_replay_valid[cpu] = false;
    native_irq_rearm_replay_count[cpu]++;
    PERCPU(native_doorbell_posted) = false;
    HV_NATIVE_AIC_HOT_TRACE(HV_NATIVE_AIC_TRACE_EVENT_REAL, *event, 1);
    hv_native_aic_doorbell_sync();
    return true;
}

bool hv_native_aic_event_read(u64 raw_event, u64 *event)
{
    u32 type = FIELD_GET(AIC_EVENT_TYPE, raw_event);
    u32 die = FIELD_GET(AIC_EVENT_DIE, raw_event);
    u32 irq = FIELD_GET(AIC_EVENT_NUM, raw_event);
    u32 flat_irq = die * aic->max_irq + irq;
    u32 ipi_event_token;
    bool reserved = type == AIC_EVENT_TYPE_HW && flat_irq >= HV_TIMER_SWIRQ_BASE &&
                    flat_irq < HV_TIMER_SWIRQ_BASE + (2 * MAX_CPUS);

    if (!hv_native_aic_windows_ready() || event == NULL)
        return false;

    native_event_read_count[smp_id()]++;

    if (raw_event != 0 && !reserved) {
        HV_NATIVE_AIC_HOT_TRACE(HV_NATIVE_AIC_TRACE_EVENT_REAL, raw_event,
                                native_irq_rearm_deferred[smp_id()]);
        /*
         * A real native AIC source won arbitration ahead of the synthetic
         * wakeup.  Reading EVENT has now auto-masked that level source, so it
         * is finally safe to restore IMO for a queued synthetic wake. This is
         * the immediate runtime path that clears deferred handoff state.  A
         * A later quiescence observation releases a source that deasserted;
         * if ISR stays asserted, the bounded capture/replay fallback retains
         * this exact ownership before restoring a synthetic wake.
         */
        native_irq_rearm_deferred[smp_id()] = false;
        native_irq_rearm_start_time[smp_id()] = 0;
        hv_native_aic_doorbell_sync();
        return false;
    }

    /*
     * A reserved Mu timer reflector can race the ExitBootServices/Windows
     * CONFIG transition on another CPU and remain latched after the bulk
     * SW_CLEAR. These implementation-private IRQ numbers must never escape
     * to the Windows controller: HalBeginSystemInterrupt treats the unknown
     * line as a fatal controller result (0x5c/0x203). Consume the stale token
     * before returning a pending processor-local source, or a spurious EVENT.
     */
    if (reserved) {
        native_irq_rearm_deferred[smp_id()] = false;
        native_irq_rearm_start_time[smp_id()] = 0;
        aic_set_sw(flat_irq, false);
        aic_set_mask(flat_irq, true);
        hv_native_aic_trace_record(HV_NATIVE_AIC_TRACE_EVENT_RESERVED, flat_irq, raw_event);
    }

    PERCPU(native_doorbell_posted) = false;

    /*
     * Fast IPIs are FIQ-class and are consumed by m1n1 for its own EL2
     * coordination.  Guest-originated sends are tagged by ipi_queued; reflect
     * only those arrivals as the architectural AIC IPI EVENT expected by the
     * native Windows controller callback.  This is an AIC EVENT/IRQ delivery,
     * not a vGIC list-register injection.
     */
    if (hv_guest_ipi_begin_delivery(&ipi_event_token)) {
        hv_timer_diag_note_ipi_bypass();
        *event = FIELD_PREP(AIC_EVENT_DIE, ipi_event_token) |
                 FIELD_PREP(AIC_EVENT_TYPE, AIC_EVENT_TYPE_IPI) | AIC_EVENT_IPI_OTHER;
        /* Windows uses the virtual timer in the proven QEMU AIC path. */
    } else if (PERCPU(timer_v_event_unread)) {
        *event = 3;
        PERCPU(timer_v_event_unread) = false;
        hv_timer_diag_note_event(false);
    } else if (PERCPU(timer_p_event_unread)) {
        *event = 2;
        PERCPU(timer_p_event_unread) = false;
        hv_timer_diag_note_event(true);
    } else {
        *event = 0;
    }

    if (*event != 0)
        HV_NATIVE_AIC_HOT_TRACE(HV_NATIVE_AIC_TRACE_EVENT, raw_event, *event);

    hv_native_aic_doorbell_sync();
    return true;
}

/*
 * A guest timer write is the reliable completion signal for a reflected tick.
 * Merely polling ISTATUS in hv_update_fiq() is racy: immediately after an MSR
 * write the old asserted status can still be observed, and if EL1 then returns
 * without another trap the reflector remains suppressed forever.  Rearm the
 * physical source at the trapped write itself.  hv_update_fiq() runs on the
 * same exception exit and will suppress/post it again if the new deadline is
 * already expired.
 */
static void hv_timer_reflect_guest_rearm(bool physical, bool control_write, u64 value,
                                         bool guest_cpu_ready)
{
    sysop("isb");

    hv_timer_diag_note_rearm(physical);

    if (!hv_native_aic_windows_active()) {
        /*
         * TimerDxe first writes CTL to disable/mask the timer, then registers
         * all callbacks, and only at the end writes ENABLE=1 with IMASK clear.
         * That final control write is the proof that native FIQ can be released.
         */
        if (control_write && (value & HV_TIMER_CTL_ENABLE) && !(value & HV_TIMER_CTL_IMASK)) {
            hv_native_aic_timer_ready();
        } else if (hv_native_aic_mu_timer_active()) {
            if (physical) {
                if (PERCPU(timer_p_reflection_pending))
                    HV_NATIVE_AIC_HOT_TRACE(HV_NATIVE_AIC_TRACE_TIMER_REARM, 0, value);
                PERCPU(timer_p_reflection_pending) = false;
                aic_set_sw(HV_TIMER_P_SWIRQ(smp_id()), false);
                vm_tmr_fiq_set(VM_TMR_FIQ_ENA_ENA_P);
            } else {
                if (PERCPU(timer_v_reflection_pending))
                    HV_NATIVE_AIC_HOT_TRACE(HV_NATIVE_AIC_TRACE_TIMER_REARM, 1, value);
                PERCPU(timer_v_reflection_pending) = false;
                aic_set_sw(HV_TIMER_V_SWIRQ(smp_id()), false);
                vm_tmr_fiq_set(VM_TMR_FIQ_ENA_ENA_V);
            }
        }
        return;
    }

    if (!hv_native_aic_windows_ready()) {
        /*
         * Readiness is CPU-local.  A BSP timer write must never arm a newly
         * entered AP: Windows has not installed that AP's KPCR/x18 yet, and an
         * early carrier tick would crash in KfRaiseIrql at x18 + 0x38.
         */
        /*
         * A timer write alone is too early on a Windows AP.  The AP bootstrap
         * programs its timer before installing the KPCR in x18; delivering an
         * LR in that interval enters KfRaiseIrql with x18 == 0 and bugchecks at
         * [x18 + 0x38].  Only arm this CPU's carrier after a trapped write also
         * proves that Windows has established its per-CPU kernel context.
         */
        /*
         * Keep the compatibility clock on the BSP.  Windows AP startup can
         * transiently populate x18 while programming its timer and then clear
         * it again before the first local tick.  SGIs still bring every AP
         * online through the carrier; local timer delivery begins only after
         * the native AIC handoff.
         */
        /*
         * Readiness is a one-way proof until the native handoff. Windows may
         * use x18 as scratch on a later trapped timer write; clearing an
         * already-proven BSP here removes the only clock source before IoUnit.
         * hv_windows_update_carrier_readiness() still validates the live x18,
         * KPCR and exception stacks before each actual carrier delivery.
         */
        PERCPU(carrier_timer_ready) |= guest_cpu_ready && smp_id() == boot_cpu_idx;
        PERCPU(timer_p_event_unread) = false;
        PERCPU(timer_v_event_unread) = false;
        if (physical) {
            PERCPU(timer_p_reflection_pending) = false;
            vm_tmr_fiq_set(VM_TMR_FIQ_ENA_ENA_P);
        } else {
            PERCPU(timer_v_reflection_pending) = false;
            vm_tmr_fiq_set(VM_TMR_FIQ_ENA_ENA_V);
        }
        hv_native_aic_apply_hcr_route(HV_NATIVE_AIC_HCR_CLEAR_VI);
        return;
    }

    if (physical) {
        if (PERCPU(timer_p_reflection_pending))
            HV_NATIVE_AIC_HOT_TRACE(HV_NATIVE_AIC_TRACE_TIMER_REARM, 0, value);
        PERCPU(timer_p_reflection_pending) = false;
        PERCPU(timer_p_event_unread) = false;
        vm_tmr_fiq_set(VM_TMR_FIQ_ENA_ENA_P);
    } else {
        if (PERCPU(timer_v_reflection_pending))
            HV_NATIVE_AIC_HOT_TRACE(HV_NATIVE_AIC_TRACE_TIMER_REARM, 1, value);
        PERCPU(timer_v_reflection_pending) = false;
        PERCPU(timer_v_event_unread) = false;
        vm_tmr_fiq_set(VM_TMR_FIQ_ENA_ENA_V);
    }
    hv_native_aic_doorbell_sync();
}

/*
 * HCR.VI is subject to PSTATE.I but not to the interrupt-controller priority
 * threshold Windows maintains through the HAL SetPriority callback.  If the
 * HAL destructively reads a synthetic timer EVENT while its cached GTDT line
 * is priority-blocked, it returns spurious and asks us to repost the SAME live
 * reflection generation after IRQL drops.  A timer write remains the only
 * completion edge, so a stale request after rearm cannot invent a new tick.
 */
static u64 hv_timer_reflect_guest_repost(bool physical)
{
    unsigned int source = hv_timer_diag_source(physical);
    struct hv_native_aic_timer_diag *diag = &native_aic_timer_diag[smp_id()];
    bool pending =
        physical ? PERCPU(timer_p_reflection_pending) : PERCPU(timer_v_reflection_pending);
    bool unread = physical ? PERCPU(timer_p_event_unread) : PERCPU(timer_v_event_unread);

    if (!hv_native_aic_windows_ready() || !pending) {
        diag->repost_stale_count[source]++;
        return HV_TIMER_REFLECT_REPOST_STALE;
    }
    if (unread) {
        diag->repost_already_unread_count[source]++;
        return HV_TIMER_REFLECT_REPOST_ALREADY_UNREAD;
    }

    if (physical)
        PERCPU(timer_p_event_unread) = true;
    else
        PERCPU(timer_v_event_unread) = true;
    diag->repost_count[source]++;
    hv_native_aic_trace_record(HV_NATIVE_AIC_TRACE_TIMER_REPOST, source,
                               diag->repost_count[source]);
    hv_native_aic_doorbell_sync();
    return HV_TIMER_REFLECT_REPOSTED;
}
#endif

#define SYSREG_MAP_IF(sr, to, available)                                                           \
    case SYSREG_ISS(sr):                                                                           \
        if (!(available))                                                                          \
            return false;                                                                          \
        if (is_read)                                                                               \
            regs[rt] = _mrs(sr_tkn(to));                                                           \
        else                                                                                       \
            _msr(sr_tkn(to), regs[rt]);                                                            \
        return true;

#define SYSREG_MAP(sr, to)                                                                         \
    case SYSREG_ISS(sr):                                                                           \
        if (is_read)                                                                               \
            regs[rt] = _mrs(sr_tkn(to));                                                           \
        else                                                                                       \
            _msr(sr_tkn(to), regs[rt]);                                                            \
        return true;

#define SYSREG_PASS(sr)                                                                            \
    case SYSREG_ISS(sr):                                                                           \
        if (is_read)                                                                               \
            regs[rt] = _mrs(sr_tkn(sr));                                                           \
        else                                                                                       \
            _msr(sr_tkn(sr), regs[rt]);                                                            \
        return true;

#define SYSREG_APPLE_MAP(sr, to)                                                                   \
    case SYSREG_ISS(sr):                                                                           \
        if (!(cpu_features->apple_sysregs_unlocked))                                               \
            return false;                                                                          \
        if (is_read)                                                                               \
            regs[rt] = _mrs(sr_tkn(to));                                                           \
        else                                                                                       \
            _msr(sr_tkn(to), regs[rt]);                                                            \
        return true;

#define SYSREG_APPLE_PASS(sr)                                                                      \
    case SYSREG_ISS(sr):                                                                           \
        if (!(cpu_features->apple_sysregs_unlocked))                                               \
            return false;                                                                          \
        if (is_read)                                                                               \
            regs[rt] = _mrs(sr_tkn(sr));                                                           \
        else                                                                                       \
            _msr(sr_tkn(sr), regs[rt]);                                                            \
        return true;

/*
 * Identical to SYSREG_PASS, except that `clr` is cleared from the
 * value handed to the guest.  For ID registers whose hardware answer is
 * architecturally correct but advertises a feature the guest must not reach.
 */
#define SYSREG_PASS_CLR(sr, clr)                                                                   \
    case SYSREG_ISS(sr):                                                                           \
        if (is_read)                                                                               \
            regs[rt] = _mrs(sr_tkn(sr)) & ~((u64)(clr));                                           \
        else                                                                                       \
            _msr(sr_tkn(sr), regs[rt]);                                                            \
        return true;

/* ID_AA64DFR0_EL1.PMUVer, bits [11:8]. */
#define ID_AA64DFR0_PMUVER_MASK (0xfUL << 8)

/*
 * Some CPUs lock or remove the Apple implementation-defined PMC register bank.
 * Accessing it from EL2 can abort, so the Apple-to-architectural redirect below
 * is restricted to CPUs whose feature table exposes the bank.  Windows preboot only requires a
 * monotonic cycle source plus well-defined PMU control registers.  Present a
 * deliberately counter-free PMUv3 view and consume writes in software.  This
 * is equivalent to the instruction-level compatibility contract previously
 * proven with patched binaries, but keeps every Microsoft image untouched.
 */
static bool hv_handle_software_pmu(u64 reg, bool is_read, u64 rt, u64 regs[32])
{
    if (cpu_features->apple_sysregs_unlocked)
        return false;

    switch (reg) {
        case SYSREG_ISS(SYS_PMCCNTR_EL0):
            if (is_read)
                regs[rt] = hv_host_counter();
            break;

        case SYSREG_ISS(SYS_PMCR_EL0):
        case SYSREG_ISS(SYS_PMCCFILTR_EL0):
        case SYSREG_ISS(SYS_PMCEID0_EL0):
        case SYSREG_ISS(SYS_PMCEID1_EL0):
        case SYSREG_ISS(SYS_PMCNTENCLR_EL0):
        case SYSREG_ISS(SYS_PMCNTENSET_EL0):
        case SYSREG_ISS(SYS_PMEVCNTR0_EL0):
        case SYSREG_ISS(SYS_PMINTENCLR_EL1):
        case SYSREG_ISS(SYS_PMINTENSET_EL1):
        case SYSREG_ISS(SYS_PMMIR_EL1):
        case SYSREG_ISS(SYS_PMOVSCLR_EL0):
        case SYSREG_ISS(SYS_PMOVSSET_EL0):
        case SYSREG_ISS(SYS_PMSELR_EL0):
        case SYSREG_ISS(SYS_PMUSERENR_EL0):
            if (is_read)
                regs[rt] = 0;
            break;

        default:
            return false;
    }

    return true;
}

/*
 * The GIC CPU interface, which this SoC also does not have.
 *
 * Apple silicon implements no GICv3 at all, so the ICC_* system registers do
 * not decode and an access from EL1 is an undefined instruction -- the same
 * failure as the absent PMU bank, in a different register file.  It reaches us
 * the same way and is fixed by the same mechanism.
 *
 * This exists because m1n1 hands the guest an emulated GICv3 distributor and
 * redistributors (hv_vgic.c) so the kernel can classify its interrupt
 * controller.  Having accepted that classification, the kernel then programs
 * the CPU interface, and Windows takes an unhandled STATUS_ILLEGAL_INSTRUCTION
 * on the first write -- measured as bugcheck 0x1E at `msr ICC_IGRPEN1_EL1, x8`.
 *
 * WHAT IS AND IS NOT REAL HERE.  The configuration registers are genuinely
 * emulated: they hold the value the guest wrote and give it back.  The
 * acknowledge/complete registers are NOT an interrupt delivery path.  IAR1 and
 * HPPIR1 return the spurious INTID 1023, which is the truthful answer for an
 * interface that never has an interrupt pending, because interrupts arrive
 * through the native-AIC bridge instead (hv_exc.c, hv_update_fiq()) and never
 * through this file.  Do not read the presence of these cases as GIC delivery
 * working; they let initialization complete and nothing more.
 */
/*
 * Guest INTID space modelled by the software CPU interface: SGIs, PPIs and the
 * whole architectural SPI range, i.e. everything below the 1020..1023 special
 * block.  hv_aic_alias.c is what keeps physical AIC lines inside it.
 */
#define HV_GIC_MAX_INTID   1020
#define HV_GIC_INTID_WORDS ((HV_GIC_MAX_INTID + 31) / 32)

static bool hv_gic_bitmap_test(const u32 *map, u32 intid)
{
    return (__atomic_load_n(&map[intid / 32], __ATOMIC_ACQUIRE) & BIT(intid % 32)) != 0;
}

static void hv_gic_bitmap_set(u32 *map, u32 intid)
{
    __atomic_fetch_or(&map[intid / 32], (u32)BIT(intid % 32), __ATOMIC_ACQ_REL);
}

static void hv_gic_bitmap_clear(u32 *map, u32 intid)
{
    __atomic_fetch_and(&map[intid / 32], ~(u32)BIT(intid % 32), __ATOMIC_ACQ_REL);
}

/* Lowest INTID set in the map, or -1 when it is empty.  Diagnostics only. */
static int hv_gic_bitmap_first_set(const u32 *map)
{
    for (u32 word = 0; word < HV_GIC_INTID_WORDS; word++) {
        u32 bits = __atomic_load_n(&map[word], __ATOMIC_ACQUIRE);
        if (bits)
            return (int)((word * 32) + __builtin_ctz(bits));
    }

    return -1;
}

struct hv_gic_cpuif {
    u64 pmr;
    u64 bpr1;
    u64 igrpen0;
    u64 igrpen1;
    u64 ctlr;
    /*
     * Pending/active bitmaps, indexed by guest INTID.
     *
     * This was a single word covering the banked 0..31 range for as long as the
     * only producer was the architectural timer, whose GTDT GSIVs are both PPIs.
     * J813's built-in keyboard and trackpad are the first device that has to be
     * delivered through this interface, and their published GSIV is an SPI, so
     * an out-of-range INTID can no longer simply be refused.
     *
     * There is still no distributor model here, and none is needed: on this
     * machine the AIC *is* the distributor.  It owns masking and it decides
     * which CPU a line targets, handing the event to that CPU's EVENT register.
     * hv_exc_irq() therefore marks the INTID pending on exactly the interface
     * the hardware already selected.  What these arrays add is the ability to
     * represent that selection at all.
     */
    u32 pending[HV_GIC_INTID_WORDS];
    u32 active[HV_GIC_INTID_WORDS];
    /*
     * Firmware residue.  Mu's TimerDxe drives the virtual timer and leaves its
     * last deadline expired when it exits, so an already-asserted CNTx sits
     * there waiting the instant delivery becomes possible.  Handing that to the
     * guest as its first interrupt is worse than dropping it: the OS has
     * enabled its CPU interface but not yet connected the clock ISR, so nothing
     * acknowledges the INTID, it stays active forever, and every genuine tick
     * after it is blocked by that active bit.  That is exactly what happened --
     * one INTID 18 delivered the moment ICC_IGRPEN1_EL1 went to 1, then silence
     * and the same three-second HAL timeout.
     *
     * So the expired deadline inherited at that moment is recorded and refused
     * until the guest programs a different one.  Nothing is written to guest
     * state to achieve this; it is purely an observation about whose deadline
     * is being reported.
     */
    u64 stale_cval_p;
    u64 stale_cval_v;
    /*
     * Running priority: the priority of the interrupt currently being serviced,
     * or 0xff when idle.  Kept alongside pmr because both gate delivery.
     */
    u64 running;
    bool stale_p;
    bool stale_v;
    bool valid;
};

static struct hv_gic_cpuif gic_cpuif[MAX_CPUS];
static bool gic_cpuif_acked[MAX_CPUS];
static bool gic_cpuif_blocked_logged[MAX_CPUS];
static bool gic_cpuif_eoi_logged[MAX_CPUS];
static bool gic_cpuif_brought_up[MAX_CPUS];
static bool gic_cpuif_vi_logged[MAX_CPUS];
static bool gic_cpuif_aic_logged[MAX_CPUS];
static u32 gic_cpuif_ack_count[MAX_CPUS];
static u32 gic_cpuif_eoi_count[MAX_CPUS];

#define GIC_SPURIOUS_INTID 1023

static struct hv_gic_cpuif *hv_gic_cpuif_this(void)
{
    if (chip_id != T8142)
        return NULL;

    u64 cpu = smp_id();
    if (cpu >= MAX_CPUS)
        return NULL;

    return &gic_cpuif[cpu];
}

/*
 * True once the guest has enabled Group 1 on this core's CPU interface.  That
 * write is the unambiguous "an OS is driving the emulated GICv3" signal: Mu is
 * an AIC build and never touches ICC_IGRPEN1_EL1, and it is the instruction NT
 * used to die on before the interface existed at all.
 */
static bool hv_gic_cpuif_active(void)
{
    struct hv_gic_cpuif *s = hv_gic_cpuif_this();

    return s != NULL && s->valid && s->igrpen1 != 0;
}

/*
 * Drive the virtual IRQ line from the software pending/active state.
 *
 * HCR.VI is only signalled while HCR.IMO is set, so enabling delivery also
 * takes ownership of physical IRQ routing on this core.  That is safe in the
 * window this path serves: Mu cleared AIC2 CONFIG on its way out and the guest
 * has not enabled it, so no physical IRQ is asserted.  IMO is left set once
 * raised -- flapping it underneath a guest that is mid-handler would lose the
 * very interrupt we are trying to deliver.
 *
 * Priority is deliberately not modelled.  HCR.VI bypasses the CPU interface's
 * priority filter, and with a single producer there is nothing to arbitrate;
 * PMR and BPR1 are stored so reads are honest, not because they gate anything.
 */
static u8 hv_gic_cpuif_priority(u32 intid)
{
#ifdef ENABLE_VGIC_MODULE
    /*
     * The guest's own GICR_IPRIORITYR state, as recorded by the vGIC carrier's
     * redistributor hooks.  Using anything else would invent a priority the OS
     * never chose and then filter against it.
     */
    return hv_vgic3_get_priority(intid);
#else
    (void)intid;
    return 0;
#endif
}

/*
 * The highest-priority interrupt this CPU interface may signal right now, or
 * -1 if none.
 *
 * GICv3 signals an interrupt only when its priority is strictly higher --
 * numerically lower -- than both ICC_PMR_EL1 and the running priority.  That
 * filter is not a refinement here, it is load-bearing: Windows implements IRQL
 * with ICC_PMR_EL1 rather than PSTATE.I, so an emulated interface that ignores
 * the mask delivers interrupts inside regions the OS believes cannot be
 * interrupted.  Measured: with the mask ignored, the first tick arrived during
 * HAL initialization and NT bugchecked 0x2B, PANIC_STACK_SWITCH, immediately --
 * no sampler reading fell between acknowledgement and the bugcheck.
 *
 * PMR resets to 0, which masks everything.  That is also why nothing can be
 * delivered before the guest has configured its own interface.
 */
static int hv_gic_cpuif_select(struct hv_gic_cpuif *s)
{
    if (!s->igrpen1)
        return -1;

    int best = -1;
    u32 best_prio = 0x100;

    for (u32 word = 0; word < HV_GIC_INTID_WORDS; word++) {
        u32 candidates = __atomic_load_n(&s->pending[word], __ATOMIC_ACQUIRE) &
                         ~__atomic_load_n(&s->active[word], __ATOMIC_ACQUIRE);

        while (candidates) {
            u32 index = __builtin_ctz(candidates);
            candidates &= ~((u32)BIT(index));

            u32 intid = (word * 32) + index;
            u32 prio = hv_gic_cpuif_priority(intid);
            if (prio >= s->pmr || prio >= s->running)
                continue;

            if (best < 0 || prio < best_prio) {
                best = (int)intid;
                best_prio = prio;
            }
        }
    }

    return best;
}

static void hv_gic_cpuif_sync_vi(void)
{
    struct hv_gic_cpuif *s = hv_gic_cpuif_this();
    if (s == NULL || !s->valid)
        return;

    u64 hcr = mrs(HCR_EL2);
    int selected = hv_gic_cpuif_select(s);

    /*
     * Assert IMO for as long as this interface is live, not merely while
     * something is already pending.
     *
     * Both directions of delivery need it: HCR.VI can only signal a virtual
     * interrupt to EL1 while IMO is set, and a physical AIC IRQ can only reach
     * hv_exc_irq() while IMO is set.  Tying it to "something is pending" works
     * for the timer, whose FIQ arrives by another route and creates the pending
     * state first, but it is a chicken-and-egg for a device: the physical IRQ
     * that would make it pending is exactly what IMO gates.
     *
     * This runs from hv_update_fiq() on every EL2 exit, so it is also what
     * makes the route self-healing -- measured on hardware, a core that took
     * IMO at its ICC_IGRPEN1_EL1 write was later observed back at the pristine
     * base HCR with IMO clear, and a one-shot assert had no way to recover.
     */
    bool live = s->igrpen1 != 0;
    u64 want = live ? (hcr | HCR_IMO) : hcr;
    want = (selected >= 0) ? (want | HCR_VI) : (want & ~HCR_VI);

    if (want == hcr)
        return;

    /*
     * One line the first time this core actually asserts the virtual IRQ line,
     * with the guest context it is about to interrupt.  Which instruction NT is
     * on, and whether it had interrupts unmasked there, is the difference
     * between "the tick was never delivered" and "the tick was delivered
     * somewhere NT could not survive it" -- and those two have opposite fixes.
     */
    if ((want & HCR_VI) && !gic_cpuif_vi_logged[smp_id()]) {
        gic_cpuif_vi_logged[smp_id()] = true;
        printf("HV: T8142: raising virtual IRQ %d on CPU %d at guest pc 0x%lx "
               "spsr 0x%lx pmr 0x%lx prio 0x%x\n",
               selected, (int)smp_id(), mrs(ELR_EL2), mrs(SPSR_EL2), s->pmr,
               hv_gic_cpuif_priority((u32)selected));
    }

    hv_write_hcr(want);
}

static void hv_gic_cpuif_set_pending(u32 intid)
{
    struct hv_gic_cpuif *s = hv_gic_cpuif_this();
    if (s == NULL || !s->valid || intid >= HV_GIC_MAX_INTID)
        return;

    /*
     * One line the first time a tick is held off by an unacknowledged
     * predecessor.  If this ever prints, the guest took an interrupt it never
     * completed and the clock has stopped for good -- worth saying out loud
     * once rather than leaving it to be inferred from a silent timeout.
     */
    if (hv_gic_bitmap_test(s->active, intid) && !gic_cpuif_blocked_logged[smp_id()]) {
        gic_cpuif_blocked_logged[smp_id()] = true;
        printf("HV: T8142: INTID %u still active on CPU %d; further ticks are "
               "blocked until the guest completes it\n",
               intid, (int)smp_id());
    }

    hv_gic_bitmap_set(s->pending, intid);
    hv_gic_cpuif_sync_vi();
}

/*
 * ICC_SGI1R_EL1.  There is no distributor to forward this to, so it is decoded
 * and delivered here: each target gets the INTID marked pending on its own
 * software CPU interface, then a physical kick so it enters EL2 and re-evaluates
 * HCR.VI for itself.  Without the kick a core already running in the guest would
 * not notice the new pending state until its next unrelated trap, which for an
 * idle core is never.
 */
static void hv_gic_cpuif_send_sgi(u64 val)
{
    u32 intid = (val >> 24) & 0xf;
    bool irm = (val >> 40) & 1;
    u64 aff1 = (val >> 16) & 0xff;
    u64 aff2 = (val >> 32) & 0xff;
    u64 aff3 = (val >> 48) & 0xff;
    u64 rs = (val >> 44) & 0xf;
    u16 targets = val & 0xffff;
    int self = smp_id();
    /* Use the same frame-to-physical-CPU map as the architectural SGI path.
     * A secondary count excludes the BSP and cannot bound sparse CPU IDs. */
    for (u16 frame = 0; frame < hv_vgic3_num_cpus(); frame++) {
        int cpu = hv_vgic3_cpu_for_frame(frame);
        if (cpu < 0)
            continue;
        if (irm) {
            /* Routing mode 1 is "everyone but me". */
            if (cpu == self)
                continue;
        } else {
            u64 mpidr = smp_get_mpidr(cpu);
            u64 aff0 = mpidr & 0xff;

            if (((mpidr >> 8) & 0xff) != aff1 || ((mpidr >> 16) & 0xff) != aff2 ||
                ((mpidr >> 32) & 0xff) != aff3)
                continue;
            if ((aff0 >> 4) != rs || !(targets & (1u << (aff0 & 0xf))))
                continue;
        }

        /*
         * smp_is_alive() means "came up through the spin table", which the boot
         * CPU never does -- its flag is always clear.  Testing it alone
         * therefore dropped every IPI aimed at the boot processor.
         *
         * That is not a lost tick, it is a hang: measured with Windows running
         * nine cores, where seven sat inside an IPI handler with active=0x1
         * forever while the initiator spun waiting for a rendezvous the boot
         * CPU was never told to join.  hv_vgic.c already words this test as
         * "cpu == boot_cpu_idx || smp_is_alive(cpu)" for the same reason.
         */
        if (cpu != self && cpu != boot_cpu_idx && !smp_is_alive(cpu))
            continue;

        struct hv_gic_cpuif *t = &gic_cpuif[cpu];
        if (!t->valid)
            continue;

        hv_gic_bitmap_set(t->pending, intid);

        if (cpu == self)
            hv_gic_cpuif_sync_vi();
        else
            smp_send_ipi(cpu);
    }
}

/*
 * Pend an architectural timer, unless the deadline being reported is the one
 * inherited from firmware rather than one the guest asked for.  See the
 * stale_cval_* fields for why that distinction is load-bearing.
 */
static void hv_gic_cpuif_timer_pend(bool physical)
{
    struct hv_gic_cpuif *s = hv_gic_cpuif_this();
    if (s == NULL || !s->valid)
        return;

    bool *stale = physical ? &s->stale_p : &s->stale_v;

    if (*stale) {
        u64 armed = physical ? mrs(CNTP_CVAL_EL02) : mrs(CNTV_CVAL_EL02);
        u64 residue = physical ? s->stale_cval_p : s->stale_cval_v;

        if (armed == residue)
            return;

        *stale = false;
    }

    hv_gic_cpuif_set_pending(physical ? HV_VGIC_TIMER_P_INTID : HV_VGIC_TIMER_V_INTID);
}

/*
 * Route the short native-AIC startup compatibility window through one CPU-
 * interface owner, regardless of how the guest access reached EL2.
 *
 * On T8142 the architectural ICC_* register file is absent at EL1.  The image
 * scanner therefore replaces those accesses with indexed HVC sites before the
 * guest executes them.  Those HVCs normally feed the standalone emulated-GIC
 * CPU interface below.  During the native-AIC startup window, however, HCR.VI
 * is driven from the carrier queues and their IAR/EOI state must be consumed by
 * that same owner.  Letting a rewritten ICC_IAR1_EL1 read fall through to the
 * standalone model returns 0x3ff even while the carrier has a pending timer;
 * the guest takes the IRQ, sees it as spurious, and never reaches IoUnit.
 *
 * The ordinary trapped-MSR path uses this helper too.  That keeps direct
 * TALL1 traps and pre-rewritten HVC sites semantically identical instead of
 * leaving two subtly different copies of the carrier protocol.
 */
static bool hv_handle_native_aic_carrier_cpuif(u64 reg, bool is_read, u64 rt, u64 regs[32],
                                               bool probe)
{
#if defined(ENABLE_VGIC_MODULE) && defined(ENABLE_NATIVE_AIC_PASSTHROUGH)
    if (probe || !hv_native_aic_windows_active() || hv_native_aic_windows_ready())
        return false;

    /*
     * Do not split one architectural CPU interface between two state
     * machines. Once the standalone T8142 model has seen Group 1 enabled, it
     * already owns PMR, IAR, EOI, and HCR.VI. The J813 step-35 trace proved
     * that diverting only IAR/EOI to this carrier delivered INTID 18 once but
     * never observed its completion, ending in HAL_INITIALIZATION_FAILED.
     *
     * Always let the Group-1 access itself reach the standalone model so the
     * first enable establishes that ownership. Keep the software carrier only
     * as a fallback for a core that never brought up the compatibility GIC.
     */
    u64 cpu = smp_id();
    if (reg == SYSREG_ISS(ICC_IGRPEN1_EL1) || (cpu < MAX_CPUS && gic_cpuif_brought_up[cpu]))
        return false;

    switch (reg) {
        case SYSREG_ISS(ICC_PMR_EL1):
            if (is_read)
                regs[rt] = PERCPU(vgic_pmr);
            else
                PERCPU(vgic_pmr) = regs[rt];
            return true;

        case SYSREG_ISS(ICC_IAR1_EL1):
            if (is_read) {
                if (!PERCPU(carrier_stack_ready) && !PERCPU(carrier_delivery_proven)) {
                    regs[rt] = 0x3ff;
                } else {
                    regs[rt] = hv_carrier_do_iar1();
                }
                if (regs[rt] != 0x3ff) {
                    u32 count = ++PERCPU(carrier_iar_count);
                    if (count <= 8)
                        hv_native_aic_trace_record(HV_NATIVE_AIC_TRACE_CARRIER_IAR,
                                                   regs[rt] | ((u64)count << 32), regs[18]);
                }
            }
            return true;

        case SYSREG_ISS(ICC_IGRPEN1_EL1):
            if (is_read)
                regs[rt] = hv_vgic3_get_igrpen1();
            else
                hv_vgic3_set_igrpen1(regs[rt]);
            hv_native_aic_trace_record(HV_NATIVE_AIC_TRACE_CARRIER_SYSREG, is_read ? 0 : BIT(32),
                                       regs[rt]);
            return true;

        case SYSREG_ISS(ICC_BPR1_EL1):
            if (is_read)
                regs[rt] = 0;
            hv_native_aic_trace_record(HV_NATIVE_AIC_TRACE_CARRIER_SYSREG,
                                       1 | (is_read ? 0 : BIT(32)), regs[rt]);
            return true;

        case SYSREG_ISS(ICC_EOIR1_EL1):
            if (is_read) {
                regs[rt] = 0;
            } else {
                hv_carrier_do_eoir1(regs[rt] & ICH_LR_VIRTUAL_MASK);
                u32 count = ++PERCPU(carrier_eoi_count);
                if (count <= 8)
                    hv_native_aic_trace_record(HV_NATIVE_AIC_TRACE_CARRIER_EOI,
                                               regs[rt] & ICH_LR_VIRTUAL_MASK, count);
            }
            return true;

        default:
            return false;
    }
#else
    (void)reg;
    (void)is_read;
    (void)rt;
    (void)regs;
    (void)probe;
    return false;
#endif
}

static bool hv_handle_t8142_gic_cpuif(u64 reg, bool is_read, u64 rt, u64 regs[32], bool probe)
{
    if (chip_id != T8142)
        return false;

    u64 cpu = smp_id();
    if (cpu >= MAX_CPUS)
        return false;

    if (hv_handle_native_aic_carrier_cpuif(reg, is_read, rt, regs, probe))
        return true;

    struct hv_gic_cpuif *s = &gic_cpuif[cpu];

    if (!s->valid) {
        /* Architectural reset values, so a read before any write is not a lie. */
        s->pmr = 0;
        s->bpr1 = 0;
        s->igrpen0 = 0;
        s->igrpen1 = 0;
        s->ctlr = 0;
        memset(s->pending, 0, sizeof(s->pending));
        memset(s->active, 0, sizeof(s->active));
        s->running = 0xff;
        s->valid = true;
    }

    switch (reg) {
        case SYSREG_ISS(ICC_PMR_EL1):
            if (is_read) {
                regs[rt] = s->pmr;
            } else if (!probe) {
                s->pmr = regs[rt] & 0xff;
                /* Lowering IRQL unmasks: re-evaluate before returning. */
                hv_gic_cpuif_sync_vi();
            }
            break;

        case SYSREG_ISS(ICC_BPR1_EL1):
            if (is_read)
                regs[rt] = s->bpr1;
            else if (!probe)
                s->bpr1 = regs[rt] & 0x7;
            break;

        case SYSREG_ISS(ICC_IGRPEN0_EL1):
            if (is_read)
                regs[rt] = s->igrpen0;
            else if (!probe)
                s->igrpen0 = regs[rt] & 1;
            break;

        case SYSREG_ISS(ICC_IGRPEN1_EL1):
            if (is_read) {
                regs[rt] = s->igrpen1;
            } else if (!probe) {
                u64 was = s->igrpen1;
                s->igrpen1 = regs[rt] & 1;
#ifdef ENABLE_VGIC_MODULE
                /* Keep transition diagnostics and fallback gates coherent. */
                hv_vgic3_set_igrpen1(s->igrpen1);
#endif
                /*
                 * Only the first enable is treated as bring-up.  An OS may
                 * toggle Group 1 around a critical section, and re-running the
                 * reset below on such a toggle would discard an interrupt the
                 * guest had already acknowledged but not yet completed.
                 */
                if (s->igrpen1 && !was && !gic_cpuif_brought_up[cpu]) {
                    gic_cpuif_brought_up[cpu] = true;
                    /*
                     * The OS is initializing this CPU interface.  Anything
                     * pending or active here predates it and belongs to
                     * firmware, so start from the architectural reset state and
                     * record the expired deadlines that must not be mistaken
                     * for the guest's own.
                     */
                    memset(s->pending, 0, sizeof(s->pending));
                    memset(s->active, 0, sizeof(s->active));
                    __atomic_thread_fence(__ATOMIC_RELEASE);
                    s->running = 0xff;

                    /*
                     * Take ownership of physical IRQ routing on this core.
                     *
                     * Both halves of delivery need it: a physical AIC IRQ only
                     * reaches hv_exc_irq() while HCR.IMO is set, and HCR.VI is
                     * only signalled to the guest while it is set.  Until now
                     * sync_vi() raised it as a side effect of the first pending
                     * timer tick, which is too late for a device -- the AIC
                     * would deliver to EL1, where an OS driving an emulated
                     * GICv3 cannot acknowledge it and the level would never
                     * drop.
                     *
                     * This is the safe moment: the guest has just enabled its
                     * CPU interface, Mu is gone, and the AIC master enable is
                     * still off until the first GICD_ISENABLER for an SPI
                     * (hv_vgic_arm_device_delivery()), so no physical interrupt
                     * can be asserted while the route changes underneath it.
                     */
                    hv_write_hcr(mrs(HCR_EL2) | HCR_IMO);
                    s->stale_p = hv_timer_firing(mrs(CNTP_CTL_EL02));
                    s->stale_v = hv_timer_firing(mrs(CNTV_CTL_EL02));
                    s->stale_cval_p = mrs(CNTP_CVAL_EL02);
                    s->stale_cval_v = mrs(CNTV_CVAL_EL02);

                    printf("HV: T8142: guest enabled emulated GIC group 1 on CPU %d; "
                           "timer PPIs %d/%d deliverable (inherited expiry p=%d v=%d)\n",
                           (int)cpu, HV_VGIC_TIMER_P_INTID, HV_VGIC_TIMER_V_INTID, s->stale_p,
                           s->stale_v);
                }
            }
            break;

        case SYSREG_ISS(ICC_SRE_EL1):
            /*
             * System-register access is the only interface we present; there
             * is no MMIO CPU interface to fall back to.  SRE is therefore
             * RAO/WI, which is exactly how a GICv3 with no memory-mapped
             * interface behaves.
             */
            if (is_read)
                regs[rt] = ICC_SRE_SRE | ICC_SRE_DFB | ICC_SRE_DIB;
            break;

        case SYSREG_ISS(ICC_CTLR_EL1):
            if (is_read) {
                /*
                 * Report 5 priority bits and 16-bit INTIDs, and leave ExtRange
                 * clear -- the kernel tests that bit before using extended
                 * INTIDs, and the emulated distributor does not implement them.
                 */
                regs[rt] = (4UL << 8);
            } else if (!probe) {
                s->ctlr = regs[rt];
            }
            break;

        case SYSREG_ISS(ICC_IAR1_EL1): {
            if (!is_read)
                break;

            int selected = hv_gic_cpuif_select(s);
            if (selected < 0) {
                regs[rt] = GIC_SPURIOUS_INTID;
                break;
            }
            u32 intid = (u32)selected;

            /*
             * Lowest INTID wins.  With one producer this is not a priority
             * decision, it is just a deterministic order; see the note on
             * hv_gic_cpuif_sync_vi() about priority not being modelled.
             *
             * A probe must not acknowledge: the pre-emptive image scan runs
             * this decoder over every word of the guest's text to decide what
             * to redirect, and consuming an interrupt while asking would drop
             * it on the floor before the guest ever ran.
             */
            if (!probe) {
                hv_gic_bitmap_clear(s->pending, intid);
                hv_gic_bitmap_set(s->active, intid);
                s->running = hv_gic_cpuif_priority(intid);
                gic_cpuif_ack_count[cpu]++;
                hv_gic_cpuif_sync_vi();
                /*
                 * First acknowledgement per core only.  The clock ticks
                 * forever once it works; logging every one is what turned a
                 * previous diagnostic into an 885k-line flood that stalled the
                 * boot it was meant to observe.
                 */
                if (!gic_cpuif_acked[cpu]) {
                    gic_cpuif_acked[cpu] = true;
                    printf("HV: T8142: guest acknowledged emulated INTID %u on CPU %d "
                           "from pc 0x%lx spsr 0x%lx\n",
                           intid, (int)cpu, mrs(ELR_EL2), mrs(SPSR_EL2));
                }
            }
            regs[rt] = intid;
            break;
        }

        case SYSREG_ISS(ICC_HPPIR1_EL1): {
            /* Same selection as IAR1, without acknowledging it. */
            if (!is_read)
                break;

            int selected = hv_gic_cpuif_select(s);
            regs[rt] = (selected >= 0) ? (u64)selected : GIC_SPURIOUS_INTID;
            break;
        }

        case SYSREG_ISS(ICC_RPR_EL1):
            /* Idle priority while nothing is active, otherwise the one value
             * a single-producer interface can honestly report. */
            if (is_read)
                regs[rt] = s->running;
            break;

        case SYSREG_ISS(ICC_EOIR1_EL1):
        case SYSREG_ISS(ICC_DIR_EL1): {
            if (is_read)
                break;

            u32 intid = regs[rt] & 0xffffff;
            if (intid < HV_GIC_MAX_INTID && !probe) {
                hv_gic_bitmap_clear(s->active, intid);
                s->running = 0xff;
                gic_cpuif_eoi_count[cpu]++;
                if (!gic_cpuif_eoi_logged[cpu]) {
                    gic_cpuif_eoi_logged[cpu] = true;
                    printf("HV: T8142: guest completed emulated INTID %u on CPU %d\n", intid,
                           (int)cpu);
                }
                /*
                 * PPIs deliberately keep their pending bit.  The GTDT declares
                 * both timer GSIVs level-triggered, and the guest's own re-arm
                 * is what drops the level -- hv_update_fiq() stops re-asserting
                 * once CNTx_CTL.ISTATUS clears.  Dropping it here instead would
                 * lose a tick whose deadline had already passed again.
                 *
                 * An SPI is the opposite case, because its level lives in real
                 * hardware rather than in this model.  Reading AIC EVENT both
                 * acknowledged and auto-masked the source, so completion has to
                 * clear the software pending bit and unmask the physical line
                 * again.  If the device is still asserting, the AIC raises a
                 * fresh event immediately and hv_exc_irq() marks it pending
                 * once more; leaving the bit set instead would re-signal an
                 * interrupt the hardware never repeated.
                 */
                if (intid >= 32) {
                    hv_gic_bitmap_clear(s->pending, intid);
                    /*
                     * Only re-arm a line m1n1 owns.  A purely virtual SPI has
                     * no AIC line behind it, and unmasking the same-numbered
                     * real one is how an unowned device ends up asserting at
                     * a CPU with no handler -- see hv_aic_alias_is_owned().
                     */
                    if (hv_aic_alias_is_owned(intid))
                        aic_set_mask(hv_aic_alias_to_physical(intid), false);
                }
                hv_gic_cpuif_sync_vi();
            }
            break;
        }

        case SYSREG_ISS(ICC_SGI1R_EL1):
            if (!is_read && !probe)
                hv_gic_cpuif_send_sgi(regs[rt]);
            break;

        default:
            return false;
    }

    return true;
}

/*
 * T8142 reports accesses to its absent architectural PMUv3 register bank as
 * an EL1 undefined instruction instead of producing an MDCR_EL2 sysreg trap.
 * Mu's exception vector therefore forwards the raw architectural system-
 * register instruction through one generic HVC.  Decode the instruction here
 * and feed it into the same EL2 PMU backend used for real ESR_EC_MSR traps.
 * No PMU policy or Microsoft-image rewriting lives in firmware.
 */
/*
 * `probe` asks only "would we service this instruction", and must not change
 * anything while asking.  The image scan tests every word in the guest's text
 * through this path, so a probe that took the write case would push a zero into
 * emulated state hundreds of times before the guest ran at all.  Probing forces
 * the read case: it answers the same membership question because that is
 * decided by the register, not the direction.
 */
static bool hv_t8142_sysreg_assist_impl(u32 instruction, u64 input, u64 *output, bool probe)
{
    bool is_read;

    switch (instruction & 0xffe00000U) {
        case 0xd5200000U: /* MRS Xt, system-register */
            is_read = true;
            break;
        case 0xd5000000U: /* MSR system-register, Xt */
            is_read = false;
            break;
        default:
            return false;
    }

    u64 reg = (((u64)(instruction >> 19) & 0x3) << ESR_ISS_MSR_OP0_SHIFT) |
              (((u64)(instruction >> 5) & 0x7) << ESR_ISS_MSR_OP2_SHIFT) |
              (((u64)(instruction >> 16) & 0x7) << ESR_ISS_MSR_OP1_SHIFT) |
              (((u64)(instruction >> 12) & 0xf) << ESR_ISS_MSR_CRn_SHIFT) |
              (((u64)(instruction >> 8) & 0xf) << ESR_ISS_MSR_CRm_SHIFT);
    u64 rt = instruction & 0x1f;
    u64 regs[32] = {0};

    if (probe)
        is_read = true;

    regs[rt] = input;
    /*
     * Both absent register files go through here, so the pre-emptive image
     * scan's "is this something we can service" predicate and the HVC handler
     * that later services it are the same decision by construction.
     */
    if (!hv_handle_software_pmu(reg, is_read, rt, regs) &&
        !hv_handle_t8142_gic_cpuif(reg, is_read, rt, regs, probe))
        return false;

    if (output)
        *output = regs[rt];
    return true;
}

bool hv_handle_t8142_sysreg_assist(u32 instruction, u64 input, u64 *output)
{
    return hv_t8142_sysreg_assist_impl(instruction, input, output, false);
}

/* Same decoder, asked without consequences -- see hv_t8142_sysreg_assist_impl. */
static bool hv_t8142_insn_is_absent_reg(u32 instruction)
{
    return hv_t8142_sysreg_assist_impl(instruction, 0, NULL, true);
}

/*
 * Recover a guest wedged on an access to a system register T8142 does not
 * implement.
 *
 * T8142 has no architectural PMUv3 bank at all -- PMCR_EL0 and friends do not
 * decode, measured from EL2 where nothing is trapping.  An access from EL1 is
 * therefore an undefined instruction delivered straight to EL1, and EL2 gets
 * no say: UNDEF is taken before MDCR_EL2.TPM can classify the access, so the
 * trap we would need never happens.  The firmware-side assist in Mu's
 * ArmExceptionLib covers this only while UEFI still owns VBAR_EL1.
 *
 * The Windows boot manager installs its own vectors and then runs an ungated
 * PMUv3 init.  Its undefined-instruction vector captures ELR/SPSR/ESR and
 * parks on a branch-to-self, which is a silent hang: no console output, no
 * firmware fault dump, nothing on the panel.
 *
 * So recover after the fact.  A guest PC sitting on a branch-to-self is
 * provably wedged -- that is the gate, and it is exact rather than heuristic.
 * The faulting access is then emulated with the same backend the HVC path
 * uses, and the guest's exception entry is unwound: resume at the instruction
 * after the access, with the PSTATE the guest had before it faulted.
 *
 * This is safe here specifically because the vector entry is reached directly
 * from the exception with no prologue, so it clobbers only x1/x2/x3 while
 * capturing the fault state; the faulting instruction's destination register
 * is untouched.  A guest whose handler saved and reused more state than that
 * could not be resumed this way.
 */
/*
 * AArch64 synchronous "current EL" vector slots.  Which one a fault uses
 * depends on the stack pointer the guest is running on, i.e. PSTATE.SP: EL1t
 * (SP_EL0) takes the 0x000 slot, EL1h (SP_ELx) takes 0x200.  This is not
 * academic here -- the Windows boot manager runs EL1h and the NT kernel runs
 * EL1t, so a redirect that assumed either one alone would silently do nothing
 * for the other.
 */
#define HV_VECTOR_SYNC_CURRENT_SP0 0x000
#define HV_VECTOR_SYNC_CURRENT_SPX 0x200
#define HV_VECTOR_SYNC_SLOT(spsr)                                                                  \
    (((spsr) & 1) ? HV_VECTOR_SYNC_CURRENT_SPX : HV_VECTOR_SYNC_CURRENT_SP0)
/*
 * Synchronous exceptions taken *from* EL0, which use a different slot entirely:
 * the "lower EL, AArch64" quarter of the table.  This is the only vector an
 * undefined instruction executed by a user-mode process can reach, so it is the
 * only place EL2 can intercept one -- nothing a process executes can trap to
 * EL2 directly (HVC is UNDEFINED at EL0), which rules out the instruction-site
 * replacement used for kernel images.
 */
#define HV_VECTOR_SYNC_LOWER_A64 0x400
/*
 * Distinctive HVC immediate so the trampoline cannot be confused with Mu's
 * "NTSRG" forwarding calls, which share the SMC/HVC service dispatcher.
 */
#define HV_T8142_UNDEF_HVC_IMM  0x4d31
#define HV_T8142_UNDEF_HVC_INSN (0xd4000002U | (HV_T8142_UNDEF_HVC_IMM << 5))

/*
 * Replaced access sites carry their own identity in the HVC immediate.
 *
 * Addressing them by location does not survive the guest moving the code.  A
 * site is recorded by VA and written by PA, and Windows relocates driver text
 * after load -- import optimisation copies it into pool pages -- so the copy
 * holds a perfectly good HVC of ours at a VA we never patched and a PA we never
 * wrote.  Neither key matches, and the old fallback stepped over the
 * instruction: measured at exactly 8 skipped guest instructions per boot, in
 * kernel code, the last of them at the line where WinPE stopped making
 * progress.  The addresses were freshly randomised each boot, which is what
 * ruled out a second view of a page we had patched.
 *
 * Putting the site's table index in the immediate makes the trap
 * position-independent: wherever the word ends up, it still says which
 * instruction it replaced.  It also turns the lookup from a scan of ~600
 * entries into an index, on a path taken thousands of times a boot.
 *
 * Indices are stable because the table only ever grows.  The range sits above
 * HV_T8142_UNDEF_HVC_IMM and does not collide with Mu's forwarding calls, which
 * are identified by a magic in a register (HV_SYSREG_ASSIST_CALL_MAGIC), not by
 * their immediate.
 */
#define HV_T8142_SITE_HVC_IMM_BASE  0x4e00
#define HV_T8142_SITE_HVC_INSN(idx) (0xd4000002U | ((HV_T8142_SITE_HVC_IMM_BASE + (u32)(idx)) << 5))

/*
 * Every vector we have redirected, not just the most recent one.
 *
 * A guest moves between vector tables constantly -- the boot manager calls back
 * into UEFI boot services, which run under the firmware's own VBAR, so the two
 * alternate many times a second.  Redirecting only the table in use at the last
 * observation means the other one is unprotected exactly when the guest returns
 * to it, which reintroduces the hang it was meant to prevent.  The tables are
 * independent memory, so simply keep them all redirected.
 */
#define HV_T8142_MAX_UNDEF_VECTORS 16

static struct {
    u64 va;       /* vbar + slot */
    u64 pa;       /* where the redirect was actually written */
    u32 orig;     /* instruction displaced there, if redirected */
    bool refused; /* examined and left alone; orig is not meaningful */
} t8142_undef_vec[HV_T8142_MAX_UNDEF_VECTORS];
static u32 t8142_undef_vec_count;

/*
 * Emulate the undefined-instruction fault the guest is currently sitting in.
 *
 * Reads the guest's pending fault state via the _EL12 aliases (under E2H the
 * plain _EL1 names would read m1n1's own registers), emulates the faulting
 * system-register access with the same backend the HVC path uses, and unwinds
 * the guest's exception entry so it resumes after the access with the PSTATE
 * it had before faulting.
 */
static bool hv_emulate_t8142_pending_undef(struct exc_info *ctx, u32 *insn_out, u64 *pc_out)
{
    if (!hv_windows_guest_compat_enabled())
        return false;

    if (mrs(ESR_EL12) != 0x02000000) /* EC 0, "unknown reason" == UNDEF */
        return false;

    u64 fault_pc = mrs(ELR_EL12);
    u64 insn_pa = hv_translate(fault_pc, false, false, NULL);
    if (!insn_pa)
        return false;

    u32 insn = read32(insn_pa);
    u64 rt = insn & 0x1f;
    u64 out = 0;

    if (!hv_handle_t8142_sysreg_assist(insn, rt < 31 ? ctx->regs[rt] : 0, &out))
        return false;

    if (((insn & 0xffe00000U) == 0xd5200000U) && rt < 31) /* MRS Xt, sysreg */
        ctx->regs[rt] = out;

    ctx->elr = fault_pc + 4;
    /*
     * Unwinding to the pre-fault PSTATE is what returns an EL0 fault to EL0
     * rather than into the guest's handler.  It reaches the CPU through
     * hv_exc_sync()'s write-back, deliberately not from here: hv_translate()
     * picks AT S12E1R vs S12E0R off SPSR_EL2, and hv_exc_exit() resolves kernel
     * addresses before it republishes SPSR.  Changing the register early makes
     * those kernel translations run as EL0 and fail.
     */
    ctx->spsr = mrs(SPSR_EL12);

    if (insn_out)
        *insn_out = insn;
    if (pc_out)
        *pc_out = fault_pc;
    return true;
}

/*
 * Point the guest's undefined-instruction vector at EL2.
 *
 * Recovering a wedged guest from the host tick proves the mechanism but runs at
 * the tick rate: the Windows boot manager's cycle-counter calibration loop
 * takes three PMCCNTR_EL0 reads per iteration, so at one recovery per tick it
 * makes no useful progress.  Overwriting the first instruction of the guest's
 * synchronous vector with an HVC turns every subsequent undefined-instruction
 * fault into a direct EL2 trap: emulate, return, no parking and no tick
 * dependency.
 *
 * It also removes the register damage the recovery path has to tolerate.  The
 * guest's vector captures ELR/SPSR/ESR into x1/x2/x3 before halting; trapping
 * on the vector's first instruction means that capture never runs, so the
 * guest's registers reach us untouched.
 */
/*
 * Reproduce the instruction our redirect displaced from a vector's first slot.
 *
 * Restoring the original and letting the guest re-run its own vector is not
 * viable: it leaves the vector unredirected until the next tick, and an absent
 * register touched inside that window reaches a handler with no answer for it.
 * The NT kernel does not survive that -- it keeps its KPCR in SP_EL1 while
 * running EL1t and recovers it in the vector's first instruction, so a fault
 * that slips through leaves x18 holding garbage and every access through it
 * aborts, recursively.
 *
 * So never unpatch: emulate the displaced instruction and continue into the
 * rest of the guest's own vector. Only instructions handled here may be
 * displaced in the first place -- see hv_can_displace_vector_insn().
 *
 * With `perform` false this only reports whether the instruction is one we can
 * reproduce, so the same decoder gates patching and performs the replay and
 * the two can never disagree.
 */
static bool hv_replay_vector_insn(struct exc_info *ctx, u64 vec_va, u32 insn, bool perform)
{
    u64 rt = insn & 0x1f;
    u64 val;

    /* B #imm26 -- vector slots commonly just branch to the real handler. */
    if ((insn & 0xfc000000U) == 0x14000000U) {
        if (perform) {
            s64 off = ((s64)((u64)(insn & 0x03ffffffU) << 38)) >> 36;
            ctx->elr = vec_va + off;
        }
        return true;
    }

    /*
     * SUB SP, SP, #imm12 (unshifted) -- a vector that opens by reserving its
     * own frame.  NT's lower-EL slots all begin "sub sp, sp, #0x370".
     *
     * Which stack that adjusts is not a guess: an exception entry always sets
     * PSTATE.SP, so the vector runs at EL1h and SP is SP_EL1.  Read it off the
     * guest's own PSTATE anyway rather than assuming, so this stays correct if
     * it is ever reached from a mode where it is not.
     */
    if ((insn & 0xffc003ffU) == 0xd10003ffU) {
        if (perform) {
            u64 imm12 = (insn >> 10) & 0xfff;

            /* Both return paths republish ctx->sp[]; see hv_exc_sync(). */
            ctx->sp[ctx->spsr & 1] -= imm12;
            ctx->elr = vec_va + sizeof(u32);
        }
        return true;
    }

    /* MRS Xt, <one of the exception-state registers a vector prologue reads> */
    switch (insn & ~0x1fU) {
        case 0xd5384020U: /* ELR_EL1  */
        case 0xd5384000U: /* SPSR_EL1 */
        case 0xd5385200U: /* ESR_EL1  */
        case 0xd5384100U: /* SP_EL0   */
            break;
        default:
            return false;
    }

    if (!perform) /* validation only; ctx is not available */
        return true;

    switch (insn & ~0x1fU) {
        case 0xd5384020U:
            val = mrs(ELR_EL12);
            break;
        case 0xd5384000U:
            val = mrs(SPSR_EL12);
            break;
        case 0xd5385200U:
            val = mrs(ESR_EL12);
            break;
        default:
            /*
             * SP_EL0, and it must be sp[0].  This encoding was labelled SP_EL1
             * here and replayed from sp[1], which is a different register:
             * 0xd5384100 is op1=0 (SP_EL0), while SP_EL1 is op1=4, 0xd53c4100 --
             * and EL1 cannot name SP_EL1 with MRS at all.
             *
             * The mistake was not cosmetic.  NT's synchronous EL1t vector opens
             * "mrs x18, sp_el0 / and sp, x18, #~0xf": it recovers the
             * interrupted kernel stack and immediately installs it.  Replaying
             * sp[1] handed it SP_EL1 instead, so the kernel resumed on a stack
             * that was never one, faulted through it recursively and reached
             * PANIC_STACK_SWITCH.  That is the bugcheck 0x2B which was recorded
             * against redirecting this slot at all, and it was this line.
             */
            val = ctx->sp[0];
            break;
    }

    if (rt < 31)
        ctx->regs[rt] = val;
    ctx->elr = vec_va + sizeof(u32);
    return true;
}

static bool hv_can_displace_vector_insn(u32 insn)
{
    return hv_replay_vector_insn(NULL, 0, insn, false);
}

/*
 * Publish an instruction written into guest memory to the guest's fetch path.
 *
 * "The word is at the right physical address" and "the guest executes it" are
 * different claims, and the gap between them is not theoretical here: a
 * redirect was observed sitting correctly at its physical address, re-read
 * from EL2 on every tick for eighty seconds, while the guest kept executing
 * the instruction it had replaced.
 *
 * So clean *and invalidate* to the point of coherency rather than only
 * cleaning to the point of unification.  A clean alone publishes our store but
 * leaves any copy the guest already holds from loading its image intact, and
 * that copy is what the fetch returns.  The DSB after the instruction-cache
 * invalidate is equally required: without it the ISB may retire before the
 * invalidate has been observed.
 */
/* Automatic guest mutation requires the explicit experimental boot profile. */
static void hv_write_guest_insn(u64 pa, u32 insn)
{
    if (!hv_windows_guest_compat_enabled())
        return;

    write32(pa, insn);
    sysop("dsb ish");
    dc_civac(pa);
    sysop("dsb ish");
    ic_ialluis();
    sysop("dsb ish");
    sysop("isb");
}

static bool hv_patch_t8142_undef_vector(u64 slot)
{
    if (!hv_windows_guest_compat_enabled())
        return false;

    u64 vbar = mrs(VBAR_EL12);

    if (!vbar)
        return false;

    u64 va = vbar + slot;

    /* Already decided about this one, either way; do not look at it again. */
    for (u32 i = 0; i < t8142_undef_vec_count; i++)
        if (t8142_undef_vec[i].va == va)
            return !t8142_undef_vec[i].refused;

    if (t8142_undef_vec_count >= HV_T8142_MAX_UNDEF_VECTORS)
        return false;

    /*
     * Translate for read, not for write, even though we are about to write.
     *
     * A write translation asks whether the *guest* may write the page, which is
     * the wrong question: a kernel's vector table sits in its read-only text
     * mapping, so AT S1E1W fails there and the redirect is silently skipped --
     * indistinguishable, from the log, from the redirect being installed and
     * not working.  We do not write through the guest's stage-1 mapping at all;
     * we resolve the address and store to the physical page through EL2's own,
     * where the guest's permissions do not apply.
     */
    u64 pa = hv_translate(va, false, false, NULL);
    if (!pa) {
        /*
         * Not necessarily an error: a guest may publish VBAR_EL1 before the
         * page is mapped.  Rate-limited because it is retried on every table
         * switch, and a boot manager alternates tables constantly.
         */
        static u64 untranslatable;

        if (untranslatable < 4 || (untranslatable % 4096) == 0)
            printf("HV: T8142: guest undef vector 0x%lx not translatable (#%ld)\n", va,
                   untranslatable);
        untranslatable++;
        return false;
    }

    u32 orig = read32(pa);
    if (orig == HV_T8142_UNDEF_HVC_INSN) /* somehow already ours; nothing to record */
        return true;

    /*
     * Only displace an instruction we can reproduce exactly, since the redirect
     * is permanent and every fault through this vector -- not just the ones we
     * emulate -- has to continue into the guest's own handler correctly.
     */
    if (!hv_can_displace_vector_insn(orig)) {
        /*
         * Record the refusal rather than just reporting it.  This runs from the
         * synchronous-exception path, so a decision that is not remembered is
         * remade on every trap the guest takes -- a page-table walk, a guest
         * read and a console write each time.  Firmware's own vector is the
         * common case and it traps constantly, which is enough to bury the
         * machine: leaving this uncached cost ~885k identical console lines and
         * stalled the boot inside UEFI, before the boot manager was ever
         * reached.  Firmware needs no redirect anyway -- it services these
         * faults itself through the sysreg assist.
         */
        printf("HV: T8142: leaving guest undef vector 0x%lx alone, cannot replay 0x%08x\n", va,
               orig);
        t8142_undef_vec[t8142_undef_vec_count].va = va;
        t8142_undef_vec[t8142_undef_vec_count].refused = true;
        t8142_undef_vec_count++;
        return false;
    }

    hv_write_guest_insn(pa, HV_T8142_UNDEF_HVC_INSN);

    /*
     * Read the write back.  "Logged as redirected" and "the guest executes our
     * HVC" are different claims, and conflating them cost a full debugging
     * cycle: the NT vector was reported redirected, then took 137 faults that
     * all reached its own handler.  Report the address actually written so a
     * translation that succeeds but resolves somewhere useless is visible.
     */
    u32 back = read32(pa);
    if (back != HV_T8142_UNDEF_HVC_INSN) {
        printf("HV: T8142: guest undef vector 0x%lx write did not stick at pa 0x%lx "
               "(read back 0x%08x)\n",
               va, pa, back);
        return false;
    }

    t8142_undef_vec[t8142_undef_vec_count].va = va;
    t8142_undef_vec[t8142_undef_vec_count].pa = pa;
    t8142_undef_vec[t8142_undef_vec_count].orig = orig;
    t8142_undef_vec_count++;

    printf("HV: T8142: guest undef vector 0x%lx (+0x%lx) redirected to EL2 at pa 0x%lx "
           "(displaced 0x%08x)\n",
           va, slot, pa, orig);
    return true;
}

/*
 * Re-assert every redirect we believe we own.
 *
 * A redirect is a write into guest memory, and the guest can invalidate it
 * without touching VBAR_EL1: the NT kernel rebuilds its page tables during
 * early initialization, so the same vector VA can resolve to a different
 * physical page than the one we patched, and any guest may simply rewrite the
 * page.  Tracking keyed on the VBAR value alone cannot see either -- it sees an
 * address it has already handled and skips it, permanently.
 *
 * So re-translate and re-check each entry from the host tick.  This is bounded
 * by the table size, runs at the tick rate rather than per trap, and self-heals
 * regardless of which of those causes applied.
 */
void hv_verify_t8142_undef_vectors(void)
{
    if (!hv_windows_guest_compat_enabled())
        return;

    if (chip_id != T8142)
        return;

    for (u32 i = 0; i < t8142_undef_vec_count; i++) {
        if (t8142_undef_vec[i].refused)
            continue;

        u64 va = t8142_undef_vec[i].va;
        u64 pa = hv_translate(va, false, false, NULL);
        if (!pa)
            continue; /* not mapped right now; nothing useful to do */

        /*
         * The VA moved to a different page.  Put the page we did patch back the
         * way we found it before adopting the new one, or our HVC stays in it
         * forever: the guest recycles that frame into pool or code, executes it,
         * and arrives here as a trampoline hit with no record -- which the
         * handler can only answer by skipping whatever instruction was there.
         * Eight of those per boot were reaching the guest before this.
         */
        if (t8142_undef_vec[i].pa && t8142_undef_vec[i].pa != pa) {
            u64 stale = t8142_undef_vec[i].pa;

            if (read32(stale) == HV_T8142_UNDEF_HVC_INSN) {
                hv_write_guest_insn(stale, t8142_undef_vec[i].orig);
                printf("HV: T8142: redirect for 0x%lx moved pa 0x%lx -> 0x%lx, restored "
                       "0x%08x at the old page\n",
                       va, stale, pa, t8142_undef_vec[i].orig);
            }
            t8142_undef_vec[i].pa = 0;
        }

        u32 cur = read32(pa);
        if (cur == HV_T8142_UNDEF_HVC_INSN) {
            t8142_undef_vec[i].pa = pa;
            /*
             * The redirect is present in memory, which does not prove the
             * guest fetches it.  Re-publishing costs one broadcast invalidate
             * per tick and closes the case where the guest is holding a stale
             * copy of the line -- the exact state observed when a redirect
             * read back correctly while the guest ignored it.
             */
            sysop("dsb ish");
            dc_civac(pa);
            sysop("dsb ish");
            ic_ialluis();
            sysop("dsb ish");
            sysop("isb");
            continue;
        }

        /*
         * The redirect is gone.  Whatever is there now is what a fault would
         * run, so it -- not the instruction we displaced originally -- is what
         * has to be replayable and recorded.
         */
        if (!hv_can_displace_vector_insn(cur)) {
            printf("HV: T8142: redirect at 0x%lx lost, and 0x%08x cannot be replayed; "
                   "dropping it\n",
                   va, cur);
            t8142_undef_vec[i] = t8142_undef_vec[--t8142_undef_vec_count];
            i--;
            continue;
        }

        hv_write_guest_insn(pa, HV_T8142_UNDEF_HVC_INSN);
        if (read32(pa) != HV_T8142_UNDEF_HVC_INSN)
            continue;

        t8142_undef_vec[i].pa = pa;
        t8142_undef_vec[i].orig = cur;
        printf("HV: T8142: redirect at 0x%lx reinstated at pa 0x%lx (displaced 0x%08x)\n", va, pa,
               cur);
    }
}

/*
 * Put one guest vector back exactly as it was and stop tracking it.
 *
 * Returns whether anything was actually restored.  Callers that rewind ELR to
 * re-execute the address must check this: rewinding onto a word we did not
 * replace means re-executing our own HVC, which never terminates.
 */
static bool hv_unpatch_t8142_undef_vector(u64 va)
{
    if (!hv_windows_guest_compat_enabled())
        return false;

    for (u32 i = 0; i < t8142_undef_vec_count; i++) {
        if (t8142_undef_vec[i].va != va || t8142_undef_vec[i].refused)
            continue;

        u64 pa = hv_translate(va, false, true, NULL);
        if (pa)
            hv_write_guest_insn(pa, t8142_undef_vec[i].orig);

        /*
         * The entry is about to go, so this is the last chance to clean up a
         * page the VA no longer resolves to.  Leaving our HVC there is what
         * produces trampoline hits at addresses nothing can account for.
         */
        u64 stale = t8142_undef_vec[i].pa;
        if (stale && stale != pa && read32(stale) == HV_T8142_UNDEF_HVC_INSN)
            hv_write_guest_insn(stale, t8142_undef_vec[i].orig);

        t8142_undef_vec[i] = t8142_undef_vec[--t8142_undef_vec_count];
        return pa != 0;
    }

    return false;
}

/*
 * Redirect the offending access itself rather than the vector that catches it.
 *
 * Patching the vector is indirect: it depends on the guest's vector being
 * replaceable, on the displaced instruction being reproducible, and on the
 * guest actually fetching our write -- and the NT kernel defeated the last of
 * those, executing its original vector for eighty seconds while the redirect
 * sat verified at the right physical address.  Replacing the absent-register
 * access itself removes all three dependencies: there is no exception, no
 * vector, no displaced prologue to replay, and nothing to unwind.
 *
 * The cost is that each site has to fault once to be found, which the tick
 * recovery already does.  Guests touch a small fixed set of these.
 */
#define HV_T8142_MAX_PMU_SITES 1024

static struct {
    u64 va;   /* guest VA of an absent-register access we replaced */
    u64 pa;   /* where the replacement was actually written */
    u32 orig; /* the access instruction itself */
} t8142_pmu_site[HV_T8142_MAX_PMU_SITES];
static u32 t8142_pmu_site_count;

static bool hv_patch_t8142_pmu_site_quiet(u64 va, u32 insn, bool verbose)
{
    if (!hv_windows_guest_compat_enabled())
        return false;

    for (u32 i = 0; i < t8142_pmu_site_count; i++)
        if (t8142_pmu_site[i].va == va)
            return true;

    if (t8142_pmu_site_count >= HV_T8142_MAX_PMU_SITES)
        return false;

    u64 pa = hv_translate(va, false, false, NULL);
    if (!pa)
        return false;

    /* Only replace the exact instruction we decoded; anything else is not ours. */
    if (read32(pa) != insn)
        return false;

    /* Encodes the slot it is about to occupy; see HV_T8142_SITE_HVC_INSN. */
    u32 trap = HV_T8142_SITE_HVC_INSN(t8142_pmu_site_count);

    hv_write_guest_insn(pa, trap);
    if (read32(pa) != trap)
        return false;

    t8142_pmu_site[t8142_pmu_site_count].va = va;
    t8142_pmu_site[t8142_pmu_site_count].pa = pa;
    t8142_pmu_site[t8142_pmu_site_count].orig = insn;
    t8142_pmu_site_count++;

    if (verbose)
        printf("HV: T8142: absent-register site 0x%lx (0x%08x) redirected to EL2 at pa 0x%lx\n", va,
               insn, pa);
    return true;
}

static bool hv_patch_t8142_pmu_site(u64 va, u32 insn)
{
    return hv_patch_t8142_pmu_site_quiet(va, insn, true);
}

/*
 * Replace every absent-register access in a guest image before any of them runs.
 *
 * Discovering these lazily, one fault at a time, cannot work for the NT kernel:
 * its undefined-instruction path ends in a bugcheck (bl; brk #0xf000; b .), so
 * the first access to reach it is already fatal.  Recovering the guest from
 * that park does not help either -- the kernel's own handler has run by then
 * and overwritten the interrupted registers, so resuming past the faulting
 * instruction lands in code whose live state is gone.  That is precisely how
 * the first emulated PMCCNTR_EL0 read was immediately followed by a write
 * through a destroyed base register.
 *
 * The only intervention that survives is one that happens before the first
 * execution.  Walk back from a known address inside the image to its PE
 * header, then replace the accesses in its executable sections.  The predicate
 * is the emulator itself, so a site can never be replaced by something the HVC
 * handler would then refuse.
 */
#define HV_PE_MAX_BACK_PAGES  16384
#define HV_PE_SECTION_EXECUTE 0x20000000

static bool hv_read_guest32(u64 va, u32 *out)
{
    u64 pa = hv_translate(va, false, false, NULL);

    if (!pa)
        return false;
    *out = read32(pa);
    return true;
}

static bool hv_read_guest64(u64 va, u64 *out)
{
    u32 lo, hi;

    if (!hv_read_guest32(va, &lo) || !hv_read_guest32(va + 4, &hi))
        return false;
    *out = ((u64)hi << 32) | lo;
    return true;
}

static u64 hv_find_guest_pe_base(u64 va)
{
    u64 page = va & ~0xfffUL;

    for (u32 i = 0; i < HV_PE_MAX_BACK_PAGES; i++, page -= 0x1000) {
        u32 mz, lfanew, sig;

        /*
         * Each iteration is a stage-1 walk plus a guest read, and the miss case
         * runs all 16384 of them.  That case became reachable once individual
         * driver images started being scanned on first fault, so pet the
         * watchdog rather than rely on the walk staying short.
         */
        if ((i & 0xff) == 0)
            hv_wdt_pet();

        if (!hv_read_guest32(page, &mz) || (mz & 0xffff) != 0x5a4d) /* 'MZ' */
            continue;
        if (!hv_read_guest32(page + 0x3c, &lfanew) || lfanew < 0x40 || lfanew > 0x1000)
            continue;
        if (!hv_read_guest32(page + lfanew, &sig) || sig != 0x00004550) /* 'PE\0\0' */
            continue;
        return page;
    }

    return 0;
}

/*
 * Base of the kernel image we scanned, kept so the bugcheck reporter below can
 * find a global inside it.
 */
static u64 t8142_kernel_base;

/*
 * Decode an AArch64 PC-relative literal load.
 *
 * Needed because the scan rewrites instructions in place, and a *data* word
 * that happens to decode as an absent-register access is indistinguishable
 * from a real one when read in isolation.  Rewriting such a word does not
 * redirect anything -- nothing executes it -- it silently replaces a constant
 * the image will later load with the HVC encoding.
 *
 * This was checked offline for ntoskrnl when the scan only covered the kernel:
 * zero of 535 candidates collided with a literal.  Sweeping 110 images extends
 * the same rewrite to code that has had no such check, so the check moves here.
 */
static bool hv_insn_is_literal_load(u32 insn, u64 pc, u64 *target, u32 *size)
{
    u32 bytes;

    switch (insn & 0xff000000) {
        case 0x18000000:
            bytes = 4;
            break; /* LDR   Wt, label      */
        case 0x58000000:
            bytes = 8;
            break; /* LDR   Xt, label      */
        case 0x98000000:
            bytes = 4;
            break; /* LDRSW Xt, label      */
        case 0x1c000000:
            bytes = 4;
            break; /* LDR   St, label      */
        case 0x5c000000:
            bytes = 8;
            break; /* LDR   Dt, label      */
        case 0x9c000000:
            bytes = 16;
            break; /* LDR   Qt, label      */
        default:
            return false;
    }

    s32 imm19 = (s32)((insn >> 5) & 0x7ffff);
    if (imm19 & 0x40000)
        imm19 |= ~0x7ffff; /* sign extend */

    *target = pc + ((s64)imm19 * 4);
    *size = bytes;
    return true;
}

#define HV_PE_MAX_SECTION_CANDIDATES 1024

static u64 scan_cand_va[HV_PE_MAX_SECTION_CANDIDATES];
static u32 scan_cand_insn[HV_PE_MAX_SECTION_CANDIDATES];
static bool scan_cand_is_data[HV_PE_MAX_SECTION_CANDIDATES];

static void hv_scan_guest_image_absent_regs(u64 va_in_image)
{
    if (!hv_windows_guest_compat_enabled())
        return;

    /*
     * Every image ever scanned, so the per-tick module walk skips the ones it
     * has already done.
     *
     * This table is not an optimisation, it is what stops the walk becoming the
     * boot's bottleneck.  A base that does not fit is never recorded, so the
     * image is rescanned on *every* tick from then on -- a full two-pass
     * instruction scan of its text plus two console lines, with the guest paused
     * for all of it.  At 64 entries that started the moment WinPE's 65th driver
     * loaded: measured at 142 images, so 78 of them rescanned once a second,
     * ~156 log lines per tick against a UART that cannot carry them.  Windows
     * stopped loading drivers and idled for 27000 log lines, which read exactly
     * like a hang and was not one.
     */
    static u64 scanned[512];
    static u32 scanned_count;

    u64 base = hv_find_guest_pe_base(va_in_image);
    if (!base)
        return;

    /*
     * The first image scanned is the one the guest's vectors belong to, i.e.
     * the kernel.  Later calls scan individual driver images, and must not
     * retarget this: the bugcheck reporter resolves KiBugCheckData relative to
     * it, and the vector gate uses it to mean "the kernel image is covered".
     */
    if (!t8142_kernel_base)
        t8142_kernel_base = base;

    for (u32 i = 0; i < scanned_count; i++)
        if (scanned[i] == base)
            return;

    if (scanned_count >= ARRAY_SIZE(scanned)) {
        /*
         * Out of room.  Skip the image rather than scan it, because scanning
         * one we cannot record means scanning it again on every tick forever,
         * and that starves the guest far more surely than a missed site hurts
         * it.  Missing it is survivable now: the redirected +0x000 vector
         * catches an absent-register access in unscanned code before NT's
         * handler runs, which is the whole reason it is redirected.
         */
        static bool full_logged;

        if (!full_logged) {
            full_logged = true;
            printf("HV: T8142: image scan table full at %d images; further images rely on the "
                   "vector redirect\n",
                   (int)scanned_count);
        }
        return;
    }
    scanned[scanned_count++] = base;

    u32 lfanew, coff, opt;
    if (!hv_read_guest32(base + 0x3c, &lfanew))
        return;

    u64 nt = base + lfanew;
    if (!hv_read_guest32(nt + 4, &coff) || !hv_read_guest32(nt + 20, &opt))
        return;

    u32 nsec = (coff >> 16) & 0xffff;
    u64 sechdr = nt + 24 + (opt & 0xffff);
    u32 words = 0, patched = 0, literals = 0, overflow = 0, cand_count = 0;

    for (u32 s = 0; s < nsec && s < 64; s++) {
        u64 h = sechdr + s * 40;
        u32 vsz, rva, chars;

        if (!hv_read_guest32(h + 8, &vsz) || !hv_read_guest32(h + 12, &rva) ||
            !hv_read_guest32(h + 36, &chars))
            continue;
        if (!(chars & HV_PE_SECTION_EXECUTE) || !vsz)
            continue;

        u64 v = (base + rva) & ~3UL;
        u64 end = base + rva + vsz;

        while (v < end) {
            u64 page_end = (v | 0xfffUL) + 1;
            if (page_end > end)
                page_end = end;

            /* Several thousand pages with the guest stopped; do not let the
             * watchdog conclude the hypervisor has hung. */
            hv_wdt_pet();

            u64 pa = hv_translate(v, false, false, NULL);
            if (!pa) {
                v = page_end;
                continue;
            }

            for (; v + 3 < page_end; v += 4, pa += 4) {
                u32 insn = read32(pa);

                words++;
                if (!hv_t8142_insn_is_absent_reg(insn))
                    continue;
                if (cand_count < HV_PE_MAX_SECTION_CANDIDATES) {
                    scan_cand_va[cand_count] = v;
                    scan_cand_insn[cand_count] = insn;
                    scan_cand_is_data[cand_count] = false;
                    cand_count++;
                } else {
                    overflow++;
                }
            }
            v = page_end;
        }

        if (!cand_count)
            continue;

        /*
         * Second pass, paid only by sections that actually contain candidates
         * -- which is a handful of images out of the hundred-odd swept.  Any
         * candidate that some instruction loads as a constant is data, not
         * code, and must be left alone.
         */
        v = (base + rva) & ~3UL;
        while (v < end) {
            u64 page_end = (v | 0xfffUL) + 1;
            if (page_end > end)
                page_end = end;

            hv_wdt_pet();

            u64 pa = hv_translate(v, false, false, NULL);
            if (!pa) {
                v = page_end;
                continue;
            }

            for (; v + 3 < page_end; v += 4, pa += 4) {
                u64 target;
                u32 bytes;

                if (!hv_insn_is_literal_load(read32(pa), v, &target, &bytes))
                    continue;

                for (u32 i = 0; i < cand_count; i++) {
                    if (scan_cand_va[i] >= target && scan_cand_va[i] < target + bytes) {
                        scan_cand_is_data[i] = true;
                    }
                }
            }
            v = page_end;
        }

        for (u32 i = 0; i < cand_count; i++) {
            if (scan_cand_is_data[i]) {
                literals++;
                continue;
            }
            if (hv_patch_t8142_pmu_site_quiet(scan_cand_va[i], scan_cand_insn[i], false))
                patched++;
        }
        cand_count = 0;
    }

    if (literals || overflow)
        printf("HV: T8142: image at 0x%lx: %ld candidate(s) left alone as literal data, "
               "%ld unchecked past the cap\n",
               base, (u64)literals, (u64)overflow);

    printf("HV: T8142: image at 0x%lx: scanned %ld words, redirected %ld absent-register "
           "accesses (%ld sites total)\n",
           base, (u64)words, (u64)patched, (u64)t8142_pmu_site_count);

    /*
     * How many rewritten sites can write a callee-saved register at all.  The
     * individual addresses were dumped once and answered their question (58 of
     * 581, six of them into x26); what matters now is which ones execute, and
     * that is reported from the emulation path instead.
     */
    u32 rt_callee_saved = 0;
    for (u32 i = 0; i < t8142_pmu_site_count; i++) {
        u32 rt = t8142_pmu_site[i].orig & 0x1f;

        if (rt >= 19 && rt <= 28)
            rt_callee_saved++;
    }
    printf("HV: T8142: absent-register sites writing x19-x28: %u of %ld\n", rt_callee_saved,
           (u64)t8142_pmu_site_count);
}

/*
 * The guest executed our HVC where an absent-register access used to be.  No
 * exception was taken, so there is no pending fault state to consult -- the
 * instruction to emulate is the one we recorded when we replaced it, and
 * ELR_EL2 already points past it.
 */
static bool hv_handle_t8142_pmu_site(struct exc_info *ctx, u32 imm)
{
    if (!hv_windows_guest_compat_enabled())
        return false;

    if (imm < HV_T8142_SITE_HVC_IMM_BASE)
        return false;

    u32 idx = imm - HV_T8142_SITE_HVC_IMM_BASE;
    if (idx >= t8142_pmu_site_count)
        return false;

    u64 site = ctx->elr - sizeof(u32);
    u32 insn = t8142_pmu_site[idx].orig;
    u64 rt = insn & 0x1f;
    u64 out = 0;

    if (!hv_handle_t8142_sysreg_assist(insn, rt < 31 ? ctx->regs[rt] : 0, &out))
        return false;

    if (((insn & 0xffe00000U) == 0xd5200000U) && rt < 31) /* MRS Xt, sysreg */
        ctx->regs[rt] = out;

    static u64 site_emulated;
    static u32 callee_saved_emulated;
    static u64 relocated;

    /*
     * Executing somewhere other than where it was planted means the guest moved
     * the code.  Worth saying once per site-ish rather than never: it used to be
     * the case that got the instruction skipped.
     */
    if (site != t8142_pmu_site[idx].va) {
        if (relocated < 8 || (relocated % 4096) == 0)
            printf("HV: T8142: site %d (0x%lx) relocated to 0x%lx, emulated 0x%08x (#%ld)\n",
                   (int)idx, t8142_pmu_site[idx].va, site, insn, relocated);
        relocated++;
    }

    /*
     * An MRS into x19-x28 makes m1n1 the writer of a register the ABI says
     * the callee must preserve.  That is the exact shape of the old
     * bugcheck 0xA, so print those in full rather than under the throttle
     * that hid them: the aggregate scan says only 58 of ~581 sites can do
     * it, and how many of those ever execute is the open question.
     */
    if (((insn & 0xffe00000U) == 0xd5200000U) && rt >= 19 && rt <= 28) {
        if (callee_saved_emulated < 64)
            printf("HV: T8142: CALLEE-SAVED WRITE x%ld = 0x%lx at 0x%lx (insn 0x%08x)\n", rt, out,
                   site, insn);
        callee_saved_emulated++;
    } else if (site_emulated < 8 || (site_emulated % 4096) == 0) {
        printf("HV: T8142: site emulated 0x%08x at 0x%lx (#%ld)\n", insn, site, site_emulated);
    }
    site_emulated++;
    return true;
}

/*
 * Traffic through the lower-EL synchronous vector.
 *
 * Redirecting +0x400 catches user-mode absent-register accesses, but it also
 * puts an EL2 round trip on every syscall and every user page fault, which is
 * the entire cost of the mechanism.  The two counts are what says whether that
 * is affordable, so keep them apart: `emul` is work only EL2 can do, `pass` is
 * pure overhead paid to see it.
 */
static u64 el0_sync_pass;
static u64 el0_sync_emul;
static u64 el1_sync_pass;
static u64 el1_sync_emul;

/*
 * Service the redirected vector.  The guest took an undefined-instruction
 * exception and its vector's first instruction is now our HVC, so the pending
 * fault state is intact and no guest register has been disturbed.
 */
static bool hv_handle_t8142_undef_trampoline(struct exc_info *ctx)
{
    if (!hv_windows_guest_compat_enabled())
        return false;

    if (chip_id != T8142)
        return false;
    u32 imm = ctx->esr & 0xffff;

    /*
     * A replaced access site names itself in the immediate, so it is
     * unambiguous, needs no fault state, and works wherever the guest has moved
     * the code to.
     */
    if (imm >= HV_T8142_SITE_HVC_IMM_BASE)
        return hv_handle_t8142_pmu_site(ctx, imm);

    if (imm != HV_T8142_UNDEF_HVC_IMM)
        return false;

    if (!t8142_undef_vec_count)
        return false;

    u32 insn = 0;
    u64 fault_pc = 0;
    /* PSTATE.M of the state that faulted; 0b0000 is EL0t, i.e. user mode. */
    bool from_el0 = (mrs(SPSR_EL12) & 0xf) == 0;

    if (hv_emulate_t8142_pending_undef(ctx, &insn, &fault_pc)) {
        /*
         * Rate-limited: this is the hot path once a guest spins on an absent
         * register, so log enough to prove it is being serviced and to show
         * the rate, without drowning the console.
         */
        static u64 emulated;

        if (from_el0) {
            el0_sync_emul++;
            /*
             * Always log the first few user-mode ones unrated: each is a
             * process that would otherwise have died on its own startup code,
             * and the addresses say which image it came out of.
             */
            if (el0_sync_emul <= 8)
                printf("HV: T8142: EL0 emulated 0x%08x at user pc 0x%lx (#%ld)\n", insn, fault_pc,
                       el0_sync_emul);
        } else {
            el1_sync_emul++;
            /*
             * An EL1 one means the pre-emptive image scan did not cover this
             * address -- a module that loaded after the sweep.  Say where, so a
             * recurring miss can be traced back to an image rather than just
             * absorbed silently by the vector.
             */
            if (el1_sync_emul <= 8)
                printf("HV: T8142: EL1 emulated 0x%08x at unscanned pc 0x%lx (#%ld)\n", insn,
                       fault_pc, el1_sync_emul);
        }

        if (emulated < 8 || (emulated % 4096) == 0)
            printf("HV: T8142: trampoline emulated 0x%08x at 0x%lx (#%ld)\n", insn, fault_pc,
                   emulated);
        emulated++;
        return true;
    }

    /*
     * Not an absent-register fault -- one of the guest's own exceptions
     * arriving through a vector we redirected.  Reproduce the instruction we
     * displaced and fall into the rest of the guest's handler, leaving the
     * redirect in place so the next absent-register access is still caught.
     *
     * ELR_EL2 holds the address after our HVC, so the vector being serviced is
     * the word before it; no need to guess which tracked entry this is.
     */
    u64 vec_va = ctx->elr - sizeof(u32);
    u32 orig = 0;

    for (u32 i = 0; i < t8142_undef_vec_count; i++)
        if (t8142_undef_vec[i].va == vec_va && !t8142_undef_vec[i].refused)
            orig = t8142_undef_vec[i].orig;

    if (orig && hv_replay_vector_insn(ctx, vec_va, orig, true)) {
        if (from_el0)
            el0_sync_pass++;
        else
            el1_sync_pass++;
        return true;
    }

    /*
     * Documented as unreachable, and it is not.  Measured on J813: 72166 times
     * in a single boot, always with orig == 0, at just two addresses -- neither
     * of them in the kernel image.  So this is an HVC carrying our immediate at
     * a location we have no record of displacing, and there is nothing to
     * replay.
     *
     * Restoring and rewinding is right when we really did displace the word,
     * and catastrophic otherwise: with nothing to put back, ELR lands on our
     * own HVC again and the guest spins here forever.  That livelock is what
     * produced those 72166 lines. Only rewind if something was actually
     * restored; otherwise step past and let the guest make progress, which
     * turns an unbounded spin into one diagnosable event.
     */
    static u64 lost;

    if (lost < 8 || (lost % 4096) == 0)
        printf("HV: T8142: trampoline lost vector 0x%lx (orig 0x%08x) (#%ld)\n", vec_va, orig,
               lost);
    lost++;

    if (hv_unpatch_t8142_undef_vector(vec_va))
        ctx->elr = vec_va; /* restored: re-execute the real instruction */

    return true;
}

/*
 * Report the guest's bugcheck code once it has stopped making progress.
 *
 * A bugchecked NT kernel is silent: it writes nothing to the console we have,
 * and its halt path is an infinite call to a branch-to-self stub, so from EL2
 * it is indistinguishable from any other wedge.  The stop code is the one piece
 * of information that says *why*, and the kernel has already written it to
 * KiBugCheckData before halting -- five words holding the code and its four
 * parameters.
 *
 * The offset is specific to the kernel build under test, recovered from its own
 * KeBugCheckEx prologue (adrp x8, #0xdbb000 / add x19, x8, #0x9a0 / stp x8, x23,
 * [x19]).  Wrong-but-mapped on another build, so this stays a diagnostic and
 * nothing depends on the value.
 */
#define HV_NT_KIBUGCHECKDATA_RVA 0xdbb9a0

/*
 * EPROCESS field offsets for the kernel under test, taken from its own exported
 * accessors: PsGetProcessId is `ldr x0, [x0, #0x1c0]` and
 * PsGetProcessImageFileName is `add x0, x0, #0x328`.  Build-specific, and only
 * ever used to annotate a bugcheck that already happened.
 */
#define HV_NT_EPROCESS_PID_OFF    0x1c0
#define HV_NT_EPROCESS_NAME_OFF   0x328
#define HV_NT_EPROCESS_STATUS_OFF 0x614 /* PsGetProcessExitStatus: ldr w0, [x0, #0x614] */

/*
 * Walk from the guest's KPCR to the name of the process currently on the CPU.
 * Recovered from this kernel's own accessors:
 *
 *   KeGetCurrentThread          ldr x0, [x18, #0x988]   KPCR    -> KTHREAD
 *   PsGetCurrentProcess         ldr x0, [x8,  #0xb0]    KTHREAD -> EPROCESS
 *   PsGetProcessImageFileName   add x0, x0,   #0x328    EPROCESS-> ImageFileName
 *
 * x18 is the KPCR on Windows ARM64, but only while the CPU is in kernel mode --
 * in user mode it is the TEB, so the caller must check the sampled EL first.
 */
#define HV_NT_KPCR_CURRENT_THREAD_OFF 0x988
#define HV_NT_KTHREAD_PROCESS_OFF     0xb0

/*
 * Guest WFIs held at EL2 -- see hv_handle_wfx().  A per-CPU count says whether
 * every core reached the idle loop and how hard it is idling, which is the
 * difference between "this core is waiting" and "this core is gone".
 */
static u64 wfi_trap_count[MAX_CPUS];
static bool wfi_trap_logged;

static void hv_report_t8142_wfi_traps(void)
{
    printf("HV: T8142: guest WFIs held at EL2:");
    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        if (cpu > 0 && !smp_is_alive(cpu))
            continue;
        printf(" cpu%d=%ld", cpu, wfi_trap_count[cpu]);
    }
    printf("\n");
}

static void hv_report_t8142_el0_vector(void)
{
    if (!el0_sync_pass && !el0_sync_emul && !el1_sync_pass && !el1_sync_emul)
        return;

    printf("HV: T8142: redirected sync vectors: EL0 %ld emulated / %ld passed, "
           "EL1 %ld emulated / %ld passed\n",
           el0_sync_emul, el0_sync_pass, el1_sync_emul, el1_sync_pass);
}

/*
 * Bugcheck 0xEF (CRITICAL_PROCESS_DIED) passes the EPROCESS of the process that
 * died as its first parameter.  Which one it is separates "Windows is broken"
 * from "one component is missing", so resolve it here rather than leaving a
 * bare pointer in the log.
 */
/*
 * Name every process seen running, once each.
 *
 * A guest that stalls without bugchecking says nothing about how far user mode
 * actually got, and "WinPE is idle" and "WinPE is idle inside wpeinit" are
 * completely different problems.  Sampling the current process on the tick and
 * printing only names not seen before turns the stall into a trace of what ran:
 * smss, csrss, wininit, winpeshl, wpeinit, setup.  Where the list stops is the
 * thing that did not finish.
 */
static void hv_report_t8142_current_process(struct exc_info *ctx)
{
    static char seen[24][16];
    static u32 seen_count;

    if (chip_id != T8142 || !t8142_kernel_base)
        return;

    /* x18 is only the KPCR in kernel mode; in user mode it is the TEB. */
    if ((ctx->spsr & 0xf) == 0)
        return;

    u64 kpcr = ctx->regs[18];
    u64 thread, process;

    if (kpcr < 0xffff000000000000UL)
        return;
    if (!hv_read_guest64(kpcr + HV_NT_KPCR_CURRENT_THREAD_OFF, &thread) ||
        thread < 0xffff000000000000UL)
        return;
    if (!hv_read_guest64(thread + HV_NT_KTHREAD_PROCESS_OFF, &process) ||
        process < 0xffff000000000000UL)
        return;

    u64 pa = hv_translate(process, false, false, NULL);
    if (!pa)
        return;

    char name[16] = {0};
    for (int i = 0; i < 15; i++) {
        char c = (char)read8(pa + HV_NT_EPROCESS_NAME_OFF + i);
        if (c < 0x20 || c > 0x7e)
            break;
        name[i] = c;
    }
    if (!name[0])
        return;

    for (u32 i = 0; i < seen_count; i++) {
        u32 j = 0;
        while (j < 15 && seen[i][j] == name[j] && name[j])
            j++;
        if (seen[i][j] == name[j])
            return; /* already reported */
    }
    if (seen_count >= ARRAY_SIZE(seen))
        return;

    for (u32 j = 0; j < 16; j++)
        seen[seen_count][j] = name[j];
    seen_count++;

    printf("HV: T8142: guest process now running: \"%s\" (pid %ld, EPROCESS 0x%lx)\n", name,
           read64(pa + HV_NT_EPROCESS_PID_OFF), process);
}

static void hv_report_t8142_dead_process(u64 code, u64 param1)
{
    if (code != 0xef || !param1)
        return;

    u64 pa = hv_translate(param1, false, false, NULL);
    if (!pa)
        return;

    char name[16] = {0};
    for (int i = 0; i < 15; i++) {
        char c = (char)read8(pa + HV_NT_EPROCESS_NAME_OFF + i);
        if (c < 0x20 || c > 0x7e)
            break;
        name[i] = c;
    }

    printf("HV: T8142:   dead process: pid %ld \"%s\" exit status 0x%08x (EPROCESS 0x%lx)\n",
           read64(pa + HV_NT_EPROCESS_PID_OFF), name, read32(pa + HV_NT_EPROCESS_STATUS_OFF),
           param1);
}

static void hv_report_t8142_bugcheck(struct exc_info *ctx)
{
    static u64 prev_pc;
    static u32 stuck;
    static bool reported;

    if (reported || !t8142_kernel_base)
        return;

    if (ctx->elr != prev_pc) {
        prev_pc = ctx->elr;
        stuck = 0;
        return;
    }
    if (++stuck < 3) /* a few ticks at the same PC, not a momentary sample */
        return;

    u64 pa = hv_translate(t8142_kernel_base + HV_NT_KIBUGCHECKDATA_RVA, false, false, NULL);
    if (!pa)
        return;

    u64 code = read64(pa);
    if (!code) /* halted somewhere that is not a bugcheck */
        return;

    printf("HV: T8142: guest bugcheck 0x%lx (%lx, %lx, %lx, %lx) at pc 0x%lx (rva 0x%lx)\n", code,
           read64(pa + 8), read64(pa + 16), read64(pa + 24), read64(pa + 32), ctx->elr,
           ctx->elr - t8142_kernel_base);
    hv_report_t8142_dead_process(code, read64(pa + 8));
    reported = true;
}

bool hv_recover_t8142_sysreg_undef(struct exc_info *ctx)
{
    if (!hv_windows_guest_compat_enabled())
        return false;

    hv_report_t8142_bugcheck(ctx);

    u32 insn = 0;
    u64 fault_pc = 0;

    if (chip_id != T8142)
        return false;

    /*
     * Gate on the guest being demonstrably stuck, not merely on it being
     * inside an exception: ESR_EL12 still reads the last fault long after a
     * handler has dealt with it, so acting on that alone would corrupt a
     * healthy guest.  A PC sitting on a branch-to-self is exact.
     */
    u64 pc_pa = hv_translate(ctx->elr, false, false, NULL);
    if (!pc_pa || read32(pc_pa) != 0x14000000) /* B .   (branch to self) */
        return false;

    if (!hv_emulate_t8142_pending_undef(ctx, &insn, &fault_pc))
        return false;

    printf("HV: T8142: recovered guest from undefined sysreg access 0x%08x at 0x%lx\n", insn,
           fault_pc);

    /*
     * Replace the access that faulted, so this site never reaches the guest's
     * vector again.  This is the direct fix; the vector redirect below is the
     * general one, and on a guest where it does not take effect this is what
     * makes progress.
     */
    hv_patch_t8142_pmu_site(fault_pc, insn);

    /*
     * Patch the rest of whatever image that site lives in, too.
     *
     * The pre-emptive scan only covers the image the guest's vectors belong to,
     * which for NT is ntoskrnl and nothing else.  A boot driver carrying its own
     * absent-register access therefore arrives here unpatched, and fixing the
     * single faulting instruction leaves every other one in that module waiting
     * to do the same again -- each costing a recovery the guest may not survive.
     *
     * Doing it here rather than in the patcher is deliberate: this path only
     * runs for a site the scan had not already replaced, so the expensive walk
     * back to the PE header is paid once per module and never on the hot path.
     * The scanner dedupes by image base, so a repeat is harmless.
     */
    if (fault_pc >= 0xffff000000000000UL)
        hv_scan_guest_image_absent_regs(fault_pc);

    /*
     * Only reachable when the guest wedged before we had its vector, so take
     * this chance to redirect it and make the tick-rate path unnecessary from
     * here on.  Use the PSTATE the guest faulted in, not its current one: the
     * exception put it on the handler's stack, which may not be the stack the
     * faulting code was using.
     */
    hv_patch_t8142_undef_vector(HV_VECTOR_SYNC_SLOT(mrs(SPSR_EL12)));
    return true;
}

/*
 * Keep the redirect pointed at whatever vector the guest is currently using.
 *
 * A guest that faults in a loop rather than halting never trips the wedge
 * detector, so waiting for a hang is not enough: the NT kernel installs its own
 * vectors and then spins on PMCCNTR_EL0, faulting continuously without ever
 * parking.  Re-evaluating each tick also covers the guest switching tables or
 * stacks, which happens at every handoff -- firmware to boot manager to kernel.
 */
/*
 * Scan every image the NT kernel has loaded, not just the one its vectors live
 * in.
 *
 * The pre-emptive scan is keyed off VBAR, so it covers ntoskrnl and nothing
 * else.  A boot driver carrying its own absent-register access is therefore
 * still live, and on this SoC that is fatal rather than recoverable: measured
 * as bugcheck 0x1E reporting instruction 0xd53b9d0b -- "mrs x11, PMCCNTR_EL0"
 * -- at an address far below the ntoskrnl base.  There is no way to intercept
 * it after the fact, because an absent-register UNDEF is delivered straight to
 * EL1 and the one place it could have been caught, the guest's own vector,
 * cannot be touched without causing 0x2B (see hv_track_t8142_undef_vector).
 *
 * So enumerate the images instead.  PsLoadedModuleList is an exported symbol
 * whose RVA is read out of the very ntoskrnl this platform boots, the same way
 * KiBugCheckData's was -- and reading both from the export directory
 * reproduced the KiBugCheckData value already in use here, which is what
 * makes the method trustworthy rather than a guess.
 *
 * The RVA is still specific to one kernel build, so it is not taken on faith:
 * the first entry of that list is always ntoskrnl itself, and the walk is
 * abandoned unless its DllBase is the kernel base already established
 * independently.  A different build fails that check and simply gets no module
 * scanning, rather than having arbitrary guest memory interpreted as a list.
 */
#define HV_NT_PSLOADEDMODULELIST_RVA 0xd6a7e0
#define HV_NT_LDR_DLLBASE            0x30
#define HV_NT_MAX_MODULES            256

/*
 * Find the other images the boot loader placed in kernel space, by looking for
 * them rather than by asking the kernel.
 *
 * Asking was tried first and cannot work this early: PsLoadedModuleList reads
 * as an empty list -- Flink 0 -- at the point the fault happens, because NT
 * populates it well after HAL initialization.  The module that faults is
 * already resident and already executing by then, so any list-based scheme is
 * structurally too late for it.
 *
 * Windows images are 64K-aligned in kernel space, so a stride of that size over
 * the region around the kernel finds every one of them.  Each candidate must
 * present MZ, a sane e_lfanew and PE\0\0 before it is scanned, and the scanner
 * only rewrites words inside executable sections, so a stale header left in
 * memory costs a few reads and changes nothing.  Unmapped probes simply fail to
 * translate.
 *
 * This is deliberately independent of any kernel build: no symbol, no RVA, no
 * structure layout.  The measured driver sat about 110MB below the kernel base,
 * hence a radius rather than a scan upward only.
 */
#define HV_PE_SWEEP_RADIUS (512UL << 20)
#define HV_PE_SWEEP_STEP   0x10000UL

static void hv_sweep_guest_images(u64 anchor)
{
    if (!hv_windows_guest_compat_enabled())
        return;

    u64 lo = (anchor - HV_PE_SWEEP_RADIUS) & ~(HV_PE_SWEEP_STEP - 1);
    u64 hi = anchor + HV_PE_SWEEP_RADIUS;
    u32 probes = 0, found = 0;

    for (u64 page = lo; page < hi; page += HV_PE_SWEEP_STEP, probes++) {
        u32 mz, lfanew, sig;

        if ((probes & 0xff) == 0)
            hv_wdt_pet();

        if (!hv_read_guest32(page, &mz) || (mz & 0xffff) != 0x5a4d)
            continue;
        if (!hv_read_guest32(page + 0x3c, &lfanew) || lfanew < 0x40 || lfanew > 0x1000)
            continue;
        if (!hv_read_guest32(page + lfanew, &sig) || sig != 0x00004550)
            continue;

        found++;
        hv_scan_guest_image_absent_regs(page);
    }

    printf("HV: T8142: swept 0x%lx..0x%lx for guest images, found %u\n", lo, hi, found);
}

static void hv_scan_guest_modules(void)
{
    if (!hv_windows_guest_compat_enabled())
        return;

    if (chip_id != T8142 || !t8142_kernel_base)
        return;

    u64 head = t8142_kernel_base + HV_NT_PSLOADEDMODULELIST_RVA;
    u64 entry;
    static bool reported;

    if (!hv_read_guest64(head, &entry) || !entry || entry == head) {
        if (!reported) {
            reported = true;
            printf("HV: T8142: module list head 0x%lx unreadable or empty "
                   "(flink 0x%lx)\n",
                   head, entry);
        }
        return;
    }

    u64 first_base;
    if (!hv_read_guest64(entry + HV_NT_LDR_DLLBASE, &first_base) ||
        first_base != t8142_kernel_base) {
        if (!reported) {
            reported = true;
            printf("HV: T8142: module list at 0x%lx rejected: first entry 0x%lx "
                   "DllBase 0x%lx != kernel 0x%lx\n",
                   head, entry, first_base, t8142_kernel_base);
        }
        return; /* not the list we think it is -- do nothing */
    }

    if (!reported) {
        reported = true;
        printf("HV: T8142: walking guest module list at 0x%lx\n", head);
    }

    for (u32 i = 0; i < HV_NT_MAX_MODULES && entry && entry != head; i++) {
        u64 base;

        hv_wdt_pet();

        if (!hv_read_guest64(entry + HV_NT_LDR_DLLBASE, &base))
            break;
        if (base >= 0xffff000000000000UL)
            hv_scan_guest_image_absent_regs(base);
        if (!hv_read_guest64(entry, &entry)) /* InLoadOrderLinks.Flink */
            break;
    }
}

/*
 * Report emulated interrupt traffic, but only when it changes.
 *
 * The per-CPU one-shot lines answer "did this ever work"; they cannot answer
 * "is it still working", and that is the question that separates a regression
 * in this delivery path from the guest waiting on a device m1n1 does not yet
 * bridge.  A guest spinning on a lock looks identical either way from the
 * outside.
 *
 * Printing only on change keeps a healthy system quiet -- and quiet is itself
 * the signal, because a clock that has stopped stops moving these counters.
 */
/*
 * Look for a bugcheck without needing the sampled CPU to be the one that took
 * it.
 *
 * hv_report_t8142_bugcheck() requires the tick to land on a core parked at the
 * same PC three times running.  That was fine while the guest was
 * single-threaded, and is useless now: with two cores healthy and ticking at
 * 1kHz, a bugcheck on any of the other seven never satisfies it.  KiBugCheckData
 * is global, so read it directly instead.
 */
void hv_check_t8142_bugcheck(void)
{
    static bool reported;

    if (chip_id != T8142 || reported || !t8142_kernel_base)
        return;

    u64 pa = hv_translate(t8142_kernel_base + HV_NT_KIBUGCHECKDATA_RVA, false, false, NULL);
    if (!pa)
        return;

    u64 code = read64(pa);
    if (!code)
        return;

    reported = true;
    printf("HV: T8142: guest bugcheck 0x%lx (%lx, %lx, %lx, %lx) seen on tick\n", code,
           read64(pa + 8), read64(pa + 16), read64(pa + 24), read64(pa + 32));
#if defined(ENABLE_VGIC_MODULE) && defined(ENABLE_NATIVE_AIC_PASSTHROUGH)
    if (hv_native_aic_windows_active()) {
        u32 cpu = smp_id();
        u64 hcr = mrs(HCR_EL2);
        u8 pmr = (mrs(ICH_VMCR_EL2) >> 24) & 0xff;
        virq_t pending = {.vintid = 0x3ff, .priority = 0xff};
        enum hv_carrier_queue queue = hv_carrier_select_pending(&pending);

        printf("HV: native-aic: carrier-gate cpu=%u local=%d timer=%d "
               "stack=%d proven=%d active=%d igrpen1=%d pmr=0x%x "
               "queue=%d intid=%u prio=0x%x fiq=%lu/%lu refl=%d/%d "
               "VI=%d\n",
               cpu, native_aic_local_unit_ready[cpu], PERCPU(carrier_timer_ready),
               PERCPU(carrier_stack_ready), PERCPU(carrier_delivery_proven),
               PERCPU(carrier_irq_active), !!hv_vgic3_get_igrpen1(), pmr, queue, pending.vintid,
               pending.priority, (unsigned long)PERCPU(timer_p_fiq_count),
               (unsigned long)PERCPU(timer_v_fiq_count), PERCPU(timer_p_reflection_pending),
               PERCPU(timer_v_reflection_pending), !!(hcr & HCR_VI));
        hv_native_aic_trace_dump();
    }
#endif
    hv_report_t8142_dead_process(code, read64(pa + 8));
    hv_report_t8142_wfi_traps();
    hv_report_t8142_el0_vector();
}

void hv_report_t8142_gic_activity(void)
{
    static u32 last_ack[MAX_CPUS];
    static u32 last_eoi[MAX_CPUS];
    bool changed = false;

    if (chip_id != T8142)
        return;

    for (u32 cpu = 0; cpu < MAX_CPUS; cpu++) {
        if (gic_cpuif_ack_count[cpu] != last_ack[cpu] ||
            gic_cpuif_eoi_count[cpu] != last_eoi[cpu]) {
            changed = true;
            break;
        }
    }

    if (!changed)
        return;

    printf("HV: T8142: emulated IRQ traffic (ack/eoi):");
    for (u32 cpu = 0; cpu < MAX_CPUS; cpu++) {
        if (!gic_cpuif_ack_count[cpu] && !gic_cpuif_eoi_count[cpu])
            continue;
        printf(" cpu%d=%u/%u", (int)cpu, gic_cpuif_ack_count[cpu], gic_cpuif_eoi_count[cpu]);
        /*
         * A core whose acknowledge count runs one ahead of its completions is
         * sitting inside a handler it never finished, which also makes it deaf
         * to everything after -- the active bit blocks further delivery.  Say
         * which interrupt, because "stuck in the clock" and "stuck in an IPI"
         * point at completely different faults.
         */
        int stuck = hv_gic_bitmap_first_set(gic_cpuif[cpu].active);
        if (stuck >= 0)
            printf("(act=%d)", stuck);
        last_ack[cpu] = gic_cpuif_ack_count[cpu];
        last_eoi[cpu] = gic_cpuif_eoi_count[cpu];
    }
    printf("\n");

    /*
     * Periodically, the counters that used to appear only on the bugcheck path.
     *
     * A guest that stalls without bugchecking reports nothing there -- which is
     * exactly what WinPE does when it is waiting on something -- so the two
     * questions that matter during a stall had no answer: is user mode still
     * making progress, and are the quiet cores quiet by choice?
     *
     * The per-CPU WFI counts settle the second one on their own.  A core parked
     * in the EL2 wait with a *frozen* count is stuck in a single WFI that
     * nothing is waking; a core whose count keeps climbing is waking, finding
     * no work, and idling again, which is Windows parking it and not a bug.
     * Cores 0-5 going silent together while 6-9 stay busy is currently
     * consistent with both.
     */
    static u32 periodic;

    if ((periodic++ % 32) == 0) {
        hv_report_t8142_wfi_traps();
        hv_report_t8142_el0_vector();
    }
}

void hv_scan_t8142_guest_modules(void)
{
    hv_scan_guest_modules();
}

void hv_report_t8142_process(struct exc_info *ctx)
{
    hv_report_t8142_current_process(ctx);
}

void hv_track_t8142_undef_vector(struct exc_info *ctx)
{
    if (!hv_windows_guest_compat_enabled())
        return;

    UNUSED(ctx);

    if (chip_id != T8142)
        return;

    /*
     * This runs on every synchronous exception from the guest, so the common
     * case -- the guest is using the same vector table it used last time -- has
     * to cost a register read and a compare, nothing more.  Anything that walks
     * page tables or touches guest memory per trap is far too expensive to sit
     * here; that is a measured statement, not a precaution.
     */
    static u64 last_vbar = ~0UL;
    u64 vbar = mrs(VBAR_EL12);

    if (vbar == last_vbar)
        return;
    last_vbar = vbar;

    /*
     * A kernel-range vector base means the guest is no longer firmware, and
     * firmware is the only guest that survives discovering these lazily -- it
     * services the fault itself.  Scan the image the vectors belong to before
     * it can execute an access we would then have to recover from.  Restricted
     * to the kernel range deliberately: UEFI and the boot manager already work
     * through the vector redirect, and this is a much larger intervention.
     */
    if (vbar >= 0xffff000000000000UL) {
        hv_scan_guest_image_absent_regs(vbar);

        /*
         * The kernel's own current-EL slots stay untouched, but not for the
         * reason recorded here previously.
         *
         * NT's table was disassembled to settle it.  Slot +0x000 is the live
         * synchronous EL1t vector and opens "mrs x18, sp_el0 / and sp, x18,
         * #~0xf" -- it recovers the interrupted kernel stack and installs it.
         * Redirecting that slot used to produce bugcheck 0x2B, which was
         * attributed to a virtual IRQ taken from EL1h landing in +0x280's
         * hard-wired PANIC_STACK_SWITCH branch.  That explanation was wrong:
         * the replay table listed 0xd5384100 as SP_EL1 when it is SP_EL0, so
         * m1n1 displaced the mrs and handed x18 the value of SP_EL1.  NT then
         * ran on a stack that was never one and faulted through it until it
         * reached PANIC_STACK_SWITCH by itself.  See hv_replay_vector_insn().
         *
         * With that fixed, +0x000 is redirected, because the pre-emptive image
         * scan cannot be relied on alone: it is a race it sometimes loses.  A
         * driver that loads after the sweep and reads an absent register before
         * the next module walk finds it takes the UNDEF for real -- measured as
         * bugcheck 0x1E with param4 0xd53b9d08, the "mrs x8, pmccntr_el0" word
         * itself, at an address 0x48 million bytes above the ntoskrnl base,
         * i.e. in a module the sweep had never seen.
         *
         * The old objection to lazy discovery does not apply to a redirect.  It
         * was that NT's undefined-instruction path ends in a bugcheck, so the
         * first access to reach it is already fatal -- true, but the whole point
         * of the redirect is that the access never reaches that path: the HVC is
         * the vector's first instruction, so EL2 sees the fault before any of
         * NT's handler has run.  The scan stays as the cheaper path for images
         * it does catch; the vector is what makes coverage total.
         *
         * +0x400 is the other half and must be redirected.  It is the slot
         * every synchronous exception *from EL0* takes, and user-mode absent-
         * register accesses cannot be reached any other way -- HVC is UNDEFINED
         * at EL0, so an instruction-site replacement has nothing to replace the
         * instruction with, and the images are demand-paged from per-process
         * ASLR bases the module walk does not cover.  Without this, the first
         * user-mode process dies on its own startup code: smss.exe's single
         * "mrs x8, pmccntr_el0" (from the /GS cookie init every MSVC ARM64
         * binary carries) UNDEFs, NT raises STATUS_ILLEGAL_INSTRUCTION, and the
         * session manager exiting is bugcheck 0xC000021A.
         */
        if (t8142_kernel_base) {
            static bool swept;

            if (!swept) {
                swept = true;
                hv_sweep_guest_images(t8142_kernel_base);
            }
            hv_patch_t8142_undef_vector(HV_VECTOR_SYNC_CURRENT_SP0);
            hv_patch_t8142_undef_vector(HV_VECTOR_SYNC_LOWER_A64);
            return;
        }
    }

    /*
     * Redirect both synchronous "current EL" slots, not just the one matching
     * the guest's PSTATE right now: which slot a future fault uses depends on
     * the stack mode it faults in, which we cannot know ahead of time.  The NT
     * kernel runs EL1t and the boot manager EL1h, and either can take the
     * fault we are here to intercept.
     */
    hv_patch_t8142_undef_vector(HV_VECTOR_SYNC_CURRENT_SP0);
    hv_patch_t8142_undef_vector(HV_VECTOR_SYNC_CURRENT_SPX);
}

static bool hv_handle_msr_unlocked(struct exc_info *ctx, u64 iss)
{
    u64 reg = iss & (ESR_ISS_MSR_OP0 | ESR_ISS_MSR_OP2 | ESR_ISS_MSR_OP1 | ESR_ISS_MSR_CRn |
                     ESR_ISS_MSR_CRm);
    u64 rt = FIELD_GET(ESR_ISS_MSR_Rt, iss);
    bool is_read = iss & ESR_ISS_MSR_DIR;

    u64 *regs = ctx->regs;

    regs[31] = 0;

    if (hv_handle_software_pmu(reg, is_read, rt, regs))
        return true;

    /*
     * Direct TALL1 traps and pre-rewritten HVC sites must reach the same
     * T8142 CPU-interface owner. Otherwise a direct EOIR can complete a
     * different state machine from the IAR that supplied its INTID.
     */
    if (hv_handle_t8142_gic_cpuif(reg, is_read, rt, regs, false))
        return true;

    if (hv_handle_native_aic_carrier_cpuif(reg, is_read, rt, regs, false))
        return true;

    switch (reg) {
        SYSREG_APPLE_PASS(SYS_IMP_APL_CORE_NRG_ACC_DAT);
        SYSREG_APPLE_PASS(SYS_IMP_APL_CORE_SRM_NRG_ACC_DAT);
        case SYSREG_ISS(sys_reg(3, 3, 14, 0, 1)): /* CNTPCT_EL0 */
            if (!is_read || chip_id != T8142)
                return false;
            /*
             * T8142's standard physical and virtual counter views remain frozen
             * while an EL1 exception is active.  Apple's ACNTVCT_EL0 alias is
             * monotonic at the same CNTFRQ in that context, so use it as the
             * timebase for the trapped physical-counter read.  Mu and bootmgfw
             * only depend on monotonic deltas here.
             */
            regs[rt] = mrs(SYS_IMP_APL_CNTVCT_ALIAS_EL0);
            return true;
            /* Architectural timer, for ECV */
#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
#define SYSREG_MAP_REFLECT(sr, to, physical, control)                                              \
    case SYSREG_ISS(sr):                                                                           \
        if (is_read) {                                                                             \
            regs[rt] = _mrs(sr_tkn(to));                                                           \
        } else {                                                                                   \
            _msr(sr_tkn(to), regs[rt]);                                                            \
            hv_timer_reflect_guest_rearm(physical, control, regs[rt], regs[18] != 0);              \
        }                                                                                          \
        return true;

            SYSREG_MAP_REFLECT(SYS_CNTV_CTL_EL0, SYS_CNTV_CTL_EL02, false, true)
            SYSREG_MAP_REFLECT(SYS_CNTV_CVAL_EL0, SYS_CNTV_CVAL_EL02, false, false)
            SYSREG_MAP_REFLECT(SYS_CNTV_TVAL_EL0, SYS_CNTV_TVAL_EL02, false, false)
            SYSREG_MAP_REFLECT(SYS_CNTP_CTL_EL0, SYS_CNTP_CTL_EL02, true, true)
            SYSREG_MAP_REFLECT(SYS_CNTP_CVAL_EL0, SYS_CNTP_CVAL_EL02, true, false)
            SYSREG_MAP_REFLECT(SYS_CNTP_TVAL_EL0, SYS_CNTP_TVAL_EL02, true, false)
#undef SYSREG_MAP_REFLECT
#else
            SYSREG_MAP(SYS_CNTV_CTL_EL0, SYS_CNTV_CTL_EL02)
            SYSREG_MAP(SYS_CNTV_CVAL_EL0, SYS_CNTV_CVAL_EL02)
            SYSREG_MAP(SYS_CNTV_TVAL_EL0, SYS_CNTV_TVAL_EL02)
            SYSREG_MAP(SYS_CNTP_CTL_EL0, SYS_CNTP_CTL_EL02)
            SYSREG_MAP(SYS_CNTP_CVAL_EL0, SYS_CNTP_CVAL_EL02)
            SYSREG_MAP(SYS_CNTP_TVAL_EL0, SYS_CNTP_TVAL_EL02)
#endif
            /* Spammy stuff seen on t600x p-cores */
            /* These are PMU/PMC registers */
            SYSREG_APPLE_PASS(sys_reg(3, 2, 15, 12, 0));
            SYSREG_APPLE_PASS(sys_reg(3, 2, 15, 13, 0));
            SYSREG_APPLE_PASS(sys_reg(3, 2, 15, 14, 0));
            SYSREG_APPLE_PASS(sys_reg(3, 2, 15, 15, 0));
            SYSREG_APPLE_PASS(sys_reg(3, 1, 15, 7, 0));
            SYSREG_APPLE_PASS(sys_reg(3, 1, 15, 8, 0));
            SYSREG_APPLE_PASS(sys_reg(3, 1, 15, 9, 0));
            SYSREG_APPLE_PASS(sys_reg(3, 1, 15, 10, 0));
            /* Noisy traps */
            SYSREG_APPLE_PASS(SYS_IMP_APL_HID4)
            SYSREG_APPLE_PASS(SYS_IMP_APL_EHID4)
            /* We don't normally trap these, but if we do, they're noisy */
            SYSREG_APPLE_PASS(SYS_IMP_APL_GXF_STATUS_EL1)
            SYSREG_PASS(SYS_IMP_APL_CNTVCT_ALIAS_EL0)
            SYSREG_APPLE_PASS(SYS_IMP_APL_TPIDR_GL1)
            SYSREG_APPLE_MAP(SYS_IMP_APL_SPSR_GL1, SYS_IMP_APL_SPSR_GL12)
            SYSREG_APPLE_MAP(SYS_IMP_APL_ASPSR_GL1, SYS_IMP_APL_ASPSR_GL12)
            SYSREG_APPLE_MAP(SYS_IMP_APL_ELR_GL1, SYS_IMP_APL_ELR_GL12)
            SYSREG_APPLE_MAP(SYS_IMP_APL_ESR_GL1, SYS_IMP_APL_ESR_GL12)
            SYSREG_APPLE_MAP(SYS_IMP_APL_SPRR_PERM_EL1, SYS_IMP_APL_SPRR_PERM_EL12)
            SYSREG_APPLE_MAP(SYS_IMP_APL_APCTL_EL1, SYS_IMP_APL_APCTL_EL12)
            SYSREG_APPLE_MAP(SYS_IMP_APL_AMX_CTL_EL1, SYS_IMP_APL_AMX_CTL_EL12)
            /* FIXME:Might be wrong */
            SYSREG_APPLE_PASS(SYS_IMP_APL_AMX_STATE_T)
            /* pass through PMU handling */
            SYSREG_APPLE_PASS(SYS_IMP_APL_PMCR1)
            SYSREG_APPLE_PASS(SYS_IMP_APL_PMCR2)
            SYSREG_APPLE_PASS(SYS_IMP_APL_PMCR3)
            SYSREG_APPLE_PASS(SYS_IMP_APL_PMCR4)
            SYSREG_APPLE_PASS(SYS_IMP_APL_PMESR0)
            SYSREG_APPLE_PASS(SYS_IMP_APL_PMESR1)
            SYSREG_APPLE_PASS(SYS_IMP_APL_PMSR)
#ifndef DEBUG_PMU_IRQ
            SYSREG_APPLE_PASS(SYS_IMP_APL_PMC0)
#endif
            SYSREG_APPLE_PASS(SYS_IMP_APL_PMC1)
            SYSREG_APPLE_PASS(SYS_IMP_APL_PMC2)
            SYSREG_APPLE_PASS(SYS_IMP_APL_PMC3)
            SYSREG_APPLE_PASS(SYS_IMP_APL_PMC4)
            SYSREG_APPLE_PASS(SYS_IMP_APL_PMC5)
            SYSREG_APPLE_PASS(SYS_IMP_APL_PMC6)
            SYSREG_APPLE_PASS(SYS_IMP_APL_PMC7)
            SYSREG_APPLE_PASS(SYS_IMP_APL_PMC8)
            SYSREG_APPLE_PASS(SYS_IMP_APL_PMC9)

            // spammy ntoskrnl regs
            SYSREG_APPLE_PASS(SYS_IMP_APL_L2C_ERR_STS)

            SYSREG_PASS(sys_reg(2, 0, 0, 1, 4))
            SYSREG_PASS(sys_reg(2, 0, 0, 1, 5))
            SYSREG_PASS(sys_reg(2, 0, 0, 1, 6))
            SYSREG_PASS(sys_reg(2, 0, 0, 1, 7))

            SYSREG_PASS(sys_reg(2, 0, 0, 2, 4))
            SYSREG_PASS(sys_reg(2, 0, 0, 2, 5))
            SYSREG_PASS(sys_reg(2, 0, 0, 2, 6))
            SYSREG_PASS(sys_reg(2, 0, 0, 2, 7))

            SYSREG_PASS(sys_reg(2, 0, 0, 3, 4))
            SYSREG_PASS(sys_reg(2, 0, 0, 3, 5))
            SYSREG_PASS(sys_reg(2, 0, 0, 3, 6))
            SYSREG_PASS(sys_reg(2, 0, 0, 3, 7))

            SYSREG_PASS(sys_reg(2, 0, 0, 4, 4))
            SYSREG_PASS(sys_reg(2, 0, 0, 4, 5))

            SYSREG_PASS(sys_reg(2, 0, 0, 5, 4))
            SYSREG_PASS(sys_reg(2, 0, 0, 5, 5))

            // these seem debugging related but looks like windbg/m1n1 debugger still works
            SYSREG_PASS(sys_reg(2, 0, 0, 0, 4))
            SYSREG_PASS(sys_reg(2, 0, 0, 0, 5))
            SYSREG_PASS(sys_reg(2, 0, 0, 0, 6))
            SYSREG_PASS(sys_reg(2, 0, 0, 0, 7))
            SYSREG_PASS(sys_reg(2, 0, 1, 1, 4))

#ifdef ENABLE_VGIC_MODULE
        /* Apple cores need these software GICv3 CPU-interface registers. */
        case SYSREG_ISS(ICC_SRE_EL1):
            if (is_read)
                regs[rt] = ICC_SRE_SRE;
            return true;
        case SYSREG_ISS(ICC_PMR_EL1):
            if (is_read)
                regs[rt] = PERCPU(vgic_pmr);
            else
                PERCPU(vgic_pmr) = regs[rt];
            return true;
        case SYSREG_ISS(ICC_CTLR_EL1):
        case SYSREG_ISS(ICC_IGRPEN0_EL1):
            if (is_read)
                regs[rt] = 0;
            return true;
#endif

#if defined(ENABLE_VGIC_MODULE) && !defined(ENABLE_NATIVE_AIC_PASSTHROUGH)
        case SYSREG_ISS(ICC_IAR1_EL1):
            if (is_read)
                regs[rt] = hv_vgic3_do_iar1();
            return true;
        case SYSREG_ISS(ICC_IGRPEN1_EL1):
            if (is_read)
                regs[rt] = hv_vgic3_get_igrpen1();
            else
                hv_vgic3_set_igrpen1(regs[rt]);
            return true;
        case SYSREG_ISS(ICC_BPR1_EL1):
            if (is_read)
                regs[rt] = 0;
            return true;
        case SYSREG_ISS(ICC_EOIR1_EL1):
            if (is_read) {
                regs[rt] = 0;
            } else {
                u32 eoi_intid = (u32)(regs[rt] & GENMASK(23, 0));

                hv_vgic3_do_eoir1(regs[rt]);
                /*
                 * regs[rt] is a guest-written register: unbounded, and a
                 * guest INTID rather than a physical line.  Passing it
                 * straight to aic_set_mask() was an arbitrary MMIO write --
                 * that function derives `die = irq / max_irq` on a signed
                 * int, so a large or negative value walks the address
                 * outside the AIC aperture and bus-errors.  Mask the INTID
                 * field, translate it, and only touch a line we own.
                 */
                if (eoi_intid >= 32 && hv_aic_alias_is_owned(eoi_intid))
                    aic_set_mask(hv_aic_alias_to_physical(eoi_intid), false);
            }
            return true;
#endif

#if defined(ENABLE_VGIC_MODULE) && defined(ENABLE_NATIVE_AIC_PASSTHROUGH)
        /*
         * The Apple cores implement the GIC virtualization registers at EL2 but
         * not a directly usable ICC_* interface at EL1.  During Windows' short
         * startup-carrier phase, ICH_HCR_EL2.TALL1 routes these accesses here so
         * they do not become illegal instructions.  No LR receives an interrupt;
         * this is register compatibility only, until Windows enables AIC2.
         */
        case SYSREG_ISS(ICC_IAR1_EL1):
            if (is_read) {
                if (hv_native_aic_windows_active() && !hv_native_aic_windows_ready() &&
                    !PERCPU(carrier_stack_ready) && !PERCPU(carrier_delivery_proven)) {
                    /*
                     * Before the BSP's KPCR and exception stacks are proven,
                     * fail closed. After that proof, x18 is not a valid gate:
                     * Windows' hardened vector saves and clears it before the
                     * trapped ICC_IAR1_EL1 read, then restores it on return.
                     */
                    regs[rt] = 0x3ff;
                } else {
                    regs[rt] = hv_carrier_do_iar1();
                }
                if (regs[rt] != 0x3ff) {
                    u32 count = ++PERCPU(carrier_iar_count);
                    if (count <= 8)
                        hv_native_aic_trace_record(HV_NATIVE_AIC_TRACE_CARRIER_IAR,
                                                   regs[rt] | ((u64)count << 32), regs[18]);
                }
            }
            return true;
        case SYSREG_ISS(ICC_IGRPEN1_EL1):
            if (is_read) {
                regs[rt] = hv_vgic3_get_igrpen1();
            } else {
                hv_vgic3_set_igrpen1(regs[rt]);
            }
            hv_native_aic_trace_record(HV_NATIVE_AIC_TRACE_CARRIER_SYSREG, is_read ? 0 : BIT(32),
                                       regs[rt]);
            return true;
        case SYSREG_ISS(ICC_BPR1_EL1):
            if (is_read) {
                regs[rt] = 0;
            }
            hv_native_aic_trace_record(HV_NATIVE_AIC_TRACE_CARRIER_SYSREG,
                                       1 | (is_read ? 0 : BIT(32)), regs[rt]);
            return true;
        case SYSREG_ISS(ICC_EOIR1_EL1):
            if (is_read) {
                regs[rt] = 0;
            } else {
                hv_carrier_do_eoir1(regs[rt] & ICH_LR_VIRTUAL_MASK);
                u32 count = ++PERCPU(carrier_eoi_count);
                if (count <= 8)
                    hv_native_aic_trace_record(HV_NATIVE_AIC_TRACE_CARRIER_EOI,
                                               regs[rt] & ICH_LR_VIRTUAL_MASK, count);
            }
            return true;
#endif

#ifdef ENABLE_VGIC_MODULE
        //
        // windows-native-aic: this trap-and-emulate is kept exactly as-is (it's a
        // synchronous sysreg trap, unrelated to HCR_EL2.IMO/FMO physical-interrupt
        // routing, so nothing about this patch disables it), but it is now VESTIGIAL
        // for a genuinely-native-AIC guest. Windows' AppleAic-equivalent HAL extension
        // is expected to generate IPIs via AIC's own mechanism instead (see the Fast-IPI
        // design decision comment in hv_exc_fiq(), item 4 / docs/windows-native-aic.md),
        // never touching this GICv3 CPU-interface sysreg. Left in place because it is
        // harmless (only fires if the guest actually executes this instruction) and
        // removing it isn't necessary to satisfy this patch's scope.
        //
        /* m1n1_windows change - emulate SGIs */
        case SYSREG_ISS(ICC_SGI1R_EL1):
            if (is_read) {
                regs[rt] = 0;
            } else {
                u64 sgir_value = regs[rt];
                u32 aff1, aff2, aff3;
                u32 aff0_targets;
                int virq, irm, rs;

                rs = (sgir_value >> ICH_SGI_RS_SHIFT) & ICH_SGI_RS_MASK;
                irm = (sgir_value >> ICH_SGI_IRQMODE_SHIFT) & ICH_SGI_IRQMODE_MASK;
                virq = (sgir_value >> ICH_SGI_IRQ_SHIFT) & ICH_SGI_IRQ_MASK;
                aff1 = ICH_SGI_AFF1(sgir_value);
                aff2 = ICH_SGI_AFF2(sgir_value);
                aff3 = ICH_SGI_AFF3(sgir_value);
                aff0_targets = sgir_value & ICH_SGI_TARGETLIST_MASK;

                /*
                 * GICR frames are dense but Apple's physical CPU IDs are not:
                 * J414s exposes frames 0..9 for CPU IDs 0..6,8,9,10.  Iterate
                 * the exact frame map so SGIs never target absent CPU7 or omit
                 * physical CPU10.
                 */
                for (u16 frame = 0; frame < hv_vgic3_num_cpus(); frame++) {
                    int cpu = hv_vgic3_cpu_for_frame(frame);
                    if (cpu < 0)
                        continue;
                    if (irm == ICH_SGI_TARGET_OTHERS) {
                        if (smp_id() == cpu)
                            continue;
                    } else if (irm == ICH_SGI_TARGET_LIST) {
                        if (aff0_targets == 0)
                            return false;
                        u64 mpidr = smp_get_mpidr(cpu);

                        if (MPIDR_AFF3(mpidr) != aff3)
                            continue;
                        if (MPIDR_AFF2(mpidr) != aff2)
                            continue;
                        if (MPIDR_AFF1(mpidr) != aff1)
                            continue;
                        //
                        // ICC_SGI1R_EL1: TargetList bit n addresses the PE with
                        // Aff0 = (RS * 16) + n. rs was decoded above but never
                        // applied, so any SGI with RS != 0 matched the wrong
                        // cores (BIT(aff0) against an un-shifted 16-bit list).
                        //
                        if ((int)(MPIDR_AFF0(mpidr) >> 4) != rs)
                            continue;
                        if (!(aff0_targets & BIT(MPIDR_AFF0(mpidr) & 0xf)))
                            continue;
                    } else {
                        return false;
                    }
                    virq_t pending = {
                        .vintid = virq,
                        .priority = hv_vgic3_get_priority_cpu(cpu, virq),
                        .active = false,
                        .pending = true,
                        .hw_status = false,
                        .hw_irq = 0,
                    };
                    if (hv_sgi_queue_push(cpu, &pending))
                        smp_send_ipi(cpu);
                }
            }
            return true;
#endif
        /* m1n1_windows change - Trap the ARM standard PMU regs */
        case SYSREG_ISS(SYS_PMCR_EL0):
            if (is_read) {
                u64 pmcr0_value = mrs(SYS_IMP_APL_PMCR0);
                u64 calculated = 0;
                u64 pmi_mask = PMCR0_IMODE_MASK;
                //
                // Are PMIs enabled? (affects bit 0 of PMCR equivalently)
                //
                if ((pmcr0_value & pmi_mask) == (PMCR0_IMODE_FIQ)) {
                    calculated |= PMCR_E;
                }
                //
                // Bits 5:1 are 0 for now,  bits 6 and 7 are on always (long events always on)
                // and bit 9 is checked by PMCR0[20] (since it deals with freeze/overflow)
                //
                if ((pmcr0_value & BIT(20)) != 0) {
                    calculated |= PMCR_FZO;
                }
                calculated |= ((BIT(6)) | (BIT(7)));
                printf("HV PMUv3 Redirect: mrs x%ld, PMCR_EL0 = 0x%lx\n", rt, calculated);
                regs[rt] = calculated;
            } else {
                //
                // Bits [63:10] will have writes discarded (mostly ARM spec, bit 32 due to lack of
                // support)
                //
                u64 pmcr0_value = mrs(SYS_IMP_APL_PMCR0);
                int cycle_reset_requested = 0;
                //
                // Bit 9 (stop count on overflow) writes affect bit 20 of APL_PMCR0
                //
                if ((regs[rt] & BIT(9)) != 0) {
                    pmcr0_value |= BIT(20);
                } else {
                    pmcr0_value &= ~(BIT(20));
                }

                //
                // Writes to bits [6:3] unsupported since no way of expressing either with Apple
                // PMUs.
                //
                // Writing bit 2 (cycle counter reset) implies setting PMC0 to 0, so check if that's
                // the case.
                //
                if ((regs[rt] & PMCR_C) != 0) {
                    cycle_reset_requested = 1;
                }
                //
                // Bit 1 is the same as bit 2 but for event counters, unimplemented for now.
                //
                // Bit 0 controls whether event counters are enabled globally, if this is being set,
                // the closest thing on Apple platforms is the IRQ mode so set that if bit 0 is
                // requested.
                //
                if ((regs[rt] & PMCR_E) != 0) {
                    pmcr0_value &= ~(PMCR0_IMODE_MASK);
                    pmcr0_value |= PMCR0_IMODE_FIQ;
                } else {
                    pmcr0_value &= ~(PMCR0_IMODE_MASK);
                    pmcr0_value |= PMCR0_IMODE_OFF;
                }
                sysop("isb");
                if (cycle_reset_requested == 1) {
                    pmcr0_value &= ~(BIT(12));
                    pmcr0_value &= ~(BIT(0));
                }
                msr(SYS_IMP_APL_PMCR0, pmcr0_value);
                sysop("isb");
                if (cycle_reset_requested == 1) {
                    sysop("isb");
                    msr(SYS_IMP_APL_PMC0, 0);
                    sysop("isb");
                    pmcr0_value |= BIT(12);
                    pmcr0_value |= BIT(0);
                    sysop("isb");
                    msr(SYS_IMP_APL_PMCR0, pmcr0_value);
                    sysop("isb");
                }
                printf("HV PMUv3 Redirect (OK): msr PMCR_EL0, x%ld = 0x%lx\n", rt, regs[rt]);
            }
            return true;
            SYSREG_APPLE_MAP(SYS_PMCCNTR_EL0, SYS_IMP_APL_PMC0)
        case SYSREG_ISS(SYS_PMCCFILTR_EL0):
            if (is_read) {
                u64 pmcr1_value = mrs(SYS_IMP_APL_PMCR1);
                u64 calculated_value = 0;
                //
                // If EL0/EL1 counting is disabled, set bit 30 of PMCCFILTR to 1.
                // (This is backwards from how I would've done it but okay I suppose...)
                //
                if ((pmcr1_value & BIT(8)) == 0) {
                    calculated_value |= BIT(30);
                }
                if ((pmcr1_value & BIT(16)) == 0) {
                    calculated_value |= BIT(31);
                }
                //
                // EL2 counting always happens as far as is known, so bit 27 is always set to 1
                // (bit set to 1 in this case means disable filtering...not sure why it's
                // backwards.)
                //
                calculated_value |= BIT(27);
                printf("HV PMUv3 Redirect: mrs x%ld, PMCCFILTR_EL0 = 0x%lx\n", rt,
                       calculated_value);
                regs[rt] = calculated_value;
            } else {
                u64 pmcr1_value = mrs(SYS_IMP_APL_PMCR1);
                //
                // If we're being asked to disable counting cycles for a given EL, set the
                // appropriate bit for PMCR1 respectively.
                //
                if ((regs[rt] & PMCCFILTR_P) == 0) {
                    pmcr1_value |= BIT(16);
                } else {
                    pmcr1_value &= ~(BIT(16));
                }
                if ((regs[rt] & PMCCFILTR_U) == 0) {
                    pmcr1_value |= BIT(8);
                } else {
                    pmcr1_value &= ~(BIT(8));
                }
                sysop("isb");
                msr(SYS_IMP_APL_PMCR1, pmcr1_value);
                sysop("isb");
                printf("HV PMUv3 Redirect (OK): msr PMCCFILTR_EL0, x%ld = 0x%lx\n", rt, regs[rt]);
            }
            return true;
        case SYSREG_ISS(SYS_PMCEID0_EL0):
            //
            // Unimplemented for now, return 0 for a read, discard writes.
            //
            if (is_read) {
                regs[rt] = 0;
                printf("HV PMUv3 Redirect: mrs x%ld, PMCEID0_EL0 = 0x%lx\n", rt, regs[rt]);
            } else {
                //
                // Do nothing here.
                //
                printf("HV PMUv3 Redirect (skipped write): msr PMCEID0_EL0, x%ld = 0x%lx\n", rt,
                       regs[rt]);
            }
            return true;
        case SYSREG_ISS(SYS_PMCEID1_EL0):
            //
            // Unimplemented for now, return 0 for a read, discard writes.
            //
            if (is_read) {
                regs[rt] = 0;
                printf("HV PMUv3 Redirect: mrs x%ld, PMCEID1_EL0 = 0x%lx\n", rt, regs[rt]);
            } else {
                printf("HV PMUv3 Redirect (skipped write): msr PMCEID1_EL0, x%ld = 0x%lx\n", rt,
                       regs[rt]);
            }
            return true;
        case SYSREG_ISS(SYS_PMCNTENCLR_EL0):
            if (is_read) {
                u64 pmcr0_value = mrs(SYS_IMP_APL_PMCR0);
                u64 calculated_value = 0;
                //
                // check what perf counters are enabled in PMCR0, and reflect that in the returned
                // PMCNTENCLR/PMCNTENSET value
                //
                if ((pmcr0_value & BIT(0)) != 0) {
                    calculated_value |= BIT(31);
                }
                printf("HV PMUv3 Redirect: mrs x%ld, PMCNTENCLR_EL0 = 0x%lx\n", rt,
                       calculated_value);
                regs[rt] = calculated_value;
            } else {
                u64 pmcr0_value = mrs(SYS_IMP_APL_PMCR0);
                u64 counters_disable_mask = GENMASK(31, 0);
                //
                // check if any counters are being requested to be disabled (cycle counter only for
                // now) and deal with those here.
                //
                if ((regs[rt] & counters_disable_mask) != 0) {
                    if ((regs[rt] & BIT(31)) != 0) {
                        pmcr0_value &= ~(BIT(0));
                    }
                    sysop("isb");
                    msr(SYS_IMP_APL_PMCR0, pmcr0_value);
                    sysop("isb");
                    printf("HV PMUv3 Redirect (OK): msr PMCNTENCLR_EL0, x%ld = 0x%lx\n", rt,
                           regs[rt]);
                }
            }
            return true;
        case SYSREG_ISS(SYS_PMCNTENSET_EL0):
            if (is_read) {
                u64 pmcr0_value = mrs(SYS_IMP_APL_PMCR0);
                u64 calculated_value = 0;
                //
                // check what perf counters are enabled in PMCR0, and reflect that in the returned
                // PMCNTENCLR/PMCNTENSET value
                //
                if ((pmcr0_value & BIT(0)) != 0) {
                    calculated_value |= BIT(31);
                }
                printf("HV PMUv3 Redirect: mrs x%ld, PMCNTENSET_EL0 = 0x%lx\n", rt,
                       calculated_value);
                regs[rt] = calculated_value;
            } else {
                u64 pmcr0_value = mrs(SYS_IMP_APL_PMCR0);
                u64 counters_enable_mask = GENMASK(31, 0);
                //
                // check if any counters are being requested to be disabled (cycle counter only for
                // now) and deal with those here.
                //
                if ((regs[rt] & counters_enable_mask) != 0) {
                    if ((regs[rt] & BIT(31)) != 0) {
                        pmcr0_value |= BIT(0);
                    }
                    sysop("isb");
                    msr(SYS_IMP_APL_PMCR0, pmcr0_value);
                    sysop("isb");
                    printf("HV PMUv3 Redirect (OK): msr PMCNTENSET_EL0, x%ld = 0x%lx\n", rt,
                           regs[rt]);
                }
            }
            return true;
            SYSREG_APPLE_MAP(SYS_PMEVCNTR0_EL0, SYS_IMP_APL_PMC2)
        // case SYSREG_ISS(SYS_PMEVTYPER0_EL0):
        //     if(is_read) {
        //         printf("mrs(PMEVTYPER0_EL0)\n");
        //         regs[rt] = 0;
        //         int value = mrs(SYS_IMP_APL_PMCR1);
        //         if(value & GENMASK(23, 16)) {
        //             regs[rt] |= BIT(31); //privileged bit
        //         }
        //         if(value & GENMASK(15, 8)) {
        //             regs[rt] |= BIT(30); //user filter bit
        //         }
        //         regs[rt] |= (mrs(SYS_IMP_APL_PMESR0) & GENMASK(7, 0));
        //     }
        //     else {
        //         int val = mrs(SYS_IMP_APL_PMCR1);
        //         int event = mrs(SYS_IMP_APL_PMESR0) & GENMASK(7, 0);
        //         if(regs[rt] & PMEVTYPER_P) {
        //             printf("msr(PMEVTYPER0_EL0, 0x%08lx): enabling el1 counting of event\n",
        //             regs[rt]); val |= BIT(16);
        //         }
        //         if(regs[rt] & GENMASK(7, 0)) {
        //             printf("msr(PMEVTYPER0_EL0, 0x%08lx): setting event\n", regs[rt]);
        //             event |= regs[rt] & GENMASK(7, 0);
        //             msr(SYS_IMP_APL_PMESR0, event);
        //         }
        //         msr(SYS_IMP_APL_PMCR1, val);
        //     }
        //     return true;
        case SYSREG_ISS(SYS_PMINTENCLR_EL1):
            if (is_read) {
                u64 pmcr0_value = mrs(SYS_IMP_APL_PMCR0);
                u64 calculated_value = 0;
                //
                // As before, cycle counter only for now. (bit 12 = PMI enabled for cycle counter)
                //
                if ((pmcr0_value & BIT(12)) != 0) {
                    calculated_value |= BIT(31);
                }
                printf("HV PMUv3 Redirect: mrs x%ld, PMINTENCLR_EL1 = 0x%lx\n", rt,
                       calculated_value);
                regs[rt] = calculated_value;
            } else {
                u64 pmcr0_value = mrs(SYS_IMP_APL_PMCR0);
                u64 counter_irqs_disabled_mask = GENMASK(31, 0);
                //
                // Cycle counter only for now (bits 19:12 control all PMIs for the PMUs)
                //
                if ((regs[rt] & counter_irqs_disabled_mask) != 0) {
                    if ((regs[rt] & (BIT(31))) != 0) {
                        pmcr0_value &= ~(BIT(12));
                    }
                    sysop("isb");
                    msr(SYS_IMP_APL_PMCR0, pmcr0_value);
                    sysop("isb");
                    printf("HV PMUv3 Redirect (OK): msr PMINTENCLR_EL1, x%ld = 0x%lx\n", rt,
                           regs[rt]);
                }
            }
            return true;
        case SYSREG_ISS(SYS_PMINTENSET_EL1):
            if (is_read) {
                u64 pmcr0_value = mrs(SYS_IMP_APL_PMCR0);
                u64 calculated_value = 0;
                //
                // As before, cycle counter only for now. (bit 12 = PMI enabled for cycle counter)
                //
                if ((pmcr0_value & BIT(12)) != 0) {
                    calculated_value |= BIT(31);
                }
                printf("HV PMUv3 Redirect: mrs x%ld, PMINTENSET_EL1 = 0x%lx\n", rt,
                       calculated_value);
                regs[rt] = calculated_value;
            } else {
                u64 pmcr0_value = mrs(SYS_IMP_APL_PMCR0);
                u64 counter_irqs_enabled_mask = GENMASK(31, 0);
                //
                // Cycle counter only for now (bits 19:12 control all PMIs for the PMUs)
                //
                if ((regs[rt] & counter_irqs_enabled_mask) != 0) {
                    if ((regs[rt] & BIT(31)) != 0) {
                        pmcr0_value |= BIT(12);
                    }
                    sysop("isb");
                    msr(SYS_IMP_APL_PMCR0, pmcr0_value);
                    sysop("isb");
                    printf("HV PMUv3 Redirect (OK): msr PMINTENSET_EL1, x%ld = 0x%lx\n", rt,
                           regs[rt]);
                }
            }
            return true;
        case SYSREG_ISS(SYS_PMMIR_EL1):
            //
            // return 0 for now, discard writes.
            //
            if (is_read) {
                regs[rt] = 0;
                printf("HV PMUv3 Redirect: mrs x%ld, PMMIR_EL1 = 0x%lx\n", rt, regs[rt]);
            } else {
                printf("HV PMUv3 Redirect (skipped write): msr PMMIR_EL1, x%ld = 0x%lx\n", rt,
                       regs[rt]);
            }
            return true;
        case SYSREG_ISS(SYS_PMOVSCLR_EL0):
            if (is_read) {
                //
                // Read the state of the PMSR register to see if a PMU has overflowed.
                // Cycle counter only for now.
                //
                u64 pmsr_value = mrs(SYS_IMP_APL_PMSR);
                u64 calculated_value = 0;
                u64 pmu_overflowed_mask = GENMASK(9, 0);
                if ((pmsr_value & pmu_overflowed_mask) != 0) {
                    //
                    // bit 0 is for PMC 0
                    //
                    if ((pmsr_value & BIT(0)) != 0) {
                        calculated_value |= BIT(31);
                    }
                }
                printf("HV PMUv3 Redirect: mrs x%ld, PMOVSCLR_EL0 = 0x%lx\n", rt, calculated_value);
                regs[rt] = calculated_value;
            } else {
                //
                // To clear the overflow bit requires a reset of the PMC (to clear PMSR)
                // so we need to disable the counter and re-enable it with the bit set to 0.
                //
                u64 pmcr0_value = mrs(SYS_IMP_APL_PMCR0);
                u64 counter_overflow_mask = GENMASK(31, 0);
                if ((regs[rt] & counter_overflow_mask) != 0) {
                    if ((regs[rt] & BIT(31)) != 0) {
                        pmcr0_value &= ~(BIT(12));
                        pmcr0_value &= ~(BIT(0));
                        sysop("isb");
                        msr(SYS_IMP_APL_PMCR0, pmcr0_value);
                        sysop("isb");
                        sysop("isb");
                        msr(SYS_IMP_APL_PMC0, 0);
                        sysop("isb");
                        pmcr0_value |= BIT(12);
                        pmcr0_value |= BIT(0);
                        sysop("isb");
                        msr(SYS_IMP_APL_PMCR0, pmcr0_value);
                        sysop("isb");
                    }
                    printf("HV PMUv3 Redirect (OK): msr PMOVSCLR_EL0, x%ld = 0x%lx\n", rt,
                           regs[rt]);
                }
            }
            return true;
        case SYSREG_ISS(SYS_PMOVSSET_EL0):
            if (is_read) {
                //
                // Read the state of the PMSR register to see if a PMU has overflowed.
                // Cycle counter only for now.
                //
                u64 pmsr_value = mrs(SYS_IMP_APL_PMSR);
                u64 calculated_value = 0;
                u64 pmu_overflowed_mask = GENMASK(9, 0);
                if ((pmsr_value & pmu_overflowed_mask) != 0) {
                    //
                    // bit 0 is for PMC 0
                    //
                    if ((pmsr_value & BIT(0)) != 0) {
                        calculated_value |= BIT(31);
                    }
                }
                printf("HV PMUv3 Redirect: mrs x%ld, PMOVSSET_EL0 = 0x%lx\n", rt, calculated_value);
                regs[rt] = calculated_value;
            } else {
                //
                // For now, don't set the overflow bit. If this needs to change, reuse the code from
                // before.
                //
            }
            return true;
        case SYSREG_ISS(SYS_PMSELR_EL0):
            // for now hardcode to set the cycle counter, this will very likely need to change
            if (is_read) {
                regs[rt] = 31;
                printf("HV PMUv3 Redirect: mrs x%ld, PMSELR_EL0 = 0x%lx\n", rt, regs[rt]);
            } else {
                printf("HV PMUv3 Redirect (skipped write): msr PMSELR_EL0, x%ld = 0x%lx\n", rt,
                       regs[rt]);
            }
            return true;
        // SYSREG_APPLE_MAP(SYS_PMSWINC_EL0, SYS_IMP_APL_PMC3)
        case SYSREG_ISS(SYS_PMUSERENR_EL0):
            if (is_read) {
                regs[rt] = PERCPU(guest_pmuserenr);
                printf("HV PMUv3 Redirect: mrs x%ld, PMUSERENR_EL0 = 0x%lx\n", rt, regs[rt]);
            } else {
                PERCPU(guest_pmuserenr) = regs[rt] & ~PMUSERENR_RESERVED;
                printf("HV PMUv3 Redirect (OK): msr PMUSERENR_EL0, x%ld = 0x%lx\n", rt,
                       PERCPU(guest_pmuserenr));
            }
            return true;
#ifdef ENABLE_VGIC_MODULE
            //
            // m1n1_windows change: since we're now going to be setting HCR_EL2.TID3 (to avoid
            // maintaining a fork of ArmGicDxe in the Mu UEFI port) we need to pass through all the
            // other registers except ID_AA64PFR0_EL1 (because that register needs to have the bit
            // OR'ed in that tells UEFI that we support the GICv3 sysreg interface) Note that this
            // only applies if the vGIC is being used, these registers should not be trapped
            // otherwise.
            //
            // windows-native-aic: HCR_EL2.TID3 stays set unconditionally in this mode too
            // (hv.c) and this pass-through/OR-in behavior is left unchanged. The GICv3 CPU
            // interface (ICH_*/ICC_*) is still enabled per-core even though the Windows-ready
            // timer no longer uses it for delivery (see hv_update_fiq(), which now reflects via
            // the synthetic EVENT(2/3) bridge) -- whatever UEFI/HAL logic probes
            // ID_AA64PFR0_EL1.GIC before the native AIC HAL extension takes over may still
            // rely on seeing this bit set. Whether Windows' own timer/HAL bring-up path
            // actually reads this field at all under a fully AIC-native design (as opposed
            // to the originally-planned GIC-PPI-for-the-timer hybrid this bit was added
            // for) is unconfirmed; left as-is rather than guessed at, see
            // docs/windows-native-aic.md.
            //
            SYSREG_PASS(ID_AA64PFR1_EL1)
            //
            // T8142 reports ID_AA64DFR0_EL1.PMUVer == 0b1111, the architectural
            // encoding for "IMPLEMENTATION DEFINED performance monitors supported,
            // PMUv3 NOT supported" (measured on J813: 0x000000f010305f0a).  The
            // hardware is telling the truth; the danger is that 0b1111 is non-zero.
            // A guest that tests PMUVer for non-zero rather than for the PMUv3
            // range concludes PMUv3 exists and issues e.g. MRS Xt, PMCCNTR_EL0 --
            // a register this core does not implement, so the core raises an EL1
            // undefined instruction (ESR 0x02000000).  Linux special-cases 0b1111
            // for exactly this reason; the Windows boot manager does not, and dies.
            //
            // EL2 cannot rescue that access.  The register is absent, so UNDEF is
            // taken to EL1 before MDCR_EL2 can classify it -- there is no EL2 trap
            // to extend.  The firmware assist in Mu's ArmExceptionLib only works
            // while UEFI owns VBAR_EL1; once bootmgfw.efi installs its own vectors
            // the same instruction lands in a handler that cannot emulate it and
            // the boot hangs silently with no serial output and a blank panel.
            //
            // So do not advertise a PMU the guest must not touch: report PMUVer 0,
            // "not implemented".  This is a truthful description of the PMUv3
            // interface available to a guest here, and it keeps every Microsoft
            // image untouched.  The IMP-DEF bank stays private to EL2 either way.
            //
            SYSREG_PASS_CLR(ID_AA64DFR0_EL1, ID_AA64DFR0_PMUVER_MASK)
            SYSREG_PASS(ID_AA64DFR1_EL1)
            SYSREG_PASS(ID_AA64ISAR0_EL1)
            SYSREG_PASS(ID_AA64ISAR1_EL1)
            SYSREG_PASS(SYS_ID_AA64MMFR0_EL1)
            SYSREG_PASS(SYS_ID_AA64MMFR1_EL1)
            SYSREG_PASS(ID_AA64AFR0_EL1)
            SYSREG_PASS(ID_AA64AFR1_EL1)
            //
            // The remainder of the AArch64 ID group 3 space.  TID3 traps all of it,
            // and anything not answered here falls through to hv_exc_proxy(), which
            // costs several serial round trips per instruction: the exception is
            // shipped to the proxy client, which issues its own remote mrs to read
            // the register and then logs the access.  Windows reads these constantly
            // during per-CPU bring-up and driver init, so leaving them unhandled
            // dominates boot time.  ID_AA64MMFR2_EL1 and ID_AA64ISAR2_EL1 were both
            // observed taking that path on ten-CPU boots.
            //
            SYSREG_PASS(ID_AA64PFR2_EL1)
            SYSREG_PASS(ID_AA64ZFR0_EL1)
            SYSREG_PASS(ID_AA64SMFR0_EL1)
            SYSREG_PASS(ID_AA64ISAR2_EL1)
            SYSREG_PASS(ID_AA64ISAR3_EL1)
            SYSREG_PASS(SYS_ID_AA64MMFR2_EL1)
            SYSREG_PASS(SYS_ID_AA64MMFR3_EL1)
            SYSREG_PASS(SYS_ID_AA64MMFR4_EL1)
        case SYSREG_ISS(ID_AA64PFR0_EL1):
            if (is_read) {
                //
                // need to OR in bit 24 to the register.
                // the (1 << 24) here makes the guest see that the MSR is reporting the GICv3 sysreg
                // interface
                //
                regs[rt] = _mrs(sr_tkn(ID_AA64PFR0_EL1)) | (1 << 24);
            } else {
                //
                // this register is architecturally RO, nothing should *ever* be attempting to write
                // this.
                //
            }
            //
            // Return here rather than falling through to hv_exc_proxy().  The
            // proxy services a trapped read by issuing its own remote mrs over
            // the serial link and overwriting regs[rt], which both discarded
            // the GIC bit this case exists to set and cost several serial round
            // trips per instruction.  ID_AA64PFR0_EL1 is roughly four fifths of
            // all ID-register traffic during Windows' per-CPU bring-up, so this
            // is where the boot time went.
            //
            return true;
#endif
        // SYSREG_APPLE_MAP(SYS_PMXEVCNTR_EL0, SYS_IMP_APL_PMC2)
        // case SYSREG_ISS(SYS_PMXEVTYPER_EL0):
        //     if(is_read) {
        //         printf("mrs(PMXEVTYPER_EL0)\n");
        //         regs[rt] = 0;
        //         int value = mrs(SYS_IMP_APL_PMCR1);
        //         if(value & GENMASK(23, 16)) {
        //             regs[rt] |= BIT(31); //privileged bit
        //         }
        //         if(value & GENMASK(15, 8)) {
        //             regs[rt] |= BIT(30); //user filter bit
        //         }
        //         regs[rt] |= (mrs(SYS_IMP_APL_PMESR0) & GENMASK(7, 0));
        //     }
        //     else {
        //         int val = mrs(SYS_IMP_APL_PMCR1);
        //         int event = mrs(SYS_IMP_APL_PMESR0) & GENMASK(7, 0);
        //         if(regs[rt] & PMEVTYPER_P) {
        //             printf("msr(PMEVTYPER0_EL0, 0x%08lx): enabling el1 counting of event\n",
        //             regs[rt]); val |= BIT(16);
        //         }
        //         if(regs[rt] & GENMASK(7, 0)) {
        //             printf("msr(PMEVTYPER0_EL0, 0x%08lx): setting event\n", regs[rt]);
        //             event |= regs[rt] & GENMASK(7, 0);
        //             msr(SYS_IMP_APL_PMESR0, event);
        //         }
        //         msr(SYS_IMP_APL_PMCR1, val);
        //     }
        //     return true;

        /*
         * Outer-shareable TLBI operations trap on Apple M2 even though the
         * guest legitimately selects them from its architectural feature
         * view.  Re-executing the trapped OS encoding at EL2 takes a nested
         * undefined exception.  Inner-shareable maintenance has the required
         * scope for this single-socket system and is implemented by the CPU,
         * so translate each OS operation to its IS counterpart.
         */
        case SYSREG_ISS(sys_reg(1, 0, 8, 1, 0)): // VMALLE1OS -> VMALLE1IS
            /* VMALLE1* is operandless and must encode Xt as XZR. */
            if (is_read)
                return false;
            sysop("tlbi vmalle1is");
            return true;
            SYSREG_MAP(sys_reg(1, 0, 8, 1, 1), sys_reg(1, 0, 8, 3, 1)) // VAE1OS -> VAE1IS
            SYSREG_MAP(sys_reg(1, 0, 8, 1, 2), sys_reg(1, 0, 8, 3, 2)) // ASIDE1OS -> ASIDE1IS
            SYSREG_MAP(sys_reg(1, 0, 8, 5, 1), sys_reg(1, 0, 8, 2, 1)) // RVAE1OS -> RVAE1IS

        case SYSREG_ISS(SYS_ACTLR_EL1):
            if (!cpu_features->actlr_el2 && !cpu_features->apple_sysregs_unlocked)
                return false;
            if (is_read) {
                if (cpu_features->actlr_el2)
                    regs[rt] = mrs(SYS_ACTLR_EL12);
                else
                    regs[rt] = mrs(SYS_IMP_APL_ACTLR_EL12);
            } else {
                if (cpu_features->actlr_el2)
                    msr(SYS_ACTLR_EL12, regs[rt]);
                else
                    msr(SYS_IMP_APL_ACTLR_EL12, regs[rt]);
            }
            return true;

        case SYSREG_ISS(SYS_IMP_APL_IPI_SR_EL1):
            if (is_read) {
                regs[rt] = (PERCPU(ipi_pending) & HV_GUEST_IPI_OUTSTANDING) ? IPI_SR_PENDING : 0;
            } else if (regs[rt] & IPI_SR_PENDING) {
#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
                if (hv_native_aic_windows_active()) {
                    /*
                     * The native HAL writes IPI_SR from its controller EOI.
                     * That is the first point at which NT has resolved vector
                     * E01 and run the IPI service path, so it is the commit
                     * boundary for exactly one INFLIGHT generation.  An init
                     * or deinit write with no INFLIGHT transaction is benign
                     * and must not discard a queued DELIVERABLE generation --
                     * including during the pre-CONFIG Windows carrier phase.
                     */
                    hv_guest_ipi_commit_delivery();
                    hv_native_aic_doorbell_sync();
                } else
#endif
                {
                    /* Preserve the original physical-register semantics. */
                    PERCPU(ipi_pending) = 0;
                }
            }
            return true;

        /* the hypervisor needs this one for breakpoints and single-stepping */
        case SYSREG_ISS(SYS_MDSCR_EL1):
            if (is_read)
                regs[rt] = PERCPU(mdscr);
            else
                PERCPU(mdscr) = regs[rt];
            return true;

        /* shadow the interrupt mode and state flag */
        case SYSREG_ISS(SYS_IMP_APL_PMCR0):
            if (!cpu_features->apple_sysregs_unlocked)
                return false;
            if (is_read) {
                u64 val = (mrs(SYS_IMP_APL_PMCR0) & ~PMCR0_IMODE_MASK) | PERCPU(pmc_irq_mode);
                regs[rt] = val | (PERCPU(pmc_pending) ? PMCR0_IACT : 0);
            } else {
                PERCPU(pmc_pending) = !!(regs[rt] & PMCR0_IACT);
                PERCPU(pmc_irq_mode) = regs[rt] & PMCR0_IMODE_MASK;
                msr(SYS_IMP_APL_PMCR0, regs[rt]);
            }
            return true;

        /*
         * Handle this one here because m1n1/Linux (will) use it for explicit cpuidle.
         * We can pass it through; going into deep sleep doesn't break the HV since we
         * don't do any wfis that assume otherwise in m1n1. However, don't het macOS
         * disable WFI ret (when going into systemwide sleep), since that breaks things.
         */
        case SYSREG_ISS(SYS_IMP_APL_CYC_OVRD):
            if (!cpu_features->apple_sysregs_unlocked)
                return false;
            if (is_read) {
                regs[rt] = mrs(SYS_IMP_APL_CYC_OVRD);
            } else {
                u64 value = regs[rt];

#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
                if (value & (CYC_OVRD_DISABLE_WFI_RET | CYC_OVRD_FIQ_MODE_MASK))
                    hv_native_aic_trace_record(HV_NATIVE_AIC_TRACE_WFI_POLICY, value, ctx->elr);
                /*
                 * Keep the native-AIC invariant installed at guest entry.
                 * Apple Fast-IPI/timer FIQ stays live with FIQ mode zero, WFI
                 * uses clock-gate-only mode 2, and architectural register
                 * retention remains enabled. Consume dangerous writes here;
                 * forwarding one to the proxy would perform the rejected
                 * physical sysreg write and could lose Windows' x18/KPCR.
                 */
                value &=
                    ~(CYC_OVRD_DISABLE_WFI_RET | CYC_OVRD_FIQ_MODE_MASK | CYC_OVRD_WFI_MODE_MASK);
                value |= CYC_OVRD_WFI_MODE(2);
#else
                if (value & (CYC_OVRD_DISABLE_WFI_RET | CYC_OVRD_FIQ_MODE_MASK))
                    return false;
#endif
                msr(SYS_IMP_APL_CYC_OVRD, value);
                sysop("isb");
            }
            return true;
            /* clang-format off */
        /* IPI handling */
        SYSREG_MAP_IF(SYS_IMP_APL_IPI_CR_EL1, SYS_IMP_APL_IPI_CR_EL1,
                      cpu_features->fast_ipi)
        /* M1RACLES reg, handle here due to silly 12.0 "mitigation" */
        case SYSREG_ISS(sys_reg(3, 5, 15, 10, 1)):
            if (is_read)
                regs[rt] = 0;
            return true;
    }
    return false;
}

static bool hv_handle_smc(struct exc_info *ctx) {
    if (ctx->regs[0] == HV_SYSREG_ASSIST_CALL_MAGIC && chip_id == T8142) {
        u64 output = 0;
        bool handled = hv_handle_t8142_sysreg_assist((u32)ctx->regs[1], ctx->regs[2],
                                                     &output);
        if (handled && !t8142_sysreg_assist_logged) {
            printf("HV: T8142: handled forwarded EL1 sysreg instruction 0x%08x at EL2\n",
                   (u32)ctx->regs[1]);
            t8142_sysreg_assist_logged = true;
        }
        ctx->regs[0] = handled;
        ctx->regs[1] = output;
        return true;
    }
#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
    if (ctx->regs[0] == HV_TIMER_REFLECT_CALL_MAGIC &&
        (ctx->regs[1] == 17 || ctx->regs[1] == 18)) {
        hv_timer_reflect_guest_rearm(ctx->regs[1] == 17, false, 0, true);
        return true;
    }
    if (ctx->regs[0] == HV_TIMER_REFLECT_CALL_MAGIC &&
        (ctx->regs[1] == HV_TIMER_REFLECT_CALL_REPOST_P ||
         ctx->regs[1] == HV_TIMER_REFLECT_CALL_REPOST_V)) {
        ctx->regs[0] = hv_timer_reflect_guest_repost(
            ctx->regs[1] == HV_TIMER_REFLECT_CALL_REPOST_P);
        return true;
    }
    if (ctx->regs[0] == HV_TIMER_REFLECT_CALL_MAGIC &&
        ctx->regs[1] == HV_TIMER_REFLECT_CALL_PROGRESS) {
        u32 cpu = smp_id();
        u32 step = (u32)ctx->regs[2];

        if (cpu < MAX_CPUS) {
            native_aic_last_progress[cpu] = step;
            if (step == NTASI_STEP_LOCAL_UNIT_READY)
                native_aic_local_unit_ready[cpu] = true;
        }
        printf("HV: ntasi-step: %ld (CPU %d, phase active=%d ready=%d)\n",
               ctx->regs[2], smp_id(), hv_native_aic_windows_active(),
               hv_native_aic_windows_ready());
        ctx->regs[0] = 0;
        return true;
    }
    if (ctx->regs[0] == HV_TIMER_REFLECT_CALL_MAGIC &&
        ctx->regs[1] == HV_TIMER_REFLECT_CALL_CONTROLLER_READY) {
        u32 cpu = smp_id();

        /*
         * Only meaningful once Mu has handed off at ExitBootServices; before
         * that windows_aic_phase is false and this is reported as ignored
         * rather than silently accepted.
         */
        if (cpu < MAX_CPUS && !hv_native_aic_windows_ready() &&
            native_aic_local_unit_ready[cpu] &&
            native_aic_last_progress[cpu] == NTASI_STEP_LOCAL_UNIT_READY) {
            /*
             * The v5 AIC3 HAL emits this same leaf at two readiness boundaries.
             * At step 35 Windows still calls ICC_IAR1_EL1; moving the timer to
             * native EVENT here creates a permanent spurious-IAR interrupt
             * loop. A later call, after step 23 (IOUNIT_ENTER), falls through
             * and performs the real one-shot handoff.
             */
            ctx->regs[0] = HV_TIMER_REFLECT_READY_ACCEPTED;
            return true;
        }
        if (hv_native_aic_windows_controller_ready("HAL extension SMC"))
            ctx->regs[0] = HV_TIMER_REFLECT_READY_ACCEPTED;
        else if (hv_native_aic_windows_ready())
            ctx->regs[0] = HV_TIMER_REFLECT_READY_ALREADY;
        else
            ctx->regs[0] = HV_TIMER_REFLECT_READY_IGNORED;
        return true;
    }
#endif
    printf("PSCI SMC DEBUG: handling PSCI request 0x%lx\n", ctx->regs[0]);
    bool handled_smc = hv_handle_psci_smc(ctx);
    return handled_smc;
}

static bool hv_handle_msr(struct exc_info *ctx, u64 iss)
{
    u64 reg = iss & (ESR_ISS_MSR_OP0 | ESR_ISS_MSR_OP2 | ESR_ISS_MSR_OP1 | ESR_ISS_MSR_CRn |
                     ESR_ISS_MSR_CRm);
    u64 rt = FIELD_GET(ESR_ISS_MSR_Rt, iss);
    bool is_read = iss & ESR_ISS_MSR_DIR;

    u64 *regs = ctx->regs;

    regs[31] = 0;

    switch (reg) {
        /* clang-format on */
        case SYSREG_ISS(SYS_IMP_APL_IPI_RR_LOCAL_EL1): {
            if (!cpu_features->fast_ipi || is_read)
                return false;
            u64 mpidr = (regs[rt] & 0xff) | (mrs(MPIDR_EL1) & 0xffff00);
            for (int i = 0; i < MAX_CPUS; i++)
                if (mpidr == smp_get_mpidr(i)) {
#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
                    __atomic_fetch_add(&pcpu[i].guest_ipi_send_count, 1, __ATOMIC_RELAXED);
                    HV_NATIVE_AIC_HOT_TRACE(HV_NATIVE_AIC_TRACE_IPI_SEND, i, regs[rt]);
                    if (hv_guest_ipi_queue_pre_config_carrier(i))
                        return true;
#endif
                    __atomic_store_n(&pcpu[i].ipi_queued, true, __ATOMIC_RELEASE);
                    /*
                     * The target can take and clear the physical Fast-IPI as
                     * soon as the system-register write is visible.  Publish
                     * the guest-origin tag first; otherwise the target may
                     * observe the FIQ before this cache line and discard the
                     * Windows IPI as an EL2-only rendezvous.
                     */
                    sysop("dsb sy");
                    msr(SYS_IMP_APL_IPI_RR_LOCAL_EL1, regs[rt]);
                    return true;
                }
            return false;
        }
        case SYSREG_ISS(SYS_IMP_APL_IPI_RR_GLOBAL_EL1):
            if (!cpu_features->fast_ipi || is_read)
                return false;
            u64 mpidr = (regs[rt] & 0xff) | ((regs[rt] & 0xff0000) >> 8);
            for (int i = 0; i < MAX_CPUS; i++) {
                if (mpidr == (smp_get_mpidr(i) & 0xffff)) {
#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
                    __atomic_fetch_add(&pcpu[i].guest_ipi_send_count, 1, __ATOMIC_RELAXED);
                    HV_NATIVE_AIC_HOT_TRACE(HV_NATIVE_AIC_TRACE_IPI_SEND, i, regs[rt]);
                    if (hv_guest_ipi_queue_pre_config_carrier(i))
                        return true;
#endif
                    __atomic_store_n(&pcpu[i].ipi_queued, true, __ATOMIC_RELEASE);
                    sysop("dsb sy");
                    msr(SYS_IMP_APL_IPI_RR_GLOBAL_EL1, regs[rt]);
                    return true;
                }
            }
            return false;
#ifdef DEBUG_PMU_IRQ
        case SYSREG_ISS(SYS_IMP_APL_PMC0):
            if (!cpu_features->apple_sysregs_unlocked)
                return false;
            if (is_read) {
                regs[rt] = mrs(SYS_IMP_APL_PMC0);
            } else {
                msr(SYS_IMP_APL_PMC0, regs[rt]);
                printf("msr(SYS_IMP_APL_PMC0, 0x%04lx_%08lx)\n", regs[rt] >> 32,
                       regs[rt] & 0xFFFFFFFF);
            }
            return true;
#endif
    }

    return false;
}

static void hv_get_context(struct exc_info *ctx)
{
    ctx->spsr = hv_get_spsr();
    ctx->elr = hv_get_elr();
    ctx->esr = hv_get_esr();
    ctx->far = hv_get_far();
    ctx->afsr1 = hv_get_afsr1();
    ctx->sp[0] = mrs(SP_EL0);
    ctx->sp[1] = mrs(SP_EL1);
    ctx->sp[2] = (u64)ctx;
    ctx->cpu_id = smp_id();
    ctx->mpidr = mrs(MPIDR_EL1);

    sysop("isb");
}

/*
 * True while another CPU is inside hv_rendezvous() waiting for this one.
 *
 * This is consulted from the hottest paths in the hypervisor, so it must stay
 * exactly one acquire load and nothing else.
 */
static inline bool hv_rendezvous_requested(void)
{
    return __atomic_load_n(&hv_rendezvous_pending, __ATOMIC_ACQUIRE) != 0;
}

/*
 * EXHAUSTIVE list of return-to-guest paths in this file that never call
 * hv_exc_entry(), and therefore hold this CPU's hv_cpus_in_guest bit for their
 * whole duration -- they are invisible to hv_rendezvous() by construction:
 *
 *   hv_exc_sync()  the `if (handled)` fast path -- breadcrumbs '#' then 's'.
 *                  Handled below by taking the full round trip when a
 *                  rendezvous is outstanding (added in 575ca6c9).
 *   hv_exc_fiq()   the non-interruptible-CPU fast path (the `smp_id() !=
 *                  interruptible_cpu` early return).  Handled by falling
 *                  through to the slow path when a rendezvous is outstanding.
 *                  Note it re-arms with hv_secondary_tick_interval
 *                  (one second under ECV), so a CPU that hides here will not
 *                  re-enter EL2 on its own inside any sane rendezvous budget.
 *   hv_exc_irq()   FIVE separate returns, and under ENABLE_VGIC_MODULE the
 *                  function never calls hv_exc_entry() on ANY path:
 *                    - native-AIC, carrier IAR still outstanding (clears
 *                      IMO|VI and returns)
 *                    - native-AIC, general (hv_native_aic_enter_cpu() and
 *                      return)
 *                    - stale startup timer-reflector SW IRQ discard
 *                    - maintenance-IRQ / list-register drain
 *                    - the implicit return after the vGIC injection tail
 *                  None of them emitted a breadcrumb before this change, so a
 *                  CPU looping in here was indistinguishable from a CPU taking
 *                  no traps at all.  Native-AIC diagnostics in these paths
 *                  must remain binary trace writes: a console line at UART
 *                  speeds is milliseconds -- far past the old ~0.3-1 ms
 *                  rendezvous budget, with FIQs masked the whole time.
 *
 * (hv_exc_serr() and hv_exc_sync()'s slow path always take the round trip.)
 *
 * hv_exc_irq() cannot simply call hv_exc_entry()/hv_exc_exit(): hv_exc_exit()
 * republishes SPSR/ELR/SP and re-runs hv_update_fiq(), which would add another
 * exception/return cycle to the real-IRQ handoff. The native update path now
 * uses the centralized, deferred-dominant routing helper, but the direct
 * return remains important: it lets EL1 execute its one destructive EVENT read
 * before any later HCR synchronization.
 *
 * So do only what the rendezvous actually needs, and touch nothing else: leave
 * the in-guest set, park on bhl until the requesting CPU is finished, rejoin.
 * The bit is cleared BEFORE bhl is taken -- the same ordering hv_exc_entry()
 * relies on, and the reason a waiter is released while this CPU queues on the
 * lock rather than deadlocking against it.
 *
 * The cost of not using the full round trip is that this CPU does not pick up
 * the new CNTVOFF_EL2/stolen_time here.  That self-corrects at its next real
 * hv_exc_exit(); a transient timebase skew is a far smaller problem than the
 * one being fixed.
 */
static void hv_rendezvous_quiesce(void)
{
    __atomic_and_fetch(&hv_cpus_in_guest, ~BIT(smp_id()), __ATOMIC_ACQUIRE);
    spin_lock(&bhl);
    spin_unlock(&bhl);
    hv_maybe_exit();
    __atomic_or_fetch(&hv_cpus_in_guest, BIT(smp_id()), __ATOMIC_ACQUIRE);
}

static void hv_exc_entry(void)
{
    // Enable SErrors in the HV, but only if not already pending
    if (!(mrs(ISR_EL1) & 0x100))
        sysop("msr daifclr, 4");

    __atomic_and_fetch(&hv_cpus_in_guest, ~BIT(smp_id()), __ATOMIC_ACQUIRE);
    spin_lock(&bhl);
    hv_wdt_breadcrumb('X');
    exc_entry_time = hv_host_counter();
    /* disable PMU counters in the hypervisor */
    if (cpu_features->apple_sysregs_unlocked) {
        u64 pmcr0 = mrs(SYS_IMP_APL_PMCR0);
        PERCPU(exc_entry_pmcr0_cnt) = pmcr0 & PMCR0_CNT_MASK;
        msr(SYS_IMP_APL_PMCR0, pmcr0 & ~PMCR0_CNT_MASK);
    }
}

static void hv_exc_exit(struct exc_info *ctx)
{
    hv_wdt_breadcrumb('x');
    hv_windows_update_carrier_readiness(ctx);
    hv_update_fiq(ctx);
    /* reenable PMU counters */
    if (cpu_features->apple_sysregs_unlocked)
        reg_set(SYS_IMP_APL_PMCR0, PERCPU(exc_entry_pmcr0_cnt));
    msr(CNTVOFF_EL2, stolen_time);
    spin_unlock(&bhl);
    hv_maybe_exit();
    __atomic_or_fetch(&hv_cpus_in_guest, BIT(smp_id()), __ATOMIC_ACQUIRE);

#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
    if (hv_windows_guest_compat_enabled() && chip_id == T8142 && hv_native_aic_mu_timer_active() &&
        !(ctx->sp[0] & 0xffffffff00000000ULL) && (ctx->sp[1] & 0xffffffff00000000ULL)) {
        u64 truncated_sp = ctx->sp[0];
        ctx->sp[0] |= ctx->sp[1] & 0xffffffff00000000ULL;

        if (!t8142_sp_el0_repair_logged[smp_id()]) {
            t8142_sp_el0_repair_logged[smp_id()] = true;
            printf("HV: T8142: repaired truncated Mu SP_EL0 %#lx -> %#lx on CPU %u\n", truncated_sp,
                   ctx->sp[0], smp_id());
        }
    }
#endif

    hv_set_spsr(ctx->spsr);
    hv_set_elr(ctx->elr);
    msr(SP_EL0, ctx->sp[0]);
    msr(SP_EL1, ctx->sp[1]);
}

/*
 * Perform a guest WFI at EL2.  See the HCR_EL2.TWI comment in hv_init() for why
 * this CPU cannot be trusted to run the instruction itself: it can return with
 * the general-purpose register file cleared, and the guest keeps live values in
 * it across the wait.  Here the whole guest register set is already in the
 * exception frame and _hv_return reloads it, so the loss cannot reach EL1.
 */
static bool hv_handle_wfx(struct exc_info *ctx)
{
    /* ISS[1:0] "TI": 0b00 WFI, 0b01 WFE, 0b10 WFIT, 0b11 WFET. */
    if (FIELD_GET(ESR_ISS, ctx->esr) & 1)
        return false; // WFE is not trapped (HCR_EL2.TWE is clear) and has no handler here

    wfi_trap_count[smp_id()]++;
    if (!wfi_trap_logged) {
        wfi_trap_logged = true;
        printf("HV: first guest WFI held at EL2 on CPU %u, guest pc 0x%lx\n", smp_id(), ctx->elr);
    }

    /*
     * Service the MTP IOP's mailbox here: still off the big hypervisor lock,
     * but deliberately *outside* the "about to go idle" test below.  That
     * test is skipped whenever a virtual interrupt is pending, which is
     * exactly the situation where the guest is busy or wedged -- measured on
     * J813 with a stuck HCR_VI, the idle block stopped running entirely (the
     * framebuffer fell to the 1 Hz tick's 64 lines/s), and a drain placed
     * inside it would have starved precisely when the IOP most needed
     * servicing.  Internally rate-limited, single-servicer, bounded, silent;
     * hv_tick() keeps a 1 Hz floor.
     */

    /*
     * Wait only when there is nothing to return to the guest with.  A pending
     * virtual interrupt is not a wakeup event for EL2 -- the guest can reach
     * WFI with interrupts masked and its only pending work injected by m1n1 --
     * so waiting on one would stall until the next physical tick.
     */
    if (mrs(ISR_EL1) == 0 && !(mrs(HCR_EL2) & (HCR_VI | HCR_VF))) {
        /*
         * Bound the wait.  A virtual interrupt does not end a WFI executed at
         * EL2, so without this an idle secondary sleeps until its own tick --
         * one hertz -- and every timed wait in the guest stretches to match.
         * See HV_WFI_WAKE_RATE.
         */
        /*
         * This core is about to go idle and we are not holding the big
         * hypervisor lock here, which makes it the right place to move the
         * guest's framebuffer to the panel. Doing it from hv_tick() instead --
         * a whole frame, under the lock -- cut guest interrupt throughput by
         * more than an order of magnitude.
         */
        hv_fb_convert_slice(HV_FB_SLICE_IDLE);
        /* Unlocked idle path, same as the slice above: this is where most of
         * the change detection actually happens, because an idle guest exits
         * here constantly and a busy one does not need watching as closely. */
        hv_arm_wfi_wake();
        cpu_wfi_stateless();
    }

    /*
     * Deliberately no hv_wdt_pet() here.  The watchdog timestamp is global, so
     * an idling CPU petting it would hide a genuine hang on another one -- and
     * hv_start_secondary() is already known to stall intermittently on the
     * first CPU_ON.  The boot CPU's tick keeps the watchdog fed.
     */
    return true;
}

void hv_exc_sync(struct exc_info *ctx)
{
#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
    hv_native_aic_exception_entry();
#endif
    hv_wdt_breadcrumb('S');
    hv_get_context(ctx);

    /*
     * Keep the absent-register redirect current from here rather than only
     * from the host tick.  A guest installs its vectors and then touches an
     * absent register within microseconds -- the NT kernel reads PMCCNTR_EL0
     * almost immediately after taking over -- so a once-per-second check loses
     * that race and the first fault reaches a handler with no answer for it,
     * which is fatal.  Synchronous traps are frequent enough (ID-register
     * reads, PSCI, our own trampoline) to close the gap; the tick stays as a
     * backstop for stretches with no traps at all.
     */
    hv_track_t8142_undef_vector(ctx);

    bool handled = false;
    u32 ec = FIELD_GET(ESR_EC, ctx->esr);

    switch (ec) {
        case ESR_EC_MSR:
            hv_wdt_breadcrumb('m');
            handled = hv_handle_msr_unlocked(ctx, FIELD_GET(ESR_ISS, ctx->esr));
            break;
        case ESR_EC_WFI:
            hv_wdt_breadcrumb('w');
            handled = hv_handle_wfx(ctx);
            break;
        //
        // for Blizzard/Avalanche and later - we need to explicitly check for SMC EC to handle SMCs
        //
        case ESR_EC_SMC:
            hv_wdt_breadcrumb('s');
            handled = hv_handle_smc(ctx);
            break;
        case ESR_EC_HVC:
            /*
             * Mu uses an architectural HVC to forward EL1 system-register
             * instructions that T8142 reports as undefined.  Keep the call
             * ABI shared with the existing SMC/IMPDEF service dispatcher;
             * hv_handle_smc() validates the magic before claiming the trap.
             */
            hv_wdt_breadcrumb('h');
            /*
             * The redirected T8142 undefined-instruction vector arrives here
             * too; it carries its own immediate so it cannot be mistaken for a
             * "NTSRG" forwarding call.
             */
            handled = (FIELD_GET(ESR_ISS, ctx->esr) == 0 && hv_windows_n1_census_hvc(ctx->regs)) ||
                      hv_handle_t8142_undef_trampoline(ctx) || hv_handle_smc(ctx);
            break;
        case ESR_EC_IMPDEF:
            hv_wdt_breadcrumb('a');
            if (ctx->afsr1 == 0x1c00000) {
                /**
                 * m1n1_windows change: add SMC handling support.
                 *
                 * right now the only reason a guest OS would fire an SMC is due to
                 * requesting a PSCI service.
                 */
                handled = hv_handle_smc(ctx);
                break;
            }
            switch (FIELD_GET(ESR_ISS, ctx->esr)) {
                case ESR_ISS_IMPDEF_MSR:
                    handled = hv_handle_msr_unlocked(ctx, ctx->afsr1);
                    break;
            }
            break;
    }

    if (handled) {
        hv_wdt_breadcrumb('#');
        /*
         * Trapped MRS/MSR/IMPDEF instructions leave ELR_EL2 pointing at the
         * faulting instruction, so those handlers must advance by one A64
         * instruction. HVC is different: the architecture records the
         * preferred return address (the instruction after HVC) in ELR_EL2.
         * Advancing it again skips the caller's first return instruction. In
         * Mu's ArmHvcLib that instruction restores the saved argument pointer,
         * so the double advance turned its following store into a write through
         * stale x9 (observed as FAR 0x02000010 on J813).
         */
        if (ec != ESR_EC_HVC)
            ctx->elr += 4;
        hv_set_elr(ctx->elr);
        /*
         * Republish the rest of the return state, not just ELR.
         *
         * Guest x0-x30 reach the guest through the exception frame, so a
         * handler that writes ctx->regs[] works on this path without help.  SP
         * and SPSR do not: they are written back only by hv_exc_exit(), which
         * this path exists to skip.  Anything a handler put in ctx->sp[] or
         * ctx->spsr was therefore dropped on the floor -- silently, and only on
         * the path taken in the common case.
         *
         * That cost two hardware debugging cycles on the T8142 EL0 vector
         * redirect: replaying NT's "sub sp, sp, #0x370" left the guest 0x370
         * above its own frame and it ran off the kernel stack into the guard
         * page (bugcheck 0x50), and unwinding an emulated EL0 fault returned to
         * user code at EL1h instead of EL0 (bugcheck 0x2B).
         *
         * These are the values hv_get_context() read on entry, so for every
         * handler that does not touch them this is a write-back of what is
         * already there -- three system-register writes against a trap that
         * already cost hundreds of cycles.
         */
        hv_set_spsr(ctx->spsr);
        msr(SP_EL0, ctx->sp[0]);
        msr(SP_EL1, ctx->sp[1]);
        /*
         * This return path is invisible to hv_rendezvous(): hv_cpus_in_guest is
         * cleared only by hv_exc_entry() below, which we are about to skip.  A
         * CPU servicing Apple IMPDEF MSR traps back to back therefore stays
         * marked "in guest" indefinitely and any other core's rendezvous spins
         * out -- measured repeatedly on the J414s, at first always naming CPU 0,
         * which carries the pure-AIC software timer reflection and so takes
         * those traps continuously.  Both arms of the `handled` switch are
         * implicated: a later capture caught CPU 5 stuck here on the plain
         * ESR_EC_MSR arm (breadcrumbs `sSm#sSm#`), not just ESR_EC_IMPDEF.
         *
         * Take the slow round trip only while a rendezvous is outstanding.
         * hv_exc_entry() clears the bit BEFORE it blocks on bhl, so the waiting
         * CPU is released immediately and we then queue on the lock exactly
         * like a CPU that arrived via the FIQ slow path.  When no rendezvous is
         * pending this costs one acquire load and the fast path is unchanged.
         *
         * Check before hv_update_fiq(): hv_exc_exit() runs it for us, and the
         * slow path below already relies on that single call, so this keeps the
         * two paths byte-for-byte equivalent in what they do to FIQ state
         * instead of running the level maintenance twice.
         */
        if (hv_rendezvous_requested()) {
            hv_exc_entry();
            hv_exc_exit(ctx);
            return;
        }
        hv_update_fiq(ctx);
        hv_wdt_breadcrumb('s');
        return;
    }

    hv_exc_entry();

    switch (ec) {
        case ESR_EC_DABORT_LOWER:
            hv_wdt_breadcrumb('D');
            handled = hv_handle_dabort(ctx);
            break;
        case ESR_EC_MSR:
            hv_wdt_breadcrumb('M');
            handled = hv_handle_msr(ctx, FIELD_GET(ESR_ISS, ctx->esr));
            break;
        case ESR_EC_IMPDEF:
            hv_wdt_breadcrumb('A');
            switch (FIELD_GET(ESR_ISS, ctx->esr)) {
                case ESR_ISS_IMPDEF_MSR:
                    handled = hv_handle_msr(ctx, ctx->afsr1);
                    break;
            }
            break;
    }

    if (handled) {
        hv_wdt_breadcrumb('+');
        ctx->elr += 4;
    } else {
        hv_wdt_breadcrumb('-');
#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
        if (ec == ESR_EC_BRK)
            hv_native_aic_trace_context(HV_NATIVE_AIC_TRACE_BRK, ctx);
#endif
        // VM code can forward a nested SError exception here
        if (FIELD_GET(ESR_EC, ctx->esr) == ESR_EC_SERROR)
            hv_exc_proxy(ctx, START_EXCEPTION_LOWER, EXC_SERROR, NULL);
        else
            hv_exc_proxy(ctx, START_EXCEPTION_LOWER, EXC_SYNC, NULL);
    }

    hv_exc_exit(ctx);
    hv_wdt_breadcrumb('s');
}

void hv_exc_irq(struct exc_info *ctx)
{
#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
    hv_native_aic_exception_entry();
#endif
    /*
     * Breadcrumb every entry.  Under ENABLE_VGIC_MODULE this vector used to
     * emit nothing at all on any path, which made a CPU spinning in here
     * indistinguishable from a CPU taking no traps whatsoever -- and both leave
     * hv_cpus_in_guest set, so both look identical to hv_rendezvous().  That
     * ambiguity is precisely what could not be resolved from the J414s capture
     * where CPU 6's trail was frozen at `57*89+xs`.  One breadcrumb per entry
     * makes the next capture decisive: a trail ending in 'I', or alternating
     * with 'I', means the CPU was in this vector rather than out in the guest.
     */
    hv_wdt_breadcrumb('I');

    /*
     * Answer an outstanding rendezvous before touching any AIC, carrier or HCR
     * state.  Every exit from this function returns to the guest without
     * hv_exc_entry() (see hv_rendezvous_quiesce() for the full enumeration and
     * for why the full round trip is not safe here). Diagnostics on those
     * paths must not touch UART: console output outlasts any sane rendezvous
     * budget with FIQs masked the whole time.
     *
     * Doing it at the top, before any mutation, is what makes this safe: no
     * native-AIC handshake state has been read or written yet, and the physical
     * source is still level-asserted and untouched, so the vector simply
     * resumes normally once the requesting CPU releases bhl.
     */
    if (hv_rendezvous_requested())
        hv_rendezvous_quiesce();

    hv_windows_update_carrier_readiness(ctx);
#ifdef ENABLE_VGIC_MODULE
#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
    /*
     * AIC2 CONFIG is enabled on the last processor to finish the deferred HAL
     * handoff.  Other processors may still have their per-CPU HCR.IMO set.
     * Do not acknowledge their pending AIC EVENT at EL2: clear IMO and return,
     * allowing the still-pending physical IRQ to be taken again by Windows EL1.
     */
    /*
     * ...unless this core is running the emulated GICv3 CPU interface, in which
     * case m1n1 owns the line and must acknowledge it here.
     *
     * The native handoff below deliberately returns *without* reading EVENT and
     * clears HCR.IMO, so that a guest with the Windows AIC HAL extension can
     * take the still-asserted physical IRQ at EL1 and read EVENT itself.  J813
     * has no such HAL extension: nothing at EL1 will ever read EVENT, so the
     * line stays asserted and unmasked forever and the device wedges.
     *
     * This test has to come first, because native_aic_active is latched for the
     * entire run on this machine -- hv_aic.c sets it the moment Mu's own AIC
     * CONFIG is observed live, long before Windows exists -- which made the
     * vGIC tail at the bottom of this function unreachable on T8142.
     * hv_gic_cpuif_active() is false until the guest enables Group 1, so Mu,
     * which does drive the AIC itself, keeps the native path unchanged.
     */
    if (hv_native_aic_active() && !hv_gic_cpuif_active()) {
        if (hv_native_aic_windows_ready() && PERCPU(carrier_irq_active)) {
            /*
             * The carrier IAR has already been returned to EL1.  Preserve the
             * software CPU-interface state and TALL1 until its EOIR trap, but
             * release this native physical IRQ to Windows by clearing IMO.
             * Calling hv_native_aic_enter_cpu() here would disable TALL1 and
             * strand carrier_irq_active permanently.
             */
            hv_native_aic_doorbell_sync();
            return;
        }
        hv_native_aic_enter_cpu();
        /*
         * The real native source remains level-asserted until Windows reads
         * AIC EVENT.  Restoring IMO here would take the same IRQ at EL2 again
         * before EL1 executes a single instruction.  Preserve all synthetic
         * pending state until the trapped AIC EVENT read proves that Windows
         * accepted and auto-masked the source.  Keep m1n1's private tick armed
         * so debugger/proxy polling continues while that handoff is pending.
         */
        if (hv_native_aic_windows_ready()) {
            native_irq_rearm_deferred[smp_id()] = true;
            native_irq_rearm_entry_epoch[smp_id()] = native_irq_entry_epoch[smp_id()];
            native_irq_rearm_start_time[smp_id()] = hv_guest_ipi_clock_now();
            native_irq_bounce_count[smp_id()]++;
            hv_arm_tick(false);
        }
        return;
    }
#endif
    //
    // windows-native-aic: under ENABLE_NATIVE_AIC_PASSTHROUGH, hv.c clears
    // HCR_EL2.IMO specifically so ordinary AIC-routed physical IRQs go straight to the
    // guest at EL1 -- this vector should not fire for them at all anymore. It is left
    // fully intact below (unreachable-in-theory, not deleted) because:
    //  (a) the GICv3 virtual-CPU-interface maintenance interrupt's routing relative to
    //      HCR_EL2.IMO on Apple Silicon specifically is UNVERIFIED here (see
    //      docs/windows-native-aic.md OQ-3) -- on textbook ARM systems it follows the
    //      same physical-IRQ routing controls as any other physical interrupt, which
    //      would mean it stops trapping to EL2 too, but this file's own existing
    //      `type == 0` heuristic below (with its own "?" in the original comment)
    //      shows even the author of this vector was not fully certain how Apple wires
    //      it through AIC; and
    //  (b) if that assumption is wrong, or anything else still forces physical-IRQ
    //      trapping, we must not silently do nothing here -- see the fail-closed
    //      fallback replacing the old AIC-IRQ-to-vGIC-injection tail, below.
    //
    u32 reason = aic_ack();
    u32 irq = FIELD_GET(AIC_EVENT_NUM, reason);
    int type = FIELD_GET(AIC_EVENT_TYPE, reason);

#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
    /*
     * AIC EVENT state can outlive a RAM chainload.  A timer-reflector SW IRQ
     * that was posted by the previous guest may therefore already be latched
     * when the new startup carrier begins.  It must not be translated into a
     * vGIC LR: unlike a real level interrupt, the startup guest has no native
     * AIC EOI path that can clear the SW-pending bit, so the same event would
     * be acknowledged and injected forever.
     */
    if ((!hv_native_aic_active() || hv_gic_cpuif_active()) && irq >= HV_TIMER_SWIRQ_BASE &&
        irq < HV_TIMER_SWIRQ_BASE + (2 * MAX_CPUS)) {
        aic_set_mask(irq, true);
        aic_set_sw(irq, false);
        hv_native_aic_trace_record(HV_NATIVE_AIC_TRACE_EVENT_RESERVED, irq, reason);
        return;
    }
#endif

    u64 misr = mrs(ICH_MISR_EL2);
    u64 eisr = mrs(ICH_EISR_EL2);

    if (type == 0) { // maintenance IRQ?
        if (misr != 0 && eisr != 0) {
            for (int lr = 0; lr < (int)hv_vgic3_num_lrs(); lr++) {
                if (eisr & BIT(lr)) {
                    u64 lr_val = hv_vgic3_read_lr(lr);
                    u64 intd = (lr_val >> ICH_LR_VIRTUAL_SHIFT) & ICH_LR_VIRTUAL_MASK;
                    hv_vgic3_write_lr(lr, 0);
                    //`intd` came out of a list register, so it is a guest
                    // INTID; aic_set_mask() wants the physical line, and only
                    // for a line m1n1 owns (hv_aic_alias_is_owned()).
                    if (intd > 31 && hv_aic_alias_is_owned(intd))
                        aic_set_mask(hv_aic_alias_to_physical(intd), false);
                }
            }
        }

        //
        // windows-native-aic: timer_queue has no producer anymore under
        // ENABLE_NATIVE_AIC_PASSTHROUGH -- the Windows-ready timer FIQ reflector
        // (hv_update_fiq(), above) uses the per-CPU synthetic EVENT(2/3) bridge; it no
        // longer pushes onto this queue or injects a timer via list register. Mu's
        // pre-ExitBootServices path uses the separate reserved AIC software lines.
        // This drain is therefore a permanent no-op in that mode; left in place rather than
        // #ifndef'd out to minimize the diff and because it is harmless (an always-empty
        // pop loop). It stays fully live (and load-bearing) when
        // ENABLE_NATIVE_AIC_PASSTHROUGH is off, i.e. the original vGIC-distributor mode.
        //
        while (hv_vgic3_get_free_lr() != -1) {
            virq_t pending;
            if (!virq_queue_pop(&PERCPU(timer_queue), &pending))
                break;
            hv_vgic3_inject_irq(pending.vintid, pending.priority, pending.active, pending.pending,
                                pending.hw_status, pending.hw_irq);
        }
        //
        // windows-native-aic: sgi_queue still has a producer -- the pre-existing,
        // vestigial ICC_SGI1R_EL1 trap-and-emulate below (case SYSREG_ISS(ICC_SGI1R_EL1))
        // pushes onto it regardless of ENABLE_NATIVE_AIC_PASSTHROUGH. Whether this drain
        // point is still reachable to service it depends on the same maintenance-interrupt
        // routing question raised above; hv_exc_fiq()'s Fast-IPI-arrival handling also
        // drains it independently (see there), so this is not the only retry point.
        //
        while (hv_vgic3_get_free_lr() != -1) {
            virq_t pending;
            if (!hv_sgi_queue_pop(&pending))
                break;
            hv_vgic3_inject_irq(pending.vintid, pending.priority, pending.active, pending.pending,
                                pending.hw_status, pending.hw_irq);
        }
        while (hv_vgic3_get_free_lr() != -1) {
            virq_t pending;
            if (!virq_queue_pop(&PERCPU(irq_queue), &pending))
                break;
            hv_vgic3_inject_irq(pending.vintid, pending.priority, pending.active, pending.pending,
                                pending.hw_status, pending.hw_irq);
        }
        return;
    }

    /*
     * Everything below this point is guest-facing: list registers and the vGIC
     * distributor are indexed by guest INTID, not by AIC line.  Translate the
     * physical line once here.  This is the identity unless an alias covers it,
     * so every platform without one is byte-for-byte unchanged; see
     * hv_aic_alias.h for why J813's MTP line needs one.
     */
    u32 guest_irq = hv_aic_alias_to_published(irq);

    /*
     * T8142 delivers through the software CPU interface, not through a list
     * register.  The guest's ICC_* accesses are trapped and emulated (see the
     * hv_gic_cpuif block above), so it never reads the hardware virtual CPU
     * interface and an LR would simply never be consumed.
     *
     * Marking the INTID pending here is the whole handoff.  This runs on the
     * CPU the AIC chose as the line's target, which is the CPU whose interface
     * must signal it, and reading EVENT above already acknowledged and
     * auto-masked the physical source -- the guest's EOIR trap is what unmasks
     * it again.
     */
    if (chip_id == T8142) {
        /*
         * One line per core the first time a real device interrupt makes it to
         * EL2.  Without it, "the AIC never presented it" and "EL2 never took
         * it" are indistinguishable from outside -- both leave the line
         * unmasked and asserted, which is the state that cost a boot cycle to
         * tell apart by hand.  One-shot, because this is a hot vector.
         */
        if (!gic_cpuif_aic_logged[smp_id()]) {
            gic_cpuif_aic_logged[smp_id()] = true;
            printf("HV: T8142: AIC line %u -> INTID %u at EL2 on CPU %d "
                   "(hcr 0x%lx)\n",
                   irq, guest_irq, (int)smp_id(), mrs(HCR_EL2));
        }
        hv_gic_cpuif_set_pending(guest_irq);
        return;
    }

#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
    if (hv_native_aic_windows_active() && !hv_native_aic_windows_ready()) {
        virq_t pending = {
            .vintid = guest_irq,
            .priority = hv_vgic3_get_priority(guest_irq),
            .active = false,
            .pending = true,
            .hw_status = false,
            .hw_irq = 0,
        };
        virq_queue_push(&PERCPU(irq_queue), &pending);
        if ((ctx->regs[18] == 0 || !PERCPU(carrier_stack_ready)) &&
            !PERCPU(carrier_stack_defer_logged)) {
            /* Trace the physical line: this records a hardware event. */
            hv_native_aic_trace_record(HV_NATIVE_AIC_TRACE_CARRIER_DEFER, irq, ctx->regs[18]);
            PERCPU(carrier_stack_defer_logged) = true;
        }
    } else
#endif
        if (hv_vgic3_get_free_lr() != -1) {
        hv_vgic3_inject_irq(guest_irq,                        // vintid
                            hv_vgic3_get_priority(guest_irq), // priority
                            false,                            // active
                            true,                             // pending
                            false,                            // hw_status
                            0                                 // hw_irq
        );
    } else {
        virq_t pending = {
            .vintid = guest_irq,
            .priority = hv_vgic3_get_priority(guest_irq),
            .active = false,
            .pending = true,
            .hw_status = false,
            .hw_irq = 0,
        };
        virq_queue_push(&PERCPU(irq_queue), &pending);
    }
#else
    /* The 'I' breadcrumb is now emitted at function entry, for every build. */
    hv_get_context(ctx);
    hv_exc_entry();
    hv_exc_proxy(ctx, START_EXCEPTION_LOWER, EXC_IRQ, NULL);
    hv_exc_exit(ctx);
    hv_wdt_breadcrumb('i');
#endif
}

void hv_exc_fiq(struct exc_info *ctx)
{
#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
    hv_native_aic_exception_entry();
#endif
    bool tick = false;

    hv_maybe_exit();

    //
    // windows-native-aic: the stale TODO that used to sit here ("inject the FIQ to the
    // guest as an IRQ if vGIC is enabled") is done, but NOT in this function -- the
    // CNTP_CTL_EL0/CNTV_CTL_EL0 reads immediately below are m1n1's OWN internal
    // periodic tick (hv_arm_tick(), HV_TICK_RATE/HV_SLOW_TICK_RATE in hv.c) and the
    // host-debugger HV_VTIMER proxy event, not the guest's virtualized timer. The
    // guest's virtualized CNTP/CNTV (CNTx_CTL_EL02) FIQ-to-synthetic-EVENT reflection
    // lives in hv_update_fiq() (called via hv_exc_exit() at the bottom of this
    // function, and directly for the non-interruptible-CPU fast path below) -- see
    // docs/windows-native-aic.md "Timer re-arm handshake".
    //

    if (hv_mask_pending_tick()) {
        tick = true;
    }

    int interruptible_cpu = hv_pinned_cpu;
    if (interruptible_cpu == -1)
        interruptible_cpu = boot_cpu_idx;

    /*
     * Recover a guest-origin Fast-IPI tag even when its physical IPI latch was
     * already acknowledged by a racing FIQ.  Every FIQ is a safe owner-CPU
     * recovery point; the second exchange below closes the arrival window
     * around the physical acknowledge.
     */
    bool guest_ipi_taken = hv_guest_ipi_take_tag();
#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
    if (guest_ipi_taken)
        HV_NATIVE_AIC_HOT_TRACE(HV_NATIVE_AIC_TRACE_IPI_FIQ, PERCPU(ipi_pending), 1);
    /*
     * The target FIQ is the only guaranteed EL2 entry for this Fast-IPI.
     * Post the native virtual-IRQ doorbell immediately after consuming a
     * post-CONFIG guest tag; an AP can mask FIQs as soon as it returns to
     * KiInitializeKernel, so deferring the CPU-local HCR.VI update to a later
     * timer/exception entry can strand an otherwise DELIVERABLE transaction.
     * hv_update_fiq() below remains the level-maintenance path and will keep
     * the same doorbell asserted until EVENT begins the transaction.
     */
    if (guest_ipi_taken && hv_native_aic_windows_ready())
        hv_native_aic_doorbell_sync();
    if (tick)
        hv_guest_ipi_retry_tick();
#endif

    /*
     * The fast path below is the second half of the same hole 575ca6c9 closed in
     * hv_exc_sync(): it returns straight to the guest without hv_exc_entry(), so
     * this CPU stays marked in-guest no matter how many FIQs it retires here.
     * It is worse than the sync one in two ways -- it emits no breadcrumb, and
     * it re-arms with hv_secondary_tick_interval, which is a full second under
     * ECV, so a CPU that hides here has no self-collection fallback at all.
     *
     * When a rendezvous is outstanding, do not take it: fall through to the slow
     * path, which is the existing, well-understood collection path and leaves an
     * 'F' in the trail.  That is deliberately preferred over bolting an
     * entry/exit pair on here -- it adds no new code path, avoids running
     * hv_update_fiq() twice, and the extra work is only ever paid while another
     * CPU is already stopped waiting for us.  Cost when idle: one acquire load.
     */
    if (smp_id() != interruptible_cpu && !(mrs(ISR_EL1) & 0x40) && hv_want_cpu == -1 &&
        !hv_rendezvous_requested()) {
        // Non-interruptible CPU and it was just a timer tick (or spurious), so just update FIQs
        hv_get_context(ctx);
        hv_update_fiq(ctx);
        hv_arm_tick(true);
        return;
    }

    // Slow (single threaded) path
    hv_wdt_breadcrumb('F');
    hv_get_context(ctx);
    hv_windows_update_carrier_readiness(ctx);
    hv_exc_entry();

    // Only poll for HV events in the interruptible CPU
    if (tick) {
        if (smp_id() == interruptible_cpu) {
            hv_tick(ctx);
            hv_arm_tick(false);
        } else {
            hv_arm_tick(true);
        }
    }

    if (mrs(CNTV_CTL_EL0) == (CNTx_CTL_ISTATUS | CNTx_CTL_ENABLE)) {
        msr(CNTV_CTL_EL0, CNTx_CTL_ISTATUS | CNTx_CTL_IMASK | CNTx_CTL_ENABLE);
        hv_exc_proxy(ctx, START_HV, HV_VTIMER, NULL);
    }

    //
    // windows-native-aic PMU-FIQ design decision (item 4, docs/windows-native-aic.md
    // "PMU / Fast-IPI FIQ handling"): fail-closed, unchanged from before this patch.
    // The physical PMU FIQ source is masked here (IACT + IMODE cleared) exactly as it
    // always was, and PERCPU(pmc_pending) is only ever exposed to the guest via the
    // pre-existing trapped MSR read of SYS_IMP_APL_PMCR0 (hv_handle_msr_unlocked(),
    // case SYSREG_ISS(SYS_IMP_APL_PMCR0)) -- nothing wakes the guest up to look at it;
    // there is no vGIC-injection or AIC-software-IRQ path for it either before or
    // after this patch. This is deliberate: the task scope for this transform is the
    // timer only, and inventing a new PMU-interrupt reflection here was explicitly out
    // of scope. Note cpu_regs.h:461 also defines PMCR0_IMODE_AIC (route the PMU
    // interrupt through AIC as an ordinary IRQ instead of FIQ) as an existing hardware
    // option nothing in this codebase currently requests -- switching the guest-PMU
    // emulation (hv_handle_msr_unlocked(), case SYSREG_ISS(SYS_PMCR_EL0)) to request
    // that mode instead of PMCR0_IMODE_FIQ could let PMU interrupts bypass EL2 entirely
    // under this patch's HCR_EL2.IMO=0, the same way ordinary peripheral IRQs do; flagged
    // as a future open question, not implemented here (unvalidated against whatever a
    // guest PMU driver expects).
    //
    if (cpu_features->apple_sysregs_unlocked) {
        u64 reg = mrs(SYS_IMP_APL_PMCR0);
        if ((reg & (PMCR0_IMODE_MASK | PMCR0_IACT)) == (PMCR0_IMODE_FIQ | PMCR0_IACT)) {
#ifdef DEBUG_PMU_IRQ
            printf("[FIQ] PMC IRQ, masking and delivering to the guest\n");
#endif
            reg_clr(SYS_IMP_APL_PMCR0, PMCR0_IACT | PMCR0_IMODE_MASK);
            PERCPU(pmc_pending) = true;
        }

        reg = mrs(SYS_IMP_APL_UPMCR0);
        if (FIELD_GET(UPMCR0_IMODE_T8020, reg) == UPMCR0_IMODE_FIQ &&
            (mrs(SYS_IMP_APL_UPMSR) & UPMSR_IACT)) {
            printf("[FIQ] UPMC IRQ, masking");
            reg_clr(SYS_IMP_APL_UPMCR0, UPMCR0_IMODE_T8020);
            hv_exc_proxy(ctx, START_EXCEPTION_LOWER, EXC_FIQ, NULL);
        }
    }

    /*
     * Apple's Fast IPI is FIQ-class and shared with m1n1's own rendezvous
     * mechanism. Guest IPI_RR writes are therefore trapped, tagged, and
     * relayed as physical Fast IPIs. The target acknowledges the physical
     * edge here, but that edge is only transport: after CONFIG handoff the
     * guest tag becomes a transactional synthetic AIC IPI EVENT driven by
     * HCR.VI. EVENT accept marks it INFLIGHT; the HAL's later IPI_SR write at
     * controller EOI commits it. A local-tick retry can re-emit an uncommitted
     * EVENT without consuming a newer DELIVERABLE generation.
     */
    u64 ipi_status = cpu_features->fast_ipi ? mrs(SYS_IMP_APL_IPI_SR_EL1) : 0;
    if (ipi_status & IPI_SR_PENDING) {
#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
        HV_NATIVE_AIC_HOT_TRACE(HV_NATIVE_AIC_TRACE_IPI_FIQ, ipi_status, PERCPU(ipi_queued));
#endif
        /*
         * Acknowledge the physical edge before consuming the guest tag.  If a
         * sender publishes a new tag while this FIQ is in flight, it either
         * re-raises IPI_SR after this acknowledge or is consumed by the
         * exchange below.  The previous consume-then-ack order could clear a
         * newly-arrived edge while stranding its tag forever.
         */
        msr(SYS_IMP_APL_IPI_SR_EL1, IPI_SR_PENDING);
        sysop("isb");
#ifdef ENABLE_VGIC_MODULE
#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
        if (!hv_native_aic_windows_active()) {
#endif
            while (hv_vgic3_get_free_lr() != -1) { // another CPU sent an IPI, check the sgi_queue
                virq_t pending;
                if (!hv_sgi_queue_pop(&pending))
                    break;
                hv_vgic3_inject_irq(pending.vintid, pending.priority, pending.active,
                                    pending.pending, pending.hw_status, pending.hw_irq);
            }
#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
        }
#endif
#endif
        hv_guest_ipi_take_tag();
    }

    hv_maybe_switch_cpu(ctx, START_HV, HV_CPU_SWITCH, NULL);

    // Handles guest timers
    hv_exc_exit(ctx);
    hv_wdt_breadcrumb('f');
}

void hv_exc_serr(struct exc_info *ctx)
{
#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
    hv_native_aic_exception_entry();
#endif
    hv_wdt_breadcrumb('E');
    hv_get_context(ctx);
    hv_exc_entry();
    hv_exc_proxy(ctx, START_EXCEPTION_LOWER, EXC_SERROR, NULL);
    hv_exc_exit(ctx);
    hv_wdt_breadcrumb('e');
}
