/* SPDX-License-Identifier: MIT */

/*
 * G6-S carrier glue for the scanout service model.  See hv_scanout.h for the contract.  Written
 * on this branch; no third-party source.  The model (scanout_model.c) is pure and host-tested;
 * this file is host-tested unchanged against stub headers (tests/e2e/windows_scanout_host.c).
 */

#include "hv_scanout.h"
#include "hv.h"
#include "hv_fatal.h"
#include "hv_t8152.h"
#include "hv_vdev_spi.h"
#include "memory.h"
#include "scanout_model.h"
#include "soc.h"
#include "utils.h"

enum {
    LOG_WIDTH = BIT(0),
    LOG_PANEL = BIT(1),
};

static bool scanout_enabled;
static bool session_used;
static bool attached;
static bool entered;
static bool exited;
static bool line;
static u32 logged;
static u32 refresh_mhz;
static struct scanout_model model;
static DECLARE_SPINLOCK(scanout_lock);

/* the panel, from hv_fb_init() */
static bool panel_set;
static u64 panel_shadow;
static u32 *panel_scanout;
static u32 panel_width, panel_height, panel_stride;

/* lock-free hints for the hot paths, rewritten under the lock after every model change */
static bool hint_copying;
static bool hint_owns;

/* the 1 Hz report */
static u64 report_next;
static u32 rep_vblanks, rep_done, rep_submitted, rep_refused, rep_rows;
static u32 reported_ctrl = ~0u;
/* fatal report */
static u64 act_accesses, act_off, act_write, act_at;
static u64 tick_at, band_at;

_Static_assert(HV_SCANOUT_INTID == HV_VDEV_SPI_SCANOUT, "one SPI number");
_Static_assert(HV_SCANOUT_SIZE == SCANOUT_PAGE_SIZE, "one page size");

void hv_scanout_enable(void)
{
    scanout_enabled = true;
}

bool hv_scanout_enabled(void)
{
    return scanout_enabled;
}

static u64 now_ticks(void)
{
    return mrs(CNTPCT_EL0);
}

/* Under scanout_lock. */
static void update_hints(void)
{
    __atomic_store_n(&hint_copying, model.slot == SCANOUT_COPYING, __ATOMIC_RELEASE);
    __atomic_store_n(&hint_owns, scanout_model_owns_panel(&model), __ATOMIC_RELEASE);
}

/* Under scanout_lock.  On changes only; never after the guest exited (see hv_input.c). */
static void sync_line(void)
{
    if (exited || !entered)
        return;
    bool level = scanout_model_line(&model);
    if (level == line)
        return;
    line = level;
    hv_t8152_gic_spi_level(HV_SCANOUT_INTID, level);
}

/*
 * A flip source is guest RAM (identity mapped: IPA == PA, normal cacheable memory the carrier
 * may read) or the BGRA shadow itself.  Never the scanout, MMIO, the AHCI store, the MTP area
 * or anything else the guest cannot reach.
 */
static bool range_ok(void *ctx, u64 base, u64 size)
{
    UNUSED(ctx);
    u64 ram_base, ram_size;
    if (!size || base > ~0UL - size)
        return false;
    if (hv_t8152_guest_ram(&ram_base, &ram_size) && base >= ram_base &&
        base + size <= ram_base + ram_size)
        return true;
    u64 shadow_size = (u64)panel_stride * panel_height;
    return panel_set && base >= panel_shadow && base + size <= panel_shadow + shadow_size;
}

/* ---- the page hook ----------------------------------------------------------------------- */

static bool hv_scanout_mmio(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    UNUSED(ctx);
    u64 off = addr - HV_SCANOUT_BASE;
    __atomic_add_fetch(&act_accesses, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&act_off, off, __ATOMIC_RELAXED);
    __atomic_store_n(&act_write, write ? 1 : 0, __ATOMIC_RELAXED);
    __atomic_store_n(&act_at, now_ticks(), __ATOMIC_RELAXED);

    spin_lock(&scanout_lock);
    bool ok = off < HV_SCANOUT_SIZE && scanout_model_mmio(&model, off, width, val, write);
    if (!ok && !write)
        for (int i = 0; width > 3 && i < (1 << (width - 3)) && i < HV_MAX_RW_WORDS; i++)
            val[i] = 0;
    update_hints();
    sync_line();
    spin_unlock(&scanout_lock);

    if (!ok && !(logged & LOG_WIDTH)) {
        logged |= LOG_WIDTH;
        printf("HV: T8152: scanout: %s of %d bytes at page+0x%lx is not an aligned 32/64-bit "
               "access: %s (logged once)\n",
               write ? "write" : "read", 1 << width, off, write ? "ignored" : "reads 0");
    }
    return true; /* RAZ/WI, never a guest abort */
}

