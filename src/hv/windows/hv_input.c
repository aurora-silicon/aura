/* SPDX-License-Identifier: GPL-2.0-only */

/*
 * I0 carrier glue for the input device model.  See hv_input.h for the contract and
 * windows-m6/INPUT-DESIGN.md for the reasoning.  Written on this branch; no third-party source.
 * The model (input_model.c) is pure and host-tested; this file is host-tested unchanged
 * against stub headers (tests/e2e/windows_input_glue_host.c).
 */

#include "hv_input.h"
#include "hv.h"
#include "hv_mtp_t8152.h"
#include "hv_vdev_spi.h"
#include "input_model.h"
#include "iodev.h"
#include "soc.h"
#include "utils.h"

enum {
    LOG_WIDTH = BIT(0),
    LOG_RX = BIT(1),
};

static bool input_enabled;
static bool session_used; /* an ATTACH reached the hook this carrier session */
static bool attached;
static bool entered;
static bool exited;
static u32 sources; /* HV_INPUT_SRC_* */
static bool line;
static u32 logged;
static struct input_model model;
static DECLARE_SPINLOCK(input_lock);
static u64 report_next;
static u32 reported_frames, reported_bad, reported_enabled = ~0u;
/* fatal report: page accesses (count, last offset, 1 = write, when) and the last feed poll */
static u64 act_accesses, act_off, act_write, act_at;
static u64 poll_at, poll_bytes;

_Static_assert(HV_INPUT_INTID == HV_VDEV_SPI_INPUT, "one SPI number");
_Static_assert(HV_INPUT_SIZE == INPUT_PAGE_SIZE, "one page size");
_Static_assert(HV_INPUT_RX_BUDGET >= INPUT_HOST_MAX_FRAME, "a tick takes a whole frame");

void hv_input_enable(void)
{
    input_enabled = true;
}

bool hv_input_enabled(void)
{
    return input_enabled;
}

bool hv_input_owns_vuart_rx(void)
{
    return __atomic_load_n(&attached, __ATOMIC_ACQUIRE) && (sources & HV_INPUT_SRC_HOST);
}

static u64 now_ticks(void)
{
    return mrs(CNTPCT_EL0);
}

/*
 * The model's line to the virtual SPI: on changes only (the level is remembered there).  Never
 * after the guest exited: the software GIC is still marked owned then, and driving it would
 * pend the SPI, kick CPUs that have left the guest and rewrite the boot CPU's HCR_EL2 while it
 * is back in the proxy.  The exit release is state only.
 */
static void sync_line(void)
{
    if (exited)
        return;
    bool level = input_model_line(&model);
    if (level == line)
        return;
    line = level;
    hv_t8152_gic_spi_level(HV_INPUT_INTID, level);
}

/* ---- the page hook ----------------------------------------------------------------------- */

static bool hv_input_mmio(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    UNUSED(ctx);
    u64 off = addr - HV_INPUT_BASE;
    __atomic_add_fetch(&act_accesses, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&act_off, off, __ATOMIC_RELAXED);
    __atomic_store_n(&act_write, write ? 1 : 0, __ATOMIC_RELAXED);
    __atomic_store_n(&act_at, now_ticks(), __ATOMIC_RELAXED);

    spin_lock(&input_lock);
    bool ok = off < HV_INPUT_SIZE && input_model_mmio(&model, off, width, val, write);
    if (!ok && !write)
        for (int i = 0; width > 3 && i < (1 << (width - 3)) && i < HV_MAX_RW_WORDS; i++)
            val[i] = 0;
    sync_line();
    spin_unlock(&input_lock);

    if (!ok && !(logged & LOG_WIDTH)) {
        logged |= LOG_WIDTH;
        printf("HV: T8152: input: %s of %d bytes at page+0x%lx is not an aligned 32/64-bit "
               "access: %s (logged once)\n",
               write ? "write" : "read", 1 << width, off, write ? "ignored" : "reads 0");
    }
    return true; /* RAZ/WI, never a guest abort */
}

