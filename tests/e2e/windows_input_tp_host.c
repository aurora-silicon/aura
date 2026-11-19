/* SPDX-License-Identifier: MIT */
/*
 * Host replay of src/hv/windows/input_tp.c with synthetic finger records written for this test
 * (the 0x75 layout of INPUT-I1I2-BUILD.md section 1; no trackpad capture exists on this Mac).
 * Default geometry: 95 raw units per mm in X, 93 in Y.
 *
 * Failure list (written before the code):
 *   T1  gain: one finger, slow, no acceleration: exactly gain counts per mm; sub-count moves
 *       are carried (1.2 counts per frame give 1,1,1,1,2)
 *   T2  acceleration: the same distance fast gives more counts than slow; at or above
 *       accel_hi the factor is 1 + accel exactly; below accel_lo it is 1
 *   T3  Y: the pad's Y grows upwards, the report's downwards
 *   T4  no jump: a lift and a touch elsewhere, and a frame whose contact count changes,
 *       move nothing
 *   T5  taps: one finger short and still -> left down, left up; two fingers -> right; too
 *       long, too far, taps off, or within 300 ms of a key -> nothing
 *   T6  physical click: one contact left, two contacts right, latched until release; motion
 *       is ignored in the settle time; click-drag with a resting thumb follows the moving
 *       finger; a click is never also a tap
 *   T7  two-finger scroll: vertical and horizontal detents in both directions, traditional
 *       and natural; a pinch (fingers apart) scrolls nothing; a scroll is never a tap
 *   T8  three fingers: nothing
 *   T9  malformed: wrong ID, shorter than the header, records past the end, more than 16
 *       records: refused and counted, state unchanged
 *   T10 silence: a held contact and button are dropped after 500 ms, once
 *   T11 geometry: 0xD9 parsed and applied (resolution); nonsense refused
 *   T12 palms: touch major above the threshold is ignored; touch major 0 is not touching
 *   T13 fuzz: random frames never overrun, reports stay in range
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "input_tp.h"

static int failures;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                    \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static void case_result(const char *name, int before, const char *extra)
{
    printf("{\"case\":\"%s\",\"result\":\"%s\"%s}\n", name, failures == before ? "passed" : "failed",
           extra ? extra : "");
}

struct finger {
    int x, y, touch;
};

static u8 frame_buf[TP_HDR_LEN + TP_MAX_CONTACTS * TP_REC_LEN + 8];

static u32 build(const struct finger *f, int n, bool button)
{
    memset(frame_buf, 0, sizeof(frame_buf));
    frame_buf[0] = TP_REPORT_ID;
    frame_buf[TP_HDR_COUNT] = (u8)n;
    frame_buf[TP_HDR_BUTTON] = button ? 1 : 0;
    for (int i = 0; i < n; i++) {
        u8 *r = frame_buf + TP_HDR_LEN + i * TP_REC_LEN;
        r[TP_REC_X] = (u8)f[i].x;
        r[TP_REC_X + 1] = (u8)((u16)f[i].x >> 8);
        r[TP_REC_Y] = (u8)f[i].y;
        r[TP_REC_Y + 1] = (u8)((u16)f[i].y >> 8);
        int t = f[i].touch ? f[i].touch : 900;
        if (f[i].touch < 0)
            t = 0;
        r[TP_REC_TOUCH] = (u8)t;
        r[TP_REC_TOUCH + 1] = (u8)(t >> 8);
        r[TP_REC_TOOL] = 0x84;
        r[TP_REC_TOOL + 1] = 0x03;
    }
    return TP_HDR_LEN + (u32)n * TP_REC_LEN;
}

static struct tp_state S;
static struct tp_report R[TP_MAX_REPORTS];
static u64 T;

/* Sums of what went out, and the button transitions. */
static long sum_dx, sum_dy, sum_wheel, sum_pan;
static u8 btn_log[64], btn_last;
static int btn_n, reports;

static void reset_sums(void)
{
    sum_dx = sum_dy = sum_wheel = sum_pan = 0;
    btn_n = reports = 0;
    btn_last = 0;
}

static int feed(const struct finger *f, int n, bool button, u32 step_us)
{
    T += step_us;
    u32 len = build(f, n, button);
    int k = tp_frame(&S, frame_buf, len, T, R);
    for (int i = 0; i < k; i++) {
        reports++;
        sum_dx += R[i].dx;
        sum_dy += R[i].dy;
        sum_wheel += R[i].wheel;
        sum_pan += R[i].pan;
        if (btn_n < 64 && R[i].buttons != btn_last)
            btn_log[btn_n++] = R[i].buttons;
        btn_last = R[i].buttons;
    }
    return k;
}

