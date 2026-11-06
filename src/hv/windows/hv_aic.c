/* SPDX-License-Identifier: MIT */

#include "adt.h"
#include "aic.h"
#include "aic_regs.h"
#include "hv.h"
#include "hv_vgic.h"
#include "smp.h"
#include "string.h"
#include "uartproxy.h"
#include "utils.h"

#define IRQTRACE_IRQ BIT(0)

#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
#define AIC2_GLOBAL_CONFIG        0x14
#define AIC2_GLOBAL_CONFIG_ENABLE BIT(0)

static bool native_aic_active;
static bool mu_aic_ready;
static bool mu_timer_ready;
static bool windows_aic_phase;
static bool windows_aic_enabled;
static bool windows_ready_claimed;

#define HV_NATIVE_AIC_TRACE_DEPTH 64

/*
 * This is intentionally not part of hv_pcpu_data: that structure is a live
 * debugger ABI with an exact 0x800-byte size.  A trace entry keeps enough
 * architectural context to distinguish a real AIC event, a synthetic wake,
 * a broken HCR re-arm, and the BRK/x18 failure mode without reading any
 * guest memory from a hot path.
 */
struct hv_native_aic_trace_entry {
    u64 timestamp;
    u64 elr;
    u64 far;
    u64 hcr;
    u64 arg0;
    u64 arg1;
    u64 x18;
    u64 tpidr_el1;
    u64 spsr;
    u32 sequence;
    u16 code;
    u8 phase;
    u8 reserved;
};

static struct hv_native_aic_trace_entry native_aic_trace[MAX_CPUS][HV_NATIVE_AIC_TRACE_DEPTH];
static u32 native_aic_trace_head[MAX_CPUS];

static u8 native_aic_trace_phase(void)
{
    if (windows_aic_enabled)
        return 2; /* Windows-ready native AIC */
    if (windows_aic_phase)
        return 1; /* Windows startup carrier */
    if (mu_aic_ready)
        return 0; /* Mu native AIC */
    return 0xff;
}

static void native_aic_trace_write(u16 code, u64 arg0, u64 arg1, u64 elr, u64 far, u64 x18,
                                   u64 spsr)
{
    u64 cpu = mrs(TPIDR_EL2);
    if (cpu >= MAX_CPUS)
        return;

    u32 sequence = __atomic_fetch_add(&native_aic_trace_head[cpu], 1, __ATOMIC_RELAXED);
    struct hv_native_aic_trace_entry *entry =
        &native_aic_trace[cpu][sequence % HV_NATIVE_AIC_TRACE_DEPTH];

    entry->timestamp = hv_host_counter();
    entry->elr = elr;
    entry->far = far;
    entry->hcr = mrs(HCR_EL2);
    entry->arg0 = arg0;
    entry->arg1 = arg1;
    entry->x18 = x18;
    entry->tpidr_el1 = mrs(TPIDR_EL1);
    entry->spsr = spsr;
    entry->code = code;
    entry->phase = native_aic_trace_phase();
    entry->reserved = 0;
    __atomic_store_n(&entry->sequence, sequence + 1, __ATOMIC_RELEASE);
}

void hv_native_aic_trace_record(u16 code, u64 arg0, u64 arg1)
{
    native_aic_trace_write(code, arg0, arg1, mrs(ELR_EL2), mrs(FAR_EL2), 0, mrs(SPSR_EL2));
}

void hv_native_aic_trace_context(u16 code, const struct exc_info *ctx)
{
    if (ctx == NULL) {
        hv_native_aic_trace_record(code, 0, 0);
        return;
    }

    native_aic_trace_write(code, ctx->esr, ctx->far, ctx->elr, ctx->far, ctx->regs[18], ctx->spsr);
}