/* ---- the host feed ----------------------------------------------------------------------- */

void hv_input_internal_keyboard(u8 mods, const u8 keys[INPUT_KBD_BITMAP], u16 consumer)
{
    spin_lock(&input_lock);
    input_model_set_keyboard(&model, INPUT_SRC_INTERNAL, mods, keys);
    input_model_set_consumer(&model, INPUT_SRC_INTERNAL, consumer);
    spin_unlock(&input_lock);
}

void hv_input_internal_pointer(u8 buttons, s16 dx, s16 dy, s8 wheel, s8 pan)
{
    spin_lock(&input_lock);
    input_model_set_relative(&model, INPUT_SRC_TRACKPAD, buttons, dx, dy, wheel, pan);
    spin_unlock(&input_lock);
}

static bool mtp_service_now(void)
{
    return __atomic_load_n(&attached, __ATOMIC_ACQUIRE) && (sources & HV_INPUT_SRC_KEYBOARD) &&
           entered && !exited;
}

void hv_input_idle_poll(void)
{
    if (mtp_service_now())
        hv_mtp_t8152_poll();
}

void hv_input_poll(void)
{
    if (!attached || !entered || exited || !(sources & HV_INPUT_SRC_HOST))
        return;
    /* Before input_lock: the MTP's keyboard updates take it themselves. */
    if (mtp_service_now())
        hv_mtp_t8152_poll();

    u8 buf[HV_INPUT_RX_BUDGET];
    u32 n = 0;
    ssize_t failed = 0;
    u64 now = now_ticks();
    /*
     * Read and parse under one lock, so bytes taken by two CPUs can never reach the parser out
     * of order.  input_lock is taken before the iodev's own lock, never after it.
     */
    spin_lock(&input_lock);
    ssize_t avail = iodev_can_read(IODEV_USB_VUART);
    if (avail > 0) {
        u32 want = avail > (ssize_t)sizeof(buf) ? (u32)sizeof(buf) : (u32)avail;
        /* Exactly what is buffered: the CDC read returns without waiting. */
        ssize_t got = iodev_read(IODEV_USB_VUART, buf, want);
        if (got > 0)
            n = (u32)got;
        else if (got < 0)
            failed = got;
    }

    poll_at = now;
    poll_bytes += n;
    if (n)
        input_model_host_rx(&model, buf, n, now);
    input_model_poll(&model, now);
    sync_line();
    spin_unlock(&input_lock);

    if (failed && !(logged & LOG_RX)) {
        logged |= LOG_RX;
        printf("HV: T8152: input: the vUART RX read failed (%ld); host input is lost until it "
               "recovers (logged once)\n",
               (long)failed);
    }
}

/* ---- ATTACH and the rest of the lifecycle ------------------------------------------------ */