static void fresh(const struct tp_config *c)
{
    struct tp_config d;
    tp_config_default(&d);
    tp_init(&S, c ? c : &d);
    T = 1000000;
    reset_sums();
}

static void lift(void)
{
    feed(NULL, 0, false, 8000);
}

/* ---- cases ---------------------------------------------------------------------------------- */

static void t1_gain(void)
{
    int before = failures;
    struct tp_config c;
    tp_config_default(&c);
    c.accel_q8 = 0;
    fresh(&c);
    CHECK(S.res_x == 95 && S.res_y == 93);
    struct finger f = {0, 0, 0};
    CHECK(feed(&f, 1, false, 8000) == 0); /* touch-down: nothing moves */
    for (int i = 0; i < 10; i++) {
        f.x += 95; /* 1 mm */
        feed(&f, 1, false, 8000);
    }
    CHECK(sum_dx == 60 && sum_dy == 0 && reports == 10);
    /* 0.4 mm steps: 2.4 counts each, the fraction carried (2, 4, 7, 9). */
    lift();
    reset_sums();
    f.x = 0;
    feed(&f, 1, false, 8000);
    int outs[4];
    for (int i = 0; i < 4; i++) {
        f.x += 38;
        feed(&f, 1, false, 8000);
        outs[i] = (int)sum_dx;
    }
    CHECK(outs[0] == 2 && outs[1] == 4 && outs[2] == 7 && outs[3] == 9);
    case_result("gain", before, NULL);
}

static long run_distance(int step_raw, int frames, u32 step_us)
{
    fresh(NULL);
    struct finger f = {0, 0, 0};
    feed(&f, 1, false, step_us);
    for (int i = 0; i < frames; i++) {
        f.x += step_raw;
        feed(&f, 1, false, step_us);
    }
    return sum_dx;
}

static void t2_accel(void)
{
    int before = failures;
    /* 20 mm slowly (10 mm/s: below accel_lo) and quickly (125 mm/s). */
    long slow = run_distance(95, 20, 100000); /* dt is clamped to 50 ms: 20 mm/s */
    long fast = run_distance(95, 20, 8000);
    struct tp_config c;
    tp_config_default(&c);
    CHECK(slow > 0 && fast > slow * 2);
    /* Saturated: 2 mm per 8 ms = 250 mm/s >= accel_hi: 6 * 4 = 24 counts per mm exactly. */
    long sat = run_distance(190, 10, 8000);
    CHECK(sat == 10 * 48);
    /* Below accel_lo: exactly the gain (0.5 mm per 50 ms = 10 mm/s). */
    fresh(NULL);
    struct finger f = {0, 0, 0};
    feed(&f, 1, false, 50000);
    for (int i = 0; i < 20; i++) {
        f.x += 95; /* 1 mm per 100 ms, clamped dt 50 ms: 20 mm/s, just above */
        feed(&f, 1, false, 100000);
    }
    long just_above = sum_dx;
    fresh(NULL);
    feed(&f, 1, false, 50000);
    for (int i = 0; i < 40; i++) {
        f.x += (i & 1) ? 48 : 47; /* 0.5 mm per 50 ms: 10 mm/s */
        feed(&f, 1, false, 50000);
    }
    /* 20 mm at the gain: 120 counts, less what the um rounding of 47/48 raw loses. */
    CHECK(sum_dx >= 117 && sum_dx <= 120 && just_above > 120);
    char extra[96];
    snprintf(extra, sizeof(extra), ",\"slow\":%ld,\"fast\":%ld,\"saturated\":%ld", slow, fast, sat);
    case_result("accel", before, extra);
}

static void t3_y_axis(void)
{
    int before = failures;
    struct tp_config c;
    tp_config_default(&c);
    c.accel_q8 = 0;
    fresh(&c);
    struct finger f = {0, 1000, 0};
    feed(&f, 1, false, 8000);
    for (int i = 0; i < 10; i++) {
        f.y += 93; /* the finger moves up the pad */
        feed(&f, 1, false, 8000);
    }
    CHECK(sum_dy == -60 && sum_dx == 0);
    case_result("y_axis", before, NULL);
}

