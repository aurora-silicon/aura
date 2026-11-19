/* SPDX-License-Identifier: MIT */

/*
 * I2a: trackpad finger records to a relative mouse.  See input_tp.h for the facts and the
 * gesture rules.  Written on this branch from our own measurements' facts; no third-party
 * source.  Integer arithmetic only: the carrier never touches the guest's FP registers.
 */

#include "input_tp.h"

static void zero(void *p, u32 n)
{
    u8 *b = p;
    for (u32 i = 0; i < n; i++)
        b[i] = 0;
}

static s32 s16le(const u8 *p)
{
    return (s16)(u16)(p[0] | (u16)p[1] << 8);
}

static u32 u16le(const u8 *p)
{
    return (u32)p[0] | (u32)p[1] << 8;
}

static u32 u32le(const u8 *p)
{
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static u32 iabs(s32 v)
{
    return v < 0 ? (u32)0 - (u32)v : (u32)v;
}

/* A cheap Euclidean length (within 12%): the larger axis plus half the smaller. */
static u32 mag(s32 x, s32 y)
{
    u32 a = iabs(x), b = iabs(y);
    return a > b ? a + b / 2 : b + a / 2;
}

static s32 to_um(s32 raw, u32 res)
{
    return (s32)((s64)raw * 1000 / (s64)res);
}

static s16 clamp16(s64 v)
{
    return v > 32767 ? 32767 : v < -32767 ? -32767 : (s16)v;
}

static s8 clamp8(s32 v)
{
    return v > 127 ? 127 : v < -127 ? -127 : (s8)v;
}

void tp_config_default(struct tp_config *c)
{
    zero(c, sizeof(*c));
    c->gain_q8 = 6 * 256;  /* 6 counts per mm when slow */
    c->accel_q8 = 3 * 256; /* up to 4x at accel_hi */
    c->accel_lo = 15;
    c->accel_hi = 150;
    c->scroll_um = 5000;
    c->tap_ms = 180;
    c->tap_travel_um = 1500;
    c->tap_key_quiet_ms = 300;
    c->click_settle_ms = 40;
    c->palm_major = 0;
    c->tap = true;
    c->natural = false;
}

/* [?] until 0xD9 answers on the M6: a 14-inch-class pad, about 95 raw units per mm. */
void tp_geometry_default(struct tp_geometry *g)
{
    g->width = 13000;
    g->height = 8100;
    g->min_x = -5900;
    g->max_x = 6500;
    g->min_y = -200;
    g->max_y = 7400;
}

bool tp_set_geometry(struct tp_state *s, const struct tp_geometry *g)
{
    s32 span_x = (s32)g->max_x - g->min_x, span_y = (s32)g->max_y - g->min_y;
    if (span_x <= 0 || span_y <= 0 || g->width < 100 || g->height < 100 || g->width > 100000 ||
        g->height > 100000)
        return false;
    u32 rx = (u32)((u64)span_x * 100 / g->width), ry = (u32)((u64)span_y * 100 / g->height);
    if (!rx || !ry)
        return false;
    s->geo = *g;
    s->res_x = rx;
    s->res_y = ry;
    return true;
}

bool tp_parse_geometry(const u8 *p, u32 len, struct tp_geometry *g)
{
    if (len < TP_GEOMETRY_LEN || p[0] != TP_FEATURE_GEOMETRY)
        return false;
    g->width = u32le(p + 1);
    g->height = u32le(p + 5);
    g->min_x = (s16)s16le(p + 9);
    g->min_y = (s16)s16le(p + 11);
    g->max_x = (s16)s16le(p + 13);
    g->max_y = (s16)s16le(p + 15);
    return true;
}

void tp_init(struct tp_state *s, const struct tp_config *cfg)
{
    zero(s, sizeof(*s));
    s->cfg = *cfg;
    struct tp_geometry g;
    tp_geometry_default(&g);
    tp_set_geometry(s, &g);
}

void tp_note_key(struct tp_state *s, u64 now)
{
    s->last_key = now;
    s->key_seen = true;
}

static void forget(struct tp_state *s)
{
    s->n_prev = 0;
    s->session = false;
    s->phys = false;
    s->buttons = 0;
    s->rem_x = s->rem_y = 0;
    s->acc_v = s->acc_h = 0;
}

void tp_release(struct tp_state *s, struct tp_report *out)
{
    forget(s);
    s->last_buttons = 0;
    *out = (struct tp_report){0, 0, 0, 0, 0};
}

int tp_idle(struct tp_state *s, u64 now, struct tp_report *out)
{
    if (!s->have_prev || (!s->n_prev && !s->phys) || now - s->t_prev < TP_STALE_US)
        return 0;
    s->stale++;
    bool held = s->last_buttons != 0;
    tp_release(s, out);
    return held ? 1 : 0;
}

/*
 * Pair each contact with the nearest previous one (closest pairs first), within TP_MATCH_MM.
 * from[i] is the previous index, or -1 for a new contact (which gets a new slot).
 */
static void match(struct tp_state *s, struct tp_contact *cur, u32 n, s32 *from)
{
    bool used[TP_MAX_CONTACTS];
    u32 limit = TP_MATCH_MM * (s->res_x > s->res_y ? s->res_x : s->res_y);
    for (u32 i = 0; i < TP_MAX_CONTACTS; i++)
        used[i] = false;
    for (u32 i = 0; i < n; i++)
        from[i] = -1;
    for (;;) {
        u32 best = ~0u, bi = 0, bj = 0;
        for (u32 i = 0; i < n; i++) {
            if (from[i] >= 0)
                continue;
            for (u32 j = 0; j < s->n_prev; j++) {
                if (used[j])
                    continue;
                u32 d = mag(cur[i].x - s->prev[j].x, cur[i].y - s->prev[j].y);
                if (d <= limit && d < best) {
                    best = d;
                    bi = i;
                    bj = j;
                }
            }
        }
        if (best == ~0u)
            break;
        from[bi] = (s32)bj;
        used[bj] = true;
        cur[bi].slot = s->prev[bj].slot;
        cur[bi].x0 = s->prev[bj].x0;
        cur[bi].y0 = s->prev[bj].y0;
    }
    for (u32 i = 0; i < n; i++)
        if (from[i] < 0) {
            cur[i].slot = s->next_slot++;
            cur[i].x0 = cur[i].x;
            cur[i].y0 = cur[i].y;
        }
}

/* One axis: Q8 counts plus the carried remainder; the whole counts go out. */
static s16 carry(s32 *rem, s64 q8)
{
    s64 t = *rem + q8;
    s64 whole = t / 256;
    *rem = (s32)(t - whole * 256);
    return clamp16(whole);
}

static void motion(struct tp_state *s, s32 rx, s32 ry, u32 dt, s16 *dx, s16 *dy)
{
    const struct tp_config *c = &s->cfg;
    s32 ux = to_um(rx, s->res_x), uy = to_um(ry, s->res_y);
    u64 speed = (u64)mag(ux, uy) * 1000 / dt; /* mm/s */
    u64 f = 256;
    if (c->accel_q8 && c->accel_hi > c->accel_lo && speed > c->accel_lo) {
        u64 over = speed - c->accel_lo, span = (u64)c->accel_hi - c->accel_lo;
        f += (u64)c->accel_q8 * (over > span ? span : over) / span;
    }
    *dx = carry(&s->rem_x, (s64)ux * c->gain_q8 * (s64)f / (1000 * 256));
    *dy = carry(&s->rem_y, (s64)uy * c->gain_q8 * (s64)f / (1000 * 256));
}

static void scroll(struct tp_state *s, s32 mx, s32 my, s8 *wheel, s8 *pan)
{
    s32 step = s->cfg.scroll_um ? s->cfg.scroll_um : 1;
    /* The dominant axis only, frame by frame. */
    if (iabs(my) >= iabs(mx))
        mx = 0;
    else
        my = 0;
    s->acc_v += my;
    s->acc_h += mx;
    s32 v = s->acc_v / step, h = s->acc_h / step;
    s->acc_v -= v * step;
    s->acc_h -= h * step;
    /* Traditional: fingers down scroll the view down (wheel < 0), fingers right pan right. */
    *wheel = clamp8(s->cfg.natural ? v : -v);
    *pan = clamp8(s->cfg.natural ? -h : h);
    if (v || h) {
        s->scrolled = true;
        s->detents += iabs(v) + iabs(h);
    }
}

int tp_frame(struct tp_state *s, const u8 *p, u32 len, u64 now, struct tp_report *out)
{
    if (len < TP_HDR_LEN || p[0] != TP_REPORT_ID) {
        s->bad++;
        return -1;
    }
    u32 count = p[TP_HDR_COUNT];
    if (count > TP_MAX_CONTACTS || TP_HDR_LEN + count * TP_REC_LEN > len) {
        s->bad++;
        return -1;
    }
    s->frames++;

    struct tp_contact cur[TP_MAX_CONTACTS];
    s32 from[TP_MAX_CONTACTS];
    u32 n = 0;
    for (u32 i = 0; i < count; i++) {
        const u8 *r = p + TP_HDR_LEN + i * TP_REC_LEN;
        u32 touch = u16le(r + TP_REC_TOUCH);
        if (!touch)
            continue;
        if (s->cfg.palm_major && touch > s->cfg.palm_major) {
            s->palms++;
            continue;
        }
        cur[n].x = s16le(r + TP_REC_X);
        cur[n].y = -s16le(r + TP_REC_Y); /* the pad's Y grows upwards */
        n++;
    }
    match(s, cur, n, from);
    u32 np = s->n_prev;
    u64 gap = s->have_prev ? now - s->t_prev : TP_DT_MAX_US;
    u32 dt = gap < TP_DT_MIN_US ? TP_DT_MIN_US : gap > TP_DT_MAX_US ? TP_DT_MAX_US : (u32)gap;

    /* The physical click: the button is chosen when it starts and kept until it ends. */
    bool phys = p[TP_HDR_BUTTON] & 1;
    if (phys && !s->phys) {
        s->buttons = n >= 2 ? TP_BTN_RIGHT : TP_BTN_LEFT;
        s->clicks++;
        s->settle_until = now + (u64)s->cfg.click_settle_ms * 1000;
        s->clicked = true;
    }
    if (!phys)
        s->buttons = 0;
    s->phys = phys;

    if (n && !s->session) {
        s->session = true;
        s->t_session = now;
        s->max_contacts = 0;
        s->max_travel_um = 0;
        s->clicked = phys;
        s->scrolled = false;
    }
    if (s->session) {
        if (n > s->max_contacts)
            s->max_contacts = n;
        for (u32 i = 0; i < n; i++) {
            u32 t = mag(to_um(cur[i].x - cur[i].x0, s->res_x), to_um(cur[i].y - cur[i].y0, s->res_y));
            if (t > s->max_travel_um)
                s->max_travel_um = t;
        }
    }

    s16 dx = 0, dy = 0;
    s8 wheel = 0, pan = 0;
    bool scrolling = false;
    if (n && n == np) {
        if (s->buttons || n == 1) {
            u32 best = 0;
            s32 bx = 0, by = 0;
            bool found = false;
            for (u32 i = 0; i < n; i++) {
                if (from[i] < 0)
                    continue;
                s32 mx = cur[i].x - s->prev[from[i]].x, my = cur[i].y - s->prev[from[i]].y;
                u32 m = mag(mx, my);
                if (!found || m > best) {
                    found = true;
                    best = m;
                    bx = mx;
                    by = my;
                }
            }
            if (found && now >= s->settle_until)
                motion(s, bx, by, dt, &dx, &dy);
        } else if (n == 2 && from[0] >= 0 && from[1] >= 0) {
            s32 ax = to_um(cur[0].x - s->prev[from[0]].x, s->res_x);
            s32 ay = to_um(cur[0].y - s->prev[from[0]].y, s->res_y);
            s32 bx = to_um(cur[1].x - s->prev[from[1]].x, s->res_x);
            s32 by = to_um(cur[1].y - s->prev[from[1]].y, s->res_y);
            if ((s64)ax * bx + (s64)ay * by > 0) {
                scrolling = true;
                scroll(s, (ax + bx) / 2, (ay + by) / 2, &wheel, &pan);
            }
        }
    }
    if (!scrolling && (n != 2 || s->buttons))
        s->acc_v = s->acc_h = 0;
    if (!n)
        s->rem_x = s->rem_y = 0;

    int nout = 0;
    if (dx || dy || wheel || pan || s->buttons != s->last_buttons) {
        out[nout++] = (struct tp_report){s->buttons, dx, dy, wheel, pan};
        s->last_buttons = s->buttons;
    }

    /* The end of a touch: a tap when it was short, still, unclicked and away from typing. */
    if (s->session && !n) {
        const struct tp_config *c = &s->cfg;
        bool quiet = !s->key_seen || now - s->last_key >= (u64)c->tap_key_quiet_ms * 1000;
        if (c->tap && !s->clicked && !s->scrolled && !s->buttons && quiet &&
            now - s->t_session <= (u64)c->tap_ms * 1000 && s->max_travel_um < c->tap_travel_um &&
            (s->max_contacts == 1 || s->max_contacts == 2)) {
            u8 b = s->max_contacts == 1 ? TP_BTN_LEFT : TP_BTN_RIGHT;
            out[nout++] = (struct tp_report){b, 0, 0, 0, 0};
            out[nout++] = (struct tp_report){0, 0, 0, 0, 0};
            s->last_buttons = 0;
            s->taps++;
        } else if (!s->clicked && !s->scrolled && s->max_contacts <= 2 &&
                   now - s->t_session <= (u64)c->tap_ms * 1000) {
            s->ignored++; /* a short touch refused as a tap (travel, typing or taps off) */
        }
        s->session = false;
    }

    for (u32 i = 0; i < n; i++)
        s->prev[i] = cur[i];
    s->n_prev = n;
    s->t_prev = now;
    s->have_prev = true;
    return nout;
}
