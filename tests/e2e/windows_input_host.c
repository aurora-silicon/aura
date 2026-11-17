/* SPDX-License-Identifier: MIT */
/*
 * I0 host replay of the carrier's input device model (src/hv/windows/input_model.c) and of the
 * carrier-emulated SPI table (src/hv/windows/hv_vdev_spi.c), both compiled unchanged under
 * ASan + UBSan by tests/python/test_windows_t8152_input.py.
 *
 * Written from the failure list in that file's header (F1..F22) before the model existed.
 * Expected values come from windows-m6/INPUT-DESIGN.md (register page 1.3, report layout
 * 1.5, wire protocol 2.2) and the HID 1.11 / HID Usage Tables report rules, not from the model.
 * Report bytes are printed as hex and compared by the Python test against its own encoder;
 * the CRC is checked against the CRC-32 check value and, in Python, against zlib.
 *
 * Output: one JSON line per case.  Usage: windows_input_host [fuzz iterations [corpus file]]
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hv_vdev_spi.h"
#include "input_model.h"

#define HZ       24000000ULL /* the J804 system counter's frequency, as on the target */
#define WATCHDOG (2 * HZ)

static struct input_model M;
static int failures;
static const char *current;
static char extra[16384];

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d [%s] CHECK(%s)\n", __FILE__, __LINE__, current, #cond);         \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