static void t4_no_jump(void)
{
    int before = failures;
    fresh(NULL);
    struct finger f = {-3000, 0, 0};
    feed(&f, 1, false, 8000);
    f.x += 95;
    feed(&f, 1, false, 8000);
    lift();
    reset_sums();
    f.x = 5000; /* 84 mm away */
    f.y = 5000;
    CHECK(feed(&f, 1, false, 8000) == 0 && sum_dx == 0);
    /* A second finger lands: that frame moves nothing, even though the first moved. */
    struct finger two[2] = {{5095, 5000, 0}, {0, 0, 0}};
    CHECK(feed(two, 2, false, 8000) == 0);
    /* It lifts: again nothing. */
    struct finger one = {5190, 5000, 0};
    CHECK(feed(&one, 1, false, 8000) == 0 && sum_dx == 0 && sum_dy == 0);
    /* The match is by position, not record order. */
    struct finger swapped[2] = {{0, 0, 0}, {5190, 5000, 0}};
    feed(swapped, 2, false, 8000);
    struct finger swapped2[2] = {{5285, 5000, 0}, {0, 0, 0}};
    feed(swapped2, 2, false, 8000); /* one of two moves: not a scroll, not a pointer move */
    CHECK(sum_wheel == 0 && sum_pan == 0 && sum_dx == 0);
    case_result("no_jump", before, NULL);
}

static void tap_with(const struct tp_config *c, int fingers, u32 hold_us, int travel_raw,
                     u64 key_ago_us)
{
    fresh(c);
    if (key_ago_us)
        tp_note_key(&S, T + 8000 - key_ago_us);
    struct finger f[2] = {{0, 0, 0}, {2000, 0, 0}};
    feed(f, fingers, false, 8000);
    f[0].x += travel_raw;
    feed(f, fingers, false, hold_us);
    feed(NULL, 0, false, 4000);
}

static void t5_taps(void)
{
    int before = failures;
    struct tp_config c;
    tp_config_default(&c);
    tap_with(&c, 1, 100000, 0, 0);
    CHECK(btn_n == 2 && btn_log[0] == TP_BTN_LEFT && btn_log[1] == 0 && S.taps == 1);
    tap_with(&c, 2, 100000, 0, 0);
    CHECK(btn_n == 2 && btn_log[0] == TP_BTN_RIGHT && btn_log[1] == 0);
    tap_with(&c, 1, 250000, 0, 0); /* too long */
    CHECK(btn_n == 0 && S.taps == 0);
    tap_with(&c, 1, 100000, 300, 0); /* 3 mm: too far */
    CHECK(btn_n == 0);
    tap_with(&c, 1, 100000, 0, 100000); /* a key 100 ms ago */
    CHECK(btn_n == 0 && S.ignored == 1);
    tap_with(&c, 1, 100000, 0, 500000); /* a key 500 ms ago */
    CHECK(btn_n == 2 && btn_log[0] == TP_BTN_LEFT);
    c.tap = false;
    tap_with(&c, 1, 100000, 0, 0);
    CHECK(btn_n == 0);
    case_result("taps", before, NULL);
}

static void t6_click(void)
{
    int before = failures;
    fresh(NULL);
    struct finger f = {0, 0, 0};
    feed(&f, 1, false, 8000);
    feed(&f, 1, true, 8000);
    CHECK(btn_n == 1 && btn_log[0] == TP_BTN_LEFT);
    f.x += 95; /* within the settle time: ignored */
    feed(&f, 1, true, 8000);
    CHECK(sum_dx == 0);
    f.x += 95;
    feed(&f, 1, true, 40000);
    CHECK(sum_dx > 0);
    feed(&f, 1, false, 8000);
    CHECK(btn_n == 2 && btn_log[1] == 0);
    feed(NULL, 0, false, 8000);
    CHECK(btn_n == 2 && S.taps == 0); /* a click is never also a tap */

    /* Two contacts: right. */
    fresh(NULL);
    struct finger two[2] = {{0, 0, 0}, {1500, 0, 0}};
    feed(two, 2, false, 8000);
    feed(two, 2, true, 8000);
    feed(two, 2, false, 8000);
    CHECK(btn_n == 2 && btn_log[0] == TP_BTN_RIGHT && btn_log[1] == 0);

    /* Click-drag: the click starts with one finger (left); a thumb rests while the index
     * finger drags: the drag follows the moving finger, the button stays left. */
    fresh(NULL);
    struct finger idx = {0, 3000, 0};
    feed(&idx, 1, false, 8000);
    feed(&idx, 1, true, 8000);
    struct finger drag[2] = {{0, 3000, 0}, {-4000, -2000, 0}}; /* thumb lands */
    feed(drag, 2, true, 50000);
    reset_sums();
    for (int i = 0; i < 5; i++) {
        drag[0].x += 190;
        feed(drag, 2, true, 50000);
    }
    CHECK(sum_dx > 0 && sum_wheel == 0 && btn_n == 1 && btn_log[0] == TP_BTN_LEFT);
    feed(drag, 2, false, 8000);
    CHECK(btn_n == 2 && btn_log[1] == 0);
    case_result("click", before, NULL);
}

