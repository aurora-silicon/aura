/* SPDX-License-Identifier: MIT */

#ifndef HV_SCANOUT_H
#define HV_SCANOUT_H

#include "types.h"

/*
 * G6-S: the scanout service for the J804 Windows guest's WDDM driver.  The device model is
 * scanout_model.c; this is its carrier glue: the trapped register page, the virtual SPI, the
 * vblank clock on the host tick, the band copier on every CPU's unlocked paths, and the proxy
 * op the launcher drives (proxyclient/aura/windows.py, tools/run_guest.py --windows-scanout).
 *
 * Contract (docs/windows-t8152.md "G6-S"; out/g6-scanout/INTERFACE.md; Mu J804Windows.h must
 * agree):
 *   page   IPA 0xF60000000, 16 KiB, hooked, never mapped;
 *   SPI    GSIV 992, level, active-high, no AIC line;
 *   vblank the system counter at REFRESH_MHZ (ATTACH a1; 0 = 60000), from guest entry;
 *   copy   bands of HV_SCANOUT_*_ROWS rows from the flipped guest buffer into the iBoot
 *          scanout, on idle CPUs (unlocked), on the 1 kHz FIQ fast path of every CPU that is
 *          not the tick's (unlocked), and a floor on the 5 kHz tick (under the big lock);
 *   panel  while CTRL.ENABLE and CTRL.FLIP_MODE are set, hv_fb_convert_slice() leaves the
 *          scanout alone (the BGRA shadow sweep stops); clearing either gives it back;
 *   Mu     /chosen/aura,windows-scanout-v1 (32 bytes, written by the launcher only after
 *          ATTACH returned 0) and the FD marker "J804SCN1" at 0x150; GPU0 then carries the
 *          page and the SPI as its resources 5 and 6.
 *
 * Requires the BGRA shadow (configure HV_WINDOWS_FB_SHADOW): hv_start refuses the session
 * otherwise.  The DCP is never touched: no swap, no vblank from it, no change to its denylist.
 */

#define HV_SCANOUT_BASE    0xf60000000UL
#define HV_SCANOUT_SIZE    0x4000UL
#define HV_SCANOUT_INTID   992u
#define HV_SCANOUT_VERSION 1
#define HV_SCANOUT_ERROR   ((u64) - 1)

/* Rows per band.  A 3024-pixel row is ~2.3 us of repacking (hv.h: 5.2 GB/s measured). */
#define HV_SCANOUT_IDLE_ROWS 160u /* a CPU about to park: the converter's own idle slice */
#define HV_SCANOUT_FAST_ROWS 32u  /* the 1 kHz FIQ fast path: ~75 us stolen from a busy CPU */
#define HV_SCANOUT_TICK_ROWS 16u  /* the 5 kHz tick, under the big lock: a floor only */
/* Refresh rates ATTACH accepts, in mHz. */
#define HV_SCANOUT_MIN_MHZ 1000u
#define HV_SCANOUT_MAX_MHZ 240000u

/* P_HV_WINDOWS_CAPS2 bit 7: the G6-S scanout service (this file, configure BIT(11)). */
#define HV_WINDOWS_CAPS2_SCANOUT BIT(7)

/* P_HV_WINDOWS_SCANOUT args[0]. */
enum hv_scanout_op {
    HV_SCANOUT_OP_QUERY = 0,  /* a1 = hv_scanout_query index */
    HV_SCANOUT_OP_ATTACH = 1, /* a1 = refresh in mHz (0 = 60000) */
    HV_SCANOUT_OP_STATUS = 2, /* a1 = hv_scanout_status index */
    HV_SCANOUT_OP_DETACH = 3, /* before guest entry only; hv_start then refuses */
};

enum hv_scanout_query {
    HV_SCANOUT_Q_VERSION = 0,
    HV_SCANOUT_Q_BASE = 1,
    HV_SCANOUT_Q_SIZE = 2,
    HV_SCANOUT_Q_INTID = 3,
    HV_SCANOUT_Q_FEATURES = 4, /* the page's FEATURES */
    HV_SCANOUT_Q_REFRESH = 5,  /* mHz after ATTACH, else 0 */
};

/* STATUS: 0..10 are the model's SCANOUT_STC_* counters; the rest below. */
enum hv_scanout_status {
    HV_SCANOUT_S_ATTACHED = 16,
    HV_SCANOUT_S_CTRL = 17,
    HV_SCANOUT_S_LINE = 18,
    HV_SCANOUT_S_VBLANK_COUNT = 19,
    HV_SCANOUT_S_DONE_COOKIE = 20,
    HV_SCANOUT_S_OWNS_PANEL = 21,
    HV_SCANOUT_S_GEOMETRY = 22, /* width | height << 16 | shadow stride << 32 (0 before entry) */
};

enum hv_scanout_err {
    HV_SCANOUT_E_STATE = -1,   /* not configured, not T8152/J804, guest started, or attached */
    HV_SCANOUT_E_REFRESH = -2, /* refresh outside [HV_SCANOUT_MIN_MHZ, HV_SCANOUT_MAX_MHZ] */
    HV_SCANOUT_E_HOOK = -4,    /* the page could not be hooked */
};

/* hv_windows_configure(HV_WINDOWS_T8152_SCANOUT). */
void hv_scanout_enable(void);
bool hv_scanout_enabled(void);

/* P_HV_WINDOWS_SCANOUT. */
u64 hv_scanout_proxy(u64 op, u64 a1, u64 a2, u64 a3);

/* hv_fb_init(): the shadow the guest sees and the scanout the panel shows. */
void hv_scanout_set_panel(u64 shadow, u32 *scanout, u32 width, u32 height, u32 stride_bytes);

/* hv_start(): with the flag set, refuse unless ATTACH succeeded, the page is wholly this
 * file's hook and the panel is known.  0, or -1 with the reason. */
int hv_scanout_verify(void);
void hv_scanout_guest_entered(void);
void hv_scanout_guest_exited(int status);

/* The host tick (tick CPU, big lock held): the vblank clock, the SPI, a floor band. */
void hv_scanout_tick(void);
/* An unlocked path with time to spare: at most one band of `rows`.  True if it copied. */
bool hv_scanout_work(u32 rows);
/* Does the flip path own the panel?  hv_fb_convert_slice() stands down while it does. */
bool hv_scanout_owns_panel(void);

/* The 1 Hz HV report: one line when the counters moved. */
void hv_scanout_report(void);
void hv_scanout_fatal_report(void);

#endif
