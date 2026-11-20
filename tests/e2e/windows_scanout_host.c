/* SPDX-License-Identifier: MIT */

/*
 * G6-S host replay of src/hv/windows/scanout_model.c and the carrier glue hv_scanout.c, both
 * compiled unchanged against stub headers from tests/python/test_windows_t8152_scanout.py,
 * ASan + UBSan.
 *
 * The machine is fake: "guest RAM" and the panel are host arrays (the carrier's identity map
 * makes an IPA a pointer, here too), the page hook is captured from hv_map_hook(), every
 * hv_t8152_gic_spi_level() call is logged, and the counter is a variable.
 *
 * Output: one JSON line per case.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hv.h"
#include "hv_fatal.h"
#include "hv_scanout.h"
#include "hv_t8152.h"
#include "hv_vdev_spi.h"
#include "scanout_model.h"
#include "utils.h"

u32 chip_id = 0x8152, board_id = 0x22;
u64 stub_cntpct = 1000, stub_cntfrq = 24000000, stub_cntvoff = 500;

void hv_scanout_host_new_session(void);

/* ---- harness ----------------------------------------------------------------------------- */

static const char *current;
static int failures, total_failures;

#define CHECK(cond)                                                                              \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            fprintf(stderr, "%s:%d [%s] check failed: %s\n", __FILE__, __LINE__, current, #cond); \
            failures++;                                                                          \
        }                                                                                        \
    } while (0)

static void begin(const char *name)
{
    current = name;
    failures = 0;
}

static void end_extra(const char *fmt, ...)
{
    char extra[256] = "";
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(extra, sizeof(extra), fmt, ap);
        va_end(ap);
    }
    printf("{\"case\":\"%s\",\"result\":\"%s\"%s}\n", current, failures ? "failed" : "passed",
           extra);
    total_failures += failures;
}

static void end(void)
{
    end_extra(NULL);
}

static u64 rng_state = 0x9e3779b97f4a7c15ULL;
static u64 rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

/* ---- the fake machine -------------------------------------------------------------------- */

#define PW 96 /* panel width */
#define PH 64 /* panel height */
#define PSTRIDE (PW * 4 + 128)

static u8 guest_ram[4 << 20] __attribute__((aligned(16384)));
static u8 shadow[PSTRIDE * PH] __attribute__((aligned(64)));
static u32 scanout[(PSTRIDE / 4) * PH] __attribute__((aligned(64)));

bool hv_t8152_guest_ram(u64 *base, u64 *size)
{
    *base = (u64)guest_ram;
    *size = sizeof(guest_ram);
    return true;
}

static hv_hook_t *hook_fn;
static u64 hook_base, hook_size;
static int hook_fail;
int hv_map_hook(u64 from, hv_hook_t *hook, u64 size)
{
    if (hook_fail)
        return -1;
    hook_fn = hook;
    hook_base = from;
    hook_size = size;
    return 0;
}

static int leaf_mode; /* 0 = the hook, 1 = a foreign leaf, 2 = nothing */
void hv_pt_for_each_leaf(hv_pt_leaf_fn *fn, void *ctx)
{
    if (leaf_mode == 2 || !hook_fn)
        return;
    u64 pte = ((u64)hook_fn & GENMASK(49, 2)) | (1UL << 50);
    if (leaf_mode == 1)
        pte |= 1;
    fn(ctx, hook_base, hook_size, pte, 3);
}

static u32 spi_log[4096];
static int spi_count;
static u32 spi_bad_intid;
void hv_t8152_gic_spi_level(u32 intid, bool level)
{
    if (intid != HV_SCANOUT_INTID)
        spi_bad_intid++;
    if (spi_count < 4096)
        spi_log[spi_count++] = level;
}

static bool guest_entered_flag;
bool hv_windows_guest_entered(void)
{
    return guest_entered_flag;
}

int fatal_open;
void hv_fatal_fb_begin(u64 start, u64 end_row)
{
    if (fatal_open || start >= end_row)
        abort();
    fatal_open = 1;
}
void hv_fatal_fb_end(void)
{
    if (!fatal_open)
        abort();
    fatal_open = 0;
}

void dc_civac_range(void *addr, size_t length)
{
    /* Must stay inside guest RAM or the shadow. */
    u8 *p = addr;
    bool in_ram = p >= guest_ram && p + length <= guest_ram + sizeof(guest_ram);
    bool in_shadow = p >= shadow && p + length <= shadow + sizeof(shadow);
    if (!in_ram && !in_shadow)
        abort();
}

/* ---- model helpers ----------------------------------------------------------------------- */

static bool model_range(void *ctx, u64 base, u64 size)
{
    (void)ctx;
    return base >= (u64)guest_ram && base + size <= (u64)guest_ram + sizeof(guest_ram);
}

static void mw(struct scanout_model *m, u32 off, u32 v)
{
    u64 val = v;
    CHECK(scanout_model_mmio(m, off, 2, &val, true));
}

static u32 mr(struct scanout_model *m, u32 off)
{
    u64 val = 0xdead;
    CHECK(scanout_model_mmio(m, off, 2, &val, false));
    return (u32)val;
}

static void model_new(struct scanout_model *m, u32 mhz)
{
    scanout_model_init(m, PW, PH, (u64)shadow, PSTRIDE, mhz, 24000000, model_range, NULL);
    scanout_model_start(m, 1000);
}

static void flip_regs(struct scanout_model *m, u64 addr, u32 stride, u32 fmt)
{
    mw(m, SCANOUT_FLIP_ADDR_LO, (u32)addr);
    mw(m, SCANOUT_FLIP_ADDR_HI, (u32)(addr >> 32));
    mw(m, SCANOUT_FLIP_STRIDE, stride);
    mw(m, SCANOUT_FLIP_FORMAT, fmt);
    mw(m, SCANOUT_FLIP_RECT_TL, 0);
    mw(m, SCANOUT_FLIP_RECT_BR, 0);
}

static void copy_all(struct scanout_model *m, u32 rows)
{
    struct scanout_band b;
    while (scanout_model_claim(m, rows, &b)) {
        scanout_copy_band(scanout, PSTRIDE / 4, &b);
        scanout_model_band_done(m, b.gen, b.y1 - b.y0);
    }
}

#define BUF0 ((u64)guest_ram)
#define BUF1 ((u64)guest_ram + 0x40000)
#define FSTRIDE (PW * 4 + 64)

