/* SPDX-License-Identifier: GPL-2.0-only */

/*
 * G6-S scanout service model.  See scanout_model.h.  Written on this branch from the carrier's
 * own converter (hv.c hv_fb_convert_slice) and the Arm generic timer; no third-party source.
 */

#include "scanout_model.h"
#include "string.h"

#define SCANOUT_DEFAULT_MHZ 60000u

static u64 edge_scale(const struct scanout_model *m)
{
    return m->freq * 1000;
}

void scanout_model_init(struct scanout_model *m, u32 width, u32 height, u64 shadow,
                        u32 shadow_stride, u32 refresh_mhz, u64 freq, scanout_range_fn *range_ok,
                        void *range_ctx)
{
    memset(m, 0, sizeof(*m));
    m->width = width;
    m->height = height;
    m->shadow = shadow;
    m->shadow_stride = shadow_stride;
    m->refresh_mhz = refresh_mhz ? refresh_mhz : SCANOUT_DEFAULT_MHZ;
    m->freq = freq;
    m->range_ok = range_ok;
    m->range_ctx = range_ctx;
}

u64 scanout_model_edge(const struct scanout_model *m, u64 n)
{
    /* epoch + n * F / mhz with F = freq * 1000, split so no product overflows for any n a
     * session reaches (n * r < n * mhz). */
    u64 f = edge_scale(m), q = f / m->refresh_mhz, r = f % m->refresh_mhz;
    return m->epoch + n * q + (n * r) / m->refresh_mhz;
}

void scanout_model_start(struct scanout_model *m, u64 now)
{
    m->epoch = now;
    m->vblank_count = 0;
    m->next_edge = scanout_model_edge(m, 1);
    m->started = true;
}

static u64 edges_by(const struct scanout_model *m, u64 now)
{
    if (now < m->epoch)
        return 0;
    u64 f = edge_scale(m), el = now - m->epoch;
    u64 n = (el / f) * m->refresh_mhz + ((el % f) * m->refresh_mhz) / f;
    while (scanout_model_edge(m, n + 1) <= now)
        n++;
    while (n && scanout_model_edge(m, n) > now)
        n--;
    return n;
}

static bool enabled(const struct scanout_model *m)
{
    return m->ctrl & SCANOUT_CTRL_ENABLE;
}

static void start_pending(struct scanout_model *m)
{
    if (m->slot != SCANOUT_IDLE || !m->pending_valid)
        return;
    m->active = m->pending;
    m->pending_valid = false;
    m->slot = SCANOUT_COPYING;
    m->next_row = m->active.y0;
    m->rows_done = 0;
    m->gen++;
    m->active_start_count = m->vblank_count;
}

static void complete_active(struct scanout_model *m)
{
    m->done_cookie = m->active.cookie;
    m->done_vblank = m->vblank_count;
    m->slot = SCANOUT_IDLE;
    m->gen++;
    m->stats[SCANOUT_STC_DONE]++;
    if (enabled(m))
        m->status |= SCANOUT_ST_FLIP_DONE;
}

bool scanout_model_tick(struct scanout_model *m, u64 now, u64 cntvoff)
{
    if (!m->started || now < m->next_edge)
        return false;
    u64 n = edges_by(m, now);
    if (n <= m->vblank_count) {
        m->next_edge = scanout_model_edge(m, m->vblank_count + 1);
        return false;
    }
    u64 passed = n - m->vblank_count;
    m->vblank_count = n;
    u64 edge = scanout_model_edge(m, n);
    m->vblank_time = edge - cntvoff;
    m->next_edge = scanout_model_edge(m, n + 1);
    m->stats[SCANOUT_STC_VBLANKS] += (u32)passed;
    m->stats[SCANOUT_STC_MISSED] += (u32)(passed - 1);
    if (enabled(m))
        m->status |= SCANOUT_ST_VBLANK;

    if (m->slot == SCANOUT_COPIED) {
        complete_active(m);
    } else if (m->slot == SCANOUT_COPYING && n >= m->active_start_count + 2 &&
               n - passed < m->active_start_count + 2) {
        /* The copy has now spanned a whole period (counted once per flip). */
        m->stats[SCANOUT_STC_LATE]++;
    }
    start_pending(m);
    return true;
}