/* ---- the copier and the clock ------------------------------------------------------------ */

bool hv_scanout_owns_panel(void)
{
    return __atomic_load_n(&hint_owns, __ATOMIC_ACQUIRE) && entered && !exited;
}

bool hv_scanout_work(u32 rows)
{
    /* The cost when nothing is in flight: two loads, no lock. */
    if (!__atomic_load_n(&hint_copying, __ATOMIC_ACQUIRE) || !entered || exited)
        return false;

    struct scanout_band band;
    spin_lock(&scanout_lock);
    bool got = scanout_model_claim(&model, rows, &band);
    if (!got)
        update_hints();
    spin_unlock(&scanout_lock);
    if (!got)
        return false;

    /* Fenced and recorded like the converter's bands (hv_fatal.h): an SError these stores
     * raise is attributed to the scanout, not to the guest. */
    hv_fatal_fb_begin(band.y0, band.y1);
    /* Clean and invalidate the source rows first: the guest may have written them through a
     * non-cacheable or write-combining mapping, and a line this carrier read before must not
     * be served stale (dirty guest lines are written back, never discarded). */
    for (u32 y = band.y0; y < band.y1; y++)
        dc_civac_range((void *)(band.src + (u64)y * band.stride + (u64)band.x0 * 4),
                       (size_t)(band.x1 - band.x0) * 4);
    scanout_copy_band(panel_scanout, panel_stride / 4, &band);
    hv_fatal_fb_end();
    __atomic_store_n(&band_at, now_ticks(), __ATOMIC_RELAXED);

    spin_lock(&scanout_lock);
    scanout_model_band_done(&model, band.gen, band.y1 - band.y0);
    update_hints();
    spin_unlock(&scanout_lock);
    return true;
}

void hv_scanout_tick(void)
{
    if (!attached || !entered || exited)
        return;
    u64 now = now_ticks();
    spin_lock(&scanout_lock);
    tick_at = now;
    scanout_model_tick(&model, now, mrs(CNTVOFF_EL2));
    update_hints();
    sync_line();
    spin_unlock(&scanout_lock);
    hv_scanout_work(HV_SCANOUT_TICK_ROWS);
}

/* ---- ATTACH and the rest of the lifecycle ------------------------------------------------ */

static s64 attach(u64 mhz)
{
    if (!scanout_enabled || chip_id != T8152 || board_id != 0x22) {
        printf("HV: T8152: scanout: attach refused: the scanout configure flag is not set (or "
               "this is not J804)\n");
        return HV_SCANOUT_E_STATE;
    }
    if (session_used || hv_windows_guest_entered()) {
        printf("HV: T8152: scanout: attach refused: already attached this carrier session, or "
               "the guest started\n");
        return HV_SCANOUT_E_STATE;
    }
    if (!mhz)
        mhz = 60000;
    if (mhz < HV_SCANOUT_MIN_MHZ || mhz > HV_SCANOUT_MAX_MHZ) {
        printf("HV: T8152: scanout: attach refused: refresh %lu mHz is outside [%u, %u]\n", mhz,
               HV_SCANOUT_MIN_MHZ, HV_SCANOUT_MAX_MHZ);
        return HV_SCANOUT_E_REFRESH;
    }
    session_used = true;
    if (hv_map_hook(HV_SCANOUT_BASE, hv_scanout_mmio, HV_SCANOUT_SIZE) < 0) {
        printf("HV: T8152: scanout: attach refused: cannot hook the page at 0x%lx\n",
               HV_SCANOUT_BASE);
        return HV_SCANOUT_E_HOOK;
    }
    refresh_mhz = (u32)mhz;
    /* Geometry 0 until hv_fb_init(): the page reads WIDTH/HEIGHT 0 and refuses every flip. */
    scanout_model_init(&model, 0, 0, 0, 0, refresh_mhz, mrs(CNTFRQ_EL0), range_ok, NULL);
    __atomic_store_n(&attached, true, __ATOMIC_RELEASE);
    printf("HV: T8152: scanout: attached; page 0x%lx hooked, SPI %u, vblank %u.%03u Hz from the "
           "system counter, copy into the iBoot scanout (no DCP)\n",
           HV_SCANOUT_BASE, HV_SCANOUT_INTID, refresh_mhz / 1000, refresh_mhz % 1000);
    return 0;
}