/* ---- model cases ------------------------------------------------------------------------- */

static void case_reset_values(void)
{
    begin("reset_values");
    struct scanout_model m;
    model_new(&m, 0);
    CHECK(mr(&m, SCANOUT_ID) == SCANOUT_ID_VALUE);
    CHECK(mr(&m, SCANOUT_VERSION_REG) == 1);
    CHECK(mr(&m, SCANOUT_FEATURES) == SCANOUT_FEATURES_V1);
    CHECK(!(mr(&m, SCANOUT_FEATURES) & (SCANOUT_FEAT_HW_VBLANK | SCANOUT_FEAT_RETARGET)));
    CHECK(mr(&m, SCANOUT_WIDTH) == PW && mr(&m, SCANOUT_HEIGHT) == PH);
    CHECK(mr(&m, SCANOUT_PANEL_FORMAT) == SCANOUT_FMT_A2R10G10B10);
    CHECK(mr(&m, SCANOUT_REFRESH_MHZ) == 60000);
    CHECK(mr(&m, SCANOUT_PERIOD_TICKS) == 400000);
    CHECK(mr(&m, SCANOUT_COUNTER_HZ) == 24000000);
    CHECK(mr(&m, SCANOUT_SHADOW_LO) == (u32)(u64)shadow);
    CHECK(mr(&m, SCANOUT_SHADOW_HI) == (u32)((u64)shadow >> 32));
    CHECK(mr(&m, SCANOUT_SHADOW_STRIDE) == PSTRIDE);
    CHECK(mr(&m, SCANOUT_CTRL) == 0 && mr(&m, SCANOUT_STATUS) == 0);
    CHECK(mr(&m, SCANOUT_FLIP_SUBMIT) == 0); /* WO */
    CHECK(!scanout_model_line(&m) && !scanout_model_owns_panel(&m));
    /* RO registers ignore writes */
    mw(&m, SCANOUT_ID, 0);
    mw(&m, SCANOUT_WIDTH, 5);
    mw(&m, SCANOUT_DONE_COOKIE, 9);
    CHECK(mr(&m, SCANOUT_ID) == SCANOUT_ID_VALUE && mr(&m, SCANOUT_WIDTH) == PW &&
          mr(&m, SCANOUT_DONE_COOKIE) == 0);
    end();
}

static void case_vblank_timing(void)
{
    begin("vblank_timing");
    struct scanout_model m;
    model_new(&m, 0);
    /* nothing before the first edge */
    CHECK(!scanout_model_tick(&m, 1000 + 399999, 500));
    CHECK(m.vblank_count == 0);
    CHECK(scanout_model_tick(&m, 1000 + 400000, 500));
    CHECK(m.vblank_count == 1 && m.vblank_time == 1000 + 400000 - 500);
    CHECK(!scanout_model_tick(&m, 1000 + 400001, 500));
    /* the 5 kHz tick: 4800 ticks per 60 Hz period, exactly one event per edge */
    u64 events = 0;
    for (u64 t = 1000 + 400000; t < 1000 + 400000ULL * 601; t += 4800)
        events += scanout_model_tick(&m, t, 500);
    CHECK(m.vblank_count == 600 && events == 599);
    CHECK(m.stats[SCANOUT_STC_MISSED] == 0);
    /* a stalled tick: three edges at once, one event, all counted */
    CHECK(scanout_model_tick(&m, 1000 + 400000ULL * 603 + 7, 500));
    CHECK(m.vblank_count == 603 && m.stats[SCANOUT_STC_MISSED] == 2);
    CHECK(m.vblank_time == 1000 + 400000ULL * 603 - 500);
    /* status latches only with ENABLE */
    CHECK(!(mr(&m, SCANOUT_STATUS) & SCANOUT_ST_VBLANK));
    mw(&m, SCANOUT_CTRL, SCANOUT_CTRL_ENABLE);
    scanout_model_tick(&m, 1000 + 400000ULL * 604, 500);
    CHECK(mr(&m, SCANOUT_STATUS) & SCANOUT_ST_VBLANK);
    u64 v = 0;
    CHECK(scanout_model_mmio(&m, SCANOUT_VBLANK_COUNT_LO, 3, &v, false) && v == 604);
    CHECK(scanout_model_mmio(&m, SCANOUT_VBLANK_TIME_LO, 3, &v, false) &&
          v == 1000 + 400000ULL * 604 - 500);

    /* 59.94 Hz: no drift over ten million edges, every edge = floor(n * F / mhz) */
    struct scanout_model d;
    model_new(&d, 59940);
    u64 f = 24000000ULL * 1000;
    int bad = 0;
    for (u64 n = 1; n < 10000000; n += 9973)
        if (scanout_model_edge(&d, n) != 1000 + (u64)((unsigned __int128)n * f / 59940))
            bad++;
    CHECK(bad == 0);
    CHECK(scanout_model_tick(&d, scanout_model_edge(&d, 123456), 0));
    CHECK(d.vblank_count == 123456);
    CHECK(!scanout_model_tick(&d, scanout_model_edge(&d, 123457) - 1, 0));
    /* 120 Hz */
    struct scanout_model h;
    model_new(&h, 120000);
    CHECK(mr(&h, SCANOUT_PERIOD_TICKS) == 200000);
    end();
}

