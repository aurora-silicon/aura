/* SPDX-License-Identifier: GPL-2.0-only */
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
 *   B2  RUN already set on an MTP whose domains are all on: refused, nothing written (no MMIO
 *       write, no table write, no RTKit); with domains off, RUN is read only after they are
 *       switched on (the fake ASC faults, all ones, while any is off), and RUN set then is
 *       refused with only the PMGR writes done.  ASC_CPU_CONTROL is never read unpowered.
 *   B3  TTBR invalid / with bits outside the address field, TCR != 9, the root refused by the
 *       glue: refused before any table write (the domains are on by then)
 *   B4  a foreign root entry where the MTP's tables go: refused, no table written
 *   B5  a PMGR state that never reaches on: refused within 10 ms, RUN never read
 *   B6  RTKit start fails: refused
 *   B7  no 0xfe, the mailbox full for good: refused (SCM timeout / send)
 *   B8  no INIT; a keyboard whose last INIT packet has no descriptor (waited for until the
 *       INIT deadline, as Linux waits on ring transports, then refused); the descriptor in a
 *       later INIT packet (the keyboard comes up); a descriptor that does not map (no enable)
 *   B9  the enable: no acknowledgement, a non-zero return code, no READY
 *   B10 a GPIO request during INIT: refused with 0xe000f00d and the request echoed; the
 *       bring-up still completes
 *   B11 run time: 3 checksum errors in a row latch (keyboard all up once); 2 then a good frame
 *       do not
 *   B12 run time: RTKit failure and ring index corruption latch; polls after a latch do nothing
 *   B13 run time: at most max_frames frames per poll; the wake ring is drained too
 *   B14 the mailbox busy at times: sends are retried, never lost
 *   B15 a GPIO request drained with the keyboard enable's acknowledgement: the enable's result
 *       is read first (no false KBD_ACK), the refusal follows
 *   B16 run time, the mailbox full: a poll never spins; the doorbell is owed and rung on a
 *       later poll; owed for more than 50 ms it latches MTP_E_SCM_SEND
 *   I2 (the trackpad; the keyboard-only cases above make no AFE call and map no firmware):
 *   P1  happy path: multi-touch enabled, the firmware's interface byte patched and written
 *       back, AFE asserted, >= 50 ms, 0x95 with the firmware's DVA (readable through the fake's
 *       own stream-4 walk) and size, power "on" announced while still in reset, AFE released,
 *       >= 50 ms, "on" confirmed, READY, 0xD9 geometry applied; exactly two AFE calls (assert,
 *       release); finger frames become pointer reports; the firmware DVA follows the pool
 *   P2  a bad parameter page or firmware CRC: no trackpad, nothing mapped for it, no AFE call,
 *       the keyboard up
 *   P3  no multi-touch INIT, the enable refused: no AFE call, the keyboard up
 *   P4  the AFE assert fails: no further AFE call, no firmware sent
 *   P5  0x95 unacknowledged or refused, the power announce refused: the AFE stays asserted
 *       (one call), the keyboard up
 *   P6  the release fails: two calls, no confirm; no READY: the trackpad down
 *   P7  no 0xD9 answer: the defaults stay, the trackpad up
 *   P8  run time: a latch releases the pointer too; a silent pad drops its held button
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
    bool gpio, busy_sometimes, run_after_power, kbd_desc_late, gpio_with_ack;
    /* I2 */
    bool bad_params, bad_fw_crc, no_mt_init, mt_enable_error, afe_assert_fails, afe_release_fails;
    bool fw_no_ack, fw_error, power_error, no_tp_ready, no_geometry;
};
static struct fault F;

static unsigned mmio_writes, mmio_bad, table_writes, rtkit_starts, app_queued;
static unsigned asc_reads, asc_unpowered;
static u64 pool_dva_seen;
static u64 reg_tx, reg_rx, reg_wake;
static bool aht_ready, rtkit_failed_now;
static unsigned enables, gpio_acks, scm_count;
static u8 gpio_ack_msg[32];
static u16 gpio_ack_len;
static u64 app_q[64];
static unsigned app_head, app_tail;
static u8 kbd_seq;

