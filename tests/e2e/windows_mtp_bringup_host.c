/* SPDX-License-Identifier: MIT */
/*
 * Host replay of src/hv/windows/mtp_bringup.c against a fake T8152 MTP written for this test.
 * The fake keeps its own PA space (the reserved area and the dart-sbr root), resolves every
 * ring DVA through the stream-4 tables with its own walk, answers the SCM protocol, and sends
 * INIT, READY, acknowledgements and keyboard reports as frames in the RX ring.
 *
 * Failure list (written before the code):
 *   B1  happy path: every step in order; exactly the PMGR states that were off are written; no
 *       MMIO write outside the PMGR states; rings registered at the measured DVAs; the keyboard
 *       enable sent once on interface 0 as {0xb4, 2}; READY; reports translated (Fn layer)
 *   B2  RUN already set: refused; nothing is written (no MMIO write, no table write, no RTKit)
 *   B3  TTBR invalid / with bits outside the address field, TCR != 9, the root refused by the
 *       glue: refused before any write
 *   B4  a foreign root entry where the MTP's tables go: refused, nothing written
 *   B5  a PMGR state that never reaches on: refused within 10 ms
 *   B6  RTKit start fails: refused
 *   B7  no 0xfe, the mailbox full for good: refused (SCM timeout / send)
 *   B8  no INIT; a complete keyboard INIT with no descriptor; a descriptor that does not map
 *       (no enable is sent then)
 *   B9  the enable: no acknowledgement, a non-zero return code, no READY
 *   B10 a GPIO request during INIT: refused with 0xe000f00d and the request echoed; the
 *       bring-up still completes
 *   B11 run time: 3 checksum errors in a row latch (keyboard all up once); 2 then a good frame
 *       do not
 *   B12 run time: RTKit failure and ring index corruption latch; polls after a latch do nothing
 *   B13 run time: at most max_frames frames per poll; the wake ring is drained too
 *   B14 the mailbox busy at times: sends are retried, never lost
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mtp_bringup.h"

static int failures;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                    \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/* ---- the fake machine ---------------------------------------------------------------------- */

#define AREA_PA 0x10f00000000ull
#define ROOT_PA 0x105ffae0000ull

static u8 *area;          /* 8 MiB, the reserved area */
static u64 root[2048];    /* the dart-sbr stream-4 root */
static u32 asc_ctrl, ttbr, tcr;
static u32 ps[MTP_T8152_PMGR_COUNT];
static u64 clock_us;

struct fault {
    bool run_set, ttbr_bad, ttbr_extra, tcr_bad, root_refused, foreign, pmgr_stuck, rtkit_fail;
    bool no_fe, mbox_full, no_init, no_kbd_desc, mouse_desc, no_ack, ack_error, no_ready;
    bool gpio, busy_sometimes;
};
static struct fault F;

static unsigned mmio_writes, mmio_bad, table_writes, rtkit_starts, app_queued;
static u64 pool_dva_seen;
static u64 reg_tx, reg_rx, reg_wake;
static bool aht_ready, rtkit_failed_now;
static unsigned enables, gpio_acks, scm_count;
static u8 gpio_ack_msg[32];
static u16 gpio_ack_len;
static u64 app_q[64];
static unsigned app_head, app_tail;
static u8 kbd_seq;

static struct mtp_bringup B;

static u64 rd_le(const u8 *p, int n)
{
    u64 v = 0;
    for (int i = 0; i < n; i++)
        v |= (u64)p[i] << (8 * i);
    return v;
}

static void put32(u8 *p, u32 v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (u8)(v >> (8 * i));
}

static u64 *mem_slot(u64 pa)
{
    if (pa >= ROOT_PA && pa < ROOT_PA + 0x4000)
        return &root[(pa - ROOT_PA) / 8];
    if (pa >= AREA_PA + MTP_AREA_TABLES && pa < AREA_PA + MTP_AREA_TABLES + 16 * 0x4000)
        return (u64 *)(area + (pa - AREA_PA));
    printf("FAIL: table access outside the root and the table pages: 0x%llx\n",
           (unsigned long long)pa);
    failures++;
    return NULL;
}