void hv_native_aic_trace_dump(void)
{
    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        u32 head = __atomic_load_n(&native_aic_trace_head[cpu], __ATOMIC_ACQUIRE);
        if (head == 0)
            continue;

        u32 first = head > HV_NATIVE_AIC_TRACE_DEPTH ? head - HV_NATIVE_AIC_TRACE_DEPTH : 0;
        printf("HV: native-aic trace CPU %d head=%u entries=%u\n", cpu, head, head - first);
        for (u32 sequence = first; sequence < head; sequence++) {
            struct hv_native_aic_trace_entry *entry =
                &native_aic_trace[cpu][sequence % HV_NATIVE_AIC_TRACE_DEPTH];
            if (__atomic_load_n(&entry->sequence, __ATOMIC_ACQUIRE) != sequence + 1)
                continue;
            printf("HV: native-aic trace cpu=%d seq=%u code=%u phase=%u "
                   "ts=0x%lx elr=0x%lx far=0x%lx hcr=0x%lx "
                   "a0=0x%lx a1=0x%lx x18=0x%lx tpidr=0x%lx spsr=0x%lx\n",
                   cpu, sequence, entry->code, entry->phase, entry->timestamp, entry->elr,
                   entry->far, entry->hcr, entry->arg0, entry->arg1, entry->x18, entry->tpidr_el1,
                   entry->spsr);
        }
    }
}

bool hv_native_aic_active(void)
{
    return __atomic_load_n(&native_aic_active, __ATOMIC_ACQUIRE);
}

bool hv_native_aic_windows_active(void)
{
    return __atomic_load_n(&windows_aic_phase, __ATOMIC_ACQUIRE);
}

bool hv_native_aic_windows_ready(void)
{
    return __atomic_load_n(&windows_aic_enabled, __ATOMIC_ACQUIRE);
}

bool hv_native_aic_mu_timer_active(void)
{
    return __atomic_load_n(&mu_timer_ready, __ATOMIC_ACQUIRE) && !hv_native_aic_windows_active();
}

void hv_native_aic_apply_hcr_route(enum hv_native_aic_hcr_route route)
{
    u64 hcr = mrs(HCR_EL2);
    u64 routed = hcr | HCR_FMO;

    switch (route) {
        case HV_NATIVE_AIC_HCR_PASSTHROUGH:
            routed &= ~(HCR_IMO | HCR_VI);
            break;
        case HV_NATIVE_AIC_HCR_STARTUP_CARRIER:
            routed = (routed & ~HCR_VI) | HCR_IMO;
            break;
        case HV_NATIVE_AIC_HCR_SYNTHETIC_DOORBELL:
            routed |= HCR_IMO | HCR_VI;
            break;
        case HV_NATIVE_AIC_HCR_STARTUP_PENDING:
            routed |= HCR_VI;
            break;
        case HV_NATIVE_AIC_HCR_CLEAR_VI:
            routed &= ~HCR_VI;
            break;
        default:
            assert(false);
            return;
    }

    if (routed != hcr)
        hv_write_hcr(routed);
}

void hv_native_aic_enter_cpu(void)
{
    bool startup_carrier =
        hv_native_aic_windows_active() && !__atomic_load_n(&windows_aic_enabled, __ATOMIC_ACQUIRE);

    for (u32 lr = 0; lr < hv_vgic3_num_lrs(); ++lr)
        hv_vgic3_write_lr(lr, 0);

    if (startup_carrier) {
        /*
         * Apple implements the EL2 list registers, but carrier LR behavior is
         * not reliable across J414s' two core types. Trap the short startup
         * CPU-interface window, keep IAR/EOIR state in software queues, and let
         * hv_update_fiq drive HCR.VI for deliverable entries. Target priorities
         * come from the guest's GICR state, and
         * hv_exc_exit observes Windows' kernel x18 alias while APs are parked.
         * The first Windows AIC2 CONFIG enable removes ICH,
         * TALL1, IMO, and the carrier VI path permanently.
         */
        msr(ICH_VMCR_EL2, BIT(1));
        msr(ICH_HCR_EL2, BIT(0) | BIT(12));
    } else {
        msr(ICH_HCR_EL2, 0);
    }
    sysop("isb");
    /*
     * Ordinary IRQs are native AIC in both phases.  Apple timer FIQ stays at
     * EL2: Mu receives it through its real-AIC software-IRQ ABI, while Windows
     * receives a synthetic AIC EVENT token (2/3) through the hooked EVENT read
     * and HCR.VI.  No vGIC interface is involved in the Windows-ready path.
     */
    hv_native_aic_apply_hcr_route(startup_carrier ? HV_NATIVE_AIC_HCR_STARTUP_CARRIER
                                                  : HV_NATIVE_AIC_HCR_PASSTHROUGH);
}

