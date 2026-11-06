/* SPDX-License-Identifier: MIT */

#ifndef FB_CAPTURE_H
#define FB_CAPTURE_H

#ifdef FB_CAPTURE_HOST_TEST
/* Host build: tests/fb_capture/ exercises the pure logic natively. Same shim
 * style as src/hv_xfer.h. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef unsigned long u64;
#else
#include "types.h"
#endif

/*
 * fb_capture: let the host see the screen without stopping the guest.
 *
 * THE PROBLEM THIS SOLVES IS NOT BANDWIDTH, IT IS THE VM EXIT.
 *
 * A host-initiated read of the framebuffer traps out of the guest and holds it
 * there for the whole transfer. Measured on J813 2026-08-12: the proxy moves
 * ~6 MiB/s, and one 2560x1664 frame is 16.25 MiB, so a full-frame screenshot
 * stopped the guest for ~2.9 s. Windows sees that as a time jump, and multi-
 * second stalls are DPC/clock-watchdog bugcheck territory (0x133, 0x101). A
 * screenshot tool must not be able to wedge the thing it is looking at.
 *
 * So the host must be told WHAT CHANGED before it reads anything. This file
 * maintains, at EL2, a per-tile hash of the framebuffer plus a dirty bitmap.
 * The host reads the descriptor (a few KiB), and then only the tiles whose
 * hash moved:
 *
 *   static screen   -> a few KiB instead of 16.25 MiB
 *   moving cursor   -> ~4 tiles, ~64 KiB, ~10 ms of stall, which is ordinary
 *                      DPC latency rather than a watchdog event
 *
 * THE SCAN IS HOST-DRIVEN. It does NOT run from the HV's exit paths -- doing
 * that killed guests with SErrors (see fb_capture_scan_now() in fb_capture.c
 * for the A/B that established it). The host advances it with
 * P_FB_CAPTURE_SCAN as part of the same call it was already making to read
 * the descriptor, so nothing here executes while the guest runs.
 *
 * TWO SURFACES, DELIBERATELY.
 *
 *   scanout - what the display controller DMAs to the panel. Allocated by
 *             iBoot, reused by m1n1, and written by everything afterwards, so
 *             it is valid with or without a guest: m1n1's own console, Mu, and
 *             (via hv_fb_convert_slice) the guest. This is the one to read
 *             when there is no guest at all.
 *   shadow  - the guest's BGRA buffer, present only while a guest runs.
 *
 * They are not interchangeable. The converter is rate-adaptive -- measured at
 * 51552 lines/s under load but 64 lines/s when the guest is idle, i.e. ~26 s
 * for a full 1664-line refresh -- so the scanout can lag the shadow badly.
 * The scanout is the truth about the PANEL; the shadow is the truth about what
 * the GUEST has drawn. The host picks, so both are described here.
 */

#define FB_CAPTURE_MAGIC   0x50424346 /* 'FBCP' little-endian */
#define FB_CAPTURE_VERSION 1

/* 64x64 px. Small enough that a cursor dirties ~4 tiles, large enough that the
 * bitmap for a 2560x1664 panel is 40x26 = 1040 tiles = 130 bytes. */
#define FB_CAPTURE_TILE 64
/* Headroom to 4096x2304 at this tile size (64x36 = 2304). */
#define FB_CAPTURE_MAX_TILES   2560
#define FB_CAPTURE_DIRTY_WORDS ((FB_CAPTURE_MAX_TILES + 31) / 32)

#define FB_CAPTURE_FMT_NONE     0
#define FB_CAPTURE_FMT_BGRA8888 1 /* guest shadow */
#define FB_CAPTURE_FMT_X2RGB10  2 /* hardware scanout */

struct fb_capture_surface {
    u64 base;
    u32 width;
    u32 height;
    u32 stride_bytes;
    u32 format;
    u32 valid;
    u32 pad;
};

/*
 * Stable ABI: mirrored by tools/aurora_fb_capture.py. Field order and sizes
 * must not change without bumping FB_CAPTURE_VERSION -- the host asserts on it
 * rather than guessing.
 */
struct fb_capture_desc {
    u32 magic;
    u32 version;
    /* Bumped once per scan pass that found ANY change. The host can poll this
     * one word and skip the frame entirely when it has not moved -- the whole
     * cost of a static screen. */
    u32 seq;
    /* Bumped on every completed sweep of the surface, changed or not, so a
     * host can tell "nothing moved" from "the scanner is not running". */
    u32 sweeps;
    u32 tile;
    u32 tiles_x;
    u32 tiles_y;
    u32 tile_count;
    /* Which surface the hashes describe: FB_CAPTURE_FMT_*. */
    u32 hashed_format;
    u32 reserved;
    struct fb_capture_surface scanout;
    struct fb_capture_surface shadow;
    u32 hash[FB_CAPTURE_MAX_TILES];
    u32 dirty[FB_CAPTURE_DIRTY_WORDS];
};

/*
 * The ABI is pinned by assertion rather than by __attribute__((packed)).
 * Packing would make seq/sweeps potentially unaligned, and they are the two
 * fields updated with __atomic_fetch_add from several cores at once -- an
 * unaligned atomic is exactly the kind of fault that only shows up on real
 * hardware under load. Every field here is naturally aligned as written.
 */
#define FB_CAPTURE_ASSERT_OFFSET(field, expect)                                                    \
    _Static_assert(__builtin_offsetof(struct fb_capture_desc, field) == (expect),                  \
                   "fb_capture ABI: " #field " moved")
FB_CAPTURE_ASSERT_OFFSET(magic, 0);
FB_CAPTURE_ASSERT_OFFSET(seq, 8);
FB_CAPTURE_ASSERT_OFFSET(sweeps, 12);
FB_CAPTURE_ASSERT_OFFSET(tile_count, 28);
FB_CAPTURE_ASSERT_OFFSET(scanout, 40);
FB_CAPTURE_ASSERT_OFFSET(shadow, 72);
FB_CAPTURE_ASSERT_OFFSET(hash, 104);
FB_CAPTURE_ASSERT_OFFSET(dirty, 104 + 4 * FB_CAPTURE_MAX_TILES);
_Static_assert(sizeof(struct fb_capture_surface) == 32, "fb_capture surface ABI");

/* Pure logic, host-testable. */
u32 fb_capture_tile_hash(const u32 *pixels, u32 stride_px, u32 tile_w, u32 tile_h);
void fb_capture_layout(struct fb_capture_desc *desc, u32 width, u32 height);
bool fb_capture_note(struct fb_capture_desc *desc, u32 index, u32 hash);

#ifndef FB_CAPTURE_HOST_TEST
/* Target side. */
void fb_capture_init(void);
void fb_capture_scan_now(u32 tile_rows);
void fb_capture_set_shadow(u64 base, u32 width, u32 height, u32 stride_bytes);
struct fb_capture_desc *fb_capture_descriptor(void);
#endif

#endif