static void case_status_and_line(void)
{
    begin("status_and_line");
    struct scanout_model m;
    model_new(&m, 0);
    mw(&m, SCANOUT_CTRL, SCANOUT_CTRL_ENABLE);
    scanout_model_tick(&m, scanout_model_edge(&m, 1), 0);
    CHECK(mr(&m, SCANOUT_STATUS) == SCANOUT_ST_VBLANK);
    CHECK(!scanout_model_line(&m)); /* not enabled as an interrupt */
    mw(&m, SCANOUT_CTRL, SCANOUT_CTRL_ENABLE | SCANOUT_CTRL_VBLANK_IRQ_EN);
    CHECK(scanout_model_line(&m));
    mw(&m, SCANOUT_STATUS, SCANOUT_ST_FLIP_DONE); /* other bit: no effect */
    CHECK(scanout_model_line(&m));
    mw(&m, SCANOUT_STATUS, SCANOUT_ST_VBLANK | SCANOUT_ST_PENDING); /* RO bit ignored */
    CHECK(!scanout_model_line(&m) && mr(&m, SCANOUT_STATUS) == 0);
    /* an error raises the line only with ERROR_IRQ_EN */
    mw(&m, SCANOUT_FLIP_SUBMIT, 5); /* FLIP_MODE off: MODE */
    CHECK(mr(&m, SCANOUT_FLIP_ERROR) == SCANOUT_ERR_MODE && mr(&m, SCANOUT_ERROR_COOKIE) == 5);
    CHECK(!scanout_model_line(&m));
    mw(&m, SCANOUT_CTRL, SCANOUT_CTRL_ENABLE | SCANOUT_CTRL_ERROR_IRQ_EN);
    CHECK(scanout_model_line(&m));
    /* ENABLE off clears STATUS and the line */
    mw(&m, SCANOUT_CTRL, SCANOUT_CTRL_ERROR_IRQ_EN);
    CHECK(mr(&m, SCANOUT_STATUS) == 0 && !scanout_model_line(&m));
    mw(&m, SCANOUT_CTRL, 0xffffffff);
    CHECK(mr(&m, SCANOUT_CTRL) == SCANOUT_CTRL_MASK);
    end();
}

static void case_flip_refusals(void)
{
    begin("flip_refusals");
    struct scanout_model m;
    model_new(&m, 0);
    mw(&m, SCANOUT_CTRL, SCANOUT_CTRL_ENABLE);
    flip_regs(&m, BUF0, FSTRIDE, SCANOUT_FMT_BGRA8888);
    mw(&m, SCANOUT_FLIP_SUBMIT, 1);
    CHECK(mr(&m, SCANOUT_FLIP_ERROR) == SCANOUT_ERR_MODE);
    mw(&m, SCANOUT_CTRL, SCANOUT_CTRL_ENABLE | SCANOUT_CTRL_FLIP_MODE);
    CHECK(scanout_model_owns_panel(&m));
    mw(&m, SCANOUT_FLIP_SUBMIT, 0);
    CHECK(mr(&m, SCANOUT_FLIP_ERROR) == SCANOUT_ERR_COOKIE);
    mw(&m, SCANOUT_FLIP_FORMAT, 3);
    mw(&m, SCANOUT_FLIP_SUBMIT, 2);
    CHECK(mr(&m, SCANOUT_FLIP_ERROR) == SCANOUT_ERR_FORMAT);
    mw(&m, SCANOUT_FLIP_FORMAT, SCANOUT_FMT_BGRA8888);
    u32 strides[] = {PW * 4 - 64, PW * 4 + 4, 65536 + 64, 0};
    for (unsigned i = 0; i < 4; i++) {
        mw(&m, SCANOUT_FLIP_STRIDE, strides[i]);
        mw(&m, SCANOUT_FLIP_SUBMIT, 3);
        CHECK(mr(&m, SCANOUT_FLIP_ERROR) == SCANOUT_ERR_STRIDE);
    }
    mw(&m, SCANOUT_FLIP_STRIDE, FSTRIDE);
    u64 addrs[] = {BUF0 + 4, 0, (u64)guest_ram + sizeof(guest_ram) - FSTRIDE * 2,
                   (u64)scanout, ~0UL - 63};
    for (unsigned i = 0; i < 5; i++) {
        mw(&m, SCANOUT_FLIP_ADDR_LO, (u32)addrs[i]);
        mw(&m, SCANOUT_FLIP_ADDR_HI, (u32)(addrs[i] >> 32));
        mw(&m, SCANOUT_FLIP_SUBMIT, 4);
        CHECK(mr(&m, SCANOUT_FLIP_ERROR) == SCANOUT_ERR_ADDR);
    }
    mw(&m, SCANOUT_FLIP_ADDR_LO, (u32)BUF0);
    mw(&m, SCANOUT_FLIP_ADDR_HI, (u32)(BUF0 >> 32));
    u32 rects[][2] = {{5 | 5 << 16, 5 | 9 << 16},       /* x0 == x1 */
                      {5 | 9 << 16, 9 | 9 << 16},       /* y0 == y1 */
                      {0, (PW + 1) | PH << 16},         /* x1 > width */
                      {0, PW | (PH + 1) << 16},         /* y1 > height */
                      {9 | 9 << 16, 5 | 12 << 16}};     /* x0 > x1 */
    for (unsigned i = 0; i < 5; i++) {
        mw(&m, SCANOUT_FLIP_RECT_TL, rects[i][0]);
        mw(&m, SCANOUT_FLIP_RECT_BR, rects[i][1]);
        mw(&m, SCANOUT_FLIP_SUBMIT, 5);
        CHECK(mr(&m, SCANOUT_FLIP_ERROR) == SCANOUT_ERR_RECT);
    }
    CHECK(m.stats[SCANOUT_STC_REFUSED] == 1 + 1 + 1 + 4 + 5 + 5);
    CHECK(m.stats[SCANOUT_STC_SUBMITTED] == 0 && !m.pending_valid && m.slot == SCANOUT_IDLE);
    CHECK(mr(&m, SCANOUT_STATUS) & SCANOUT_ST_FLIP_ERROR);
    /* a good one clears FLIP_ERROR (the register), not the sticky STATUS bit */
    mw(&m, SCANOUT_FLIP_RECT_TL, 0);
    mw(&m, SCANOUT_FLIP_RECT_BR, 0);
    mw(&m, SCANOUT_FLIP_SUBMIT, 6);
    CHECK(mr(&m, SCANOUT_FLIP_ERROR) == 0 && mr(&m, SCANOUT_ACTIVE_COOKIE) == 6);
    end();
}

static void fill_bgra(u64 buf, u32 seed)
{
    for (u32 y = 0; y < PH; y++)
        for (u32 x = 0; x < PW; x++)
            ((u32 *)(buf + (u64)y * FSTRIDE))[x] = seed * 2654435761u ^ (y * 977 + x * 131);
}

static bool scanout_matches(u64 buf, u32 fmt, u32 x0, u32 y0, u32 x1, u32 y1)
{
    for (u32 y = y0; y < y1; y++)
        for (u32 x = x0; x < x1; x++) {
            u32 p = ((u32 *)(buf + (u64)y * FSTRIDE))[x];
            u32 want = fmt == SCANOUT_FMT_BGRA8888 ? scanout_bgra_to_x2r10(p) : p;
            if (scanout[y * (PSTRIDE / 4) + x] != want)
                return false;
        }
    return true;
}