void hv_native_aic_timer_ready(void)
{
    if (__atomic_load_n(&mu_timer_ready, __ATOMIC_ACQUIRE) || hv_native_aic_windows_active())
        return;

    /*
     * mu_aic_ready is latched by handle_native_aic_transition() when AppleAicDxe
     * writes CONFIG.  Until that has happened there is nothing to reflect.
     *
     * T8142 used to compensate here by polling the real CONFIG register,
     * because its transition hooks were deliberately not installed and the
     * write could not be observed.  The hooks are installed now, so the poll
     * would only pre-empt the real phase machine with a less informed guess.
     */
    if (!__atomic_load_n(&mu_aic_ready, __ATOMIC_ACQUIRE))
        return;

    /*
     * TimerDxe registers its callback before enabling the architectural timer.
     * From this point EL2 may reflect timer FIQ as a real AIC software IRQ; Mu's
     * native AIC handler maps the reserved per-CPU source back to logical 17/18.
     */
    __atomic_store_n(&mu_timer_ready, true, __ATOMIC_RELEASE);
    hv_timer_native_enable();
    hv_native_aic_enter_cpu();
    printf("HV: windows-native-aic: Mu timer handler ready; pure-AIC SW timer reflection active on "
           "CPU %d\n",
           smp_id());
}

static bool handle_native_aic_transition(struct exc_info *ctx, u64 addr, u64 *val, bool write,
                                         int width)
{
    bool config_write = write && width == 2 && addr == aic->base + AIC2_GLOBAL_CONFIG;

    /*
     * A timeout-captured physical token already owns the next EVENT read.
     * Return it before touching the destructive hardware aperture; a newer
     * physical source must remain asserted until the older token reaches EOI.
     */
    if (!write && width == 2 && addr == aic->base + aic->regs.event &&
        hv_native_aic_event_replay(val))
        return true;

    /* The hook replaces the normal identity mapping for this page. */
    if (!hv_pa_rw(ctx, addr, val, write, width))
        return false;

    if (config_write)
        hv_native_aic_trace_record(HV_NATIVE_AIC_TRACE_CONFIG, *val, hv_native_aic_windows_ready());

    /*
     * A post-handoff timer/IPI is signaled by the m1n1 synthetic doorbell
     * state. Consume any physical EVENT first, then expose the processor-local
     * source token expected by the Windows AIC HAL extension through the
     * synthetic EVENT path. Mu-only reserved software IRQs are not used here.
     */
    if (!write && width == 2 && addr == aic->base + aic->regs.event) {
        u64 raw_event = *val;

        if (hv_native_aic_event_read(raw_event, val))
            return true;
    }

    if (config_write && (*val & AIC2_GLOBAL_CONFIG_ENABLE) &&
        !__atomic_load_n(&mu_aic_ready, __ATOMIC_ACQUIRE) && !hv_native_aic_windows_active()) {
        /*
         * AppleAicDxe enables CONFIG before it registers its exception handler.
         * Record controller readiness here, but keep timer FIQ held until the
         * first timer programming write proves TimerDxe registered its callback.
         */
        __atomic_store_n(&mu_aic_ready, true, __ATOMIC_RELEASE);
        printf("HV: windows-native-aic: Mu AIC2 configured; waiting for timer handler on CPU %d\n",
               smp_id());
    } else if (config_write && !(*val & AIC2_GLOBAL_CONFIG_ENABLE) &&
               !hv_native_aic_windows_active()) {
        /*
         * AppleAicDxe masks every source and clears CONFIG in its
         * ExitBootServices callback.  That is the exact firmware/Windows
         * boundary: keep IRQs native, but start trapping the Apple timer FIQ
         * so Windows never receives an architectural FIQ exception.
         */
        __atomic_store_n(&windows_aic_phase, true, __ATOMIC_RELEASE);
        hv_timer_reflect_hold();
        hv_native_aic_enter_cpu();
        printf("HV: windows-native-aic: Mu ExitBootServices observed; startup carrier active on "
               "CPU %d\n",
               smp_id());
    } else if (config_write && (*val & AIC2_GLOBAL_CONFIG_ENABLE) &&
               hv_native_aic_windows_active()) {
        hv_native_aic_windows_controller_ready("AIC2 CONFIG enable");
    }
    return true;
}