static void scroll_run(const struct tp_config *c, int ddx, int ddy, int frames)
{
    fresh(c);
    struct finger f[2] = {{0, 2000, 0}, {1800, 2000, 0}};
    feed(f, 2, false, 8000);
    for (int i = 0; i < frames; i++) {
        f[0].x += ddx;
        f[1].x += ddx;
        f[0].y += ddy;
        f[1].y += ddy;
        feed(f, 2, false, 8000);
    }
    feed(NULL, 0, false, 8000);
}

static void t7_scroll(void)
{
    int before = failures;
    struct tp_config c;
    tp_config_default(&c);
    /* Fingers move down the pad (pad Y decreases) 1 mm a frame, 20 frames: 4 detents. */
    scroll_run(&c, 0, -93, 20);
    CHECK(sum_wheel == -4 && sum_pan == 0 && sum_dx == 0 && sum_dy == 0 && btn_n == 0);
    scroll_run(&c, 0, 93, 20);
    CHECK(sum_wheel == 4 && btn_n == 0);
    scroll_run(&c, 95, 0, 20);
    CHECK(sum_pan == 4 && sum_wheel == 0);
    scroll_run(&c, -95, 0, 20);
    CHECK(sum_pan == -4);
    c.natural = true;
    scroll_run(&c, 0, -93, 20);
    CHECK(sum_wheel == 4);
    scroll_run(&c, 95, 0, 20);
    CHECK(sum_pan == -4);
    /* A short scroll (2 mm, under a detent) is still not a tap?  2 mm is past the tap
     * travel, so no tap either way. */
    c.natural = false;
    scroll_run(&c, 0, -93, 2);
    CHECK(sum_wheel == 0 && btn_n == 0);
    /* A pinch: the fingers move apart; nothing. */
    fresh(&c);
    struct finger p[2] = {{0, 2000, 0}, {1800, 2000, 0}};
    feed(p, 2, false, 8000);
    for (int i = 0; i < 20; i++) {
        p[0].x -= 95;
        p[1].x += 95;
        feed(p, 2, false, 8000);
    }
    CHECK(sum_wheel == 0 && sum_pan == 0 && sum_dx == 0);
    case_result("scroll", before, NULL);
}

static void t8_three(void)
{
    int before = failures;
    fresh(NULL);
    struct finger f[3] = {{0, 0, 0}, {1500, 0, 0}, {3000, 0, 0}};
    feed(f, 3, false, 8000);
    for (int i = 0; i < 10; i++) {
        for (int k = 0; k < 3; k++)
            f[k].y -= 93;
        feed(f, 3, false, 8000);
    }
    feed(NULL, 0, false, 8000);
    CHECK(reports == 0);
    case_result("three", before, NULL);
}

static void t9_malformed(void)
{
    int before = failures;
    fresh(NULL);
    struct finger f[2] = {{0, 0, 0}, {100, 0, 0}};
    u32 len = build(f, 2, false);
    frame_buf[0] = 0x74;
    CHECK(tp_frame(&S, frame_buf, len, T, R) == -1);
    frame_buf[0] = TP_REPORT_ID;
    CHECK(tp_frame(&S, frame_buf, TP_HDR_LEN - 1, T, R) == -1);
    CHECK(tp_frame(&S, frame_buf, len - 1, T, R) == -1); /* the second record is cut */
    frame_buf[TP_HDR_COUNT] = 17;
    CHECK(tp_frame(&S, frame_buf, sizeof(frame_buf), T, R) == -1);
    CHECK(S.bad == 4 && S.frames == 0 && S.n_prev == 0);
    /* Trailing bytes after the records are fine. */
    frame_buf[TP_HDR_COUNT] = 2;
    CHECK(tp_frame(&S, frame_buf, len + 8, T, R) == 0 && S.n_prev == 2);
    case_result("malformed", before, NULL);
}

static void t10_silence(void)
{
    int before = failures;
    fresh(NULL);
    struct finger f = {0, 0, 0};
    feed(&f, 1, false, 8000);
    feed(&f, 1, true, 8000);
    CHECK(btn_n == 1 && btn_log[0] == TP_BTN_LEFT);
    struct tp_report r;
    CHECK(tp_idle(&S, T + 400000, &r) == 0);
    CHECK(tp_idle(&S, T + 600000, &r) == 1 && r.buttons == 0 && S.stale == 1);
    CHECK(tp_idle(&S, T + 900000, &r) == 0 && S.stale == 1);
    /* The next touch starts clean. */
    T += 1000000;
    reset_sums();
    CHECK(feed(&f, 1, false, 8000) == 0);
    case_result("silence", before, NULL);
}