static void case_flip_lifecycle(void)
{
    begin("flip_lifecycle");
    struct scanout_model m;
    model_new(&m, 0);
    memset(scanout, 0, sizeof(scanout));
    fill_bgra(BUF0, 1);
    mw(&m, SCANOUT_CTRL, SCANOUT_CTRL_ENABLE | SCANOUT_CTRL_FLIP_MODE | SCANOUT_CTRL_FLIP_IRQ_EN);
    flip_regs(&m, BUF0, FSTRIDE, SCANOUT_FMT_BGRA8888);
    mw(&m, SCANOUT_FLIP_SUBMIT, 0x101);
    /* nothing in flight: copying at once */
    CHECK(mr(&m, SCANOUT_STATUS) == SCANOUT_ST_COPYING);
    CHECK(mr(&m, SCANOUT_ACTIVE_COOKIE) == 0x101 && mr(&m, SCANOUT_PENDING_COOKIE) == 0);
    struct scanout_band b;
    CHECK(scanout_model_claim(&m, 10, &b) && b.y0 == 0 && b.y1 == 10 && b.x0 == 0 && b.x1 == PW);
    scanout_copy_band(scanout, PSTRIDE / 4, &b);
    scanout_model_band_done(&m, b.gen, 10);
    copy_all(&m, 7);
    CHECK(mr(&m, SCANOUT_STATUS) == SCANOUT_ST_COPIED);
    CHECK(!scanout_model_claim(&m, 10, &b));
    CHECK(scanout_matches(BUF0, SCANOUT_FMT_BGRA8888, 0, 0, PW, PH));
    CHECK(mr(&m, SCANOUT_DONE_COOKIE) == 0 && !scanout_model_line(&m));
    /* reported at the next vblank, with its count */
    scanout_model_tick(&m, scanout_model_edge(&m, 1), 0);
    CHECK(mr(&m, SCANOUT_DONE_COOKIE) == 0x101 && mr(&m, SCANOUT_DONE_VBLANK_LO) == 1);
    CHECK((mr(&m, SCANOUT_STATUS) & 0x7ff) == (SCANOUT_ST_VBLANK | SCANOUT_ST_FLIP_DONE));
    CHECK(scanout_model_line(&m));
    mw(&m, SCANOUT_STATUS, SCANOUT_ST_FLIP_DONE);
    CHECK(!scanout_model_line(&m));
    CHECK(m.stats[SCANOUT_STC_SUBMITTED] == 1 && m.stats[SCANOUT_STC_DONE] == 1 &&
          m.stats[SCANOUT_STC_ROWS] == PH && m.stats[SCANOUT_STC_LATE] == 0);

    /* native A2R10G10B10 with a dirty rectangle: only the rect changes */
    u32 before[(PSTRIDE / 4) * PH];
    memcpy(before, scanout, sizeof(before));
    for (u32 y = 0; y < PH; y++)
        for (u32 x = 0; x < PW; x++)
            ((u32 *)(BUF1 + (u64)y * FSTRIDE))[x] = 0x3fffffffu - (y * PW + x);
    flip_regs(&m, BUF1, FSTRIDE, SCANOUT_FMT_A2R10G10B10);
    mw(&m, SCANOUT_FLIP_RECT_TL, 7 | 11 << 16);
    mw(&m, SCANOUT_FLIP_RECT_BR, 40 | 30 << 16);
    mw(&m, SCANOUT_FLIP_SUBMIT, 0x102);
    CHECK(scanout_model_claim(&m, 100, &b) && b.y0 == 11 && b.y1 == 30 && b.x0 == 7 &&
          b.x1 == 40);
    scanout_copy_band(scanout, PSTRIDE / 4, &b);
    scanout_model_band_done(&m, b.gen, b.y1 - b.y0);
    CHECK(scanout_matches(BUF1, SCANOUT_FMT_A2R10G10B10, 7, 11, 40, 30));
    int outside = 0;
    for (u32 y = 0; y < PH; y++)
        for (u32 x = 0; x < PSTRIDE / 4; x++)
            if (!(y >= 11 && y < 30 && x >= 7 && x < 40) &&
                scanout[y * (PSTRIDE / 4) + x] != before[y * (PSTRIDE / 4) + x])
                outside++;
    CHECK(outside == 0);
    scanout_model_tick(&m, scanout_model_edge(&m, 2), 0);
    CHECK(mr(&m, SCANOUT_DONE_COOKIE) == 0x102 && mr(&m, SCANOUT_DONE_VBLANK_LO) == 2);
    /* 64-bit writes: the rect pair at RECT_TL, then SUBMIT (its high word is the RO error) */
    flip_regs(&m, BUF0, FSTRIDE, SCANOUT_FMT_BGRA8888);
    u64 pair = (u64)(0 | 2 << 16) | (u64)(PW | 5 << 16) << 32;
    CHECK(scanout_model_mmio(&m, SCANOUT_FLIP_RECT_TL, 3, &pair, true));
    pair = 0x103 | (u64)7 << 32;
    CHECK(scanout_model_mmio(&m, SCANOUT_FLIP_SUBMIT, 3, &pair, true));
    CHECK(mr(&m, SCANOUT_FLIP_ERROR) == 0);
    CHECK(m.slot == SCANOUT_COPYING && m.active.y0 == 2 && m.active.y1 == 5 &&
          m.active.cookie == 0x103);
    end();
}