/*
 * The AIC2 path above learns that Windows has taken the controller by watching
 * for CONFIG |= ENABLE.  AIC3 has no such register: aic3_layout.c never sets
 * has_global_config, so AppleAicInitializeIoUnit skips that write entirely and
 * the branch is *structurally* unreachable -- not racy, not late.
 *
 * Measured on J813: the extension reached NTASI_STEP_IOUNIT_OK with all eleven
 * HAL callbacks installed and ten local units up, while m1n1 was still printing
 * "holding timer bridge until Windows enables AIC2".  Windows therefore never
 * received a timer tick and HAL init failed with bugcheck 0x5C (0x110, _, 0x19,
 * 0xC0000001).
 *
 * So take the handover from the extension directly, over the private SMC
 * channel it already uses for timer reposts.  This is strictly better evidence
 * than the CONFIG write ever was: a CONFIG write only says "the controller is
 * powered on", whereas this is issued by the native controller itself, once its
 * callbacks are installed and its line states replayed.
 *
 * The CONFIG branch is deliberately left in place -- AIC2 targets (J414s) keep
 * their existing trigger and simply find the work already done here.  Whichever
 * arrives first wins; the loser is a no-op.
 */
bool hv_native_aic_windows_controller_ready(const char *source)
{
    bool expected = false;

    if (!__atomic_load_n(&native_aic_active, __ATOMIC_ACQUIRE) || !hv_native_aic_windows_active())
        return false;

    /*
     * Claim separately from publishing.  The ordering below is the CONFIG
     * branch's ordering exactly, and it matters: hv_native_aic_enter_cpu()
     * decides between the startup carrier and full passthrough by reading
     * windows_aic_enabled, so the flag must be set before it runs and the SGI
     * retirement must happen before that.
     */
    if (!__atomic_compare_exchange_n(&windows_ready_claimed, &expected, true, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return false;

    hv_carrier_retire_active_sgis();
    __atomic_store_n(&windows_aic_enabled, true, __ATOMIC_RELEASE);
    hv_native_aic_enter_cpu();
    hv_timer_reflect_enable();
    printf("HV: windows-native-aic: Windows controller ready (%s) on CPU %d\n", source, smp_id());
    return true;
}

void hv_native_aic_transition_init(void)
{
    printf("HV: windows-native-aic: initializing transition state\n");
    memset(native_aic_trace, 0, sizeof(native_aic_trace));
    memset(native_aic_trace_head, 0, sizeof(native_aic_trace_head));
    __atomic_store_n(&native_aic_active, true, __ATOMIC_RELEASE);
    __atomic_store_n(&mu_aic_ready, false, __ATOMIC_RELEASE);
    __atomic_store_n(&mu_timer_ready, false, __ATOMIC_RELEASE);
    __atomic_store_n(&windows_aic_phase, false, __ATOMIC_RELEASE);
    __atomic_store_n(&windows_aic_enabled, false, __ATOMIC_RELEASE);
    __atomic_store_n(&windows_ready_claimed, false, __ATOMIC_RELEASE);
    /*
     * Install after the host has completed its broad MMIO mappings (hv_start,
     * not hv_init), otherwise pt_update overwrites these hooks.  CONFIG lives
     * in the first page and EVENT in the split AIC2 event aperture.
     */
    printf("HV: windows-native-aic: mapping CONFIG hook\n");
    hv_map_hook(aic->base, handle_native_aic_transition, 0x1000);
    printf("HV: windows-native-aic: mapping EVENT hook\n");
    hv_map_hook((aic->base + aic->regs.event) & ~0xfffULL, handle_native_aic_transition, 0x1000);
    printf("HV: windows-native-aic: initializing timer reflection\n");
    hv_timer_reflect_init();
    printf("HV: windows-native-aic: applying boot-CPU routing\n");
    hv_native_aic_enter_cpu();
    printf("HV: windows-native-aic: pure AIC active; watching CONFIG 0x%llx and EVENT 0x%llx\n",
           (unsigned long long)(aic->base + AIC2_GLOBAL_CONFIG),
           (unsigned long long)(aic->base + aic->regs.event));
}
#endif

static u32 trace_hw_num[AIC_MAX_DIES][AIC_MAX_HW_NUM / 32];

static void emit_irqtrace(u16 die, u16 type, u16 num)
{
    struct hv_evt_irqtrace evt = {
        .flags = IRQTRACE_IRQ,
        .type = type,
        .num = die * aic->max_irq + num,
    };

    hv_wdt_suspend();
    uartproxy_send_event(EVT_IRQTRACE, &evt, sizeof(evt));
    hv_wdt_resume();
}

static bool trace_aic_event(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    if (!hv_pa_rw(ctx, addr, val, write, width))
        return false;

    if (addr != (aic->base + aic->regs.event) || write || width != 2) {
        return true;
    }

    u16 die = FIELD_GET(AIC_EVENT_DIE, *val);
    u16 type = FIELD_GET(AIC_EVENT_TYPE, *val);
    u16 num = FIELD_GET(AIC_EVENT_NUM, *val);

    if (die > AIC_MAX_DIES)
        return true;

    switch (type) {
        case AIC_EVENT_TYPE_HW:
            if (trace_hw_num[die][num / 32] & BIT(num & 31)) {
                emit_irqtrace(die, type, num);
            }
            break;
        default:
            // ignore
            break;
    }

    return true;
}

bool hv_trace_irq(u32 type, u32 num, u32 count, u32 flags)
{
    dprintf("HV: hv_trace_irq type: %u start: %u num: %u flags: 0x%x\n", type, num, count, flags);
    if (type == AIC_EVENT_TYPE_HW) {
        u32 die = num / aic->max_irq;
        num %= AIC_MAX_HW_NUM;
        if (die >= aic->max_irq || num >= AIC_MAX_HW_NUM || count > AIC_MAX_HW_NUM - num) {
            printf("HV: invalid IRQ range: (%u, %u) for die %u\n", num, num + count, die);
            return false;
        }
        for (u32 n = num; n < num + count; n++) {
            switch (flags) {
                case IRQTRACE_IRQ:
                    trace_hw_num[die][n / 32] |= BIT(n & 31);
                    break;
                default:
                    trace_hw_num[die][n / 32] &= ~(BIT(n & 31));
                    break;
            }
        }
    } else {
        printf("HV: not handling AIC event type: 0x%02x num: %u\n", type, num);
        return false;
    }

    if (!aic) {
        printf("HV: AIC not initialized\n");
        return false;
    }

    static bool hooked = false;

    if (aic && !hooked) {
        hv_map_hook(aic->base, trace_aic_event, aic->regs.reg_size);
        hooked = true;
    }

    return true;
}