static s64 attach(u64 mask, u64 area_base, u64 area_size)
{
    if (!input_enabled || chip_id != T8152 || board_id != 0x22) {
        printf("HV: T8152: input: attach refused: the input configure flag is not set (or this "
               "is not J804)\n");
        return HV_INPUT_E_STATE;
    }
    if (session_used || hv_windows_guest_entered()) {
        printf("HV: T8152: input: attach refused: already attached this carrier session, or the "
               "guest started\n");
        return HV_INPUT_E_STATE;
    }
    u64 src = mask & HV_INPUT_SRC_MASK, opts = mask & ~HV_INPUT_SRC_MASK;
    if (!src || (src & ~(u64)(HV_INPUT_SRC_HOST | HV_INPUT_SRC_KEYBOARD | HV_INPUT_SRC_TRACKPAD)) ||
        (opts & ~(u64)HV_INPUT_OPT_MASK) || (opts && !(src & HV_INPUT_SRC_KEYBOARD))) {
        printf("HV: T8152: input: attach refused: bad source mask 0x%lx\n", mask);
        return HV_INPUT_E_SOURCES;
    }
    if (src & ~(u64)HV_INPUT_SRC_SUPPORTED) {
        printf("HV: T8152: input: attach refused: source mask 0x%lx has a source this carrier "
               "does not run\n",
               mask);
        return HV_INPUT_E_SOURCES;
    }
    if (!(src & HV_INPUT_SRC_HOST)) {
        printf("HV: T8152: input: attach refused: the internal keyboard needs the host source "
               "too (the Mac-fed path stays the fallback)\n");
        return HV_INPUT_E_SOURCES;
    }
    if ((src & HV_INPUT_SRC_TRACKPAD) && !(src & HV_INPUT_SRC_KEYBOARD)) {
        printf("HV: T8152: input: attach refused: the trackpad needs the internal keyboard too "
               "(one MTP bring-up serves both)\n");
        return HV_INPUT_E_SOURCES;
    }
    bool kbd = src & HV_INPUT_SRC_KEYBOARD;
    if (kbd ? (!area_base || !area_size) : (area_base || area_size)) {
        printf("HV: T8152: input: attach refused: %s (0x%lx +0x%lx)\n",
               kbd ? "the internal keyboard needs the reserved MTP area"
                   : "the host source takes no reserved area",
               area_base, area_size);
        return HV_INPUT_E_AREA;
    }
    u64 hz = mrs(CNTFRQ_EL0);
    input_model_init(&model, INPUT_FEAT_SRC_HOST, hz * HV_INPUT_WATCHDOG_MS / 1000);
    line = false;
    session_used = true;
    if (hv_map_hook(HV_INPUT_BASE, hv_input_mmio, HV_INPUT_SIZE) < 0) {
        printf("HV: T8152: input: attach refused: cannot hook the page at 0x%lx\n",
               HV_INPUT_BASE);
        return HV_INPUT_E_HOOK;
    }
    sources = (u32)src;
    __atomic_store_n(&attached, true, __ATOMIC_RELEASE);
    printf("HV: T8152: input: attached (host feed on the vUART RX); page 0x%lx hooked, SPI %u; "
           "held-key watchdog %u ms\n",
           HV_INPUT_BASE, HV_INPUT_INTID, HV_INPUT_WATCHDOG_MS);
    return 0;
}

static u64 query(u64 index)
{
    switch (index) {
        case HV_INPUT_Q_VERSION:
            return HV_INPUT_VERSION;
        case HV_INPUT_Q_BASE:
            return HV_INPUT_BASE;
        case HV_INPUT_Q_SIZE:
            return HV_INPUT_SIZE;
        case HV_INPUT_Q_INTID:
            return HV_INPUT_INTID;
        case HV_INPUT_Q_FEATURES:
            /* After ATTACH the live sources (bit 17 only once the keyboard is READY). */
            return __atomic_load_n(&attached, __ATOMIC_ACQUIRE) ? model.features
                                                                 : INPUT_FEAT_REPORTS |
                                                                       INPUT_FEAT_SRC_HOST;
        case HV_INPUT_Q_SOURCES:
            return HV_INPUT_SRC_SUPPORTED;
        default:
            return HV_INPUT_ERROR;
    }
}

static u64 status(u64 index)
{
    u64 v;
    spin_lock(&input_lock);
    if (index < INPUT_ST_COUNT) {
        v = model.stats[index];
    } else if (index >= HV_INPUT_S_MTP_STATE && index <= HV_INPUT_S_MTP_GPIO) {
        v = (sources & HV_INPUT_SRC_KEYBOARD) ? hv_mtp_t8152_status((u32)index) : 0;
    } else {
        switch (index) {
            case HV_INPUT_S_ATTACHED:
                v = attached ? sources : 0;
                break;
            case HV_INPUT_S_CTRL:
                v = model.ctrl;
                break;
            case HV_INPUT_S_DEPTH:
                v = input_model_depth(&model);
                break;
            case HV_INPUT_S_DROPPED:
                v = model.dropped;
                break;
            case HV_INPUT_S_LEDS:
                v = model.leds;
                break;
            case HV_INPUT_S_LINE:
                v = line;
                break;
            case HV_INPUT_S_OWNS_RX:
                v = hv_input_owns_vuart_rx();
                break;
            default:
                v = HV_INPUT_ERROR;
                break;
        }
    }
    spin_unlock(&input_lock);
    return v;
}