#define EQ(a, b)                                                                                   \
    do {                                                                                           \
        unsigned long long _a = (unsigned long long)(a), _b = (unsigned long long)(b);             \
        if (_a != _b) {                                                                            \
            fprintf(stderr, "%s:%d [%s] %s = 0x%llx, want 0x%llx\n", __FILE__, __LINE__, current,  \
                    #a, _a, _b);                                                                   \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static void add_extra(const char *fmt, ...)
{
    size_t n = strlen(extra);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(extra + n, sizeof(extra) - n, fmt, ap);
    va_end(ap);
}

/* ---- register access --------------------------------------------------------------------- */

static u32 rd32(u32 off)
{
    u64 v = 0xdeadbeefdeadbeefULL;
    CHECK(input_model_mmio(&M, off, 2, &v, false));
    return (u32)v;
}

static u64 rd64(u32 off)
{
    u64 v = 0xdeadbeefdeadbeefULL;
    CHECK(input_model_mmio(&M, off, 3, &v, false));
    return v;
}

static void wr32(u32 off, u32 val)
{
    u64 v = val;
    CHECK(input_model_mmio(&M, off, 2, &v, true));
}

static void wr64(u32 off, u64 val)
{
    u64 v = val;
    CHECK(input_model_mmio(&M, off, 3, &v, true));
}

static u32 depth(void)
{
    return rd32(INPUT_REG_STATUS) >> INPUT_STATUS_DEPTH_SHIFT;
}

static void enable(void)
{
    wr32(INPUT_REG_CTRL, INPUT_CTRL_ENABLE | INPUT_CTRL_IRQ_EN);
}

/* The driver's DPC: read the latched report with 64-bit loads, then POP. */
static int pop_report(u8 *out, u32 *seq)
{
    u32 rl = rd32(INPUT_REG_REPORT_LEN);
    u32 len = rl & 0xff;
    if (!len)
        return 0;
    if (seq)
        *seq = rd32(INPUT_REG_SEQ);
    for (u32 i = 0; i < INPUT_REPORT_MAX; i += 8) {
        u64 v = rd64(INPUT_REG_REPORT + i);
        for (u32 b = 0; b < 8; b++)
            out[i + b] = (u8)(v >> (8 * b));
    }
    EQ(out[0], (rl >> 8) & 0xff);
    wr32(INPUT_REG_POP, 1);
    return (int)len;
}

struct got {
    int n;
    u8 r[300][INPUT_REPORT_MAX];
    int len[300];
};

static void drain(struct got *g)
{
    g->n = 0;
    for (;;) {
        u8 buf[INPUT_REPORT_MAX];
        int len = pop_report(buf, NULL);
        if (!len)
            break;
        CHECK(g->n < 300);
        if (g->n >= 300)
            break;
        memcpy(g->r[g->n], buf, INPUT_REPORT_MAX);
        g->len[g->n] = len;
        g->n++;
    }
}

static void hex_report(const char *name, const u8 *r, int len)
{
    add_extra(",\"%s\":\"", name);
    for (int i = 0; i < len; i++)
        add_extra("%02x", r[i]);
    add_extra("\"");
}

/* ---- host frames (CRC from the model, checked against the CRC-32 check value first) ------ */

static u16 seq_out;

static u32 frame(u8 *buf, u8 type, const u8 *payload, u8 len)
{
    buf[0] = INPUT_HOST_SYNC0;
    buf[1] = INPUT_HOST_SYNC1;
    buf[2] = type;
    buf[3] = len;
    buf[4] = (u8)seq_out;
    buf[5] = (u8)(seq_out >> 8);
    seq_out++;
    if (len)
        memcpy(buf + 6, payload, len);
    u32 crc = input_model_crc32(0, buf + 2, 4u + len);
    for (int i = 0; i < 4; i++)
        buf[6 + len + i] = (u8)(crc >> (8 * i));
    return 10u + len;
}

static u64 now;

static void send(u8 type, const u8 *payload, u8 len)
{
    u8 buf[INPUT_HOST_MAX_FRAME + 8];
    u32 n = frame(buf, type, payload, len);
    input_model_host_rx(&M, buf, n, now);
}

static void keys(u8 mods, int n, ...)
{
    u8 p[2 + 16];
    va_list ap;
    va_start(ap, n);
    p[0] = mods;
    p[1] = (u8)n;
    for (int i = 0; i < n; i++)
        p[2 + i] = (u8)va_arg(ap, int);
    va_end(ap);
    send(INPUT_HOST_KEYS, p, (u8)(2 + n));
}

static void absf(u8 buttons, u16 x, u16 y, s8 wheel, s8 pan)
{
    u8 p[7] = {buttons, (u8)x, (u8)(x >> 8), (u8)y, (u8)(y >> 8), (u8)wheel, (u8)pan};
    send(INPUT_HOST_ABS, p, 7);
}

static void relf(u8 buttons, s16 dx, s16 dy, s8 wheel, s8 pan)
{
    u8 p[7] = {buttons, (u8)dx, (u8)((u16)dx >> 8), (u8)dy, (u8)((u16)dy >> 8), (u8)wheel,
               (u8)pan};
    send(INPUT_HOST_REL, p, 7);
}

static void consumer(u16 usage)
{
    u8 p[2] = {(u8)usage, (u8)(usage >> 8)};
    send(INPUT_HOST_CONSUMER, p, 2);
}

static void hello(void)
{
    u8 p[4] = {1, 0, 0, 0};
    send(INPUT_HOST_HELLO, p, 4);
}

static u32 stat(enum input_stat s)
{
    return rd32(INPUT_REG_STATS + 4 * s);
}

static bool key_down(const u8 *r, u8 usage)
{
    return r[2 + usage / 8] & (1u << (usage % 8));
}

static void fresh(void)
{
    input_model_init(&M, INPUT_FEAT_SRC_HOST, WATCHDOG);
    now = 1000;
    seq_out = 0;
}

static void begin(const char *name)
{
    current = name;
    extra[0] = 0;
    failures = 0;
    fresh();
}

static int total_failures;

static void end(void)
{
    printf("{\"case\":\"%s\",\"result\":\"%s\"%s}\n", current, failures ? "failed" : "passed",
           extra);
    total_failures += failures;
}

/* ---- cases ------------------------------------------------------------------------------- */

/* F19: CRC-32 as zlib. */
static void case_crc(void)
{
    begin("crc");
    EQ(input_model_crc32(0, "123456789", 9), 0xcbf43926u);
    EQ(input_model_crc32(0, "", 0), 0);
    /* Incremental use gives the same value. */
    EQ(input_model_crc32(input_model_crc32(0, "1234", 4), "56789", 5), 0xcbf43926u);
    /* Buffers for the Python test to recompute with zlib. */
    u8 buf[97];
    for (u32 i = 0; i < sizeof(buf); i++)
        buf[i] = (u8)(i * 37 + 11);
    add_extra(",\"buf\":\"");
    for (u32 i = 0; i < sizeof(buf); i++)
        add_extra("%02x", buf[i]);
    add_extra("\",\"crc\":%u", input_model_crc32(0, buf, sizeof(buf)));
    end();
}

/* F14/F15: reset values; RO registers ignore writes; WO registers and holes read 0. */
static void case_reset_values(void)
{
    begin("reset_values");
    EQ(rd32(INPUT_REG_ID), 0x4e49364d);
    EQ(rd32(INPUT_REG_VERSION), 0x00010000);
    EQ(rd32(INPUT_REG_FEATURES), 0x0001000f);
    EQ(rd32(INPUT_REG_CTRL), 0);
    EQ(rd32(INPUT_REG_STATUS), 0);
    EQ(rd32(INPUT_REG_IRQ_STATUS), 0);
    EQ(rd32(INPUT_REG_SEQ), 0);
    EQ(rd32(INPUT_REG_DROPPED), 0);
    EQ(rd32(INPUT_REG_POP), 0);
    EQ(rd32(INPUT_REG_REPORT_LEN), 0);
    EQ(rd32(INPUT_REG_OUT_LEN), 0);
    EQ(rd32(INPUT_REG_OUT_DATA), 0);
    EQ(rd64(INPUT_REG_REPORT), 0);
    EQ(rd64(INPUT_REG_ID), 0x000100004e49364dULL);
    for (u32 off = 0x028; off < 0x040; off += 4)
        EQ(rd32(off), 0);
    EQ(rd32(0x1c0), 0);
    EQ(rd32(0x200), 0);
    EQ(rd32(0x3ffc), 0);
    EQ(rd64(0x3ff8), 0);
    /* Writes to RO registers and holes change nothing. */
    wr32(INPUT_REG_ID, 0);
    wr32(INPUT_REG_FEATURES, 0xffffffff);
    wr32(INPUT_REG_STATUS, 0xffffffff);
    wr32(INPUT_REG_REPORT_LEN, 0x1616);
    wr32(0x200, 0xffffffff);
    wr32(0x3ffc, 0xffffffff);
    EQ(rd32(INPUT_REG_ID), 0x4e49364d);
    EQ(rd32(INPUT_REG_FEATURES), 0x0001000f);
    EQ(rd32(INPUT_REG_STATUS), 0);
    EQ(rd32(INPUT_REG_REPORT_LEN), 0);
    EQ(stat(INPUT_ST_BAD_ACCESS), 0);
    CHECK(!input_model_line(&M));
    /* CTRL keeps only its RW bits; RESET reads 0. */
    wr32(INPUT_REG_CTRL, 0x7ffffff8u | INPUT_CTRL_PTP_MODE);
    EQ(rd32(INPUT_REG_CTRL), INPUT_CTRL_PTP_MODE);
    wr32(INPUT_REG_CTRL, INPUT_CTRL_RESET);
    EQ(rd32(INPUT_REG_CTRL), 0);
    /* The sources field follows init. */
    input_model_init(&M, INPUT_FEAT_SRC_HOST | INPUT_FEAT_SRC_KBD, WATCHDOG);
    EQ(rd32(INPUT_REG_FEATURES), 0x0003000f);
    end();
}

/* F20: the report bytes of every collection (the Python test compares them). */
static void case_report_layout(void)
{
    begin("report_layout");
    enable();
    struct got g;
    drain(&g);
    keys(0x02, 3, 0x04, 0x9f, 0x28);
    absf(0x01, 32767, 12345, -1, 2);
    relf(0x12, -32767, 100, 127, -127);
    consumer(0xe9);
    drain(&g);
    EQ(g.n, 4);
    EQ(g.len[0], 22);
    EQ(g.len[1], 8);
    EQ(g.len[2], 8);
    EQ(g.len[3], 3);
    hex_report("keyboard", g.r[0], g.len[0]);
    hex_report("absolute", g.r[1], g.len[1]);
    hex_report("relative", g.r[2], g.len[2]);
    hex_report("consumer", g.r[3], g.len[3]);
    EQ(stat(INPUT_ST_FRAMES_OK), 4);
    EQ(stat(INPUT_ST_REPORTS), 4 + 2); /* + keyboard and consumer at ENABLE */
    end();
}

/* F9: nothing is queued before ENABLE; ENABLE emits the current state. */
static void case_before_enable(void)
{
    begin("before_enable");
    keys(0x01, 1, 0x04);
    absf(0, 100, 200, 3, 0);
    relf(0, 5, 5, 0, 0);
    consumer(0xe2);
    EQ(depth(), 0);
    EQ(rd32(INPUT_REG_REPORT_LEN), 0);
    EQ(stat(INPUT_ST_FRAMES_OK), 4);
    EQ(stat(INPUT_ST_REPORTS), 0);
    /* ENABLE without IRQ_EN: reports queue, the line stays low. */
    wr32(INPUT_REG_CTRL, INPUT_CTRL_ENABLE);
    CHECK(!input_model_line(&M));
    EQ(rd32(INPUT_REG_IRQ_STATUS), 0);
    EQ(rd32(INPUT_REG_STATUS) & INPUT_STATUS_PENDING, INPUT_STATUS_PENDING);
    struct got g;
    drain(&g);
    /* Keyboard, absolute (it was given, wheel not repeated), consumer; no relative (no
     * buttons held, motion is not state). */
    EQ(g.n, 3);
    EQ(g.r[0][0], 1);
    EQ(g.r[0][1], 0x01);
    CHECK(key_down(g.r[0], 0x04));
    EQ(g.r[1][0], 2);
    EQ(g.r[1][2] | g.r[1][3] << 8, 100);
    EQ(g.r[1][4] | g.r[1][5] << 8, 200);
    EQ(g.r[1][6], 0);
    EQ(g.r[2][0], 4);
    EQ(g.r[2][1] | g.r[2][2] << 8, 0xe2);
    /* A fresh model: only keyboard and consumer, both all-up. */
    fresh();
    wr32(INPUT_REG_CTRL, INPUT_CTRL_ENABLE);
    drain(&g);
    EQ(g.n, 2);
    EQ(g.r[0][0], 1);
    EQ(g.r[1][0], 4);
    for (int i = 1; i < 22; i++)
        EQ(g.r[0][i], 0);
    /* Relative buttons held at ENABLE are state. */
    fresh();
    relf(0x04, 0, 0, 0, 0);
    wr32(INPUT_REG_CTRL, INPUT_CTRL_ENABLE);
    drain(&g);
    EQ(g.n, 3);
    EQ(g.r[1][0], 3);
    EQ(g.r[1][1], 0x04);
    EQ(g.r[1][2] | g.r[1][3] | g.r[1][4] | g.r[1][5] | g.r[1][6] | g.r[1][7], 0);
    end();
}

/* F13/F12: the line equation, POP, SEQ, depth. */
static void case_pop_and_line(void)
{
    begin("pop_and_line");
    wr32(INPUT_REG_CTRL, INPUT_CTRL_ENABLE);
    CHECK(!input_model_line(&M)); /* pending, IRQ_EN clear */
    wr32(INPUT_REG_CTRL, INPUT_CTRL_ENABLE | INPUT_CTRL_IRQ_EN);
    CHECK(input_model_line(&M));
    EQ(rd32(INPUT_REG_IRQ_STATUS), 1);
    EQ(depth(), 2);
    u32 s1 = rd32(INPUT_REG_SEQ);
    EQ(rd32(INPUT_REG_REPORT_LEN), 22 | 1 << 8);
    /* Masking drops the line, the queue stays. */
    wr32(INPUT_REG_CTRL, INPUT_CTRL_ENABLE);
    CHECK(!input_model_line(&M));
    EQ(depth(), 2);
    wr32(INPUT_REG_CTRL, INPUT_CTRL_ENABLE | INPUT_CTRL_IRQ_EN);
    /* POP needs the value 1. */
    wr32(INPUT_REG_POP, 0);
    EQ(depth(), 2);
    wr32(INPUT_REG_POP, 1);
    EQ(depth(), 1);
    EQ(rd32(INPUT_REG_SEQ), s1 + 1);
    EQ(rd32(INPUT_REG_REPORT_LEN), 3 | 4 << 8);
    wr32(INPUT_REG_POP, 1);
    EQ(depth(), 0);
    CHECK(!input_model_line(&M));
    EQ(rd32(INPUT_REG_STATUS), 0);
    EQ(rd32(INPUT_REG_REPORT_LEN), 0);
    EQ(rd64(INPUT_REG_REPORT), 0);
    /* POP on an empty queue: nothing. */
    wr32(INPUT_REG_POP, 1);
    EQ(depth(), 0);
    /* A host frame raises it again, sequence continues. */
    keys(0, 1, 0x05);
    CHECK(input_model_line(&M));
    EQ(rd32(INPUT_REG_SEQ), s1 + 2);
    EQ(rd32(INPUT_REG_STATUS), 1 | 1 << 16);
    /* POP while disabled does nothing (the queue is flushed then anyway). */
    wr32(INPUT_REG_CTRL, 0);
    EQ(depth(), 0);
    wr32(INPUT_REG_POP, 1);
    CHECK(!input_model_line(&M));
    end();
}

/* F10: ENABLE 1->0 flushes; RESET flushes and re-emits, clears OVERFLOW; one emit per write. */
static void case_enable_reset(void)
{
    begin("enable_reset");
    keys(0, 1, 0x04);
    enable();
    EQ(depth(), 2);
    keys(0, 1, 0x05);
    EQ(depth(), 3);
    wr32(INPUT_REG_CTRL, 0);
    EQ(depth(), 0);
    keys(0, 1, 0x06); /* disabled: state only */
    EQ(depth(), 0);
    /* ENABLE and RESET in one write emit the state once. */
    wr32(INPUT_REG_CTRL, INPUT_CTRL_ENABLE | INPUT_CTRL_IRQ_EN | INPUT_CTRL_RESET);
    EQ(depth(), 2);
    struct got g;
    u8 r[INPUT_REPORT_MAX];
    EQ(pop_report(r, NULL), 22);
    CHECK(key_down(r, 0x06) && !key_down(r, 0x04));
    /* RESET while enabled: flush, re-emit; the CTRL bits stay. */
    keys(0, 2, 0x06, 0x07);
    keys(0, 1, 0x07);
    EQ(depth(), 3);
    wr32(INPUT_REG_CTRL, INPUT_CTRL_ENABLE | INPUT_CTRL_IRQ_EN | INPUT_CTRL_RESET);
    EQ(rd32(INPUT_REG_CTRL), INPUT_CTRL_ENABLE | INPUT_CTRL_IRQ_EN);
    drain(&g);
    EQ(g.n, 2);
    CHECK(key_down(g.r[0], 0x07) && !key_down(g.r[0], 0x06));
    /* RESET while disabled: no reports. */
    wr32(INPUT_REG_CTRL, INPUT_CTRL_RESET);
    EQ(depth(), 0);
    /* OVERFLOW is sticky until RESET; DROPPED counts since ENABLE. */
    enable();
    drain(&g);
    for (int i = 0; i < 300; i++)
        keys(0, 1, (u8)(0x04 + (i & 1)));
    CHECK(rd32(INPUT_REG_STATUS) & INPUT_STATUS_OVERFLOW);
    CHECK(rd32(INPUT_REG_DROPPED) > 0);
    drain(&g);
    CHECK(rd32(INPUT_REG_STATUS) & INPUT_STATUS_OVERFLOW);
    wr32(INPUT_REG_CTRL, INPUT_CTRL_ENABLE | INPUT_CTRL_IRQ_EN | INPUT_CTRL_RESET);
    EQ(rd32(INPUT_REG_STATUS) & INPUT_STATUS_OVERFLOW, 0);
    u32 dropped = rd32(INPUT_REG_DROPPED);
    CHECK(dropped > 0);
    wr32(INPUT_REG_CTRL, 0);
    enable();
    EQ(rd32(INPUT_REG_DROPPED), 0);
    CHECK(stat(INPUT_ST_DROPPED) >= dropped);
    end();
}

/* Identical states queue nothing; KEEPALIVE queues nothing. */
static void case_no_duplicate(void)
{
    begin("no_duplicate");
    enable();
    struct got g;
    drain(&g);
    keys(0x02, 1, 0x04);
    keys(0x02, 1, 0x04);
    send(INPUT_HOST_KEEPALIVE, NULL, 0);
    EQ(depth(), 1);
    absf(1, 10, 10, 0, 0);
    absf(1, 10, 10, 0, 0);
    EQ(depth(), 2);
    absf(1, 10, 10, 1, 0); /* wheel motion is a report even at the same position */
    EQ(depth(), 3);
    relf(0, 0, 0, 0, 0);
    EQ(depth(), 3);
    relf(1, 0, 0, 0, 0);
    EQ(depth(), 4);
    relf(1, 0, 0, 0, 0);
    EQ(depth(), 4);
    relf(1, 1, 0, 0, 0);
    EQ(depth(), 5);
    consumer(0xe9);
    consumer(0xe9);
    EQ(depth(), 6);
    /* Keys given in a different order are the same state. */
    keys(0x02, 2, 0x05, 0x04);
    keys(0x02, 2, 0x04, 0x05);
    EQ(depth(), 7);
    EQ(stat(INPUT_ST_FRAMES_OK), 14);
    end();
}

/* F1: garbage before, between and inside frames is skipped and counted. */
static void case_garbage_resync(void)
{
    begin("garbage_resync");
    enable();
    struct got g;
    drain(&g);
    u8 junk[] = {0x00, 0xff, 'M', 'M', 0x36 ^ 1, '6', 'x', 'M'};
    input_model_host_rx(&M, junk, sizeof(junk), now);
    keys(0, 1, 0x04);
    EQ(stat(INPUT_ST_FRAMES_OK), 1);
    CHECK(stat(INPUT_ST_SKIPPED) >= 7);
    EQ(stat(INPUT_ST_BAD_CRC), 0);
    drain(&g);
    EQ(g.n, 1);
    CHECK(key_down(g.r[0], 0x04));
    /* A lone 'M' at the end of a burst, then a frame. */
    u8 m = 'M';
    input_model_host_rx(&M, &m, 1, now);
    keys(0, 1, 0x05);
    EQ(stat(INPUT_ST_FRAMES_OK), 2);
    drain(&g);
    EQ(g.n, 1);
    CHECK(key_down(g.r[0], 0x05));
    EQ(stat(INPUT_ST_RX_BYTES), sizeof(junk) + 1 + 2 * 13);
    end();
}

/* F2: a bad CRC drops the frame and the scan resumes at the byte after its sync. */
static void case_bad_crc_rescan(void)
{
    begin("bad_crc_rescan");
    enable();
    struct got g;
    drain(&g);
    /* An outer KEYS frame whose payload bytes contain a complete valid frame. */
    u8 inner[32];
    u8 p[3] = {0, 1, 0x1d};
    u32 ni = frame(inner, INPUT_HOST_KEYS, p, 3);
    u8 outer[64];
    outer[0] = 'M';
    outer[1] = '6';
    outer[2] = INPUT_HOST_KEYS;
    outer[3] = (u8)(ni + 2);
    outer[4] = outer[5] = 0;
    outer[6] = 0;
    outer[7] = 0;
    memcpy(outer + 8, inner, ni);
    u32 n = 6 + ni + 2;
    for (int i = 0; i < 4; i++)
        outer[n + i] = 0x5a; /* wrong CRC */
    n += 4;
    input_model_host_rx(&M, outer, n, now);
    EQ(stat(INPUT_ST_BAD_CRC), 1);
    EQ(stat(INPUT_ST_FRAMES_OK), 1);
    drain(&g);
    EQ(g.n, 1);
    CHECK(key_down(g.r[0], 0x1d));
    /* A valid frame with one flipped payload bit. */
    u8 buf[32];
    u8 q[3] = {0, 1, 0x04};
    u32 k = frame(buf, INPUT_HOST_KEYS, q, 3);
    buf[8] ^= 0x01;
    input_model_host_rx(&M, buf, k, now);
    EQ(stat(INPUT_ST_BAD_CRC), 2);
    EQ(depth(), 0);
    /* A flipped CRC byte. */
    k = frame(buf, INPUT_HOST_KEYS, q, 3);
    buf[k - 1] ^= 0x80;
    input_model_host_rx(&M, buf, k, now);
    EQ(stat(INPUT_ST_BAD_CRC), 3);
    EQ(depth(), 0);
    /* And the stream is still in sync. */
    keys(0, 1, 0x04);
    EQ(depth(), 1);
    end();
}

/* F3/F4: a bad header is refused before any payload is waited for. */
static void case_bad_header(void)
{
    begin("bad_header");
    enable();
    struct got g;
    drain(&g);
    u8 h1[] = {'M', '6', INPUT_HOST_KEYS, 65};
    input_model_host_rx(&M, h1, sizeof(h1), now);
    EQ(stat(INPUT_ST_BAD_HEADER), 1);
    u8 h2[] = {'M', '6', 0x00, 0};
    input_model_host_rx(&M, h2, sizeof(h2), now);
    u8 h3[] = {'M', '6', 0x08, 0};
    input_model_host_rx(&M, h3, sizeof(h3), now);
    u8 h4[] = {'M', '6', 0xff, 255};
    input_model_host_rx(&M, h4, sizeof(h4), now);
    EQ(stat(INPUT_ST_BAD_HEADER), 4);
    /* The next valid frame right behind them is found. */
    keys(0, 1, 0x04);
    EQ(stat(INPUT_ST_FRAMES_OK), 1);
    EQ(depth(), 1);
    /* A header whose length byte reads as a sync of the next frame. */
    u8 h5[] = {'M', '6', 0x4d, 0x36};
    input_model_host_rx(&M, h5, sizeof(h5), now);
    keys(0, 1, 0x05);
    EQ(stat(INPUT_ST_FRAMES_OK), 2);
    EQ(depth(), 2);
    end();
}

/* F3b: a damaged header that claims a long payload holds the frames behind it only until the
 * feed has been quiet for 50 ms (watchdog / 40); then they are re-scanned. */
static void case_stalled_header(void)
{
    begin("stalled_header");
    enable();
    struct got g;
    drain(&g);
    u8 h[] = {'M', '6', INPUT_HOST_KEYS, 64};
    input_model_host_rx(&M, h, sizeof(h), now);
    keys(0x01, 1, 0x04); /* swallowed as payload for now */
    EQ(depth(), 0);
    input_model_poll(&M, now + WATCHDOG / 40 - 1);
    EQ(depth(), 0);
    input_model_poll(&M, now + WATCHDOG / 40);
    EQ(depth(), 1);
    EQ(stat(INPUT_ST_FRAMES_OK), 1);
    drain(&g);
    CHECK(key_down(g.r[0], 0x04));
    EQ(M.rx_len, 0);
    /* A frame cut short by the host is given up the same way, and the next one parses. */
    u8 buf[32];
    u8 p[3] = {0, 1, 0x05};
    u32 n = frame(buf, INPUT_HOST_KEYS, p, 3);
    input_model_host_rx(&M, buf, n - 3, now);
    input_model_poll(&M, now + WATCHDOG / 40);
    EQ(M.rx_len, 0);
    now += WATCHDOG / 40;
    keys(0, 1, 0x06);
    EQ(stat(INPUT_ST_FRAMES_OK), 2);
    drain(&g);
    EQ(g.n, 1);
    CHECK(key_down(g.r[0], 0x06) && !key_down(g.r[0], 0x05));
    end();
}

/* L6: frames written by the launcher's encoder (tests/python), fed in one burst. */
static void case_corpus(const char *path)
{
    begin("corpus");
    FILE *f = fopen(path, "rb");
    CHECK(f);
    static u8 data[65536];
    u32 n = f ? (u32)fread(data, 1, sizeof(data), f) : 0;
    if (f)
        fclose(f);
    enable();
    input_model_host_rx(&M, data, n, now);
    input_model_poll(&M, now + WATCHDOG / 40); /* nothing may be left waiting */
    EQ(M.rx_len, 0);
    struct got g;
    drain(&g);
    add_extra(",\"reports\":[");
    for (int i = 0; i < g.n; i++) {
        add_extra("%s\"", i ? "," : "");
        for (int k = 0; k < g.len[i]; k++)
            add_extra("%02x", g.r[i][k]);
        add_extra("\"");
    }
    add_extra("],\"frames_ok\":%u,\"bad_crc\":%u,\"seq_gaps\":%u", stat(INPUT_ST_FRAMES_OK),
              stat(INPUT_ST_BAD_CRC), stat(INPUT_ST_SEQ_GAPS));
    end();
}

/* F5: a valid CRC with an invalid payload changes nothing. */
static void case_bad_payload(void)
{
    begin("bad_payload");
    keys(0x01, 1, 0x04);
    enable();
    struct got g;
    drain(&g);
    u32 bad = 0;
#define BAD(type, ...)                                                                             \
    do {                                                                                           \
        u8 _p[] = {__VA_ARGS__};                                                                   \
        send(type, _p, sizeof(_p));                                                                \
        bad++;                                                                                     \
    } while (0)
    /* KEYS: short, n too big, n disagrees with len, usages outside 0x04..0x9F. */
    BAD(INPUT_HOST_KEYS, 0);
    BAD(INPUT_HOST_KEYS, 0, 17, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4);
    BAD(INPUT_HOST_KEYS, 0, 2, 0x04);
    BAD(INPUT_HOST_KEYS, 0, 1, 0x04, 0x05);
    BAD(INPUT_HOST_KEYS, 0, 1, 0x03);
    BAD(INPUT_HOST_KEYS, 0, 1, 0x00);
    BAD(INPUT_HOST_KEYS, 0, 1, 0xa0);
    BAD(INPUT_HOST_KEYS, 0, 1, 0xe0);
    /* ABS: length, X or Y above 32767, buttons above 5, wheel/pan -128. */
    BAD(INPUT_HOST_ABS, 0, 0, 0, 0, 0, 0);
    BAD(INPUT_HOST_ABS, 0, 0, 0, 0, 0, 0, 0, 0);
    BAD(INPUT_HOST_ABS, 0, 0x00, 0x80, 0, 0, 0, 0);
    BAD(INPUT_HOST_ABS, 0, 0, 0, 0x00, 0x80, 0, 0);
    BAD(INPUT_HOST_ABS, 0x20, 0, 0, 0, 0, 0, 0);
    BAD(INPUT_HOST_ABS, 0, 0, 0, 0, 0, 0x80, 0);
    BAD(INPUT_HOST_ABS, 0, 0, 0, 0, 0, 0, 0x80);
    /* REL: length, -32768, buttons, wheel. */
    BAD(INPUT_HOST_REL, 0, 0, 0, 0, 0, 0);
    BAD(INPUT_HOST_REL, 0, 0x00, 0x80, 0, 0, 0, 0);
    BAD(INPUT_HOST_REL, 0, 0, 0, 0x00, 0x80, 0, 0);
    BAD(INPUT_HOST_REL, 0x80, 0, 0, 0, 0, 0, 0);
    BAD(INPUT_HOST_REL, 0, 0, 0, 0, 0, 0x80, 0);
    /* CONSUMER: length, above 0x3FF. */
    BAD(INPUT_HOST_CONSUMER, 0xe9);
    BAD(INPUT_HOST_CONSUMER, 0x00, 0x04);
    /* HELLO: length, version, reserved flags. */
    BAD(INPUT_HOST_HELLO, 1, 0);
    BAD(INPUT_HOST_HELLO, 2, 0, 0, 0);
    BAD(INPUT_HOST_HELLO, 1, 0, 1, 0);
    /* KEEPALIVE and RELEASE_ALL carry nothing. */
    BAD(INPUT_HOST_KEEPALIVE, 0);
    BAD(INPUT_HOST_RELEASE_ALL, 0);
#undef BAD
    EQ(stat(INPUT_ST_BAD_PAYLOAD), bad);
    EQ(stat(INPUT_ST_FRAMES_OK), 1);
    EQ(stat(INPUT_ST_HELLOS), 0);
    EQ(depth(), 0);
    /* The held key survives all of it (RELEASE_ALL with a payload did not act). */
    keys(0x01, 2, 0x04, 0x05);
    drain(&g);
    EQ(g.n, 1);
    CHECK(key_down(g.r[0], 0x04) && key_down(g.r[0], 0x05));
    EQ(g.r[0][1], 0x01);
    /* The boundaries themselves are valid. */
    keys(0, 16, 0x04, 0x9f, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19);
    absf(0x1f, 32767, 32767, -127, 127);
    relf(0x1f, -32767, 32767, 127, -127);
    consumer(0x3ff);
    EQ(stat(INPUT_ST_BAD_PAYLOAD), bad);
    EQ(stat(INPUT_ST_FRAMES_OK), 6);
    end();
}

/* F6: any split of the stream gives the same result. */
static unsigned int rng_state = 12345;
static unsigned int rng(void)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return rng_state >> 8;
}

static u32 build_stream(u8 *s)
{
    u32 n = 0;
    u8 junk[] = {0x13, 'M', '6', 0x99, 0x01};
    seq_out = 7;
    u8 p[18];
    for (int i = 0; i < 40; i++) {
        switch (i % 5) {
            case 0:
                p[0] = (u8)i;
                p[1] = 2;
                p[2] = (u8)(4 + i);
                p[3] = (u8)(5 + i);
                n += frame(s + n, INPUT_HOST_KEYS, p, 4);
                break;
            case 1: {
                u8 a[7] = {1, (u8)i, 1, 2, 0, 1, 0};
                n += frame(s + n, INPUT_HOST_ABS, a, 7);
                break;
            }
            case 2:
                memcpy(s + n, junk, sizeof(junk));
                n += sizeof(junk);
                break;
            case 3: {
                u8 r[7] = {0, (u8)i, 0, 0xff, 0xff, 0, 0};
                n += frame(s + n, INPUT_HOST_REL, r, 7);
                break;
            }
            default: {
                u8 c[2] = {(u8)(0xe0 + i), 0};
                n += frame(s + n, INPUT_HOST_CONSUMER, c, 2);
                break;
            }
        }
    }
    return n;
}

static void case_split_feed(void)
{
    begin("split_feed");
    static u8 stream[4096];
    u32 n = build_stream(stream);
    struct got a, b;
    static struct input_model ref;

    enable();
    input_model_host_rx(&M, stream, n, now);
    ref = M;
    drain(&a);

    for (int mode = 0; mode < 3; mode++) {
        fresh();
        enable();
        u32 off = 0;
        while (off < n) {
            u32 chunk = mode == 0 ? 1 : mode == 1 ? 1 + rng() % 7 : 1 + rng() % 90;
            if (chunk > n - off)
                chunk = n - off;
            input_model_host_rx(&M, stream + off, chunk, now);
            off += chunk;
        }
        for (int s = 0; s < INPUT_ST_COUNT; s++)
            EQ(M.stats[s], ref.stats[s]);
        drain(&b);
        EQ(b.n, a.n);
        for (int i = 0; i < a.n && i < b.n; i++)
            CHECK(!memcmp(a.r[i], b.r[i], (size_t)a.len[i]));
    }
    EQ(ref.stats[INPUT_ST_FRAMES_OK], 32);
    add_extra(",\"frames\":%u,\"reports\":%d", ref.stats[INPUT_ST_FRAMES_OK], a.n);
    end();
}

/* F7: seq gaps are counted, never enforced. */
static void case_seq_gaps(void)
{
    begin("seq_gaps");
    enable();
    seq_out = 1;
    keys(0, 1, 4);
    keys(0, 1, 5);
    seq_out = 4;
    keys(0, 1, 6);
    EQ(stat(INPUT_ST_SEQ_GAPS), 1);
    EQ(stat(INPUT_ST_FRAMES_OK), 3);
    seq_out = 0xffff;
    keys(0, 1, 7);
    keys(0, 1, 8); /* seq 0 after 0xffff: no gap */
    EQ(stat(INPUT_ST_SEQ_GAPS), 2);
    /* HELLO restarts the sequence. */
    seq_out = 100;
    hello();
    seq_out = 50;
    keys(0, 1, 9);
    /* HELLO is not checked and becomes the reference: 50 after it is a gap. */
    EQ(stat(INPUT_ST_SEQ_GAPS), 3);
    EQ(stat(INPUT_ST_HELLOS), 1);
    end();
}

/* F8: the held-key watchdog. */
static void case_watchdog(void)
{
    begin("watchdog");
    enable();
    struct got g;
    drain(&g);
    keys(0x05, 2, 0x04, 0x2a);
    absf(0x01, 50, 60, 0, 0);
    consumer(0xe9);
    drain(&g);
    u64 t0 = now;
    input_model_poll(&M, t0 + WATCHDOG - 1);
    EQ(depth(), 0);
    EQ(stat(INPUT_ST_WATCHDOG), 0);
    /* A KEEPALIVE refreshes it. */
    now = t0 + WATCHDOG - 1;
    send(INPUT_HOST_KEEPALIVE, NULL, 0);
    input_model_poll(&M, t0 + WATCHDOG + 10);
    EQ(depth(), 0);
    /* Invalid frames do not. */
    u8 bad[] = {0, 0};
    now = t0 + 2 * WATCHDOG - 10;
    send(INPUT_HOST_KEYS, bad, 1);
    input_model_poll(&M, t0 + 2 * WATCHDOG - 2);
    EQ(depth(), 0);
    /* Exactly 2 s after the KEEPALIVE. */
    input_model_poll(&M, t0 + 2 * WATCHDOG - 1);
    EQ(stat(INPUT_ST_WATCHDOG), 1);
    drain(&g);
    /* All up: keyboard, absolute buttons (position kept), consumer. */
    EQ(g.n, 3);
    EQ(g.r[0][0], 1);
    for (int i = 1; i < 22; i++)
        EQ(g.r[0][i], 0);
    EQ(g.r[1][0], 2);
    EQ(g.r[1][1], 0);
    EQ(g.r[1][2], 50);
    EQ(g.r[1][4], 60);
    EQ(g.r[2][0], 4);
    EQ(g.r[2][1] | g.r[2][2], 0);
    /* Nothing held: the watchdog stays quiet. */
    input_model_poll(&M, t0 + 100 * WATCHDOG);
    EQ(depth(), 0);
    EQ(stat(INPUT_ST_WATCHDOG), 1);
    /* Relative buttons held count too; the release is a buttons-up report. */
    now = t0 + 200 * WATCHDOG;
    relf(0x02, 3, 4, 0, 0);
    drain(&g);
    input_model_poll(&M, now + WATCHDOG);
    drain(&g);
    EQ(g.n, 1);
    EQ(g.r[0][0], 3);
    EQ(g.r[0][1], 0);
    EQ(stat(INPUT_ST_WATCHDOG), 2);
    /* Disabled: the state is released, nothing is queued. */
    wr32(INPUT_REG_CTRL, 0);
    now += 10 * WATCHDOG;
    keys(0, 1, 0x04);
    input_model_poll(&M, now + WATCHDOG);
    EQ(stat(INPUT_ST_WATCHDOG), 3);
    enable();
    drain(&g);
    EQ(g.n, 3); /* keyboard (all up), absolute (it was given), consumer */
    CHECK(!key_down(g.r[0], 0x04));
    EQ(g.r[1][0], 2);
    end();
}

/* F15b: RELEASE_ALL and HELLO put the host state all up. */
static void case_release_all(void)
{
    begin("release_all");
    enable();
    struct got g;
    drain(&g);
    keys(0xff, 1, 0x04);
    relf(0x01, 0, 0, 0, 0);
    consumer(0xea);
    drain(&g);
    send(INPUT_HOST_RELEASE_ALL, NULL, 0);
    drain(&g);
    EQ(g.n, 3);
    EQ(g.r[0][1], 0);
    CHECK(!key_down(g.r[0], 0x04));
    EQ(g.r[1][0], 3);
    EQ(g.r[1][1], 0);
    EQ(g.r[2][0], 4);
    keys(0x01, 1, 0x05);
    drain(&g);
    hello();
    drain(&g);
    EQ(g.n, 1);
    EQ(g.r[0][1], 0);
    EQ(stat(INPUT_ST_HELLOS), 1);
    /* Carrier exit path: input_model_release(host). */
    keys(0x01, 1, 0x05);
    drain(&g);
    input_model_release(&M, INPUT_SRC_HOST);
    drain(&g);
    EQ(g.n, 1);
    EQ(g.r[0][1], 0);
    /* Releasing nothing queues nothing. */
    input_model_release(&M, INPUT_SRC_HOST);
    EQ(depth(), 0);
    end();
}

/* F18: the keyboard report is the OR of every source. */
static void case_merge(void)
{
    begin("merge");
    enable();
    struct got g;
    drain(&g);
    u8 bm[INPUT_KBD_BITMAP] = {0};
    bm[0x2c / 8] |= 1u << (0x2c % 8); /* space */
    input_model_set_keyboard(&M, INPUT_SRC_INTERNAL, 0x10, bm);
    keys(0x01, 1, 0x04);
    drain(&g);
    EQ(g.n, 2);
    EQ(g.r[1][1], 0x11);
    CHECK(key_down(g.r[1], 0x04) && key_down(g.r[1], 0x2c));
    /* The same key from both sources: releasing one keeps it down. */
    keys(0x01, 2, 0x04, 0x2c);
    drain(&g);
    EQ(g.n, 0); /* the merged state did not change */
    send(INPUT_HOST_RELEASE_ALL, NULL, 0);
    drain(&g);
    EQ(g.n, 1);
    EQ(g.r[0][1], 0x10);
    CHECK(!key_down(g.r[0], 0x04) && key_down(g.r[0], 0x2c));
    memset(bm, 0, sizeof(bm));
    input_model_set_keyboard(&M, 7, 0, bm); /* an unknown source: ignored */
    input_model_set_keyboard(&M, INPUT_SRC_INTERNAL, 0, bm);
    drain(&g);
    EQ(g.n, 1);
    EQ(g.r[0][1], 0);
    end();
}

/* F11/F17: overflow keeps every collection's final state; pointers go first; the latched
 * report never changes under the driver. */
static void case_overflow(void)
{
    begin("overflow");
    enable();
    struct got g;
    drain(&g);
    u32 before = stat(INPUT_ST_REPORTS);
    /* Keyboard and consumer only. */
    for (int i = 0; i < 400; i++) {
        keys(0, 1, (u8)(0x04 + (i % 20)));
        if (i % 3 == 0)
            consumer((u16)(0xe0 + (i % 7)));
    }
    EQ(depth(), INPUT_QUEUE_LEN);
    CHECK(rd32(INPUT_REG_STATUS) & INPUT_STATUS_OVERFLOW);
    u32 dropped = rd32(INPUT_REG_DROPPED);
    EQ(stat(INPUT_ST_REPORTS) - before - dropped, INPUT_QUEUE_LEN);
    EQ(stat(INPUT_ST_HIGH_WATER), INPUT_QUEUE_LEN);
    drain(&g);
    EQ(g.n, (int)INPUT_QUEUE_LEN);
    /* The last keyboard and consumer reports in the queue are the final states. */
    int lk = -1, lc = -1;
    for (int i = 0; i < g.n; i++) {
        if (g.r[i][0] == 1)
            lk = i;
        if (g.r[i][0] == 4)
            lc = i;
    }
    CHECK(lk >= 0 && lc >= 0);
    if (lk >= 0)
        CHECK(key_down(g.r[lk], (u8)(0x04 + (399 % 20))));
    if (lc >= 0)
        EQ(g.r[lc][1], 0xe0 + (399 % 7));

    /* Pointer reports are dropped first: 10 keyboard changes, then 600 absolute moves. */
    wr32(INPUT_REG_CTRL, INPUT_CTRL_ENABLE | INPUT_CTRL_IRQ_EN | INPUT_CTRL_RESET);
    drain(&g);
    for (int i = 0; i < 10; i++)
        keys(0, 1, (u8)(0x10 + i));
    for (int i = 0; i < 600; i++)
        absf(0, (u16)i, (u16)(i * 2), 0, 0);
    drain(&g);
    int kb = 0;
    int last_abs = -1;
    for (int i = 0; i < g.n; i++) {
        if (g.r[i][0] == 1)
            kb++;
        if (g.r[i][0] == 2)
            last_abs = i;
    }
    EQ(kb, 10);
    CHECK(last_abs >= 0);
    if (last_abs >= 0) {
        EQ(g.r[last_abs][2] | g.r[last_abs][3] << 8, 599);
        EQ(g.r[last_abs][4] | g.r[last_abs][5] << 8, 1198);
    }

    /* The latched report and SEQ never change while the queue overflows. */
    wr32(INPUT_REG_CTRL, INPUT_CTRL_ENABLE | INPUT_CTRL_IRQ_EN | INPUT_CTRL_RESET);
    drain(&g);
    keys(0, 1, 0x3a);
    u32 seq = rd32(INPUT_REG_SEQ);
    u64 r0 = rd64(INPUT_REG_REPORT), r1 = rd64(INPUT_REG_REPORT + 8);
    for (int i = 0; i < 1000; i++)
        keys(0, 1, (u8)(0x04 + (i & 1)));
    EQ(rd32(INPUT_REG_SEQ), seq);
    EQ(rd64(INPUT_REG_REPORT), r0);
    EQ(rd64(INPUT_REG_REPORT + 8), r1);
    EQ(depth(), INPUT_QUEUE_LEN);
    /* Sequence numbers in the queue still increase. */
    u32 prev = 0;
    bool ok = true;
    for (u32 i = 0; i < INPUT_QUEUE_LEN; i++) {
        u32 s = rd32(INPUT_REG_SEQ);
        if (i && s <= prev)
            ok = false;
        prev = s;
        wr32(INPUT_REG_POP, 1);
    }
    CHECK(ok);
    add_extra(",\"dropped\":%u", stat(INPUT_ST_DROPPED));
    end();
}

/*
 * F11b: the O(n) victim choice and the slot-order ring drop exactly what the original
 * quadratic scan-and-shift queue dropped.  A second model R sees the same frames and is popped
 * after every step, which gives the stream of pushed reports (pushes do not depend on the
 * queue); a reference queue in this file applies the original rule to that stream.  M is
 * popped rarely, so it overflows nearly all the time.
 */
static struct input_model R;

struct ref_entry {
    u32 seq;
    u8 id, len;
    u8 data[INPUT_REPORT_MAX];
};
static struct ref_entry refq[INPUT_QUEUE_LEN];
static u32 ref_n;

static int ref_victim(u8 incoming)
{
    int fallback = -1;
    for (u32 i = 1; i < ref_n; i++) {
        u8 id = refq[i].id;
        bool superseded = id == incoming;
        for (u32 k = i + 1; !superseded && k < ref_n; k++)
            superseded = refq[k].id == id;
        if (!superseded)
            continue;
        if (id == INPUT_RID_ABSOLUTE || id == INPUT_RID_RELATIVE)
            return (int)i;
        if (fallback < 0)
            fallback = (int)i;
    }
    return fallback;
}

static void ref_push(const struct ref_entry *e)
{
    if (ref_n == INPUT_QUEUE_LEN) {
        int v = ref_victim(e->id);
        if (v < 0)
            return;
        memmove(&refq[v], &refq[v + 1], (ref_n - (u32)v - 1) * sizeof(refq[0]));
        ref_n--;
    }
    refq[ref_n++] = *e;
}

static int model_pop(struct input_model *m, struct ref_entry *e)
{
    u64 v = 0;
    CHECK(input_model_mmio(m, INPUT_REG_REPORT_LEN, 2, &v, false));
    u32 len = (u32)v & 0xff;
    if (!len)
        return 0;
    e->len = (u8)len;
    e->id = (u8)(v >> 8);
    CHECK(input_model_mmio(m, INPUT_REG_SEQ, 2, &v, false));
    e->seq = (u32)v;
    for (u32 i = 0; i < INPUT_REPORT_MAX; i += 8) {
        CHECK(input_model_mmio(m, INPUT_REG_REPORT + i, 3, &v, false));
        for (u32 b = 0; b < 8; b++)
            e->data[i + b] = (u8)(v >> (8 * b));
    }
    v = 1;
    CHECK(input_model_mmio(m, INPUT_REG_POP, 2, &v, true));
    return 1;
}

static void both_rx(const u8 *buf, u32 n)
{
    input_model_host_rx(&M, buf, n, now);
    input_model_host_rx(&R, buf, n, now);
}

static void case_overflow_reference(void)
{
    begin("overflow_reference");
    input_model_init(&R, INPUT_FEAT_SRC_HOST, WATCHDOG);
    ref_n = 0;
    rng_state = 0x0f10;
    u64 en = INPUT_CTRL_ENABLE | INPUT_CTRL_IRQ_EN;
    CHECK(input_model_mmio(&M, INPUT_REG_CTRL, 2, &en, true));
    CHECK(input_model_mmio(&R, INPUT_REG_CTRL, 2, &en, true));
    u32 mismatches = 0, pops = 0;
    u8 buf[INPUT_HOST_MAX_FRAME + 8];
    for (int i = 0; i < 40000 && mismatches < 5; i++) {
        unsigned int r = rng();
        u32 n;
        switch (r % 6) {
            case 0:
            case 1: {
                u8 p[4] = {(u8)(rng() & 3), 1, (u8)(4 + rng() % 8)};
                n = frame(buf, INPUT_HOST_KEYS, p, 3);
                break;
            }
            case 2: {
                u16 x = (u16)(rng() % 32768), y = (u16)(rng() % 32768);
                u8 p[7] = {(u8)(rng() & 1), (u8)x, (u8)(x >> 8), (u8)y, (u8)(y >> 8), 0, 0};
                n = frame(buf, INPUT_HOST_ABS, p, 7);
                break;
            }
            case 3: {
                s16 dx = (s16)(rng() % 200 - 100);
                u8 p[7] = {(u8)(rng() & 1), (u8)dx, (u8)((u16)dx >> 8), 1, 0, 0, 0};
                n = frame(buf, INPUT_HOST_REL, p, 7);
                break;
            }
            case 4: {
                u16 u = (u16)(rng() % 3 ? 0xe9 + rng() % 2 : 0);
                u8 p[2] = {(u8)u, (u8)(u >> 8)};
                n = frame(buf, INPUT_HOST_CONSUMER, p, 2);
                break;
            }
            default:
                n = frame(buf, INPUT_HOST_RELEASE_ALL, NULL, 0);
                break;
        }
        both_rx(buf, n);
        struct ref_entry e;
        while (model_pop(&R, &e))
            ref_push(&e);
        if (rng() % 64 == 0 && ref_n) {
            struct ref_entry got;
            CHECK(model_pop(&M, &got));
            if (got.seq != refq[0].seq || memcmp(got.data, refq[0].data, refq[0].len))
                mismatches++;
            memmove(&refq[0], &refq[1], (ref_n - 1) * sizeof(refq[0]));
            ref_n--;
            pops++;
        }
    }
    u32 n = 0;
    struct ref_entry got;
    while (model_pop(&M, &got)) {
        if (n >= ref_n || got.seq != refq[n].seq || got.len != refq[n].len ||
            memcmp(got.data, refq[n].data, got.len))
            mismatches++;
        n++;
    }
    EQ(n, ref_n);
    EQ(mismatches, 0);
    CHECK(stat(INPUT_ST_DROPPED) > 10000);
    add_extra(",\"pops\":%u,\"dropped\":%u", pops, stat(INPUT_ST_DROPPED));
    end();
}

/* F14: widths and alignment. */
static void case_access_widths(void)
{
    begin("access_widths");
    u64 v;
    /* 8- and 16-bit, unaligned 32/64, and a 128-bit pair: RAZ/WI, counted. */
    v = INPUT_CTRL_ENABLE;
    CHECK(!input_model_mmio(&M, INPUT_REG_CTRL, 0, &v, true));
    v = INPUT_CTRL_ENABLE;
    CHECK(!input_model_mmio(&M, INPUT_REG_CTRL, 1, &v, true));
    v = 0xffffffff;
    CHECK(!input_model_mmio(&M, INPUT_REG_CTRL + 2, 2, &v, true));
    v = 0xffffffffffffffffULL;
    CHECK(!input_model_mmio(&M, INPUT_REG_FEATURES + 4, 3, &v, true)); /* 64-bit at 0xC */
    EQ(rd32(INPUT_REG_CTRL), 0);
    v = 1;
    CHECK(!input_model_mmio(&M, INPUT_REG_ID, 0, &v, false));
    EQ(v, 0);
    v = 1;
    CHECK(!input_model_mmio(&M, INPUT_REG_ID + 1, 2, &v, false));
    EQ(v, 0);
    u64 pair[2] = {1, 1};
    CHECK(!input_model_mmio(&M, INPUT_REG_REPORT, 4, pair, false));
    EQ(pair[0], 0);
    EQ(pair[1], 0);
    v = 1;
    CHECK(!input_model_mmio(&M, 0x3ffc, 3, &v, false)); /* 64-bit at 0x3FFC: unaligned */
    EQ(stat(INPUT_ST_BAD_ACCESS), 8);
    /* 64-bit accesses act as two 32-bit ones. */
    wr64(INPUT_REG_FEATURES, (u64)(INPUT_CTRL_ENABLE | INPUT_CTRL_IRQ_EN) << 32);
    EQ(rd32(INPUT_REG_CTRL), INPUT_CTRL_ENABLE | INPUT_CTRL_IRQ_EN);
    EQ(rd64(INPUT_REG_FEATURES),
       0x0001000fULL | (u64)(INPUT_CTRL_ENABLE | INPUT_CTRL_IRQ_EN) << 32);
    EQ(depth(), 2);
    wr64(INPUT_REG_POP, 1); /* POP + REPORT_LEN (RO) */
    EQ(depth(), 1);
    /* A 32-bit read inside the report window. */
    EQ(rd32(INPUT_REG_REPORT), 4);
    EQ(stat(INPUT_ST_BAD_ACCESS), 8);
    end();
}

/* F16: output and feature reports. */
static void case_out_reports(void)
{
    begin("out_reports");
    enable();
    /* Keyboard LEDs: ID 1, 2 bytes, OUT_DATA[0] = ID. */
    wr32(INPUT_REG_OUT_DATA, 0x0201); /* id 1, Caps Lock */
    wr32(INPUT_REG_OUT_LEN, 2 | 1 << 8);
    EQ(M.leds, 0x02);
    EQ(stat(INPUT_ST_OUT_OK), 1);
    wr64(INPUT_REG_OUT_DATA, 0xff01);
    wr32(INPUT_REG_OUT_LEN, 2 | 1 << 8);
    EQ(M.leds, 0x1f); /* reserved LED bits dropped */
    /* Refused: a feature, another ID, a wrong length, len 0 or > 64, ID byte mismatch. */
    wr32(INPUT_REG_OUT_LEN, 2 | 1 << 8 | INPUT_OUT_FEATURE);
    wr32(INPUT_REG_OUT_LEN, 2 | 2 << 8);
    wr32(INPUT_REG_OUT_LEN, 3 | 1 << 8);
    wr32(INPUT_REG_OUT_LEN, 0 | 1 << 8);
    wr32(INPUT_REG_OUT_LEN, 65 | 1 << 8);
    wr32(INPUT_REG_OUT_DATA, 0x0202);
    wr32(INPUT_REG_OUT_LEN, 2 | 1 << 8);
    EQ(stat(INPUT_ST_OUT_BAD), 6);
    EQ(stat(INPUT_ST_OUT_OK), 2);
    EQ(M.leds, 0x1f);
    /* OUT_DATA's last word is writable; past it is the report window (RO). */
    wr32(INPUT_REG_OUT_DATA + 60, 0x11223344);
    EQ(M.out_data[63], 0x11);
    end();
}

/* F21: the carrier-emulated SPI table: 990 alone behaves as before; 991 only with input. */
static void case_spi_gate(void)
{
    begin("spi_gate");
    CHECK(vdev_spi_is_virtual(990));
    CHECK(vdev_spi_is_virtual(991));
    CHECK(!vdev_spi_is_virtual(989) && !vdev_spi_is_virtual(992) && !vdev_spi_is_virtual(995));
    EQ(vdev_spi_device(990), VDEV_SPI_DEV_AHCI);
    EQ(vdev_spi_device(991), VDEV_SPI_DEV_INPUT);
    EQ(vdev_spi_device(995), 0);
    EQ(vdev_spi_device(30), 0);
    /* An AHCI-only launch: 990 live, 991 dead. */
    CHECK(vdev_spi_live(990, VDEV_SPI_DEV_AHCI));
    CHECK(!vdev_spi_live(991, VDEV_SPI_DEV_AHCI));
    /* Input only. */
    CHECK(!vdev_spi_live(990, VDEV_SPI_DEV_INPUT));
    CHECK(vdev_spi_live(991, VDEV_SPI_DEV_INPUT));
    /* Neither: the pre-D1 path for both. */
    CHECK(!vdev_spi_live(990, 0) && !vdev_spi_live(991, 0));
    /* Both. */
    u32 both = VDEV_SPI_DEV_AHCI | VDEV_SPI_DEV_INPUT;
    CHECK(vdev_spi_live(990, both) && vdev_spi_live(991, both));
    CHECK(!vdev_spi_live(995, both) && !vdev_spi_live(1020, both) && !vdev_spi_live(27, both));
    EQ(VDEV_SPI_COUNT, 2);
    EQ(vdev_spi_intids[0], 990);
    EQ(vdev_spi_intids[1], 991);
    /*
     * 990 and 991 both high: each has its own state machine.  990 active on CPU 3 while 991
     * becomes pending: 991 is raised on its target, 990 nowhere else.
     */
    struct vdev_spi_plan p990 = vdev_spi_plan(true, 0, 0, BIT(3));
    struct vdev_spi_plan p991 = vdev_spi_plan(true, 0, 0, 0);
    EQ(p990.raise, -1);
    EQ(p990.clear, 0);
    EQ(p991.raise, 0);
    EQ(p991.clear, 0);
    /* One pending copy each, never two CPUs for one INTID. */
    p991 = vdev_spi_plan(true, 5, BIT(0) | BIT(2), 0);
    EQ(p991.raise, 5);
    EQ(p991.clear, BIT(0) | BIT(2));
    end();
}

/* F22: seeded random host bytes, frames and register operations; invariants only. */
static void case_fuzz(long iters)
{
    begin("fuzz");
    rng_state = 0x5eed;
    enable();
    u8 buf[INPUT_HOST_MAX_FRAME + 8];
    for (long i = 0; i < iters; i++) {
        unsigned int r = rng();
        now += r % 4096;
        switch (r % 11) {
            case 0: {
                u8 junk[16];
                u32 n = 1 + rng() % 16;
                for (u32 k = 0; k < n; k++)
                    junk[k] = (u8)(rng() % 3 ? rng() : (rng() & 1 ? 'M' : '6'));
                input_model_host_rx(&M, junk, n, now);
                break;
            }
            case 1:
            case 2: {
                u8 p[18];
                u8 n = (u8)(rng() % 5);
                p[0] = (u8)rng();
                p[1] = n;
                for (u8 k = 0; k < n; k++)
                    p[2 + k] = (u8)(4 + rng() % 0x9c);
                u32 len = frame(buf, INPUT_HOST_KEYS, p, (u8)(2 + n));
                if (rng() % 8 == 0)
                    buf[rng() % len] ^= (u8)(1u << (rng() % 8));
                u32 split = rng() % (len + 1);
                input_model_host_rx(&M, buf, split, now);
                input_model_host_rx(&M, buf + split, len - split, now);
                break;
            }
            case 3:
                absf((u8)(rng() & 0x1f), (u16)(rng() % 32768), (u16)(rng() % 32768),
                     (s8)(rng() % 255 - 127), 0);
                break;
            case 4:
                relf((u8)(rng() & 0x1f), (s16)(rng() % 65535 - 32767), 0, 0, 0);
                break;
            case 5:
                consumer((u16)(rng() % 0x400));
                break;
            case 6: {
                u32 ctrl = rng();
                ctrl = (ctrl & 7) | (rng() % 16 == 0 ? INPUT_CTRL_RESET : 0);
                if (rng() % 4)
                    ctrl |= INPUT_CTRL_ENABLE;
                wr32(INPUT_REG_CTRL, ctrl);
                break;
            }
            case 7:
            case 8: {
                u8 rep[INPUT_REPORT_MAX];
                pop_report(rep, NULL);
                break;
            }
            case 9: {
                u64 v = rng();
                int w = (int)(rng() % 5);
                u64 pair[2] = {v, v};
                input_model_mmio(&M, rng() % INPUT_PAGE_SIZE, w, pair, rng() & 1);
                break;
            }
            default:
                input_model_poll(&M, now);
                break;
        }
        u32 st = rd32(INPUT_REG_STATUS);
        u32 d = st >> 16;
        u32 ctrl = rd32(INPUT_REG_CTRL);
        CHECK(d <= INPUT_QUEUE_LEN);
        CHECK(!!(st & INPUT_STATUS_PENDING) == (d > 0));
        bool line = (ctrl & INPUT_CTRL_ENABLE) && (ctrl & INPUT_CTRL_IRQ_EN) && d;
        CHECK(input_model_line(&M) == line);
        CHECK((rd32(INPUT_REG_IRQ_STATUS) & 1) == line);
        if (!(ctrl & INPUT_CTRL_ENABLE))
            CHECK(d == 0);
        u32 rl = rd32(INPUT_REG_REPORT_LEN);
        u32 len = rl & 0xff, id = rl >> 8 & 0xff;
        CHECK((len == 0 && id == 0) || (id == 1 && len == 22) || (id == 2 && len == 8) ||
              (id == 3 && len == 8) || (id == 4 && len == 3));
        if (failures > 20)
            break;
    }
    /* Drain: the last keyboard report is the merged host state. */
    struct got g;
    wr32(INPUT_REG_CTRL, INPUT_CTRL_ENABLE | INPUT_CTRL_IRQ_EN | INPUT_CTRL_RESET);
    drain(&g);
    CHECK(g.n >= 2);
    if (g.n >= 1) {
        EQ(g.r[0][1], M.src[INPUT_SRC_HOST].mods);
        CHECK(!memcmp(g.r[0] + 2, M.src[INPUT_SRC_HOST].keys, INPUT_KBD_BITMAP));
    }
    add_extra(",\"iterations\":%ld,\"frames_ok\":%u,\"bad_crc\":%u,\"reports\":%u", iters,
              stat(INPUT_ST_FRAMES_OK), stat(INPUT_ST_BAD_CRC), stat(INPUT_ST_REPORTS));
    end();
}

int main(int argc, char **argv)
{
    long iters = argc > 1 ? strtol(argv[1], NULL, 0) : 200000;
    case_crc();
    case_reset_values();
    case_report_layout();
    case_before_enable();
    case_pop_and_line();
    case_enable_reset();
    case_no_duplicate();
    case_garbage_resync();
    case_bad_crc_rescan();
    case_bad_header();
    case_stalled_header();
    case_bad_payload();
    case_split_feed();
    case_seq_gaps();
    case_watchdog();
    case_release_all();
    case_merge();
    case_overflow();
    case_overflow_reference();
    case_access_widths();
    case_out_reports();
    case_spi_gate();
    case_fuzz(iters);
    if (argc > 2)
        case_corpus(argv[2]);
    return total_failures ? 1 : 0;
}