void hv_scanout_set_panel(u64 shadow, u32 *scanout, u32 width, u32 height, u32 stride_bytes)
{
    if (!attached)
        return;
    spin_lock(&scanout_lock);
    panel_shadow = shadow;
    panel_scanout = scanout;
    panel_width = width;
    panel_height = height;
    panel_stride = stride_bytes;
    panel_set = shadow && scanout && width && height && width <= 0xffff && height <= 0xffff &&
                stride_bytes >= width * 4;
    if (panel_set)
        scanout_model_init(&model, width, height, shadow, stride_bytes, refresh_mhz,
                           mrs(CNTFRQ_EL0), range_ok, NULL);
    spin_unlock(&scanout_lock);
    if (panel_set)
        printf("HV: T8152: scanout: panel %ux%u, stride %u; shadow 0x%lx, scanout %p\n", width,
               height, stride_bytes, shadow, scanout);
}

static u64 query(u64 index)
{
    switch (index) {
        case HV_SCANOUT_Q_VERSION:
            return HV_SCANOUT_VERSION;
        case HV_SCANOUT_Q_BASE:
            return HV_SCANOUT_BASE;
        case HV_SCANOUT_Q_SIZE:
            return HV_SCANOUT_SIZE;
        case HV_SCANOUT_Q_INTID:
            return HV_SCANOUT_INTID;
        case HV_SCANOUT_Q_FEATURES:
            return SCANOUT_FEATURES_V1;
        case HV_SCANOUT_Q_REFRESH:
            return __atomic_load_n(&attached, __ATOMIC_ACQUIRE) ? refresh_mhz : 0;
        default:
            return HV_SCANOUT_ERROR;
    }
}

static u64 status(u64 index)
{
    u64 v;
    spin_lock(&scanout_lock);
    if (index < SCANOUT_STC_COUNT) {
        v = model.stats[index];
    } else {
        switch (index) {
            case HV_SCANOUT_S_ATTACHED:
                v = attached;
                break;
            case HV_SCANOUT_S_CTRL:
                v = model.ctrl;
                break;
            case HV_SCANOUT_S_LINE:
                v = line;
                break;
            case HV_SCANOUT_S_VBLANK_COUNT:
                v = model.vblank_count;
                break;
            case HV_SCANOUT_S_DONE_COOKIE:
                v = model.done_cookie;
                break;
            case HV_SCANOUT_S_OWNS_PANEL:
                v = scanout_model_owns_panel(&model);
                break;
            case HV_SCANOUT_S_GEOMETRY:
                v = panel_set ? panel_width | (u64)panel_height << 16 | (u64)panel_stride << 32
                              : 0;
                break;
            default:
                v = HV_SCANOUT_ERROR;
                break;
        }
    }
    spin_unlock(&scanout_lock);
    return v;
}