static u64 mem_read(void *ctx, u64 pa)
{
    (void)ctx;
    u64 *s = mem_slot(pa);
    return s ? *s : 0;
}

static void mem_write(void *ctx, u64 pa, u64 v)
{
    (void)ctx;
    u64 *s = mem_slot(pa);
    if (s)
        *s = v;
    table_writes++;
}

/* The MTP's own walk of stream 4 (independent of mtp_dart_t8152.c). */
static u8 *dva_ptr(u64 dva)
{
    u64 idx[3] = {(dva >> 36) & 0x7ff, (dva >> 25) & 0x7ff, (dva >> 14) & 0x7ff};
    u64 table = ROOT_PA;
    for (int level = 0; level < 3; level++) {
        u64 *s = mem_slot(table + idx[level] * 8);
        if (!s || !(*s & 1))
            return NULL;
        table = ((*s >> 10) & ((1ull << 28) - 1)) << 14;
    }
    if (table < AREA_PA || table >= AREA_PA + MTP_AREA_SIZE)
        return NULL;
    return area + (table - AREA_PA) + (dva & 0x3fff);
}

static u64 now_us(void *ctx)
{
    (void)ctx;
    return clock_us += 10;
}

static bool allowed_read(u64 a)
{
    if (a == MTP_T8152_ASC_CPU_CTRL || a == MTP_T8152_DART_TTBR || a == MTP_T8152_DART_TCR)
        return true;
    return a >= MTP_T8152_PMGR_PS &&
           a < MTP_T8152_PMGR_PS + MTP_T8152_PMGR_COUNT * MTP_T8152_PMGR_STRIDE &&
           !((a - MTP_T8152_PMGR_PS) % MTP_T8152_PMGR_STRIDE);
}

static u32 read32(void *ctx, u64 a)
{
    (void)ctx;
    if (!allowed_read(a)) {
        mmio_bad++;
        return 0;
    }
    if (a == MTP_T8152_ASC_CPU_CTRL)
        return asc_ctrl;
    if (a == MTP_T8152_DART_TTBR)
        return ttbr;
    if (a == MTP_T8152_DART_TCR)
        return tcr;
    u32 i = (u32)((a - MTP_T8152_PMGR_PS) / MTP_T8152_PMGR_STRIDE);
    /* A state follows its target on the next read, unless stuck. */
    if (!(F.pmgr_stuck && i == 6))
        ps[i] = (ps[i] & ~0xf0u) | (ps[i] & 0xf) << 4;
    return ps[i];
}

static void write32(void *ctx, u64 a, u32 v)
{
    (void)ctx;
    mmio_writes++;
    if (a < MTP_T8152_PMGR_PS || a >= MTP_T8152_PMGR_PS + 13 * 0x20 || (a & 0x1f)) {
        mmio_bad++;
        return;
    }
    ps[(a - MTP_T8152_PMGR_PS) / 0x20] = v;
}

static void dart_sync(void *ctx)
{
    (void)ctx;
}

static bool root_ok(void *ctx, u64 r)
{
    (void)ctx;
    return r == ROOT_PA && !F.root_refused;
}

static bool rtkit_start(void *ctx, u64 pool_dva, u32 timeout_ms)
{
    (void)ctx;
    (void)timeout_ms;
    rtkit_starts++;
    pool_dva_seen = pool_dva;
    if (F.rtkit_fail)
        return false;
    asc_ctrl |= MTP_T8152_RUN;
    return true;
}

static void app_push(u64 msg)
{
    app_q[app_tail++ % 64] = msg;
    app_queued++;
}

static int rtkit_service(void *ctx, int max)
{
    (void)ctx;
    if (rtkit_failed_now)
        return -1;
    int n = 0;
    while (n < max && app_head != app_tail) {
        mtp_bringup_app_msg(&B, 0x20, app_q[app_head++ % 64]);
        n++;
    }
    return n;
}

/* Far side writes into an IOP->AP ring (producer at +0, consumer at +4). */
static bool far_write(u8 *ring, u32 cap, const u8 *p, u32 len)
{
    u32 prod = (u32)rd_le(ring, 4), cons = (u32)rd_le(ring + 4, 4);
    u32 used = prod >= cons ? prod - cons : cap - cons + prod;
    if (used + len + 4 > cap)
        return false;
    for (u32 i = 0; i < len; i++)
        ring[8 + (prod + i) % cap] = p[i];
    put32(ring, (prod + len) % cap);
    return true;
}

