/* SPDX-License-Identifier: MIT */

#ifndef HV_H
#define HV_H

#include "../../../build/build_cfg.h"

#include "../../../config.h"

#include "exception.h"
#include "iodev.h"
#include "types.h"
#include "uartproxy.h"
#include "windows_compat.h"
#include "windows_regs.h"

void hv_windows_system_exit(bool reset) __attribute__((noreturn));
int hv_windows_configure(u64 flags);
bool hv_windows_guest_compat_enabled(void);
bool hv_windows_can_prepare_device(void);
void hv_windows_quarantine(void);
#define HV_WINDOWS_GUEST_COMPAT BIT(0)
#define HV_WINDOWS_FB_SHADOW    BIT(1)

typedef bool(hv_hook_t)(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width);

#define MMIO_EVT_ATTR  GENMASK(31, 24)
#define MMIO_EVT_CPU   GENMASK(23, 16)
#define MMIO_EVT_SH    GENMASK(15, 14)
#define MMIO_EVT_MULTI BIT(6)
#define MMIO_EVT_WRITE BIT(5)
#define MMIO_EVT_WIDTH GENMASK(4, 0)

struct hv_evt_mmiotrace {
    u32 flags;
    u32 reserved;
    u64 pc;
    u64 addr;
    u64 data;
};

struct hv_evt_irqtrace {
    u32 flags;
    u16 type;
    u16 num;
};

#define HV_MAX_RW_SIZE  64
#define HV_MAX_RW_WORDS (HV_MAX_RW_SIZE >> 3)

struct hv_vm_proxy_hook_data {
    u32 flags;
    u32 id;
    u64 addr;
    u64 data[HV_MAX_RW_WORDS];
};

typedef enum _hv_entry_type {
    HV_HOOK_VM = 1,
    HV_VTIMER,
    HV_USER_INTERRUPT,
    HV_WDT_BARK,
    HV_CPU_SWITCH,
    HV_VIRTIO,
    HV_PANIC,
    HV_TPM,  /* one event per TPM command -- see hv_tpm.c */
    HV_XFER, /* one event per bulk-channel doorbell -- see hv_xfer.c */
    /* Mu has initialized its vectors and reached a safe exception-hook point. */
    HV_SYSREG_ASSIST_READY,
} hv_entry_type;

/* VM */
void hv_pt_init(void);
int hv_map(u64 from, u64 to, u64 size, u64 incr);
int hv_unmap(u64 from, u64 size);
int hv_map_hw(u64 from, u64 to, u64 size);
int hv_map_hw_ro(u64 from, u64 to, u64 size);
int hv_map_sw(u64 from, u64 to, u64 size);
int hv_map_hook(u64 from, hv_hook_t *hook, u64 size);
bool hv_pt_is_ram(u64 ipa);
int hv_pt_set_writable(u64 ipa, bool writable);
u64 hv_translate(u64 addr, bool s1only, bool w, u64 *par_out);
u64 hv_pt_walk(u64 addr);
bool hv_handle_dabort(struct exc_info *ctx);
bool hv_pa_write(struct exc_info *ctx, u64 addr, u64 *val, int width);
bool hv_pa_read(struct exc_info *ctx, u64 addr, u64 *val, int width);
bool hv_pa_rw(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width);

/* AIC events through tracing the MMIO event address */
bool hv_trace_irq(u32 type, u32 num, u32 count, u32 flags);

/* Virtual peripherals */
void hv_vuart_poll(void);
void hv_map_vuart(u64 base, int irq, iodev_id_t iodev);
struct virtio_conf;
void hv_map_virtio(u64 base, struct virtio_conf *conf);
void virtio_put_buffer(u64 base, int qu, u32 id, u32 len);

/* Exceptions */
void hv_exc_proxy(struct exc_info *ctx, uartproxy_boot_reason_t reason, u32 type, void *extra);
void hv_set_time_stealing(bool enabled, bool reset);
void hv_add_time(s64 time);

/* WDT */
void hv_wdt_pet(void);
void hv_wdt_suspend(void);
void hv_wdt_resume(void);
void hv_wdt_init(void);
int hv_wdt_start(int cpu);
int hv_wdt_stop(void);
void hv_wdt_breadcrumb(char c);
void hv_do_panic(void);