u64 hv_input_proxy(u64 op, u64 a1, u64 a2, u64 a3)
{
    switch (op) {
        case HV_INPUT_OP_QUERY:
            return query(a1);
        case HV_INPUT_OP_ATTACH: {
            spin_lock(&input_lock);
            s64 ret = attach(a1, a2, a3);
            spin_unlock(&input_lock);
            if (ret || !(sources & HV_INPUT_SRC_KEYBOARD))
                return (u64)ret;
            /*
             * The MTP outside input_lock: its keyboard updates take the lock.  A failure keeps
             * the host source: ATTACH returns 0 and FEATURES lacks bit 17.
             */
            bool tp = sources & HV_INPUT_SRC_TRACKPAD;
            bool up = hv_mtp_t8152_attach(a2, a3, a1 & HV_INPUT_OPT_FN_FKEYS, tp);
            bool tp_up = up && tp && hv_mtp_t8152_trackpad_up();
            spin_lock(&input_lock);
            if (up)
                model.features |= INPUT_FEAT_SRC_KBD;
            if (tp_up)
                model.features |= INPUT_FEAT_SRC_TP;
            spin_unlock(&input_lock);
            printf("HV: T8152: input: internal keyboard %s%s\n",
                   up ? "live (FEATURES bit 17)" : "not available; host input only",
                   !tp      ? ""
                   : tp_up  ? "; trackpad live (FEATURES bit 18)"
                            : "; trackpad not available");
            return 0;
        }
        case HV_INPUT_OP_PROBE:
            if (!attached || entered || hv_windows_guest_entered() ||
                !(sources & HV_INPUT_SRC_KEYBOARD)) {
                printf("HV: T8152: input: PROBE needs the internal keyboard, before entry\n");
                return HV_INPUT_ERROR;
            }
            return (u64)hv_mtp_t8152_probe((u32)a1);
        case HV_INPUT_OP_STATUS:
            return status(a1);
        case HV_INPUT_OP_DETACH:
            /* Before entry only (a failed handshake); hv_start then refuses: fail closed. */
            if (hv_windows_guest_entered() || entered) {
                printf("HV: T8152: input: DETACH refused after guest entry\n");
                return HV_INPUT_ERROR;
            }
            __atomic_store_n(&attached, false, __ATOMIC_RELEASE);
            return 0;
        default:
            return HV_INPUT_ERROR;
    }
}

struct hook_ctx {
    u64 covered;
    unsigned int bad;
};

static bool hook_leaf(void *opaque, u64 ipa, u64 size, u64 pte, int level)
{
    struct hook_ctx *ctx = opaque;
    UNUSED(level);
    if (ipa >= HV_INPUT_BASE + HV_INPUT_SIZE || ipa + size <= HV_INPUT_BASE)
        return true;
    /* A software leaf (bit 0 clear) of type SPTE_HOOK (1) whose target is this file's hook. */
    if ((pte & BIT(0)) || FIELD_GET(GENMASK(52, 50), pte) != 1 ||
        (pte & GENMASK(49, 2)) != ((u64)hv_input_mmio & GENMASK(49, 2)))
        ctx->bad++;
    else
        ctx->covered += size;
    return true;
}