static void case_busy_and_order(void)
{
    begin("busy_and_order");
    struct scanout_model m;
    model_new(&m, 0);
    mw(&m, SCANOUT_CTRL, SCANOUT_CTRL_ENABLE | SCANOUT_CTRL_FLIP_MODE);
    flip_regs(&m, BUF0, FSTRIDE, SCANOUT_FMT_BGRA8888);
    mw(&m, SCANOUT_FLIP_SUBMIT, 1);
    flip_regs(&m, BUF1, FSTRIDE, SCANOUT_FMT_BGRA8888);
    mw(&m, SCANOUT_FLIP_SUBMIT, 2);
    CHECK(mr(&m, SCANOUT_ACTIVE_COOKIE) == 1 && mr(&m, SCANOUT_PENDING_COOKIE) == 2);
    CHECK(mr(&m, SCANOUT_STATUS) == (SCANOUT_ST_PENDING | SCANOUT_ST_COPYING));
    mw(&m, SCANOUT_FLIP_SUBMIT, 3);
    CHECK(mr(&m, SCANOUT_FLIP_ERROR) == SCANOUT_ERR_BUSY && m.stats[SCANOUT_STC_BUSY] == 1);
    CHECK(mr(&m, SCANOUT_PENDING_COOKIE) == 2); /* unchanged */
    /* an edge with the copy unfinished reports nothing and starts nothing */
    struct scanout_band b;
    CHECK(scanout_model_claim(&m, 8, &b));
    scanout_model_tick(&m, scanout_model_edge(&m, 1), 0);
    CHECK(mr(&m, SCANOUT_DONE_COOKIE) == 0 && mr(&m, SCANOUT_ACTIVE_COOKIE) == 1);
    scanout_model_band_done(&m, b.gen, 8);
    copy_all(&m, 16);
    /* a second edge with the copy now done: 1 reported, 2 starts at the same edge */
    scanout_model_tick(&m, scanout_model_edge(&m, 2), 0);
    CHECK(mr(&m, SCANOUT_DONE_COOKIE) == 1 && mr(&m, SCANOUT_ACTIVE_COOKIE) == 2);
    CHECK(m.slot == SCANOUT_COPYING && !m.pending_valid);
    /* 1 started at count 0 and was still copying at edge 2? No: it was copied before edge 2. */
    CHECK(m.stats[SCANOUT_STC_LATE] == 0);
    /* 2 spans edges 3 and 4: late once */
    scanout_model_tick(&m, scanout_model_edge(&m, 3), 0);
    scanout_model_tick(&m, scanout_model_edge(&m, 4), 0);
    scanout_model_tick(&m, scanout_model_edge(&m, 5), 0);
    CHECK(m.stats[SCANOUT_STC_LATE] == 1);
    copy_all(&m, 64);
    scanout_model_tick(&m, scanout_model_edge(&m, 6), 0);
    CHECK(mr(&m, SCANOUT_DONE_COOKIE) == 2 && mr(&m, SCANOUT_DONE_VBLANK_LO) == 6);
    end();
}

static void case_retire_and_stale(void)
{
    begin("retire_and_stale");
    struct scanout_model m;
    model_new(&m, 0);
    mw(&m, SCANOUT_CTRL, SCANOUT_CTRL_ENABLE | SCANOUT_CTRL_FLIP_MODE);
    flip_regs(&m, BUF0, FSTRIDE, SCANOUT_FMT_BGRA8888);
    mw(&m, SCANOUT_FLIP_SUBMIT, 10);
    mw(&m, SCANOUT_FLIP_SUBMIT, 11);
    struct scanout_band b;
    CHECK(scanout_model_claim(&m, 8, &b));
    /* FLIP_MODE off: both retired, the later cookie is DONE, FLIP_DONE latched */
    mw(&m, SCANOUT_CTRL, SCANOUT_CTRL_ENABLE);
    CHECK(!scanout_model_owns_panel(&m));
    CHECK(mr(&m, SCANOUT_DONE_COOKIE) == 11 && m.stats[SCANOUT_STC_RETIRED] == 2);
    CHECK(mr(&m, SCANOUT_STATUS) == SCANOUT_ST_FLIP_DONE);
    /* the band claimed before is stale now: counted, ignored */
    scanout_model_band_done(&m, b.gen, 8);
    CHECK(m.stats[SCANOUT_STC_STALE_BANDS] == 1 && m.stats[SCANOUT_STC_ROWS] == 0);
    CHECK(!scanout_model_claim(&m, 8, &b));
    /* submits are refused until FLIP_MODE is back */
    mw(&m, SCANOUT_FLIP_SUBMIT, 12);
    CHECK(mr(&m, SCANOUT_FLIP_ERROR) == SCANOUT_ERR_MODE);
    mw(&m, SCANOUT_CTRL, SCANOUT_CTRL_ENABLE | SCANOUT_CTRL_FLIP_MODE);
    mw(&m, SCANOUT_FLIP_SUBMIT, 13);
    CHECK(mr(&m, SCANOUT_ACTIVE_COOKIE) == 13);
    /* ENABLE off retires too and clears STATUS */
    mw(&m, SCANOUT_CTRL, 0);
    CHECK(mr(&m, SCANOUT_DONE_COOKIE) == 13 && mr(&m, SCANOUT_STATUS) == 0);
    /* retire (guest exit) with nothing in flight changes nothing */
    u32 retired = m.stats[SCANOUT_STC_RETIRED];
    scanout_model_retire(&m);
    CHECK(m.stats[SCANOUT_STC_RETIRED] == retired);
    end();
}

static void case_copy_formats(void)
{
    begin("copy_formats");
    /* the BGRA mapping is the carrier converter's: value << 2 into each 10-bit field */
    CHECK(scanout_bgra_to_x2r10(0x00000000) == 0);
    CHECK(scanout_bgra_to_x2r10(0xff0000ff) == 0x3fc);
    CHECK(scanout_bgra_to_x2r10(0x0000ff00) == 0x3fc << 10);
    CHECK(scanout_bgra_to_x2r10(0x00ff0000) == 0x3fcu << 20);
    CHECK(scanout_bgra_to_x2r10(0xffffffff) == 0x3fcff3fcu);
    int bad = 0;
    for (u32 i = 0; i < 1000000; i++) {
        u32 p = (u32)rng();
        u32 ref = ((p & 0xff) << 2) | (((p >> 8) & 0xff) << 12) | (((p >> 16) & 0xff) << 22);
        if (scanout_bgra_to_x2r10(p) != ref)
            bad++;
    }
    CHECK(bad == 0);
    end();
}