void scanout_model_retire(struct scanout_model *m)
{
    bool any = false;
    if (m->slot != SCANOUT_IDLE) {
        m->done_cookie = m->active.cookie;
        m->stats[SCANOUT_STC_RETIRED]++;
        any = true;
    }
    if (m->pending_valid) {
        m->done_cookie = m->pending.cookie;
        m->stats[SCANOUT_STC_RETIRED]++;
        any = true;
    }
    m->pending_valid = false;
    m->slot = SCANOUT_IDLE;
    m->gen++;
    if (any) {
        m->done_vblank = m->vblank_count;
        if (enabled(m))
            m->status |= SCANOUT_ST_FLIP_DONE;
    }
}

static void refuse(struct scanout_model *m, u32 cookie, u32 err)
{
    m->last_error = err;
    m->error_cookie = cookie;
    m->stats[SCANOUT_STC_REFUSED]++;
    if (err == SCANOUT_ERR_BUSY)
        m->stats[SCANOUT_STC_BUSY]++;
    if (enabled(m))
        m->status |= SCANOUT_ST_FLIP_ERROR;
}

static u32 check_flip(struct scanout_model *m, u32 cookie, struct scanout_frame *f)
{
    if (!enabled(m) || !(m->ctrl & SCANOUT_CTRL_FLIP_MODE))
        return SCANOUT_ERR_MODE;
    if (!cookie)
        return SCANOUT_ERR_COOKIE;
    if (m->pending_valid)
        return SCANOUT_ERR_BUSY;
    if (m->flip_format != SCANOUT_FMT_BGRA8888 && m->flip_format != SCANOUT_FMT_A2R10G10B10)
        return SCANOUT_ERR_FORMAT;
    u32 stride = m->flip_stride;
    if (stride < m->width * 4 || stride > 65536 || (stride & 63))
        return SCANOUT_ERR_STRIDE;
    u64 addr = m->flip_addr;
    u64 size = (u64)stride * (m->height - 1) + (u64)m->width * 4;
    if (!addr || (addr & 63) || addr > ~0UL - size || !m->range_ok ||
        !m->range_ok(m->range_ctx, addr, size))
        return SCANOUT_ERR_ADDR;

    u32 x0 = 0, y0 = 0, x1 = m->width, y1 = m->height;
    if (m->rect_tl || m->rect_br) {
        x0 = m->rect_tl & 0xffff;
        y0 = m->rect_tl >> 16;
        x1 = m->rect_br & 0xffff;
        y1 = m->rect_br >> 16;
        if (x0 >= x1 || y0 >= y1 || x1 > m->width || y1 > m->height)
            return SCANOUT_ERR_RECT;
    }
    f->src = addr;
    f->stride = stride;
    f->format = m->flip_format;
    f->x0 = x0;
    f->y0 = y0;
    f->x1 = x1;
    f->y1 = y1;
    f->cookie = cookie;
    return SCANOUT_ERR_NONE;
}

static void submit(struct scanout_model *m, u32 cookie)
{
    struct scanout_frame f;
    u32 err = check_flip(m, cookie, &f);
    if (err) {
        refuse(m, cookie, err);
        return;
    }
    m->pending = f;
    m->pending_valid = true;
    m->last_error = SCANOUT_ERR_NONE;
    m->stats[SCANOUT_STC_SUBMITTED]++;
    /* Nothing in flight: start copying now; it is reported at the first vblank after it. */
    start_pending(m);
}

static void write_ctrl(struct scanout_model *m, u32 v)
{
    u32 old = m->ctrl, now = v & SCANOUT_CTRL_MASK;
    bool flips_before = (old & SCANOUT_CTRL_ENABLE) && (old & SCANOUT_CTRL_FLIP_MODE);
    bool flips_after = (now & SCANOUT_CTRL_ENABLE) && (now & SCANOUT_CTRL_FLIP_MODE);
    if (flips_before && !flips_after)
        scanout_model_retire(m);
    m->ctrl = now;
    if (!(now & SCANOUT_CTRL_ENABLE))
        m->status = 0;
}