/* A frame the MTP sends: channel 0x14 with a timestamp, or 0x11 acknowledgements. */
static u32 mtp_frame(u8 *out, u8 ch, u8 seq, u8 iface, u8 flags, u32 ret, const u8 *msg, u16 n)
{
    bool ts = ch == MTP_CH_SCM_REPORT;
    u32 body = 8 + (ts ? 8 : 0) + ((n + 3u) & ~3u);
    memset(out, 0, 8 + body + 4);
    out[0] = 8;
    out[1] = ch;
    out[2] = (u8)body;
    out[3] = (u8)(body >> 8);
    out[4] = seq;
    out[5] = iface;
    out[8] = flags;
    out[10] = (u8)n;
    out[11] = (u8)(n >> 8);
    put32(out + 12, ret);
    if (ts)
        put32(out + 16, 0x12345678);
    memcpy(out + 16 + (ts ? 8 : 0), msg, n);
    u32 sum = 0;
    for (u32 i = 0; i < 8 + body; i += 4)
        sum += (u32)rd_le(out + i, 4);
    put32(out + 8 + body, 0xffffffffu - sum);
    return 8 + body + 4;
}

static void send_to_host(u8 ch, u8 seq, u8 iface, u8 flags, u32 ret, const u8 *msg, u16 n)
{
    u8 f[MTP_FRAME_MAX];
    u32 len = mtp_frame(f, ch, seq, iface, flags, ret, msg, n);
    u8 *rx = dva_ptr(reg_rx);
    if (!rx || !far_write(rx, MTP_RING_BYTES - 8, f, len)) {
        printf("FAIL: the fake MTP could not write the RX ring\n");
        failures++;
    }
    app_push(MTP_SCM_IOP_DOORBELL);
}

static const u8 kbd_desc[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15,
    0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x26, 0xff, 0x00, 0x05, 0x07, 0x19, 0x00, 0x29, 0xff,
    0x81, 0x00, 0x06, 0xff, 0x00, 0x09, 0x03, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01,
    0x81, 0x02, 0x75, 0x07, 0x81, 0x01, 0xc0,
};
static const u8 mouse_desc[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00,
    0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01, 0xc0,
};

static void send_init(u8 iface, const char *name, const u8 *desc, u16 dlen)
{
    u8 ev[512];
    memset(ev, 0, sizeof(ev));
    ev[0] = MTP_EV_INIT;
    ev[1] = 1;
    ev[3] = iface;
    strncpy((char *)ev + 4, name, 16);
    u16 n = MTP_INIT_HDR;
    if (desc) {
        ev[n] = 0;
        ev[n + 2] = (u8)dlen;
        ev[n + 3] = (u8)(dlen >> 8);
        memcpy(ev + n + 4, desc, dlen);
        n += 4 + dlen;
    }
    ev[n] = 2;
    ev[n + 2] = 6;
    n += 4 + 6;
    send_to_host(MTP_CH_SCM_REPORT, 0, 0, MTP_FLAGS_INPUT, 0, ev, n);
}

