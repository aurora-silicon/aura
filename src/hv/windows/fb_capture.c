/* SPDX-License-Identifier: MIT */

#include "fb_capture.h"

#ifndef FB_CAPTURE_HOST_TEST
#include "string.h"
#include "utils.h"
#include "xnuboot.h"
#endif

/*
 * FNV-1a over the tile's pixels.
 *
 * Not a checksum for integrity -- a change detector. What matters is that a
 * single flipped pixel changes the value (FNV-1a avalanches well enough for
 * that) and that it costs one multiply per word, because this runs at EL2 in
 * the middle of the guest's exits. A 64x64 tile is 4096 words; at that size the
 * loop is memory-bound, which is the point: the pixels are already being
 * touched by the converter.
 *
 * Collisions mean a missed update, not corruption: the tile simply is not
 * re-sent until it changes again. At 2^32 that is not a risk worth a wider
 * hash and the extra bandwidth of carrying it.
 */
u32 fb_capture_tile_hash(const u32 *pixels, u32 stride_px, u32 tile_w, u32 tile_h)
{
    u32 hash = 0x811c9dc5;

    for (u32 y = 0; y < tile_h; y++) {
        const u32 *row = pixels + (size_t)y * stride_px;
        for (u32 x = 0; x < tile_w; x++) {
            hash ^= row[x];
            hash *= 0x01000193;
        }
    }
    /* Never return 0: the descriptor is zeroed at init, so a genuine hash of 0
     * would read as "never hashed" and the first real content would not be
     * reported as dirty. */
    return hash ? hash : 1;
}

void fb_capture_layout(struct fb_capture_desc *desc, u32 width, u32 height)
{
    desc->tile = FB_CAPTURE_TILE;
    /* Round UP: the right and bottom edges are partial tiles when the panel is
     * not a multiple of 64, and dropping them would leave a strip of the screen
     * permanently un-watched. 2560x1664 happens to divide evenly; 1920x1200
     * does not. */
    desc->tiles_x = (width + FB_CAPTURE_TILE - 1) / FB_CAPTURE_TILE;
    desc->tiles_y = (height + FB_CAPTURE_TILE - 1) / FB_CAPTURE_TILE;
    desc->tile_count = desc->tiles_x * desc->tiles_y;
    if (desc->tile_count > FB_CAPTURE_MAX_TILES) {
        /* Fail closed and visibly rather than writing past the arrays. */
        desc->tiles_x = 0;
        desc->tiles_y = 0;
        desc->tile_count = 0;
    }
}

bool fb_capture_note(struct fb_capture_desc *desc, u32 index, u32 hash)
{
    if (index >= desc->tile_count)
        return false;
    if (desc->hash[index] == hash)
        return false;
    desc->hash[index] = hash;
    /* The host CLEARS these bits as it consumes tiles, so this only ever sets.
     * Setting a bit that is already set is correct and idempotent: it means the
     * tile changed again before the host got to it. */
    desc->dirty[index / 32] |= 1u << (index % 32);
    return true;
}

#ifndef FB_CAPTURE_HOST_TEST

static struct fb_capture_desc fb_capture_state;
static u32 fb_capture_cursor;

struct fb_capture_desc *fb_capture_descriptor(void)
{
    return &fb_capture_state;
}

/*
 * Fill in what we know. Called after hv_fb_init() so the shadow fields are
 * populated when a guest is being set up, and safe to call when there is no
 * shadow at all -- the scanout half still describes m1n1's own console.
 */
void fb_capture_init(void)
{
    struct fb_capture_desc *desc = &fb_capture_state;

    memset(desc, 0, sizeof(*desc));
    desc->magic = FB_CAPTURE_MAGIC;
    desc->version = FB_CAPTURE_VERSION;

    desc->scanout.base = cur_boot_args.video.base;
    desc->scanout.width = cur_boot_args.video.width;
    desc->scanout.height = cur_boot_args.video.height;
    desc->scanout.stride_bytes = cur_boot_args.video.stride;
    /* iBoot hands this panel over as 30bpp X2R10G10B10; a depth of 32 means a
     * plain BGRA scanout with no HV conversion in the path. */
    desc->scanout.format =
        (cur_boot_args.video.depth & 0xff) == 32 ? FB_CAPTURE_FMT_BGRA8888 : FB_CAPTURE_FMT_X2RGB10;
    desc->scanout.valid = desc->scanout.base && desc->scanout.height;

    fb_capture_layout(desc, desc->scanout.width, desc->scanout.height);
    desc->hashed_format = desc->scanout.format;
    fb_capture_cursor = 0;
}