/* I2: the fake multi-touch side. */
#define FW_LEN       3000u
#define FW_IFACE_OFF 0x10u
static bool want_tp;
static u8 fw_payload[FW_LEN];
static unsigned afe_calls, afe_asserts, afe_releases, mt_enables, fw_msgs, power_msgs, geom_reqs;
static unsigned syncs, ptr_calls;
static bool afe_asserted, fw_ok_seen;
static u64 afe_assert_at, afe_release_at, fw_dva_seen, sync_pa, sync_size;
static u32 fw_size_seen;
static struct tp_report last_ptr;
static long ptr_dx;
static u8 mt_seq;

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
    if (a == MTP_T8152_ASC_CPU_CTRL) {
        asc_reads++;
        /* The ASC block is in the MTP's domains: unpowered, the glue's guarded read gives ~0. */
        for (u32 i = 0; i < MTP_T8152_PMGR_COUNT; i++)
            if (MTP_PS_ACTUAL(ps[i]) != 0xf) {
                asc_unpowered++;
                return ~0u;
            }
        return asc_ctrl;
    }
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
            !((m.iface == 0 && m.flags == MTP_FLAGS_SET_FEATURE) ||
              (m.iface == 1 && m.flags == MTP_FLAGS_GET_FEATURE && m.len == 1 &&
               m.data[0] == TP_FEATURE_GEOMETRY))) {
            printf("FAIL: a malformed command frame\n");
            failures++;
            continue;
        }
        if (m.iface == 1) {
            /* 0xD9: width 125 mm, height 80 mm, X -6000..6500, Y -100..7900 (100 units/mm). */
            geom_reqs++;
            if (!F.no_geometry) {
                u8 g[TP_GEOMETRY_LEN] = {TP_FEATURE_GEOMETRY};
                put32(g + 1, 12500);
                put32(g + 5, 8000);
                const s16 v[4] = {-6000, -100, 6500, 7900};
                memcpy(g + 9, v, 8);
                send_to_host(MTP_CH_CMD, m.seq, 1, m.flags, 0, g, sizeof(g));
            }
            continue;
        }
        if (m.data[0] == MTP_CMD_ENABLE && m.len == 2 && m.data[1] == 1) {
            mt_enables++;
            send_to_host(MTP_CH_CMD, m.seq, 0, m.flags, F.mt_enable_error ? 0xe00002c2 : 0, m.data,
                         m.len);
        } else if (m.data[0] == MTP_CMD_FIRMWARE) {
            fw_msgs++;
            CHECK(m.len == 16 && m.data[1] == 2 && m.data[2] == 0 && m.data[3] == 1);
            CHECK(afe_asserted && clock_us - afe_assert_at >= 50000);
            fw_dva_seen = rd_le(m.data + 4, 8);
            fw_size_seen = (u32)rd_le(m.data + 12, 4);
            fw_ok_seen = fw_size_seen == FW_LEN;
            for (u32 k = 0; fw_ok_seen && k < FW_LEN; k++) {
                u8 *b = dva_ptr(fw_dva_seen + k);
                u8 want = k == FW_IFACE_OFF ? 1 : fw_payload[k];
                fw_ok_seen = b && *b == want;
            }
            CHECK(syncs == 1 && sync_pa == AREA_PA + MTP_AREA_FW && sync_size == FW_LEN);
            if (!F.fw_no_ack)
                send_to_host(MTP_CH_CMD, m.seq, 0, m.flags, F.fw_error ? 0xe00002bc : 0, m.data,
                             m.len);
        } else if (m.data[0] == MTP_CMD_POWER) {
            power_msgs++;
            CHECK(m.len == 9 && m.data[1] == 2 && m.data[2] == 1 && m.data[3] == 2);
            for (u32 k = 5; k < 9 && k < m.len; k++)
                CHECK(m.data[k] == 0);
            bool confirm = m.data[4] == 1;
            if (!confirm)
                CHECK(afe_asserted && fw_msgs == 1 && power_msgs == 1);
            else
                CHECK(!afe_asserted && clock_us - afe_release_at >= 50000 && power_msgs == 2);
            send_to_host(MTP_CH_CMD, m.seq, 0, m.flags, F.power_error ? 0xe00002bc : 0, m.data,
                         m.len);
            if (confirm && !F.no_tp_ready) {
                u8 ready[4] = {MTP_EV_READY, 1, 0, 0};
                send_to_host(MTP_CH_SCM_REPORT, 0, 0, MTP_FLAGS_INPUT, 0, ready, 4);
            }
        } else if (m.data[0] == MTP_CMD_ENABLE) {
            enables++;
            CHECK(m.len == 2 && m.data[1] == 2);
            if (!F.no_ack)
                send_to_host(MTP_CH_CMD, m.seq, 0, m.flags, F.ack_error ? 0xe00002c2 : 0, m.data,
                             m.len);
            if (F.gpio_with_ack) {
                u8 g[5] = {MTP_EV_GPIO, 2, 0, 0, 5};
                send_to_host(MTP_CH_SCM_REPORT, 0, 0, MTP_FLAGS_INPUT, 0, g, 5);
            }
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
                        else if (F.kbd_desc_late) {
                            send_init(2, names[i], NULL, 0); /* last packet, no descriptor */
                            send_init(2, names[i], kbd_desc, sizeof(kbd_desc));
                        }
                        else if (F.mouse_desc)
                            send_init(2, names[i], mouse_desc, sizeof(mouse_desc));
                        else
                            send_init(2, names[i], kbd_desc, sizeof(kbd_desc));
                    } else {
                        u8 d[4] = {0x06, 0x00, 0xff, 0xc0};
                        if (i == 1 && F.no_mt_init)
                            continue;
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

static bool afe_reset(void *ctx, bool assert)
{
    (void)ctx;
    afe_calls++;
    if (assert) {
        afe_asserts++;
        CHECK(!afe_asserted && mt_enables == 1 && !fw_msgs);
        if (F.afe_assert_fails)
            return false;
        afe_asserted = true;
        afe_assert_at = clock_us;
    } else {
        afe_releases++;
        CHECK(afe_asserted && fw_msgs == 1 && power_msgs == 1);
        if (F.afe_release_fails)
            return false;
        afe_asserted = false;
        afe_release_at = clock_us;
    }
    return true;
}

static void dma_sync(void *ctx, u64 pa, u64 size)
{
    (void)ctx;
    syncs++;
    sync_pa = pa;
    sync_size = size;
}

static void pointer(void *ctx, const struct tp_report *r)
{
    (void)ctx;
    ptr_calls++;
    last_ptr = *r;
    ptr_dx += r->dx;
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
    .pointer = pointer,
    .afe_reset = afe_reset,
    .dma_sync = dma_sync,
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
    asc_reads = asc_unpowered = 0;
    pool_dva_seen = reg_tx = reg_rx = reg_wake = 0;
    aht_ready = rtkit_failed_now = false;
    enables = gpio_acks = scm_count = 0;
    gpio_ack_len = 0;
    app_head = app_tail = 0;
    kbd_seq = 0;
    afe_calls = afe_asserts = afe_releases = mt_enables = fw_msgs = power_msgs = geom_reqs = 0;
    syncs = ptr_calls = 0;
    afe_asserted = fw_ok_seen = false;
    afe_assert_at = afe_release_at = fw_dva_seen = sync_pa = sync_size = 0;
    fw_size_seen = 0;
    memset(&last_ptr, 0, sizeof(last_ptr));
    ptr_dx = 0;
    mt_seq = 0;
    if (want_tp) {
        /* The launcher's work: the payload and the parameter page (tp defaults). */
        for (u32 k = 0; k < FW_LEN; k++)
            fw_payload[k] = (u8)(k * 7 + 3);
        memcpy(area + MTP_AREA_FW, fw_payload, FW_LEN);
        u8 *pg = area + MTP_AREA_PARAMS;
        memcpy(pg, MTP_PARAMS_MAGIC, 8);
        put32(pg + 8, MTP_PARAMS_VERSION);
        put32(pg + 12, MTP_PARAMS_LEN);
        put32(pg + 16, MTP_AREA_FW);
        put32(pg + 20, FW_LEN);
        put32(pg + 24, mtp_crc32(0, fw_payload, FW_LEN));
        put32(pg + 28, FW_IFACE_OFF);
        put32(pg + 32, MTP_PARAMS_TAP);
        const u16 v[10] = {6 * 256, 3 * 256, 15, 150, 5000, 180, 1500, 300, 40, 0};
        memcpy(pg + 36, v, sizeof(v));
        put32(pg + 60, mtp_crc32(0, pg, 60));
        if (F.bad_params)
            pg[3] ^= 1;
        if (F.bad_fw_crc)
            area[MTP_AREA_FW + 100] ^= 1;
    }
    busy_tick = 0;
    kbd_calls = 0;
    last_mods = 0;
    last_cons = 0;
    memset(last_keys, 0, sizeof(last_keys));
    if (F.run_set) {
        /* A running MTP has every domain on. */
        asc_ctrl = MTP_T8152_RUN;
        for (u32 i = 0; i < MTP_T8152_PMGR_COUNT; i++)
            ps[i] = 0xff;
    }
    if (F.run_after_power)
        asc_ctrl = MTP_T8152_RUN; /* (inconsistent hardware) seen only once powered */
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
    return mtp_bringup_attach(&B, &ops, AREA_PA, area, KBD_FN_MEDIA, want_tp);
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
    CHECK(asc_reads == 1 && !asc_unpowered && !B.run_read_powered && B.asc_ctrl == 0);
    CHECK(B.step_ms[MTP_ST_POWER] <= B.step_ms[MTP_ST_DART]);
    for (u32 i = 0; i < MTP_T8152_PMGR_COUNT; i++)
        CHECK(MTP_PS_ACTUAL(ps[i]) == 0xf);
    CHECK(reg_tx == 0x10040000000ull && reg_rx == 0x10040204000ull && reg_wake == 0x10040408000ull);
    CHECK(pool_dva_seen == 0x10040410000ull && rtkit_starts == 1);
    CHECK(root[0x11] == 0x1234567c01ull);
    CHECK(enables == 1 && B.kbd_iface == 2 && B.comm.inits == 7);
    /* Keyboard only: no trackpad step, no AFE call, no firmware mapped. */
    CHECK(!afe_calls && !mt_enables && !fw_msgs && !syncs && !B.fw_dva && !B.tp_live &&
          B.tp_iface == -1);
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

enum written { W_NOTHING, W_PMGR, W_ANY };

static void refused(const char *name, struct fault f, int reason, u8 step, enum written w)
{
    int before = failures;
    F = f;
    int rc = attach();
    CHECK(rc == reason);
    CHECK(B.reason == reason && B.state == (MTP_ST_FAILED | step) && !B.kbd_live);
    if (w == W_NOTHING)
        CHECK(!mmio_writes && !table_writes && !rtkit_starts && !scm_count);
    if (w == W_PMGR)
        CHECK(mmio_writes == 3 && !table_writes && !rtkit_starts && !scm_count);
    CHECK(!mmio_bad && !asc_unpowered);
    CHECK(mtp_bringup_poll(&B, 32) == -1);
    result(name, before, NULL);
}

static void b2_b9_refusals(void)
{
    refused("run_set", (struct fault){.run_set = true}, MTP_E_RUNNING, MTP_ST_IDENTITY,
            W_NOTHING);
    CHECK(B.run_read_powered && asc_reads == 1);
    refused("run_after_power", (struct fault){.run_after_power = true}, MTP_E_RUNNING,
            MTP_ST_IDENTITY, W_PMGR);
    CHECK(!B.run_read_powered && asc_reads == 1);
    refused("ttbr_invalid", (struct fault){.ttbr_bad = true}, MTP_E_TTBR, MTP_ST_POWER, W_PMGR);
    refused("ttbr_extra", (struct fault){.ttbr_extra = true}, MTP_E_TTBR, MTP_ST_POWER, W_PMGR);
    refused("tcr", (struct fault){.tcr_bad = true}, MTP_E_TCR, MTP_ST_POWER, W_PMGR);
    refused("root", (struct fault){.root_refused = true}, MTP_E_ROOT, MTP_ST_POWER, W_PMGR);
    refused("foreign", (struct fault){.foreign = true}, MTP_E_MAP, MTP_ST_POWER, W_PMGR);
    {
        int before = failures;
        F = (struct fault){.pmgr_stuck = true};
        u64 t0 = clock_us;
        CHECK(attach() == MTP_E_POWER && B.state == (MTP_ST_FAILED | MTP_ST_IDENTITY));
        CHECK(clock_us - t0 < 20000 && !rtkit_starts && !scm_count && !table_writes);
        CHECK(!asc_reads && !asc_unpowered); /* RUN is never read with a domain off */
        result("pmgr_stuck", before, NULL);
    }
    refused("rtkit", (struct fault){.rtkit_fail = true}, MTP_E_RTKIT, MTP_ST_DART, W_ANY);
    refused("no_fe", (struct fault){.no_fe = true}, MTP_E_SCM_TIMEOUT, MTP_ST_RTKIT, W_ANY);
    refused("mbox_full", (struct fault){.mbox_full = true}, MTP_E_SCM_SEND, MTP_ST_RTKIT, W_ANY);
    refused("no_init", (struct fault){.no_init = true}, MTP_E_INIT_TIMEOUT, MTP_ST_SCM, W_ANY);
    /* The keyboard's last INIT packet came without a descriptor: waited for until the INIT
     * deadline (a later packet may bring it), then no layout. */
    {
        int before = failures;
        F = (struct fault){.no_kbd_desc = true};
        CHECK(attach() == MTP_E_LAYOUT && B.state == (MTP_ST_FAILED | MTP_ST_INIT) && !enables);
        CHECK(B.step_ms[MTP_ST_INIT] - B.step_ms[MTP_ST_SCM] >= MTP_T_INIT_MS);
        CHECK(B.comm.iface[2].last && !B.comm.iface[2].complete && !mmio_bad && !asc_unpowered);
        CHECK(mtp_bringup_poll(&B, 32) == -1);
        result("no_kbd_desc", before, NULL);
    }
    {
        int before = failures;
        F = (struct fault){.kbd_desc_late = true};
        CHECK(attach() == MTP_E_NONE && B.state == MTP_ST_KBD_READY && enables == 1 &&
              B.comm.iface[2].complete && B.comm.iface[2].desc_len == sizeof(kbd_desc));
        type_report(0, 0x04, false);
        mtp_bringup_poll(&B, 32);
        CHECK(down(0x04));
        result("kbd_desc_late", before, NULL);
    }
    {
        int before = failures;
        F = (struct fault){.mouse_desc = true};
        CHECK(attach() == MTP_E_LAYOUT && B.state == (MTP_ST_FAILED | MTP_ST_INIT) && !enables);
        result("layout", before, NULL);
    }
    refused("no_ack", (struct fault){.no_ack = true}, MTP_E_KBD_ACK, MTP_ST_INIT, W_ANY);
    refused("ack_error", (struct fault){.ack_error = true}, MTP_E_KBD_ENABLE, MTP_ST_INIT, W_ANY);
    refused("no_ready", (struct fault){.no_ready = true}, MTP_E_KBD_READY, MTP_ST_INIT, W_ANY);
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

static void b15_gpio_with_ack(void)
{
    int before = failures;
    F = (struct fault){.gpio_with_ack = true};
    CHECK(attach() == MTP_E_NONE && B.state == MTP_ST_KBD_READY && enables == 1);
    mtp_bringup_poll(&B, 32);
    CHECK(gpio_acks == 1 && B.gpio_refused == 1 && gpio_ack_msg[5] == MTP_EV_GPIO &&
          gpio_ack_msg[9] == 5);
    result("gpio_with_ack", before, NULL);
}

static void b16_doorbell_owed(void)
{
    int before = failures;
    F = (struct fault){0};
    CHECK(attach() == MTP_E_NONE);
    /* A GPIO request at run time while the mailbox is full. */
    F.mbox_full = true;
    u8 g[5] = {MTP_EV_GPIO, 2, 0, 0, 7};
    send_to_host(MTP_CH_SCM_REPORT, 0, 0, MTP_FLAGS_INPUT, 0, g, 5);
    u64 t0 = clock_us;
    CHECK(mtp_bringup_poll(&B, 32) >= 1 && B.kbd_live);
    CHECK(clock_us - t0 < 1000 && B.doorbell_owed && B.doorbells_owed == 1 && !gpio_acks);
    CHECK(mtp_bringup_poll(&B, 32) >= 0 && B.doorbell_owed && B.kbd_live);
    /* The mailbox frees: the doorbell is rung, the refusal is taken and acknowledged. */
    F.mbox_full = false;
    CHECK(mtp_bringup_poll(&B, 32) >= 0 && !B.doorbell_owed && gpio_acks == 1);
    mtp_bringup_poll(&B, 32);
    CHECK(!B.cmd.busy && B.kbd_live);
    /* Owed for more than 50 ms: latched. */
    F.mbox_full = true;
    send_to_host(MTP_CH_SCM_REPORT, 0, 0, MTP_FLAGS_INPUT, 0, g, 5);
    CHECK(mtp_bringup_poll(&B, 32) >= 0 && B.doorbell_owed);
    clock_us += 60000;
    CHECK(mtp_bringup_poll(&B, 32) == -1 && B.reason == MTP_E_SCM_SEND && !B.kbd_live);
    F.mbox_full = false;
    result("doorbell_owed", before, NULL);
}

/* ---- I2: the trackpad ------------------------------------------------------------------------ */

static void tp_frame_to_host(int x, int y, bool touching, bool button)
{
    u8 r[TP_HDR_LEN + TP_REC_LEN];
    memset(r, 0, sizeof(r));
    r[0] = TP_REPORT_ID;
    r[TP_HDR_COUNT] = touching ? 1 : 0;
    r[TP_HDR_BUTTON] = button ? 1 : 0;
    u8 *f = r + TP_HDR_LEN;
    f[TP_REC_X] = (u8)x;
    f[TP_REC_X + 1] = (u8)((u16)x >> 8);
    f[TP_REC_Y] = (u8)y;
    f[TP_REC_Y + 1] = (u8)((u16)y >> 8);
    f[TP_REC_TOUCH] = 0x80;
    f[TP_REC_TOUCH + 1] = 0x03;
    send_to_host(MTP_CH_SCM_REPORT, mt_seq++, 1, MTP_FLAGS_INPUT, 0, r,
                 (u16)(TP_HDR_LEN + (touching ? TP_REC_LEN : 0)));
}

static void p1_happy(void)
{
    int before = failures;
    F = (struct fault){0};
    want_tp = true;
    CHECK(mtp_crc32(0, (const u8 *)"123456789", 9) == 0xcbf43926u);
    CHECK(attach() == MTP_E_NONE);
    CHECK(B.state == MTP_ST_TP_READY && B.reason == MTP_E_NONE && !B.tp_reason && B.kbd_live &&
          B.tp_live && B.tp_iface == 1 && B.tp_step == 8);
    CHECK(afe_calls == 2 && afe_asserts == 1 && afe_releases == 1 && !afe_asserted);
    CHECK(mt_enables == 1 && enables == 1 && fw_msgs == 1 && power_msgs == 2 && geom_reqs == 1);
    CHECK(fw_ok_seen && fw_size_seen == FW_LEN && fw_dva_seen == 0x10040514000ull &&
          B.fw_dva == fw_dva_seen);
    CHECK(B.tp_geometry_device && B.tp.res_x == 100 && B.tp.res_y == 100);
    CHECK(mmio_writes == 3 && !mmio_bad && B.afe_writes == 2 && !B.afe_failed);
    CHECK(area[MTP_AREA_FW + FW_IFACE_OFF] == 1 && area[MTP_AREA_FW + FW_IFACE_OFF + 1] ==
                                                       fw_payload[FW_IFACE_OFF + 1]);
    /* Frames: a touch, five 1 mm moves, a click and its release. */
    tp_frame_to_host(0, 0, true, false);
    for (int i = 1; i <= 5; i++)
        tp_frame_to_host(i * 100, 0, true, false);
    mtp_bringup_poll(&B, 32);
    CHECK(B.tp_frames == 6 && ptr_dx > 0 && !B.tp_bad);
    tp_frame_to_host(500, 0, true, true);
    mtp_bringup_poll(&B, 32);
    CHECK(last_ptr.buttons == TP_BTN_LEFT);
    tp_frame_to_host(500, 0, true, false);
    mtp_bringup_poll(&B, 32);
    CHECK(last_ptr.buttons == 0);
    /* The keyboard still works next to it. */
    type_report(0, 0x04, false);
    mtp_bringup_poll(&B, 32);
    CHECK(down(0x04));
    char extra[160];
    snprintf(extra, sizeof(extra),
             ",\"fw_dva\":\"0x%llx\",\"afe_calls\":%u,\"tp_ready_ms\":%u,\"tables\":%u",
             (unsigned long long)fw_dva_seen, afe_calls, B.step_ms[MTP_ST_TP_READY], B.dart.used);
    result("tp_happy", before, extra);
    want_tp = false;
}

static void tp_refused(const char *name, struct fault f, int reason, unsigned afe, u8 fws)
{
    int before = failures;
    F = f;
    want_tp = true;
    CHECK(attach() == MTP_E_NONE);
    CHECK(B.state == MTP_ST_KBD_READY && B.reason == MTP_E_NONE && B.kbd_live);
    CHECK(B.tp_reason == reason && !B.tp_live);
    CHECK(afe_calls == afe && fw_msgs == fws && !mmio_bad);
    /* Trackpad frames now are counted, never translated. */
    tp_frame_to_host(0, 0, true, true);
    mtp_bringup_poll(&B, 32);
    CHECK(!ptr_calls || (last_ptr.buttons == 0 && ptr_calls == 1));
    type_report(0, 0x05, false);
    mtp_bringup_poll(&B, 32);
    CHECK(down(0x05));
    result(name, before, NULL);
    want_tp = false;
}

static void p2_p7(void)
{
    {
        int before = failures;
        F = (struct fault){.bad_params = true};
        want_tp = true;
        CHECK(attach() == MTP_E_NONE && B.tp_reason == MTP_E_TP_PARAMS && !B.want_tp &&
              !B.fw_dva && !afe_calls && !mt_enables && !syncs && B.state == MTP_ST_KBD_READY);
        F = (struct fault){.bad_fw_crc = true};
        CHECK(attach() == MTP_E_NONE && B.tp_reason == MTP_E_TP_PARAMS && !B.fw_dva &&
              !afe_calls && !mt_enables);
        result("tp_params", before, NULL);
        want_tp = false;
    }
    tp_refused("tp_no_init", (struct fault){.no_mt_init = true}, MTP_E_TP_INIT, 0, 0);
    tp_refused("tp_enable", (struct fault){.mt_enable_error = true}, MTP_E_TP_ENABLE, 0, 0);
    tp_refused("tp_afe_assert", (struct fault){.afe_assert_fails = true}, MTP_E_TP_SMC, 1, 0);
    tp_refused("tp_fw_no_ack", (struct fault){.fw_no_ack = true}, MTP_E_TP_FW, 1, 1);
    tp_refused("tp_fw_error", (struct fault){.fw_error = true}, MTP_E_TP_FW, 1, 1);
    tp_refused("tp_power_error", (struct fault){.power_error = true}, MTP_E_TP_POWER, 1, 1);
    tp_refused("tp_afe_release", (struct fault){.afe_release_fails = true}, MTP_E_TP_SMC, 2, 1);
    tp_refused("tp_no_ready", (struct fault){.no_tp_ready = true}, MTP_E_TP_READY, 2, 1);
    {
        int before = failures;
        F = (struct fault){.no_geometry = true};
        want_tp = true;
        CHECK(attach() == MTP_E_NONE && B.state == MTP_ST_TP_READY && B.tp_live &&
              !B.tp_geometry_device && B.tp.res_x == 95 && geom_reqs == 1);
        result("tp_no_geometry", before, NULL);
        want_tp = false;
    }
}

static void p8_runtime(void)
{
    int before = failures;
    F = (struct fault){0};
    want_tp = true;
    CHECK(attach() == MTP_E_NONE && B.tp_live);
    tp_frame_to_host(0, 0, true, false);
    tp_frame_to_host(0, 0, true, true);
    mtp_bringup_poll(&B, 32);
    CHECK(last_ptr.buttons == TP_BTN_LEFT);
    rtkit_failed_now = true;
    CHECK(mtp_bringup_poll(&B, 32) == -1 && !B.tp_live && last_ptr.buttons == 0);
    result("tp_latch", before, NULL);

    before = failures;
    CHECK(attach() == MTP_E_NONE && B.tp_live);
    tp_frame_to_host(0, 0, true, false);
    tp_frame_to_host(0, 0, true, true);
    mtp_bringup_poll(&B, 32);
    CHECK(last_ptr.buttons == TP_BTN_LEFT);
    clock_us += 600000; /* the pad goes silent with the button down */
    mtp_bringup_poll(&B, 32);
    CHECK(last_ptr.buttons == 0 && B.tp.stale == 1 && B.tp_live);
    result("tp_silence", before, NULL);
    want_tp = false;
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
    b15_gpio_with_ack();
    b16_doorbell_owed();
    p1_happy();
    p2_p7();
    p8_runtime();
    free(area);
    return failures ? 1 : 0;
}