u64 hv_scanout_proxy(u64 op, u64 a1, u64 a2, u64 a3)
{
    UNUSED(a2);
    UNUSED(a3);
    switch (op) {
        case HV_SCANOUT_OP_QUERY:
            return query(a1);
        case HV_SCANOUT_OP_ATTACH: {
            spin_lock(&scanout_lock);
            s64 ret = attach(a1);
            spin_unlock(&scanout_lock);
            return (u64)ret;
        }
        case HV_SCANOUT_OP_STATUS:
            return status(a1);
        case HV_SCANOUT_OP_DETACH:
            if (hv_windows_guest_entered() || entered) {
                printf("HV: T8152: scanout: DETACH refused after guest entry\n");
                return HV_SCANOUT_ERROR;
            }
            __atomic_store_n(&attached, false, __ATOMIC_RELEASE);
            return 0;
        default:
            return HV_SCANOUT_ERROR;
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
    if (ipa >= HV_SCANOUT_BASE + HV_SCANOUT_SIZE || ipa + size <= HV_SCANOUT_BASE)
        return true;
    if ((pte & BIT(0)) || FIELD_GET(GENMASK(52, 50), pte) != 1 ||
        (pte & GENMASK(49, 2)) != ((u64)hv_scanout_mmio & GENMASK(49, 2)))
        ctx->bad++;
    else
        ctx->covered += size;
    return true;
}

int hv_scanout_verify(void)
{
    if (!scanout_enabled)
        return 0;
    if (!attached) {
        printf("HV: T8152: scanout: the configure flag is set but nothing is attached; refusing "
               "guest entry\n");
        return -1;
    }
    if (!panel_set) {
        printf("HV: T8152: scanout: no BGRA shadow panel (--windows-fb-shadow is required); "
               "refusing guest entry\n");
        return -1;
    }
    struct hook_ctx ctx = {0, 0};
    hv_pt_for_each_leaf(hook_leaf, &ctx);
    if (ctx.bad || ctx.covered < HV_SCANOUT_SIZE) {
        printf("HV: T8152: scanout: the page 0x%lx is not wholly this carrier's hook (%u foreign "
               "leaves, 0x%lx bytes hooked); refusing guest entry\n",
               HV_SCANOUT_BASE, ctx.bad, ctx.covered);
        return -1;
    }
    printf("HV: T8152: scanout: verified: page 0x%lx hooked, SPI %u, panel %ux%u\n",
           HV_SCANOUT_BASE, HV_SCANOUT_INTID, panel_width, panel_height);
    return 0;
}

void hv_scanout_guest_entered(void)
{
    if (!scanout_enabled || !attached)
        return;
    spin_lock(&scanout_lock);
    scanout_model_start(&model, now_ticks());
    entered = true;
    spin_unlock(&scanout_lock);
}

void hv_scanout_guest_exited(int status_code)
{
    if (!attached || !entered)
        return;
    spin_lock(&scanout_lock);
    exited = true;
    scanout_model_retire(&model);
    update_hints();
    spin_unlock(&scanout_lock);
    printf("HV: T8152: scanout: guest exit %d: flips retired; the panel shows the last copied "
           "frame\n",
           status_code);
    hv_scanout_report();
}

void hv_scanout_report(void)
{
    if (!attached || !entered)
        return;
    u64 now = now_ticks();
    if (!exited && now < report_next)
        return;
    report_next = now + mrs(CNTFRQ_EL0);
    spin_lock(&scanout_lock);
    u32 vb = model.stats[SCANOUT_STC_VBLANKS], done = model.stats[SCANOUT_STC_DONE];
    u32 sub = model.stats[SCANOUT_STC_SUBMITTED], ref = model.stats[SCANOUT_STC_REFUSED];
    u32 rows = model.stats[SCANOUT_STC_ROWS], late = model.stats[SCANOUT_STC_LATE];
    u32 busy = model.stats[SCANOUT_STC_BUSY], missed = model.stats[SCANOUT_STC_MISSED];
    u32 ctrl = model.ctrl;
    spin_unlock(&scanout_lock);
    /* Silent while the driver has not enabled anything and nothing moved. */
    if (done == rep_done && sub == rep_submitted && ref == rep_refused && ctrl == reported_ctrl &&
        !exited && !(ctrl & SCANOUT_CTRL_ENABLE)) {
        rep_vblanks = vb;
        return;
    }
    printf("HV: scanout: %u vblank/s, %u flip/s, %u rows/s; total %u submitted, %u done, %u "
           "refused (%u busy), %u late, %u missed edges; ctrl 0x%x%s\n",
           vb - rep_vblanks, done - rep_done, rows - rep_rows, sub, done, ref, busy, late, missed,
           ctrl, (ctrl & SCANOUT_CTRL_FLIP_MODE) ? " (flips own the panel)" : "");
    rep_vblanks = vb;
    rep_done = done;
    rep_submitted = sub;
    rep_refused = ref;
    rep_rows = rows;
    reported_ctrl = ctrl;
}

static u64 fatal_age_ms(u64 now, u64 then)
{
    u64 freq = mrs(CNTFRQ_EL0);
    if (!then || then > now || freq < 1000)
        return ~0UL;
    return (now - then) / (freq / 1000);
}

void hv_scanout_fatal_report(void)
{
    if (!session_used)
        return;
    u64 now = now_ticks();
    printf("HV: fatal: scanout: attached %d entered %d exited %d; ctrl 0x%x slot %d; %u flips "
           "done, %u rows; page 0x%lx is a carrier hook (never hardware)\n",
           (int)attached, (int)entered, (int)exited, model.ctrl, (int)model.slot,
           model.stats[SCANOUT_STC_DONE], model.stats[SCANOUT_STC_ROWS], HV_SCANOUT_BASE);
    printf("HV: fatal: scanout: %lu guest page accesses, last %s at +0x%lx %lu ms ago; last "
           "tick %lu ms ago, last band %lu ms ago\n",
           act_accesses, act_write ? "write" : "read", act_off,
           fatal_age_ms(now, __atomic_load_n(&act_at, __ATOMIC_RELAXED)),
           fatal_age_ms(now, tick_at), fatal_age_ms(now, __atomic_load_n(&band_at, __ATOMIC_RELAXED)));
}

#ifdef HV_SCANOUT_HOST_TEST
/* tests/e2e/windows_scanout_host.c: a fresh carrier session between scenarios. */
void hv_scanout_host_new_session(void)
{
    scanout_enabled = session_used = attached = entered = exited = false;
    line = false;
    logged = 0;
    refresh_mhz = 0;
    panel_set = false;
    panel_shadow = 0;
    panel_scanout = NULL;
    panel_width = panel_height = panel_stride = 0;
    hint_copying = hint_owns = false;
    report_next = 0;
    rep_vblanks = rep_done = rep_submitted = rep_refused = rep_rows = 0;
    reported_ctrl = ~0u;
    act_accesses = act_off = act_write = act_at = 0;
    tick_at = band_at = 0;
}
#endif
