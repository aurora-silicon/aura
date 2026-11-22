/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef SCANOUT_MODEL_H
#define SCANOUT_MODEL_H

#include "types.h"

/*
 * G6-S: the scanout service's device model (pure; host-tested by
 * tests/e2e/windows_scanout_host.c).  The carrier glue is hv_scanout.c; the guest contract is
 * docs/windows-t8152.md "G6-S" (and out/g6-scanout/INTERFACE.md for the WDDM driver).
 *
 * WHAT THE PANEL IS TODAY.  iBoot leaves the internal panel scanning one X2R10G10B10 surface
 * (the boot framebuffer); the DCP that does it is not the carrier's and stays denied to the
 * guest (the stage-2 denylist's dcp* entries; GPU plan section 7).  The guest draws into a BGRA
 * shadow and the carrier repacks it into that surface from idle cores (hv_fb_convert_slice), a
 * free-running sweep with no notion of frames.
 *
 * WHAT THIS ADDS.  A page the WDDM driver drives with frames instead of a sweep:
 *  - a virtual vblank at the panel rate (60 Hz unless the launcher says otherwise) from the
 *    system counter, with a 64-bit count and the guest-virtual time of each edge;
 *  - FLIP: "show this guest buffer" (BGRA8888 or the panel's own A2R10G10B10, any 64-byte
 *    aligned stride, optional dirty rectangle).  The carrier copies (or converts) the buffer
 *    into the scanout in row bands on several CPUs and reports it done at the first vblank after
 *    the copy finished.  The scanout base itself never moves: retargeting it needs the DCP.
 *
 * One flip is in flight (copying, or copied and waiting for its vblank) and one more may wait
 * behind it; a third is refused with BUSY and changes nothing.  The guest owns a buffer again
 * once DONE_COOKIE names its flip (or a later one).
 */

#define SCANOUT_PAGE_SIZE 0x4000u
#define SCANOUT_ID_VALUE  0x4f53364du /* "M6SO" */
#define SCANOUT_VERSION   1u

/* Registers (byte offsets; 32-bit, 64-bit reads/writes at 8-aligned offsets also allowed). */
enum scanout_reg {
    SCANOUT_ID = 0x000,
    SCANOUT_VERSION_REG = 0x004,
    SCANOUT_FEATURES = 0x008,
    SCANOUT_CTRL = 0x00c,
    SCANOUT_STATUS = 0x010,
    SCANOUT_WIDTH = 0x014,
    SCANOUT_HEIGHT = 0x018,
    SCANOUT_PANEL_FORMAT = 0x01c,
    SCANOUT_REFRESH_MHZ = 0x020,
    SCANOUT_PERIOD_TICKS = 0x024,
    SCANOUT_SHADOW_LO = 0x028,
    SCANOUT_SHADOW_HI = 0x02c,
    SCANOUT_SHADOW_STRIDE = 0x030,
    SCANOUT_COUNTER_HZ = 0x034,
    SCANOUT_VBLANK_COUNT_LO = 0x038,
    SCANOUT_VBLANK_COUNT_HI = 0x03c,
    SCANOUT_VBLANK_TIME_LO = 0x040,
    SCANOUT_VBLANK_TIME_HI = 0x044,
    SCANOUT_FLIP_ADDR_LO = 0x080,
    SCANOUT_FLIP_ADDR_HI = 0x084,
    SCANOUT_FLIP_STRIDE = 0x088,
    SCANOUT_FLIP_FORMAT = 0x08c,
    SCANOUT_FLIP_RECT_TL = 0x090, /* x0 | y0 << 16 */
    SCANOUT_FLIP_RECT_BR = 0x094, /* x1 | y1 << 16, exclusive; TL = BR = 0: the whole frame */
    SCANOUT_FLIP_SUBMIT = 0x098,  /* WO: the cookie (nonzero) */
    SCANOUT_FLIP_ERROR = 0x09c,   /* the last refusal's SCANOUT_ERR_* */
    SCANOUT_PENDING_COOKIE = 0x0a0,
    SCANOUT_ACTIVE_COOKIE = 0x0a4,
    SCANOUT_DONE_COOKIE = 0x0a8,
    SCANOUT_ERROR_COOKIE = 0x0ac,
    SCANOUT_DONE_VBLANK_LO = 0x0b0,
    SCANOUT_DONE_VBLANK_HI = 0x0b4,
    SCANOUT_STATS = 0x100, /* SCANOUT_ST_COUNT x 32-bit counters, RO */
    SCANOUT_REG_END = 0x200,
};