/* The MTP's command receiver: frames in the TX ring. */
static void take_commands(void)
{
    u8 *tx = dva_ptr(reg_tx);
    if (!tx)
        return;
    const u32 cap = MTP_RING_BYTES - 8;
    for (;;) {
        u32 prod = (u32)rd_le(tx, 4), cons = (u32)rd_le(tx + 4, 4);
        if (prod == cons)
            return;
        u8 hdr[8], f[256];
        for (int i = 0; i < 8; i++)
            hdr[i] = tx[8 + (cons + i) % cap];
        u32 len = 8 + (u32)rd_le(hdr + 2, 2) + 4;
        for (u32 i = 0; i < len && i < sizeof(f); i++)
            f[i] = tx[8 + (cons + i) % cap];
        put32(tx + 4, (cons + len) % cap);
        struct mtp_msg m;
        if (mtp_frame_parse(f, len, &m) != MTP_PARSE_OK || m.channel != MTP_CH_CMD ||
            m.iface != 0 || m.flags != MTP_FLAGS_SET_FEATURE) {
            printf("FAIL: a malformed command frame\n");
            failures++;
            continue;
        }
        if (m.data[0] == MTP_CMD_ENABLE) {
            enables++;
            CHECK(m.len == 2 && m.data[1] == 2);
            if (!F.no_ack)
                send_to_host(MTP_CH_CMD, m.seq, 0, m.flags, F.ack_error ? 0xe00002c2 : 0, m.data,
                             m.len);
            if (!F.no_ready && !F.no_ack && !F.ack_error) {
                u8 ready[4] = {MTP_EV_READY, m.data[1], 0, 0};
                send_to_host(MTP_CH_SCM_REPORT, 0, 0, MTP_FLAGS_INPUT, 0, ready, 4);
            }
        } else if (m.data[0] == MTP_CMD_GPIO_ACK) {
            gpio_acks++;
            gpio_ack_len = m.len;
            memcpy(gpio_ack_msg, m.data, m.len < 32 ? m.len : 32);
            send_to_host(MTP_CH_CMD, m.seq, 0, m.flags, 0, m.data, m.len);
        }
    }
}

static unsigned busy_tick;

static bool scm_send(void *ctx, u64 msg)
{
    (void)ctx;
    if (F.mbox_full || (F.busy_sometimes && (busy_tick++ % 3) == 0))
        return false;
    scm_count++;
    u8 id = msg & 0xff;
    switch (id) {
        case MTP_SCM_POWER:
            CHECK(msg == 0x2fd);
            if (!F.no_fe)
                app_push(MTP_SCM_IOP_READY);
            break;
        case MTP_SCM_AP_FIFO:
            reg_tx = msg & ~0xfffull;
            break;
        case MTP_SCM_IOP_FIFO:
            reg_rx = msg & ~0xfffull;
            break;
        case MTP_SCM_WAKE_FIFO:
            reg_wake = msg & ~0xfffull;
            break;
        case MTP_SCM_AHT_READY:
            aht_ready = true;
            if (F.no_init)
                break;
            {
                static const char *names[7] = {"comm",     "multi-touch", "keyboard", "stm",
                                               "actuator", "tp-accel",    "mtp"};
                for (u8 i = 0; i < 7; i++) {
                    if (i == 2) {
                        if (F.gpio) {
                            u8 g[5] = {MTP_EV_GPIO, 1, 0, 0, 3};
                            send_to_host(MTP_CH_SCM_REPORT, 0, 0, MTP_FLAGS_INPUT, 0, g, 5);
                        }
                        if (F.no_kbd_desc)
                            send_init(2, names[i], NULL, 0);
                        else if (F.mouse_desc)
                            send_init(2, names[i], mouse_desc, sizeof(mouse_desc));
                        else
                            send_init(2, names[i], kbd_desc, sizeof(kbd_desc));
                    } else {
                        u8 d[4] = {0x06, 0x00, 0xff, 0xc0};
                        send_init(i, names[i], i ? d : NULL, i ? 4 : 0);
                    }
                }
            }
            break;
        case MTP_SCM_AP_DOORBELL:
            take_commands();
            break;
        default:
            CHECK(!"unknown SCM message");
    }
    return true;
}

static u8 last_mods, last_keys[KBD_BITMAP_BYTES];
static u16 last_cons;
static unsigned kbd_calls;

static void keyboard(void *ctx, u8 mods, const u8 keys[KBD_BITMAP_BYTES], u16 consumer)
{
    (void)ctx;
    kbd_calls++;
    last_mods = mods;
    memcpy(last_keys, keys, KBD_BITMAP_BYTES);
    last_cons = consumer;
}

static const struct mtp_ops ops = {
    .ctx = NULL,
    .now_us = now_us,
    .read32 = read32,
    .write32 = write32,
    .dart_mem = {NULL, mem_read, mem_write},
    .dart_sync = dart_sync,
    .root_ok = root_ok,
    .rtkit_start = rtkit_start,
    .rtkit_service = rtkit_service,
    .scm_send = scm_send,
    .keyboard = keyboard,
    .frame = NULL,
};