static u32 read32(const struct scanout_model *m, u32 o)
{
    if (o >= SCANOUT_STATS && o < SCANOUT_STATS + 4 * SCANOUT_STC_COUNT)
        return m->stats[(o - SCANOUT_STATS) / 4];
    switch (o) {
        case SCANOUT_ID:
            return SCANOUT_ID_VALUE;
        case SCANOUT_VERSION_REG:
            return SCANOUT_VERSION;
        case SCANOUT_FEATURES:
            return SCANOUT_FEATURES_V1;
        case SCANOUT_CTRL:
            return m->ctrl;
        case SCANOUT_STATUS:
            return m->status | (m->pending_valid ? SCANOUT_ST_PENDING : 0) |
                   (m->slot == SCANOUT_COPYING ? SCANOUT_ST_COPYING : 0) |
                   (m->slot == SCANOUT_COPIED ? SCANOUT_ST_COPIED : 0);
        case SCANOUT_WIDTH:
            return m->width;
        case SCANOUT_HEIGHT:
            return m->height;
        case SCANOUT_PANEL_FORMAT:
            return SCANOUT_FMT_A2R10G10B10;
        case SCANOUT_REFRESH_MHZ:
            return m->refresh_mhz;
        case SCANOUT_PERIOD_TICKS:
            return (u32)(edge_scale(m) / m->refresh_mhz);
        case SCANOUT_SHADOW_LO:
            return (u32)m->shadow;
        case SCANOUT_SHADOW_HI:
            return (u32)(m->shadow >> 32);
        case SCANOUT_SHADOW_STRIDE:
            return m->shadow_stride;
        case SCANOUT_COUNTER_HZ:
            return (u32)m->freq;
        case SCANOUT_VBLANK_COUNT_LO:
            return (u32)m->vblank_count;
        case SCANOUT_VBLANK_COUNT_HI:
            return (u32)(m->vblank_count >> 32);
        case SCANOUT_VBLANK_TIME_LO:
            return (u32)m->vblank_time;
        case SCANOUT_VBLANK_TIME_HI:
            return (u32)(m->vblank_time >> 32);
        case SCANOUT_FLIP_ADDR_LO:
            return (u32)m->flip_addr;
        case SCANOUT_FLIP_ADDR_HI:
            return (u32)(m->flip_addr >> 32);
        case SCANOUT_FLIP_STRIDE:
            return m->flip_stride;
        case SCANOUT_FLIP_FORMAT:
            return m->flip_format;
        case SCANOUT_FLIP_RECT_TL:
            return m->rect_tl;
        case SCANOUT_FLIP_RECT_BR:
            return m->rect_br;
        case SCANOUT_FLIP_ERROR:
            return m->last_error;
        case SCANOUT_PENDING_COOKIE:
            return m->pending_valid ? m->pending.cookie : 0;
        case SCANOUT_ACTIVE_COOKIE:
            return m->slot != SCANOUT_IDLE ? m->active.cookie : 0;
        case SCANOUT_DONE_COOKIE:
            return m->done_cookie;
        case SCANOUT_ERROR_COOKIE:
            return m->error_cookie;
        case SCANOUT_DONE_VBLANK_LO:
            return (u32)m->done_vblank;
        case SCANOUT_DONE_VBLANK_HI:
            return (u32)(m->done_vblank >> 32);
        default:
            return 0; /* SCANOUT_FLIP_SUBMIT is write-only; holes read 0 */
    }
}

static void write32(struct scanout_model *m, u32 o, u32 v)
{
    switch (o) {
        case SCANOUT_CTRL:
            write_ctrl(m, v);
            break;
        case SCANOUT_STATUS:
            m->status &= ~(v & SCANOUT_ST_W1C);
            break;
        case SCANOUT_FLIP_ADDR_LO:
            m->flip_addr = (m->flip_addr & ~0xffffffffUL) | v;
            break;
        case SCANOUT_FLIP_ADDR_HI:
            m->flip_addr = (m->flip_addr & 0xffffffffUL) | ((u64)v << 32);
            break;
        case SCANOUT_FLIP_STRIDE:
            m->flip_stride = v;
            break;
        case SCANOUT_FLIP_FORMAT:
            m->flip_format = v;
            break;
        case SCANOUT_FLIP_RECT_TL:
            m->rect_tl = v;
            break;
        case SCANOUT_FLIP_RECT_BR:
            m->rect_br = v;
            break;
        case SCANOUT_FLIP_SUBMIT:
            submit(m, v);
            break;
        default:
            break; /* RO registers and holes ignore writes */
    }
}