/* FEATURES */
#define SCANOUT_FEAT_VBLANK_TIMER BIT(0) /* the vblank comes from the system counter */
#define SCANOUT_FEAT_FLIP_COPY    BIT(1) /* FLIP copies into the fixed scanout */
#define SCANOUT_FEAT_FMT_BGRA     BIT(2) /* SCANOUT_FMT_BGRA8888 accepted */
#define SCANOUT_FEAT_FMT_A2R10    BIT(3) /* SCANOUT_FMT_A2R10G10B10 accepted */
#define SCANOUT_FEAT_DIRTY_RECT   BIT(4)
#define SCANOUT_FEAT_HW_VBLANK    BIT(5) /* never set by this version: no DCP vblank */
#define SCANOUT_FEAT_RETARGET     BIT(6) /* never set by this version: the base never moves */
#define SCANOUT_FEATURES_V1                                                                      \
    (SCANOUT_FEAT_VBLANK_TIMER | SCANOUT_FEAT_FLIP_COPY | SCANOUT_FEAT_FMT_BGRA |                 \
     SCANOUT_FEAT_FMT_A2R10 | SCANOUT_FEAT_DIRTY_RECT)

/* CTRL */
#define SCANOUT_CTRL_ENABLE        BIT(0) /* the driver is present; STATUS bits latch */
#define SCANOUT_CTRL_FLIP_MODE     BIT(1) /* frames own the panel; the shadow sweep stops */
#define SCANOUT_CTRL_VBLANK_IRQ_EN BIT(2)
#define SCANOUT_CTRL_FLIP_IRQ_EN   BIT(3)
#define SCANOUT_CTRL_ERROR_IRQ_EN  BIT(4)
#define SCANOUT_CTRL_MASK          0x1fu

/* STATUS: bits 0-2 write-one-to-clear; 8-10 read-only state. */
#define SCANOUT_ST_VBLANK      BIT(0)
#define SCANOUT_ST_FLIP_DONE   BIT(1)
#define SCANOUT_ST_FLIP_ERROR  BIT(2)
#define SCANOUT_ST_W1C         0x7u
#define SCANOUT_ST_PENDING     BIT(8)
#define SCANOUT_ST_COPYING     BIT(9)
#define SCANOUT_ST_COPIED      BIT(10)

/* Pixel formats (FLIP_FORMAT, PANEL_FORMAT). */
#define SCANOUT_FMT_BGRA8888    1u /* B in bits 0-7 ... X/A in 24-31: DXGI B8G8R8A8/B8G8R8X8 */
#define SCANOUT_FMT_A2R10G10B10 2u /* B 0-9, G 10-19, R 20-29: VK_FORMAT_A2R10G10B10_UNORM_PACK32 */

/* FLIP_ERROR codes. */
enum scanout_err {
    SCANOUT_ERR_NONE = 0,
    SCANOUT_ERR_BUSY = 1,   /* a flip is already waiting behind the one in flight */
    SCANOUT_ERR_ADDR = 2,   /* not 64-byte aligned, or not wholly inside guest RAM */
    SCANOUT_ERR_STRIDE = 3, /* < WIDTH * 4, > 65536, or not a multiple of 64 */
    SCANOUT_ERR_FORMAT = 4,
    SCANOUT_ERR_RECT = 5,
    SCANOUT_ERR_MODE = 6,   /* CTRL.ENABLE and CTRL.FLIP_MODE are not both set */
    SCANOUT_ERR_COOKIE = 7, /* cookie 0 */
};

/* Counters at SCANOUT_STATS + 4 * index. */
enum scanout_stat {
    SCANOUT_STC_VBLANKS = 0,     /* vblank edges taken (a late tick takes several at once) */
    SCANOUT_STC_SUBMITTED = 1,   /* flips accepted */
    SCANOUT_STC_DONE = 2,        /* flips reported done */
    SCANOUT_STC_REFUSED = 3,     /* flips refused (any error) */
    SCANOUT_STC_BUSY = 4,        /* of those, BUSY */
    SCANOUT_STC_LATE = 5,        /* flips whose copy spanned a whole vblank period */
    SCANOUT_STC_ROWS = 6,        /* scanout rows written by flips */
    SCANOUT_STC_RETIRED = 7,     /* flips dropped by FLIP_MODE off or RESET (reported done) */
    SCANOUT_STC_BAD_ACCESS = 8,  /* bad widths, alignment or offsets */
    SCANOUT_STC_MISSED = 9,      /* vblank edges that fell between two ticks (count - events) */
    SCANOUT_STC_STALE_BANDS = 10, /* band completions for a frame no longer in flight */
    SCANOUT_STC_COUNT = 11,
};