static void machine(void)
{
    memset(area, 0, MTP_AREA_SIZE);
    memset(root, 0, sizeof(root));
    root[0x11] = 0x1234567c01ull; /* another coprocessor's table, elsewhere in the root */
    asc_ctrl = 0;
    ttbr = (u32)(ROOT_PA >> 12) | 1;
    tcr = 9;
    for (u32 i = 0; i < MTP_T8152_PMGR_COUNT; i++)
        ps[i] = (i == 6 || i == 10 || i == 11) ? 0x0 : 0xff; /* three off (as measured) */
    clock_us = 1000;
    mmio_writes = mmio_bad = table_writes = rtkit_starts = app_queued = 0;
    pool_dva_seen = reg_tx = reg_rx = reg_wake = 0;
    aht_ready = rtkit_failed_now = false;
    enables = gpio_acks = scm_count = 0;
    gpio_ack_len = 0;
    app_head = app_tail = 0;
    kbd_seq = 0;
    busy_tick = 0;
    kbd_calls = 0;
    last_mods = 0;
    last_cons = 0;
    memset(last_keys, 0, sizeof(last_keys));
    if (F.run_set)
        asc_ctrl = MTP_T8152_RUN;
    if (F.ttbr_bad)
        ttbr &= ~1u;
    if (F.ttbr_extra)
        ttbr |= 0x80000000u;
    if (F.tcr_bad)
        tcr = 0x109;
    if (F.foreign)
        root[0x10] = 0x2000000001ull;
}

static int attach(void)
{
    machine();
    return mtp_bringup_attach(&B, &ops, AREA_PA, area, KBD_FN_MEDIA);
}

static void type_report(u8 mods, u8 key, bool fn)
{
    u8 r[10] = {1, mods, 0, key, 0, 0, 0, 0, 0, (u8)(fn ? 1 : 0)};
    send_to_host(MTP_CH_SCM_REPORT, kbd_seq++, 2, MTP_FLAGS_INPUT, 0, r, sizeof(r));
}

static bool down(u8 usage)
{
    return (last_keys[usage / 8] >> (usage % 8)) & 1;
}

static void result(const char *name, int before, const char *extra)
{
    printf("{\"case\":\"%s\",\"result\":\"%s\"%s}\n", name,
           failures == before ? "passed" : "failed", extra ? extra : "");
}

/* ---- cases --------------------------------------------------------------------------------- */

static void b1_happy(void)
{
    int before = failures;
    memset(&F, 0, sizeof(F));
    CHECK(attach() == MTP_E_NONE);
    CHECK(B.state == MTP_ST_KBD_READY && B.reason == MTP_E_NONE && B.kbd_live);
    CHECK(mmio_writes == 3 && B.pmgr_writes == 3 && !mmio_bad);
    for (u32 i = 0; i < MTP_T8152_PMGR_COUNT; i++)
        CHECK(MTP_PS_ACTUAL(ps[i]) == 0xf);
    CHECK(reg_tx == 0x10040000000ull && reg_rx == 0x10040204000ull && reg_wake == 0x10040408000ull);
    CHECK(pool_dva_seen == 0x10040410000ull && rtkit_starts == 1);
    CHECK(root[0x11] == 0x1234567c01ull);
    CHECK(enables == 1 && B.kbd_iface == 2 && B.comm.inits == 7);
    /* Reports. */
    type_report(0, 0x04, false);
    CHECK(mtp_bringup_poll(&B, 32) >= 1 && down(0x04) && !last_cons);
    type_report(0x05, 0x4c, false);
    mtp_bringup_poll(&B, 32);
    CHECK(last_mods == 0x05 && down(0x4c) && !down(0x04));
    type_report(0, 0x45, false); /* F12: volume up */
    mtp_bringup_poll(&B, 32);
    CHECK(last_cons == 0xe9 && !down(0x45));
    type_report(0, 0x51, true); /* Fn+Down: Page Down */
    mtp_bringup_poll(&B, 32);
    CHECK(down(0x4e) && !last_cons);
    type_report(0, 0, false);
    mtp_bringup_poll(&B, 32);
    CHECK(!down(0x4e) && !last_mods);
    CHECK(B.kbd_reports == 5 && !B.csum_errors && !B.ring_errors);
    char extra[256];
    snprintf(extra, sizeof(extra),
             ",\"pmgr_writes\":%u,\"tables\":%u,\"tx\":\"0x%llx\",\"rx\":\"0x%llx\",\"wake\":"
             "\"0x%llx\",\"pool\":\"0x%llx\",\"scm_sends\":%u",
             B.pmgr_writes, B.dart.used, (unsigned long long)reg_tx, (unsigned long long)reg_rx,
             (unsigned long long)reg_wake, (unsigned long long)pool_dva_seen, scm_count);
    result("happy", before, extra);
}