int hv_input_verify(void)
{
    if (!input_enabled)
        return 0;
    if (!attached) {
        printf("HV: T8152: input: the configure flag is set but nothing is attached; refusing "
               "guest entry\n");
        return -1;
    }
    struct hook_ctx ctx = {0, 0};
    hv_pt_for_each_leaf(hook_leaf, &ctx);
    if (ctx.bad || ctx.covered < HV_INPUT_SIZE) {
        printf("HV: T8152: input: the page 0x%lx is not wholly this carrier's hook (%u foreign "
               "leaves, 0x%lx bytes hooked); refusing guest entry\n",
               HV_INPUT_BASE, ctx.bad, ctx.covered);
        return -1;
    }
    printf("HV: T8152: input: verified: page 0x%lx hooked, SPI %u, host feed owns the vUART RX\n",
           HV_INPUT_BASE, HV_INPUT_INTID);
    return 0;
}

void hv_input_guest_entered(void)
{
    if (!input_enabled || !attached)
        return;
    /*
     * Bytes that arrived between ATTACH and entry are not the guest session's: the vUART left
     * them in the CDC ring (RX is ours from ATTACH, and the tick reads nothing before entry).
     * Discard them, bounded by the ring's size, and start the parser and the host source clean,
     * so no key, button or position sent before entry reaches the model.  A frame cut by the
     * drain is resynchronised by the parser like any other damage.
     */
    u8 scratch[HV_INPUT_RX_BUDGET];
    u64 dropped = 0;
    bool failed = false;
    spin_lock(&input_lock);
    while (dropped < HV_INPUT_ENTRY_DRAIN_MAX) {
        ssize_t avail = iodev_can_read(IODEV_USB_VUART);
        if (avail <= 0)
            break;
        u32 want = avail > (ssize_t)sizeof(scratch) ? (u32)sizeof(scratch) : (u32)avail;
        ssize_t got = iodev_read(IODEV_USB_VUART, scratch, want);
        if (got <= 0) {
            failed = got < 0;
            break;
        }
        dropped += (u64)got;
    }
    input_model_host_restart(&model);
    /* Keys the internal keyboard held before entry: its next report sends the full state. */
    input_model_release(&model, INPUT_SRC_INTERNAL);
    input_model_release(&model, INPUT_SRC_TRACKPAD);
    model.host_last = now_ticks();
    entered = true;
    spin_unlock(&input_lock);
    if (dropped || failed)
        printf("HV: T8152: input: discarded %lu host byte(s) sent before guest entry%s\n",
               (unsigned long)dropped, failed ? " (a read failed; the rest is parsed)" : "");
}

void hv_input_guest_exited(int status_code)
{
    if (!attached || !entered)
        return;
    spin_lock(&input_lock);
    /*
     * All up, state only: a later session (or the queue's reader, if any) never sees a key
     * held.  `exited` first, so sync_line() leaves the vSPI alone (see there).
     */
    exited = true;
    input_model_release(&model, INPUT_SRC_HOST);
    input_model_release(&model, INPUT_SRC_INTERNAL);
    input_model_release(&model, INPUT_SRC_TRACKPAD);
    spin_unlock(&input_lock);
    printf("HV: T8152: input: guest exit %d: host keys and buttons released%s\n", status_code,
           (sources & HV_INPUT_SRC_KEYBOARD) ? ", internal keyboard and trackpad released (the "
                                               "MTP is left running: leave by a reboot)"
                                             : "");
    hv_input_report();
}