#define hv_panic(fmt, ...)                                                                         \
    do {                                                                                           \
        debug_printf("HV panic:" fmt, ##__VA_ARGS__);                                              \
        hv_do_panic();                                                                             \
        flush_and_reboot();                                                                        \
    } while (0)

/* Utilities */
void hv_write_hcr(u64 val);
u64 hv_get_spsr(void);
void hv_set_spsr(u64 val);
u64 hv_get_esr(void);
u64 hv_get_far(void);
u64 hv_get_elr(void);
u64 hv_get_afsr1(void);
void hv_set_elr(u64 val);

/* HV main */
int hv_init(void);
int hv_start(void *entry, u64 regs[4]);
int hv_start_secondary(int cpu, void *entry, u64 regs[4]);
void hv_exit_cpu(int cpu);
void hv_rendezvous(void);
bool hv_switch_cpu(int cpu);
void hv_pin_cpu(int cpu);
u64 hv_host_counter(void);
void hv_arm_tick(bool secondary);
void hv_arm_wfi_wake(void);

/*
 * Scanlines converted per call from the guest's BGRA shadow into the panel's
 * X2R10G10B10 scanout. Idle cores about to park in WFI do the bulk of it on the
 * unlocked path; the tick's slice is a floor for a guest that never idles, and
 * is bounded because hv_tick() holds the big hypervisor lock.
 *
 * Sized from a measurement of this machine's memory system, taken from
 * hv_fb_init() before the guest exists:
 *
 *   read cached 38 GB/s, read NC 20 GB/s, write NC 21 GB/s
 *   full-frame convert 5239 MB/s -- 3 ms for all 17 MB, a 322 fps ceiling
 *
 * So a scanline costs about 1.95 us and bandwidth was never the constraint.
 * The old idle slice of 8 was: cores reach the WFI path roughly 80 times a
 * second each, so nine of them moved 9 * 80 * 8 = 5760 lines/s, and against a
 * 1664-line panel that is 3.5 fps -- which is exactly the "4 fps" this was
 * assumed to be capped at by memory bandwidth.  It was a scheduling limit the
 * whole time.
 *
 * 160 lines puts the same nine cores at 9 * 80 * 160 = 115,200 lines/s, near
 * 70 fps, for 0.31 ms of work per wakeup on a core that was about to sleep.
 */
#define HV_FB_SLICE_IDLE 160
#define HV_FB_SLICE_TICK 64

void hv_fb_convert_slice(u32 lines);
void hv_fb_report(void);
bool hv_mask_pending_tick(void);
void hv_rearm(void);
void hv_maybe_exit(void);
void hv_tick(struct exc_info *ctx);
bool hv_handle_t8142_sysreg_assist(u32 instruction, u64 input, u64 *output);
bool hv_recover_t8142_sysreg_undef(struct exc_info *ctx);
void hv_track_t8142_undef_vector(struct exc_info *ctx);
void hv_verify_t8142_undef_vectors(void);
void hv_scan_t8142_guest_modules(void);
void hv_report_t8142_process(struct exc_info *ctx);
void hv_report_t8142_gic_activity(void);
void hv_check_t8142_bugcheck(void);

//
// PSCI init
//

void hv_psci_init(void);
#ifdef ENABLE_VGIC_MODULE
void hv_vgicv3_init(void);
// Bounded vGIC trace counters -- see hv_vgic.c VGIC_TRACE_LIMIT.
void hv_vgic3_trace_summary(void);
void init_vgic_irq_queues(void);
void hv_vgicv3_init_list_registers(void);
int hv_vgicv3_enable_virtual_interrupts(void);
#endif
#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
//
// windows-native-aic: initialize the Mu-only reserved AIC software-line cleanup
// state used by the timer-FIQ reflector (hv_exc.c, hv_update_fiq()). Windows-ready
// timer delivery uses the per-CPU HCR.VI/AIC EVENT(2/3) bridge instead; it does not
// use AIC2 target affinity or these reserved lines. Must run after AIC and SMP CPU
// discovery are up (see the call site in hv_init(), hv.c).
//
void hv_timer_reflect_init(void);
void hv_timer_native_enable(void);
void hv_timer_reflect_hold(void);
void hv_timer_reflect_enable(void);
void hv_carrier_retire_active_sgis(void);
bool hv_native_aic_event_replay(u64 *event);
bool hv_native_aic_event_read(u64 raw_event, u64 *event);
/*
 * Mu and Windows both own the real AIC.  Mu receives native IRQ and FIQ
 * exceptions; when Mu disables AIC2 CONFIG at ExitBootServices, the hook
 * changes only timer delivery: FIQ remains trapped at EL2 and is presented to
 * Windows as a synthetic IRQ whose AIC EVENT value is the Apple timer source.
 */
void hv_native_aic_transition_init(void);
bool hv_native_aic_active(void);
bool hv_native_aic_windows_active(void);
bool hv_native_aic_windows_ready(void);
bool hv_native_aic_mu_timer_active(void);
void hv_native_aic_timer_ready(void);
void hv_native_aic_enter_cpu(void);
/*
 * One-shot, idempotent handover: Windows' native AIC controller is installed
 * and may now own interrupt delivery.  Two things can report it -- an AIC2
 * CONFIG enable write, or the HAL extension's explicit SMC (AIC3 has no global
 * CONFIG register, so there is no write to watch).  Returns true for the caller
 * that actually performed the transition.
 */
bool hv_native_aic_windows_controller_ready(const char *source);
void hv_native_aic_delivery_report(void);

/*
 * Keep the mechanics of changing HCR.FMO/IMO/VI in one place. The helper
 * always reads the current CPU-local HCR itself, keeps FIQ routed to EL2, and
 * changes only the IRQ route selected by the caller.
 */
enum hv_native_aic_hcr_route {
    HV_NATIVE_AIC_HCR_PASSTHROUGH,
    HV_NATIVE_AIC_HCR_STARTUP_CARRIER,
    HV_NATIVE_AIC_HCR_SYNTHETIC_DOORBELL,
    HV_NATIVE_AIC_HCR_STARTUP_PENDING,
    HV_NATIVE_AIC_HCR_CLEAR_VI,
};

void hv_native_aic_apply_hcr_route(enum hv_native_aic_hcr_route route);

/*
 * Native-AIC diagnostics are deliberately a fixed-size binary ring rather
 * than printf calls in interrupt paths.  The ring is dumped only on a
 * watchdog/panic path, where the extra output is useful and bounded.
 */
enum hv_native_aic_trace_code {
    HV_NATIVE_AIC_TRACE_HCR = 1,
    HV_NATIVE_AIC_TRACE_CONFIG,
    HV_NATIVE_AIC_TRACE_EVENT,
    HV_NATIVE_AIC_TRACE_EVENT_REAL,
    HV_NATIVE_AIC_TRACE_EVENT_RESERVED,
    HV_NATIVE_AIC_TRACE_IPI_SEND,
    HV_NATIVE_AIC_TRACE_IPI_FIQ,
    HV_NATIVE_AIC_TRACE_IPI_BEGIN,
    HV_NATIVE_AIC_TRACE_IPI_COMMIT,
    HV_NATIVE_AIC_TRACE_TIMER_FIQ,
    HV_NATIVE_AIC_TRACE_TIMER_REARM,
    HV_NATIVE_AIC_TRACE_X18,
    HV_NATIVE_AIC_TRACE_BRK,
    HV_NATIVE_AIC_TRACE_STACK,
    HV_NATIVE_AIC_TRACE_CARRIER_VI,
    HV_NATIVE_AIC_TRACE_CARRIER_IAR,
    HV_NATIVE_AIC_TRACE_CARRIER_EOI,
    HV_NATIVE_AIC_TRACE_CARRIER_SYSREG,
    HV_NATIVE_AIC_TRACE_CARRIER_DEFER,
    HV_NATIVE_AIC_TRACE_CARRIER_MIGRATE,
    HV_NATIVE_AIC_TRACE_CARRIER_RETIRE,
    HV_NATIVE_AIC_TRACE_WFI_POLICY,
    HV_NATIVE_AIC_TRACE_IRQ_REARM,
    HV_NATIVE_AIC_TRACE_TIMER_REPOST,
};

void hv_native_aic_trace_record(u16 code, u64 arg0, u64 arg1);
void hv_native_aic_trace_context(u16 code, const struct exc_info *ctx);
void hv_native_aic_trace_dump(void);

/*
 * Timer/IPI/EVENT and deferred-IRQ release traces are useful in a diagnostic
 * image but cost an atomic sequence update plus nine architectural/context
 * stores per occurrence.  The production bridge keeps its plain per-CPU
 * counters and all rare/failure records while compiling routine traffic out
 * completely.
 */
#ifdef ENABLE_NATIVE_AIC_HOT_TRACE
#define HV_NATIVE_AIC_HOT_TRACE(code, arg0, arg1) hv_native_aic_trace_record((code), (arg0), (arg1))
#else
#define HV_NATIVE_AIC_HOT_TRACE(code, arg0, arg1)                                                  \
    do {                                                                                           \
    } while (0)
#endif
#endif
bool hv_handle_psci_smc(struct exc_info *ctx);
int hv_handle_psci_smc_python_entry(uint64_t regs[4]);

#endif