static void case_access_widths(void)
{
    begin("access_widths");
    struct scanout_model m;
    model_new(&m, 0);
    u64 v = 0x55;
    CHECK(!scanout_model_mmio(&m, SCANOUT_CTRL, 0, &v, false) && v == 0);
    v = 0x55;
    CHECK(!scanout_model_mmio(&m, SCANOUT_CTRL, 1, &v, false) && v == 0);
    v = 0x55;
    CHECK(!scanout_model_mmio(&m, SCANOUT_CTRL + 2, 2, &v, false) && v == 0);
    v = 0x55;
    CHECK(!scanout_model_mmio(&m, SCANOUT_CTRL, 3, &v, false) && v == 0); /* 64-bit at 4-aligned */
    v = SCANOUT_CTRL_ENABLE;
    CHECK(!scanout_model_mmio(&m, SCANOUT_CTRL, 0, &v, true) && m.ctrl == 0);
    v = 0;
    CHECK(!scanout_model_mmio(&m, SCANOUT_PAGE_SIZE, 2, &v, false));
    CHECK(m.stats[SCANOUT_STC_BAD_ACCESS] == 6);
    CHECK(mr(&m, SCANOUT_STATS + 4 * SCANOUT_STC_BAD_ACCESS) == 6);
    /* holes and the rest of the page: RAZ/WI, not bad accesses */
    CHECK(mr(&m, 0x048) == 0 && mr(&m, 0x1fc) == 0 && mr(&m, 0x3ffc) == 0);
    mw(&m, 0x3ffc, 1);
    CHECK(m.stats[SCANOUT_STC_BAD_ACCESS] == 6);
    /* 64-bit reads pair the words */
    v = 0;
    CHECK(scanout_model_mmio(&m, SCANOUT_ID, 3, &v, false) &&
          v == (SCANOUT_ID_VALUE | (u64)SCANOUT_VERSION << 32));
    end();
}

/* Every accepted flip completes exactly once (DONE or RETIRED), in order; the line follows
 * the formula; bands never leave the active rect. */
static void case_fuzz(int iterations)
{
    begin("fuzz");
    struct scanout_model m;
    model_new(&m, 0);
    u64 now = 1000;
    u32 next_cookie = 1, last_done = 0, accepted = 0, completions = 0;
    u32 order[64];
    u32 head = 0, tail = 0; /* accepted cookies not yet complete */
    struct scanout_band held[4];
    int nheld = 0;
    for (int i = 0; i < iterations; i++) {
        u32 r = (u32)rng() % 100;
        u32 before_sub = m.stats[SCANOUT_STC_SUBMITTED];
        if (r < 30) {
            now += rng() % 300000;
            scanout_model_tick(&m, now, 77);
        } else if (r < 55) {
            u64 buf = (rng() & 1) ? BUF0 : BUF1;
            flip_regs(&m, buf, (rng() % 16) ? FSTRIDE : 4, (rng() % 16) ? 1 + (rng() & 1) : 9);
            if (rng() & 1) {
                u32 x0 = rng() % PW, y0 = rng() % PH;
                u32 x1 = x0 + 1 + rng() % (PW - x0), y1 = y0 + 1 + rng() % (PH - y0);
                mw(&m, SCANOUT_FLIP_RECT_TL, x0 | y0 << 16);
                mw(&m, SCANOUT_FLIP_RECT_BR, x1 | y1 << 16);
            }
            u32 c = next_cookie++;
            mw(&m, SCANOUT_FLIP_SUBMIT, c);
            if (m.stats[SCANOUT_STC_SUBMITTED] != before_sub)
                order[tail++ % 64] = c, accepted++;
        } else if (r < 85) {
            if (nheld < 4 && scanout_model_claim(&m, 1 + rng() % 40, &held[nheld])) {
                struct scanout_band *b = &held[nheld];
                CHECK(b->y0 >= m.active.y0 && b->y1 <= m.active.y1 && b->x0 == m.active.x0 &&
                      b->x1 == m.active.x1 && b->y0 < b->y1);
                nheld++;
            }
            if (nheld && (rng() & 1)) {
                int k = (int)(rng() % (u64)nheld);
                scanout_copy_band(scanout, PSTRIDE / 4, &held[k]);
                scanout_model_band_done(&m, held[k].gen, held[k].y1 - held[k].y0);
                held[k] = held[--nheld];
            }
        } else if (r < 95) {
            u32 c = (u32)rng() & SCANOUT_CTRL_MASK;
            if (rng() % 4)
                c |= SCANOUT_CTRL_ENABLE | SCANOUT_CTRL_FLIP_MODE;
            mw(&m, SCANOUT_CTRL, c);
        } else {
            mw(&m, SCANOUT_STATUS, (u32)rng());
        }
        /* completions in submission order */
        u32 done = m.done_cookie;
        if (done != last_done) {
            bool found = false;
            while (head != tail) {
                u32 c = order[head++ % 64];
                completions++;
                if (c == done) {
                    found = true;
                    break;
                }
            }
            CHECK(found);
            last_done = done;
        }
        CHECK(tail - head <= 2);
        CHECK(m.pending_valid ? m.slot != SCANOUT_IDLE : true);
        bool want = (m.ctrl & SCANOUT_CTRL_ENABLE) &&
                    (((m.ctrl & SCANOUT_CTRL_VBLANK_IRQ_EN) && (m.status & SCANOUT_ST_VBLANK)) ||
                     ((m.ctrl & SCANOUT_CTRL_FLIP_IRQ_EN) && (m.status & SCANOUT_ST_FLIP_DONE)) ||
                     ((m.ctrl & SCANOUT_CTRL_ERROR_IRQ_EN) && (m.status & SCANOUT_ST_FLIP_ERROR)));
        CHECK(scanout_model_line(&m) == want);
        if (failures > 20)
            break;
    }
    CHECK(m.stats[SCANOUT_STC_DONE] + m.stats[SCANOUT_STC_RETIRED] >= completions);
    end_extra(",\"iterations\":%d,\"accepted\":%u,\"done\":%u,\"retired\":%u", iterations, accepted,
              m.stats[SCANOUT_STC_DONE], m.stats[SCANOUT_STC_RETIRED]);
}

/* ---- glue cases -------------------------------------------------------------------------- */

static void glue_reset(void)
{
    hv_scanout_host_new_session();
    hook_fn = NULL;
    hook_fail = 0;
    leaf_mode = 0;
    spi_count = 0;
    spi_bad_intid = 0;
    guest_entered_flag = false;
    chip_id = 0x8152;
    board_id = 0x22;
    stub_cntpct = 1000;
}

static void gw(u32 off, u32 v)
{
    u64 val = v;
    hook_fn(NULL, HV_SCANOUT_BASE + off, &val, true, 2);
}

static u32 gr(u32 off)
{
    u64 val = 0;
    hook_fn(NULL, HV_SCANOUT_BASE + off, &val, false, 2);
    return (u32)val;
}

