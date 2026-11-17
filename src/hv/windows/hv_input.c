/* SPDX-License-Identifier: MIT */

/*
 * I0 carrier glue for the input device model.  See hv_input.h for the contract and
 * windows-m6/INPUT-DESIGN.md for the reasoning.  Written on this branch; no third-party source.
 * The model (input_model.c) is pure and host-tested; this file is host-tested unchanged
 * against stub headers (tests/e2e/windows_input_glue_host.c).
 */

#include "hv_input.h"
#include "hv.h"
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

void hv_input_poll(void)
{
    if (!attached || !entered || exited || !(sources & HV_INPUT_SRC_HOST))
        return;

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
    if (!mask || (mask & ~(u64)(HV_INPUT_SRC_HOST | HV_INPUT_SRC_KEYBOARD |
                                HV_INPUT_SRC_TRACKPAD))) {
        printf("HV: T8152: input: attach refused: bad source mask 0x%lx\n", mask);
        return HV_INPUT_E_SOURCES;
    }
    if (mask & ~(u64)HV_INPUT_SRC_SUPPORTED) {
        printf("HV: T8152: input: attach refused: source mask 0x%lx asks for the internal "
               "keyboard/trackpad, which this carrier does not run yet (host only)\n",
               mask);
        return HV_INPUT_E_SOURCES;
    }
    if (area_base || area_size) {
        printf("HV: T8152: input: attach refused: the host source takes no reserved area "
               "(0x%lx +0x%lx)\n",
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
    sources = (u32)mask;
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
            return INPUT_FEAT_REPORTS | INPUT_FEAT_SRC_HOST;
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
            return (u64)ret;
        }
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
    spin_unlock(&input_lock);
    printf("HV: T8152: input: guest exit %d: host keys and buttons released\n", status_code);
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
}
#endif