static void refused(const char *name, struct fault f, int reason, u8 step, bool nothing_written)
{
    int before = failures;
    F = f;
    int rc = attach();
    CHECK(rc == reason);
    CHECK(B.reason == reason && B.state == (MTP_ST_FAILED | step) && !B.kbd_live);
    if (nothing_written)
        CHECK(!mmio_writes && !table_writes && !rtkit_starts && !scm_count);
    CHECK(!mmio_bad);
    CHECK(mtp_bringup_poll(&B, 32) == -1);
    result(name, before, NULL);
}

static void b2_b9_refusals(void)
{
    refused("run_set", (struct fault){.run_set = true}, MTP_E_RUNNING, MTP_ST_IDENTITY, true);
    refused("ttbr_invalid", (struct fault){.ttbr_bad = true}, MTP_E_TTBR, MTP_ST_IDENTITY, true);
    refused("ttbr_extra", (struct fault){.ttbr_extra = true}, MTP_E_TTBR, MTP_ST_IDENTITY, true);
    refused("tcr", (struct fault){.tcr_bad = true}, MTP_E_TCR, MTP_ST_IDENTITY, true);
    refused("root", (struct fault){.root_refused = true}, MTP_E_ROOT, MTP_ST_IDENTITY, true);
    refused("foreign", (struct fault){.foreign = true}, MTP_E_MAP, MTP_ST_DART, true);
    {
        int before = failures;
        F = (struct fault){.pmgr_stuck = true};
        u64 t0 = clock_us;
        CHECK(attach() == MTP_E_POWER && B.state == (MTP_ST_FAILED | MTP_ST_DART));
        CHECK(clock_us - t0 < 20000 && !rtkit_starts && !scm_count);
        result("pmgr_stuck", before, NULL);
    }
    refused("rtkit", (struct fault){.rtkit_fail = true}, MTP_E_RTKIT, MTP_ST_POWER, false);
    refused("no_fe", (struct fault){.no_fe = true}, MTP_E_SCM_TIMEOUT, MTP_ST_RTKIT, false);
    refused("mbox_full", (struct fault){.mbox_full = true}, MTP_E_SCM_SEND, MTP_ST_RTKIT, false);
    refused("no_init", (struct fault){.no_init = true}, MTP_E_INIT_TIMEOUT, MTP_ST_SCM, false);
    /* The keyboard's last INIT packet came without a descriptor: no layout, at once. */
    refused("no_kbd_desc", (struct fault){.no_kbd_desc = true}, MTP_E_LAYOUT, MTP_ST_INIT, false);
    {
        int before = failures;
        F = (struct fault){.mouse_desc = true};
        CHECK(attach() == MTP_E_LAYOUT && B.state == (MTP_ST_FAILED | MTP_ST_INIT) && !enables);
        result("layout", before, NULL);
    }
    refused("no_ack", (struct fault){.no_ack = true}, MTP_E_KBD_ACK, MTP_ST_INIT, false);
    refused("ack_error", (struct fault){.ack_error = true}, MTP_E_KBD_ENABLE, MTP_ST_INIT, false);
    refused("no_ready", (struct fault){.no_ready = true}, MTP_E_KBD_READY, MTP_ST_INIT, false);
}