/*
 * Publish the guest's shadow once the HV has allocated it. Separate from
 * fb_capture_init() because the shadow does not exist until a guest starts and
 * is re-allocated per guest -- the address moved between two consecutive boots
 * on J813 (0x10005280000 -> 0x10005968000), so nothing may cache it.
 */
void fb_capture_set_shadow(u64 base, u32 width, u32 height, u32 stride_bytes)
{
    struct fb_capture_desc *desc = &fb_capture_state;

    desc->shadow.base = base;
    desc->shadow.width = width;
    desc->shadow.height = height;
    desc->shadow.stride_bytes = stride_bytes;
    desc->shadow.format = FB_CAPTURE_FMT_BGRA8888;
    desc->shadow.valid = base != 0;

    /* Hash the shadow when there is one: it is what the guest has actually
     * drawn, whereas the scanout trails it by however far behind the converter
     * is running. */
    if (desc->shadow.valid) {
        fb_capture_layout(desc, width, height);
        desc->hashed_format = FB_CAPTURE_FMT_BGRA8888;
        fb_capture_cursor = 0;
    }
}

/* Hash `tile_rows` rows of tiles and advance the cursor. */
static void fb_capture_scan_body(u32 tile_rows)
{
    struct fb_capture_desc *desc = &fb_capture_state;
    const struct fb_capture_surface *surface =
        desc->hashed_format == FB_CAPTURE_FMT_BGRA8888 && desc->shadow.valid ? &desc->shadow
                                                                             : &desc->scanout;

    if (!desc->tile_count || !surface->valid || !tile_rows)
        return;

    u32 stride_px = surface->stride_bytes / 4;
    bool changed = false;

    for (u32 n = 0; n < tile_rows; n++) {
        u32 row = __atomic_fetch_add(&fb_capture_cursor, 1, __ATOMIC_RELAXED);
        if (row % desc->tiles_y == 0 && row != 0)
            __atomic_fetch_add(&desc->sweeps, 1, __ATOMIC_RELAXED);
        row %= desc->tiles_y;

        u32 y0 = row * desc->tile;
        u32 tile_h = surface->height - y0;
        if (tile_h > desc->tile)
            tile_h = desc->tile;

        for (u32 tx = 0; tx < desc->tiles_x; tx++) {
            u32 x0 = tx * desc->tile;
            u32 tile_w = surface->width - x0;
            if (tile_w > desc->tile)
                tile_w = desc->tile;

            const u32 *pixels = (const u32 *)(uintptr_t)surface->base + (size_t)y0 * stride_px + x0;
            u32 hash = fb_capture_tile_hash(pixels, stride_px, tile_w, tile_h);
            if (fb_capture_note(desc, row * desc->tiles_x + tx, hash))
                changed = true;
        }
    }

    if (changed)
        __atomic_fetch_add(&desc->seq, 1, __ATOMIC_RELAXED);
}

/*
 * The ONLY entry point, and it is host-driven on purpose.
 *
 * This used to also run from hv_tick() and the WFI idle path, so the detector
 * stayed current on its own. That cost a guest: measured on J813 2026-08-12,
 * pinning the identical Mu/WinPE boot against the m1n1 immediately before this
 * file existed,
 *
 *   with the HV call sites:    2 of 3 boots died on a guest SError (EC=0x2f,
 *                              L2C_ERR_STS 0x82), one at 628 KB of log
 *   without them (0d7b56a5):   717 KB, more NT activity, zero SErrors
 *
 * An 8 ms rate gate reduced the frequency and did NOT fix it, which points at
 * the access pattern rather than the rate: hashing tiles pulls the framebuffer
 * through the MAIR_IDX_NORMAL_NC mapping from inside the guest's own exit
 * paths.
 *
 * Host-driven costs nothing, because the host is already paying. A capture
 * with a guest running happens inside an `hv eval`, which stops the guest for
 * 0.4-1.0 s whatever it does; one extra proxy request to advance the detector
 * disappears into that. And with no guest there is nothing to disturb at all.
 *
 * The rule this encodes: a debug facility may cost the host whatever it likes,
 * but it must not sit on the guest's execution path.
 */
void fb_capture_scan_now(u32 tile_rows)
{
    fb_capture_scan_body(tile_rows);
}

#endif