static void t11_geometry(void)
{
    int before = failures;
    fresh(NULL);
    /* width 125 mm, height 80 mm, X -6000..6500, Y -100..7900: 100 units/mm each. */
    u8 d9[TP_GEOMETRY_LEN] = {0xd9};
    u32 w = 12500, h = 8000;
    memcpy(d9 + 1, &w, 4);
    memcpy(d9 + 5, &h, 4);
    s16 v[4] = {-6000, -100, 6500, 7900};
    memcpy(d9 + 9, v, 8);
    struct tp_geometry g;
    CHECK(tp_parse_geometry(d9, sizeof(d9), &g) && g.width == 12500 && g.min_x == -6000 &&
          g.max_y == 7900);
    CHECK(tp_set_geometry(&S, &g) && S.res_x == 100 && S.res_y == 100);
    CHECK(!tp_parse_geometry(d9, sizeof(d9) - 1, &g));
    d9[0] = 0xd8;
    CHECK(!tp_parse_geometry(d9, sizeof(d9), &g));
    struct tp_geometry bad = g;
    bad.max_x = bad.min_x;
    CHECK(!tp_set_geometry(&S, &bad) && S.res_x == 100);
    bad = g;
    bad.width = 0;
    CHECK(!tp_set_geometry(&S, &bad));
    bad = g;
    bad.width = 50000000;
    CHECK(!tp_set_geometry(&S, &bad));
    case_result("geometry", before, NULL);
}

static void t12_palms(void)
{
    int before = failures;
    struct tp_config c;
    tp_config_default(&c);
    c.palm_major = 1500;
    fresh(&c);
    struct finger f[2] = {{0, 0, 0}, {-4000, -1000, 3000}}; /* a finger and a palm */
    feed(f, 2, false, 8000);
    for (int i = 0; i < 5; i++) {
        f[0].x += 95;
        feed(f, 2, false, 8000);
    }
    CHECK(sum_dx > 0 && sum_wheel == 0 && S.palms == 6); /* moves as one finger */
    struct finger lifted = {f[0].x, 0, -1};              /* touch major 0: not touching */
    feed(&lifted, 1, false, 8000);
    CHECK(S.n_prev == 0);
    case_result("palms", before, NULL);
}

static u32 rng = 12345;
static u32 rnd(void)
{
    rng = rng * 1103515245u + 12345u;
    return rng >> 8;
}

static void t13_fuzz(long iterations)
{
    int before = failures;
    fresh(NULL);
    static u8 buf[TP_HDR_LEN + 20 * TP_REC_LEN];
    long ok = 0;
    for (long it = 0; it < iterations; it++) {
        u32 len = rnd() % sizeof(buf);
        for (u32 i = 0; i < len; i++)
            buf[i] = (u8)rnd();
        if (rnd() % 2) {
            buf[0] = TP_REPORT_ID;
            if (len > TP_HDR_COUNT)
                buf[TP_HDR_COUNT] = (u8)(rnd() % 18);
        }
        T += rnd() % 60000;
        int k = tp_frame(&S, buf, len, T, R);
        CHECK(k >= -1 && k <= (int)TP_MAX_REPORTS);
        for (int i = 0; i < k; i++)
            CHECK(R[i].buttons <= 3 && R[i].dx >= -32767 && R[i].dy >= -32767 &&
                  R[i].wheel >= -127 && R[i].pan >= -127);
        if (k >= 0)
            ok++;
        if (rnd() % 64 == 0) {
            struct tp_report r;
            tp_idle(&S, T + 600000, &r);
        }
    }
    char extra[64];
    snprintf(extra, sizeof(extra), ",\"iterations\":%ld,\"frames\":%ld", iterations, ok);
    case_result("fuzz", before, extra);
}

int main(int argc, char **argv)
{
    long iterations = argc > 1 ? atol(argv[1]) : 200000;
    t1_gain();
    t2_accel();
    t3_y_axis();
    t4_no_jump();
    t5_taps();
    t6_click();
    t7_scroll();
    t8_three();
    t9_malformed();
    t10_silence();
    t11_geometry();
    t12_palms();
    t13_fuzz(iterations);
    return failures ? 1 : 0;
}