bool scanout_model_mmio(struct scanout_model *m, u64 off, int width, u64 *val, bool write)
{
    bool ok = (width == 2 && !(off & 3)) || (width == 3 && !(off & 7));
    if (!ok || off >= SCANOUT_PAGE_SIZE) {
        m->stats[SCANOUT_STC_BAD_ACCESS]++;
        if (!write)
            val[0] = 0;
        return false;
    }
    if (off >= SCANOUT_REG_END) {
        if (!write)
            val[0] = 0;
        return true;
    }
    u32 o = (u32)off;
    if (write) {
        /* Low word first: a 64-bit write at FLIP_RECT_BR/FLIP_SUBMIT submits after the rect. */
        write32(m, o, (u32)*val);
        if (width == 3)
            write32(m, o + 4, (u32)(*val >> 32));
    } else {
        u64 v = read32(m, o);
        if (width == 3)
            v |= (u64)read32(m, o + 4) << 32;
        *val = v;
    }
    return true;
}

bool scanout_model_line(const struct scanout_model *m)
{
    if (!enabled(m))
        return false;
    u32 want = ((m->ctrl & SCANOUT_CTRL_VBLANK_IRQ_EN) ? SCANOUT_ST_VBLANK : 0) |
               ((m->ctrl & SCANOUT_CTRL_FLIP_IRQ_EN) ? SCANOUT_ST_FLIP_DONE : 0) |
               ((m->ctrl & SCANOUT_CTRL_ERROR_IRQ_EN) ? SCANOUT_ST_FLIP_ERROR : 0);
    return (m->status & want) != 0;
}

bool scanout_model_owns_panel(const struct scanout_model *m)
{
    return enabled(m) && (m->ctrl & SCANOUT_CTRL_FLIP_MODE);
}

bool scanout_model_claim(struct scanout_model *m, u32 max_rows, struct scanout_band *band)
{
    if (m->slot != SCANOUT_COPYING || m->next_row >= m->active.y1 || !max_rows)
        return false;
    u32 n = m->active.y1 - m->next_row;
    if (n > max_rows)
        n = max_rows;
    band->src = m->active.src;
    band->stride = m->active.stride;
    band->format = m->active.format;
    band->x0 = m->active.x0;
    band->x1 = m->active.x1;
    band->y0 = m->next_row;
    band->y1 = m->next_row + n;
    band->gen = m->gen;
    m->next_row += n;
    return true;
}

void scanout_model_band_done(struct scanout_model *m, u32 gen, u32 rows)
{
    if (gen != m->gen || m->slot != SCANOUT_COPYING) {
        m->stats[SCANOUT_STC_STALE_BANDS]++;
        return;
    }
    m->rows_done += rows;
    m->stats[SCANOUT_STC_ROWS] += rows;
    if (m->rows_done >= m->active.y1 - m->active.y0)
        m->slot = SCANOUT_COPIED;
}

void scanout_copy_band(u32 *dst, u32 dst_stride_px, const struct scanout_band *b)
{
    u32 w = b->x1 - b->x0;
    for (u32 y = b->y0; y < b->y1; y++) {
        const u32 *s = (const u32 *)(b->src + (u64)y * b->stride) + b->x0;
        u32 *d = dst + (u64)y * dst_stride_px + b->x0;
        if (b->format == SCANOUT_FMT_A2R10G10B10) {
            memcpy(d, s, (size_t)w * 4);
        } else {
            for (u32 x = 0; x < w; x++)
                d[x] = scanout_bgra_to_x2r10(s[x]);
        }
    }
}