void hv_input_report(void)
{
    if (!attached)
        return;
    u64 now = now_ticks();
    if (!exited && now < report_next)
        return;
    report_next = now + mrs(CNTFRQ_EL0);
    if (sources & HV_INPUT_SRC_KEYBOARD)
        hv_mtp_t8152_report(); /* its own line, when its counters moved */
    spin_lock(&input_lock);
    u32 ok = model.stats[INPUT_ST_FRAMES_OK];
    u32 bad = model.stats[INPUT_ST_BAD_CRC] + model.stats[INPUT_ST_BAD_HEADER] +
              model.stats[INPUT_ST_BAD_PAYLOAD];
    u32 enabled = model.ctrl & INPUT_CTRL_ENABLE;
    u32 depth = input_model_depth(&model);
    u32 dropped = model.stats[INPUT_ST_DROPPED], wd = model.stats[INPUT_ST_WATCHDOG];
    u32 gaps = model.stats[INPUT_ST_SEQ_GAPS], acc = model.stats[INPUT_ST_BAD_ACCESS];
    spin_unlock(&input_lock);
    if (ok == reported_frames && bad == reported_bad && enabled == reported_enabled && !exited)
        return;
    reported_frames = ok;
    reported_bad = bad;
    reported_enabled = enabled;
    printf("HV: input: host frames %u ok, %u bad, %u seq gaps; depth %u, dropped %u, watchdog "
           "%u, bad accesses %u; driver enabled %s\n",
           ok, bad, gaps, depth, dropped, wd, acc, enabled ? "yes" : "no");
}

void hv_input_counts(u32 *ok, u32 *bad)
{
    /* G5b status block: relaxed reads without input_lock (the tick may race the feed by one). */
    *ok = __atomic_load_n(&model.stats[INPUT_ST_FRAMES_OK], __ATOMIC_RELAXED);
    *bad = __atomic_load_n(&model.stats[INPUT_ST_BAD_CRC], __ATOMIC_RELAXED) +
           __atomic_load_n(&model.stats[INPUT_ST_BAD_HEADER], __ATOMIC_RELAXED) +
           __atomic_load_n(&model.stats[INPUT_ST_BAD_PAYLOAD], __ATOMIC_RELAXED);
}

static u64 fatal_age_ms(u64 now, u64 then)
{
    u64 freq = mrs(CNTFRQ_EL0);
    if (!then || then > now || freq < 1000)
        return ~0UL;
    return (now - then) / (freq / 1000);
}

void hv_input_fatal_report(void)
{
    if (!session_used)
        return;
    u64 now = now_ticks();
    u64 acc_age = fatal_age_ms(now, __atomic_load_n(&act_at, __ATOMIC_RELAXED));
    u64 poll_age = fatal_age_ms(now, poll_at);
    printf("HV: fatal: input: attached %d entered %d exited %d; host frames %u ok, %u bad CRC, "
           "%u bad accesses; driver enabled %s; page 0x%lx is a carrier hook (never hardware)\n",
           (int)attached, (int)entered, (int)exited, model.stats[INPUT_ST_FRAMES_OK],
           model.stats[INPUT_ST_BAD_CRC], model.stats[INPUT_ST_BAD_ACCESS],
           (model.ctrl & INPUT_CTRL_ENABLE) ? "yes" : "no", HV_INPUT_BASE);
    printf("HV: fatal: input: %lu guest page accesses", act_accesses);
    if (acc_age != ~0UL)
        printf(", last %s at +0x%lx %lu ms ago", act_write ? "write" : "read", act_off, acc_age);
    printf("; last host-feed poll (CDC link read) ");
    if (poll_age == ~0UL)
        printf("never");
    else
        printf("%lu ms ago", poll_age);
    printf(", %lu bytes in all\n", poll_bytes);
    if (sources & HV_INPUT_SRC_KEYBOARD)
        hv_mtp_t8152_fatal_report();
}

#ifdef HV_INPUT_HOST_TEST
/* tests/e2e/windows_input_glue_host.c: a fresh carrier session between scenarios. */
void hv_input_host_new_session(void)
{
    input_enabled = session_used = attached = entered = exited = false;
    sources = 0;
    line = false;
    logged = 0;
    report_next = 0;
    reported_frames = reported_bad = 0;
    reported_enabled = ~0u;
    act_accesses = act_off = act_write = act_at = 0;
    poll_at = poll_bytes = 0;
}
#endif