/* A flip as accepted: what to copy where. */
struct scanout_frame {
    u64 src;
    u32 stride;
    u32 format;
    u32 x0, y0, x1, y1;
    u32 cookie;
};

/* One unit of copy work handed to a CPU (rows [y0, y1) of the frame's columns [x0, x1)). */
struct scanout_band {
    u64 src;
    u32 stride;
    u32 format;
    u32 x0, x1, y0, y1;
    u32 gen;
};

typedef bool(scanout_range_fn)(void *ctx, u64 base, u64 size);

enum scanout_slot { SCANOUT_IDLE = 0, SCANOUT_COPYING = 1, SCANOUT_COPIED = 2 };

struct scanout_model {
    /* panel and timing (scanout_model_init / scanout_model_start) */
    u32 width, height;
    u32 refresh_mhz;
    u64 freq;
    u64 shadow;
    u32 shadow_stride;
    u64 epoch;     /* host counter at the vblank sequence start */
    u64 next_edge; /* host counter of the next vblank edge */
    u64 vblank_count;
    u64 vblank_time; /* guest-virtual counter at the last edge */
    bool started;

    u32 ctrl, status;

    /* the flip staging registers */
    u64 flip_addr;
    u32 flip_stride, flip_format, rect_tl, rect_br;
    u32 last_error;

    /* the two slots */
    bool pending_valid;
    struct scanout_frame pending;
    enum scanout_slot slot;
    struct scanout_frame active;
    u32 next_row;  /* claim cursor inside [active.y0, active.y1) */
    u32 rows_done; /* completed rows of the active frame */
    u32 gen;       /* bumps whenever the active frame changes; stale bands are ignored */
    u64 active_start_count; /* vblank_count when the copy started */

    u32 done_cookie, error_cookie;
    u64 done_vblank;
    u32 stats[SCANOUT_STC_COUNT];

    scanout_range_fn *range_ok;
    void *range_ctx;
};

/*
 * Panel geometry and timing.  refresh_mhz 0 means 60000.  `range_ok` decides whether a flip
 * source [base, base + size) is guest memory the carrier may read.
 */
void scanout_model_init(struct scanout_model *m, u32 width, u32 height, u64 shadow,
                        u32 shadow_stride, u32 refresh_mhz, u64 freq, scanout_range_fn *range_ok,
                        void *range_ctx);
/* The vblank sequence starts at host counter `now` (guest entry): first edge one period on. */
void scanout_model_start(struct scanout_model *m, u64 now);

/* Host counter of vblank edge n (n = 1 is the first after the epoch). */
u64 scanout_model_edge(const struct scanout_model *m, u64 n);

/*
 * The vblank clock.  `now` is the host counter, `cntvoff` the guest's CNTVOFF_EL2.  Returns
 * true when at least one edge passed (one event, however many edges: they are all counted).
 */
bool scanout_model_tick(struct scanout_model *m, u64 now, u64 cntvoff);

/* Register access: width 2 (32-bit) or 3 (64-bit), naturally aligned.  False: RAZ/WI. */
bool scanout_model_mmio(struct scanout_model *m, u64 off, int width, u64 *val, bool write);

/* The SPI level. */
bool scanout_model_line(const struct scanout_model *m);

/* Do frames own the panel (CTRL.ENABLE and FLIP_MODE)? */
bool scanout_model_owns_panel(const struct scanout_model *m);

/* Take up to `max_rows` rows of the active frame; false when there is nothing to copy. */
bool scanout_model_claim(struct scanout_model *m, u32 max_rows, struct scanout_band *band);
/* A band claimed with generation `gen` finished `rows` rows. */
void scanout_model_band_done(struct scanout_model *m, u32 gen, u32 rows);

/* Guest exit or RESET: drop both slots (reported done), clear STATUS, CTRL stays. */
void scanout_model_retire(struct scanout_model *m);

/*
 * Copy one band into the scanout (dst = the scanout's first pixel, dst_stride_px its stride).
 * BGRA8888 is repacked exactly as hv_fb_convert_slice does; A2R10G10B10 is copied.
 */
void scanout_copy_band(u32 *dst, u32 dst_stride_px, const struct scanout_band *band);

/* BGRA8888 -> the panel's X2R10G10B10, the carrier converter's mapping (value << 2). */
static inline u32 scanout_bgra_to_x2r10(u32 p)
{
    return ((p & 0xff) << 2) | (((p >> 8) & 0xff) << 12) | (((p >> 16) & 0xff) << 22);
}

#endif