static void case_glue_attach(void)
{
    begin("glue_attach");
    glue_reset();
    CHECK((s64)hv_scanout_proxy(HV_SCANOUT_OP_ATTACH, 0, 0, 0) == HV_SCANOUT_E_STATE);
    hv_scanout_enable();
    board_id = 0x24;
    CHECK((s64)hv_scanout_proxy(HV_SCANOUT_OP_ATTACH, 0, 0, 0) == HV_SCANOUT_E_STATE);
    board_id = 0x22;
    CHECK((s64)hv_scanout_proxy(HV_SCANOUT_OP_ATTACH, 999, 0, 0) == HV_SCANOUT_E_REFRESH);
    CHECK((s64)hv_scanout_proxy(HV_SCANOUT_OP_ATTACH, 240001, 0, 0) == HV_SCANOUT_E_REFRESH);
    guest_entered_flag = true;
    CHECK((s64)hv_scanout_proxy(HV_SCANOUT_OP_ATTACH, 0, 0, 0) == HV_SCANOUT_E_STATE);
    guest_entered_flag = false;
    CHECK(hook_fn == NULL);
    CHECK(hv_scanout_proxy(HV_SCANOUT_OP_QUERY, HV_SCANOUT_Q_REFRESH, 0, 0) == 0);
    CHECK(hv_scanout_verify() < 0); /* flag set, nothing attached */
    hook_fail = 1;
    CHECK((s64)hv_scanout_proxy(HV_SCANOUT_OP_ATTACH, 0, 0, 0) == HV_SCANOUT_E_HOOK);
    /* one ATTACH per session, even after a failed hook */
    hook_fail = 0;
    CHECK((s64)hv_scanout_proxy(HV_SCANOUT_OP_ATTACH, 0, 0, 0) == HV_SCANOUT_E_STATE);

    glue_reset();
    hv_scanout_enable();
    CHECK(hv_scanout_proxy(HV_SCANOUT_OP_ATTACH, 120000, 0, 0) == 0);
    CHECK(hook_base == HV_SCANOUT_BASE && hook_size == HV_SCANOUT_SIZE);
    CHECK((s64)hv_scanout_proxy(HV_SCANOUT_OP_ATTACH, 0, 0, 0) == HV_SCANOUT_E_STATE);
    CHECK(hv_scanout_proxy(HV_SCANOUT_OP_QUERY, HV_SCANOUT_Q_VERSION, 0, 0) == 1);
    CHECK(hv_scanout_proxy(HV_SCANOUT_OP_QUERY, HV_SCANOUT_Q_BASE, 0, 0) == 0xf60000000UL);
    CHECK(hv_scanout_proxy(HV_SCANOUT_OP_QUERY, HV_SCANOUT_Q_SIZE, 0, 0) == 0x4000);
    CHECK(hv_scanout_proxy(HV_SCANOUT_OP_QUERY, HV_SCANOUT_Q_INTID, 0, 0) == 992);
    CHECK(hv_scanout_proxy(HV_SCANOUT_OP_QUERY, HV_SCANOUT_Q_FEATURES, 0, 0) ==
          SCANOUT_FEATURES_V1);
    CHECK(hv_scanout_proxy(HV_SCANOUT_OP_QUERY, HV_SCANOUT_Q_REFRESH, 0, 0) == 120000);
    CHECK(hv_scanout_proxy(HV_SCANOUT_OP_QUERY, 99, 0, 0) == HV_SCANOUT_ERROR);
    CHECK(hv_scanout_proxy(9, 0, 0, 0) == HV_SCANOUT_ERROR);
    /* verify: no panel yet, then a foreign leaf, then good */
    CHECK(hv_scanout_verify() < 0);
    hv_scanout_set_panel((u64)shadow, scanout, PW, PH, PSTRIDE);
    CHECK(hv_scanout_proxy(HV_SCANOUT_OP_STATUS, HV_SCANOUT_S_GEOMETRY, 0, 0) ==
          (PW | (u64)PH << 16 | (u64)PSTRIDE << 32));
    leaf_mode = 1;
    CHECK(hv_scanout_verify() < 0);
    leaf_mode = 2;
    CHECK(hv_scanout_verify() < 0);
    leaf_mode = 0;
    CHECK(hv_scanout_verify() == 0);
    /* DETACH before entry is allowed, after entry refused */
    hv_scanout_guest_entered();
    CHECK(hv_scanout_proxy(HV_SCANOUT_OP_DETACH, 0, 0, 0) == HV_SCANOUT_ERROR);
    end();
}