static void b10_gpio(void)
{
    int before = failures;
    F = (struct fault){.gpio = true};
    CHECK(attach() == MTP_E_NONE && B.state == MTP_ST_KBD_READY);
    mtp_bringup_poll(&B, 32);
    CHECK(gpio_acks == 1 && B.gpio_refused == 1 && gpio_ack_len == 10);
    CHECK(gpio_ack_msg[0] == MTP_CMD_GPIO_ACK && rd_le(gpio_ack_msg + 1, 4) == MTP_GPIO_REFUSED &&
          gpio_ack_msg[5] == MTP_EV_GPIO && gpio_ack_msg[9] == 3);
    result("gpio", before, NULL);
}

static void corrupt_next_frame(void)
{
    u8 r[10] = {1, 0, 0, 4, 0, 0, 0, 0, 0, 0};
    u8 f[64];
    u32 len = mtp_frame(f, MTP_CH_SCM_REPORT, 0, 2, 0, 0, r, sizeof(r));
    f[len - 1] ^= 0x40;
    far_write(dva_ptr(reg_rx), MTP_RING_BYTES - 8, f, len);
}

static void b11_b13_runtime(void)
{
    int before = failures;
    F = (struct fault){0};
    CHECK(attach() == MTP_E_NONE);
    type_report(0, 0x04, false);
    mtp_bringup_poll(&B, 32);
    CHECK(down(0x04));
    /* Two bad, then a good frame: no latch. */
    corrupt_next_frame();
    corrupt_next_frame();
    type_report(0, 0x05, false);
    CHECK(mtp_bringup_poll(&B, 32) >= 0 && B.kbd_live && down(0x05) && B.csum_errors == 2);
    /* Three in a row: latched, keyboard all up once. */
    corrupt_next_frame();
    corrupt_next_frame();
    corrupt_next_frame();
    unsigned calls = kbd_calls;
    CHECK(mtp_bringup_poll(&B, 32) == -1 && !B.kbd_live && B.reason == MTP_E_CSUM);
    CHECK(kbd_calls == calls + 1 && !down(0x05) && !last_mods && !last_cons);
    type_report(0, 0x06, false);
    CHECK(mtp_bringup_poll(&B, 32) == -1 && kbd_calls == calls + 1);
    result("csum_latch", before, NULL);

    before = failures;
    CHECK(attach() == MTP_E_NONE);
    rtkit_failed_now = true;
    CHECK(mtp_bringup_poll(&B, 32) == -1 && B.reason == MTP_E_RTKIT_FAILED);
    result("rtkit_latch", before, NULL);

    before = failures;
    CHECK(attach() == MTP_E_NONE);
    put32(dva_ptr(reg_rx), 0x200001); /* producer past the capacity */
    CHECK(mtp_bringup_poll(&B, 32) == -1 && B.reason == MTP_E_RING && B.ring_errors == 1);
    result("ring_latch", before, NULL);

    before = failures;
    CHECK(attach() == MTP_E_NONE);
    for (int i = 0; i < 40; i++)
        type_report(0, (u8)(4 + i % 20), false);
    CHECK(mtp_bringup_poll(&B, 32) == 32 && mtp_bringup_poll(&B, 32) == 8);
    /* A frame in the wake ring (4 KiB used of 16). */
    u8 r[10] = {1, 0, 0, 0x2c, 0, 0, 0, 0, 0, 0}, f[64];
    u32 len = mtp_frame(f, MTP_CH_SCM_REPORT, 0, 2, 0, 0, r, sizeof(r));
    CHECK(far_write(dva_ptr(reg_wake), MTP_WAKE_USED_BYTES - 8, f, len));
    CHECK(mtp_bringup_poll(&B, 32) == 1 && down(0x2c));
    result("budget_wake", before, NULL);
}

static void b14_busy(void)
{
    int before = failures;
    F = (struct fault){.busy_sometimes = true};
    CHECK(attach() == MTP_E_NONE && B.scm_busy > 0 && enables == 1);
    type_report(0, 0x04, false);
    mtp_bringup_poll(&B, 32);
    CHECK(down(0x04));
    result("mbox_busy", before, NULL);
}

int main(void)
{
    area = aligned_alloc(0x4000, MTP_AREA_SIZE);
    if (!area)
        return 2;
    b1_happy();
    b2_b9_refusals();
    b10_gpio();
    b11_b13_runtime();
    b14_busy();
    free(area);
    return failures ? 1 : 0;
}