static void case_glue_session(void)
{
    begin("glue_session");
    glue_reset();
    hv_scanout_enable();
    CHECK(hv_scanout_proxy(HV_SCANOUT_OP_ATTACH, 0, 0, 0) == 0);
    hv_scanout_set_panel((u64)shadow, scanout, PW, PH, PSTRIDE);
    CHECK(hv_scanout_verify() == 0);
    /* nothing before entry */
    CHECK(!hv_scanout_work(1000) && !hv_scanout_owns_panel());
    stub_cntpct = 5000;
    hv_scanout_guest_entered();
    CHECK(gr(SCANOUT_ID) == SCANOUT_ID_VALUE && gr(SCANOUT_WIDTH) == PW);
    CHECK(gr(SCANOUT_SHADOW_LO) == (u32)(u64)shadow);
    /* the driver: enable, vblank IRQ */
    gw(SCANOUT_CTRL, SCANOUT_CTRL_ENABLE | SCANOUT_CTRL_VBLANK_IRQ_EN);
    CHECK(!hv_scanout_owns_panel());
    stub_cntpct = 5000 + 400000;
    hv_scanout_tick();
    CHECK(spi_count == 1 && spi_log[0] == 1 && spi_bad_intid == 0);
    CHECK(gr(SCANOUT_VBLANK_COUNT_LO) == 1);
    CHECK(gr(SCANOUT_VBLANK_TIME_LO) == (u32)(5000 + 400000 - stub_cntvoff));
    hv_scanout_tick(); /* no change: no call */
    CHECK(spi_count == 1);
    gw(SCANOUT_STATUS, SCANOUT_ST_VBLANK);
    CHECK(spi_count == 2 && spi_log[1] == 0);

    /* flips own the panel; a guest-RAM BGRA frame lands in the scanout through work() */
    gw(SCANOUT_CTRL, SCANOUT_CTRL_ENABLE | SCANOUT_CTRL_FLIP_MODE | SCANOUT_CTRL_FLIP_IRQ_EN);
    CHECK(hv_scanout_owns_panel());
    memset(scanout, 0, sizeof(scanout));
    fill_bgra(BUF0, 7);
    gw(SCANOUT_FLIP_ADDR_LO, (u32)BUF0);
    gw(SCANOUT_FLIP_ADDR_HI, (u32)(BUF0 >> 32));
    gw(SCANOUT_FLIP_STRIDE, FSTRIDE);
    gw(SCANOUT_FLIP_FORMAT, SCANOUT_FMT_BGRA8888);
    gw(SCANOUT_FLIP_SUBMIT, 0x55);
    CHECK(gr(SCANOUT_FLIP_ERROR) == 0);
    int bands = 0;
    while (hv_scanout_work(HV_SCANOUT_FAST_ROWS))
        bands++;
    CHECK(bands == (PH + HV_SCANOUT_FAST_ROWS - 1) / HV_SCANOUT_FAST_ROWS);
    CHECK(!fatal_open);
    CHECK(scanout_matches(BUF0, SCANOUT_FMT_BGRA8888, 0, 0, PW, PH));
    CHECK(gr(SCANOUT_STATUS) & SCANOUT_ST_COPIED);
    CHECK(gr(SCANOUT_DONE_COOKIE) == 0);
    stub_cntpct = 5000 + 800000;
    hv_scanout_tick();
    CHECK(gr(SCANOUT_DONE_COOKIE) == 0x55 && gr(SCANOUT_DONE_VBLANK_LO) == 2);
    CHECK(spi_count == 3 && spi_log[2] == 1);
    CHECK(hv_scanout_proxy(HV_SCANOUT_OP_STATUS, HV_SCANOUT_S_DONE_COOKIE, 0, 0) == 0x55);
    CHECK(hv_scanout_proxy(HV_SCANOUT_OP_STATUS, SCANOUT_STC_DONE, 0, 0) == 1);
    CHECK(hv_scanout_proxy(HV_SCANOUT_OP_STATUS, HV_SCANOUT_S_LINE, 0, 0) == 1);
    CHECK(hv_scanout_proxy(HV_SCANOUT_OP_STATUS, HV_SCANOUT_S_OWNS_PANEL, 0, 0) == 1);
    gw(SCANOUT_STATUS, SCANOUT_ST_W1C);
    CHECK(spi_count == 4 && spi_log[3] == 0);

    /* the shadow itself is a valid source; the scanout and foreign memory are not */
    gw(SCANOUT_FLIP_ADDR_LO, (u32)(u64)shadow);
    gw(SCANOUT_FLIP_ADDR_HI, (u32)((u64)shadow >> 32));
    gw(SCANOUT_FLIP_STRIDE, PSTRIDE);
    gw(SCANOUT_FLIP_SUBMIT, 0x56);
    CHECK(gr(SCANOUT_FLIP_ERROR) == 0);
    while (hv_scanout_work(HV_SCANOUT_IDLE_ROWS))
        ;
    stub_cntpct = 5000 + 1200000;
    hv_scanout_tick();
    CHECK(gr(SCANOUT_DONE_COOKIE) == 0x56);
    gw(SCANOUT_FLIP_ADDR_LO, (u32)(u64)scanout);
    gw(SCANOUT_FLIP_ADDR_HI, (u32)((u64)scanout >> 32));
    gw(SCANOUT_FLIP_SUBMIT, 0x57);
    CHECK(gr(SCANOUT_FLIP_ERROR) == SCANOUT_ERR_ADDR);

    /* bad widths through the hook: RAZ/WI, never a guest abort */
    u64 v[2] = {0x77, 0x77};
    CHECK(hook_fn(NULL, HV_SCANOUT_BASE + 1, v, false, 0) && v[0] == 0);
    v[0] = v[1] = 0x77;
    CHECK(hook_fn(NULL, HV_SCANOUT_BASE, v, false, 4) && v[0] == 0 && v[1] == 0);

    /* the tick's floor band copies too */
    gw(SCANOUT_FLIP_ADDR_LO, (u32)BUF1);
    gw(SCANOUT_FLIP_ADDR_HI, (u32)(BUF1 >> 32));
    gw(SCANOUT_FLIP_STRIDE, FSTRIDE);
    fill_bgra(BUF1, 9);
    gw(SCANOUT_FLIP_SUBMIT, 0x58);
    int ticks = 0;
    for (; ticks < 100 && !(gr(SCANOUT_STATUS) & SCANOUT_ST_COPIED); ticks++) {
        stub_cntpct += 4800;
        hv_scanout_tick();
    }
    CHECK(ticks == (PH + HV_SCANOUT_TICK_ROWS - 1) / HV_SCANOUT_TICK_ROWS);
    CHECK(scanout_matches(BUF1, SCANOUT_FMT_BGRA8888, 0, 0, PW, PH));

    /* exit: retired, the SPI is not driven afterwards */
    gw(SCANOUT_FLIP_ADDR_LO, (u32)BUF0);
    gw(SCANOUT_FLIP_ADDR_HI, (u32)(BUF0 >> 32));
    stub_cntpct += 400000;
    hv_scanout_tick();
    gw(SCANOUT_FLIP_SUBMIT, 0x59);
    int before = spi_count;
    hv_scanout_guest_exited(0);
    CHECK(!hv_scanout_owns_panel() && !hv_scanout_work(1000));
    gw(SCANOUT_CTRL, SCANOUT_CTRL_ENABLE | SCANOUT_CTRL_FLIP_IRQ_EN | SCANOUT_CTRL_VBLANK_IRQ_EN);
    stub_cntpct += 400000;
    hv_scanout_tick();
    CHECK(spi_count == before && spi_bad_intid == 0);
    end();
}

int main(int argc, char **argv)
{
    int iterations = argc > 1 ? atoi(argv[1]) : 200000;
    case_reset_values();
    case_vblank_timing();
    case_status_and_line();
    case_flip_refusals();
    case_flip_lifecycle();
    case_busy_and_order();
    case_retire_and_stale();
    case_copy_formats();
    case_access_widths();
    case_fuzz(iterations);
    case_glue_attach();
    case_glue_session();
    return total_failures ? 1 : 0;
}
