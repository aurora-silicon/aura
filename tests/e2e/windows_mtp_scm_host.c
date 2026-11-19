/* SPDX-License-Identifier: MIT */
/*
 * Host replay of src/hv/windows/mtp_scm.c (the T8152 MTP SCM transport, pure).  Built by
 * tests/python/test_windows_t8152_input_mtp.py with ASan/UBSan.  Synthetic frames only.
 *
 * Failure list (written before the code):
 *   S1  the checksum is the little-endian u32 sum to 0xFFFFFFFF; every one-bit flip fails it
 *   S2  ring RX: empty, a partial header, a partial frame, frames that wrap byte-wise at the end
 *       (header and body), several frames in one producer step
 *   S3  ring RX refusals consume nothing: producer >= capacity, an unaligned producer, a
 *       consumer the other side moved, hdr_len != 8, an unaligned length, a frame larger than
 *       the ring
 *   S4  a frame larger than the caller's buffer is consumed unread (SKIPPED)
 *   S5  ring TX: wrap, "full" when frame + 4 does not fit, a foreign producer, a consumer out of
 *       range, unaligned or empty frames
 *   S6  parse: short, checksum, subheader past the frame, channel 0x14 timestamp strip, a 0x14
 *       frame shorter than subheader + timestamp, an empty notification
 *   S7  build: the bytes of a hand-encoded frame (header, subheader, padding, checksum)
 *   S8  INIT: one packet; several packets (more_packets); a descriptor over the cap is marked
 *       truncated; an interface out of range; a block running past the event; a name with no NUL
 *   S9  READY, GPIO, empty and unknown events
 *   S10 acknowledgements: matched only on channel 0x11 with the same iface, seq, flags and
 *       first byte, and a non-empty message
 *   S11 seeded random rings, frames and events never read or write out of bounds
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mtp_scm.h"

static int failures;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                    \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static void case_result(const char *name, int before)
{
    printf("{\"case\":\"%s\",\"result\":\"%s\"}\n", name, failures == before ? "passed" : "failed");
}

static u32 le32(const u8 *p)
{
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static void put32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

/* A frame from the documented layout, independent of mtp_frame_build(). */
static u32 hand_frame(u8 *out, u8 ch, u8 seq, u8 iface, u8 flags, const u8 *msg, u16 n, bool ts)
{
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
    memcpy(out + 16 + (ts ? 8 : 0), msg, n);
    u32 sum = 0;
    for (u32 i = 0; i < 8 + body; i += 4)
        sum += le32(out + i);
    put32(out + 8 + body, 0xffffffffu - sum);
    return 8 + body + 4;
}

/* Write `len` bytes into a ring the way the far side does (byte-wise wrap), then publish. */
static void far_write(u8 *ring, u32 bytes, const u8 *p, u32 len)
{
    u32 cap = bytes - 8, prod = le32(ring);
    for (u32 i = 0; i < len; i++)
        ring[8 + (prod + i) % cap] = p[i];
    put32(ring, (prod + len) % cap);
}

static u8 *new_ring(u32 bytes)
{
    u8 *r = calloc(1, bytes);
    if (!r)
        abort();
    return r;
}

static void s1_checksum(void)
{
    int before = failures;
    u8 f[64], msg[5] = {0xb4, 0x02, 0xaa, 0xbb, 0xcc};
    u32 n = hand_frame(f, MTP_CH_CMD, 7, 0, MTP_FLAGS_SET_FEATURE, msg, 5, false);
    CHECK(mtp_csum(f, n) == 0xffffffffu);
    struct mtp_msg m;
    CHECK(mtp_frame_parse(f, n, &m) == MTP_PARSE_OK);
    for (u32 bit = 0; bit < n * 8; bit++) {
        f[bit / 8] ^= (u8)(1u << (bit % 8));
        int rc = mtp_frame_parse(f, n, &m);
        CHECK(rc != MTP_PARSE_OK);
        f[bit / 8] ^= (u8)(1u << (bit % 8));
    }
    case_result("checksum", before);
}

static void s2_ring_rx(void)
{
    int before = failures;
    const u32 bytes = 256 + 8; /* a small ring: capacity 256 */
    u8 *ring = new_ring(bytes);
    struct mtp_ring r;
    mtp_ring_init(&r, ring, bytes);
    CHECK(r.cap == 256);
    u8 buf[512], f[128], msg[20];
    u32 len = 0;
    for (u32 i = 0; i < sizeof(msg); i++)
        msg[i] = (u8)(0x40 + i);
    CHECK(mtp_ring_rx(&r, buf, sizeof(buf), &len) == MTP_RX_EMPTY);
    u32 n = hand_frame(f, MTP_CH_SCM_REPORT, 1, 2, 0, msg, 9, true); /* 8+8+8+12+4 = 40 */
    CHECK(n == 40);
    /* A partial header, then a partial frame: nothing is taken. */
    far_write(ring, bytes, f, 4);
    CHECK(mtp_ring_rx(&r, buf, sizeof(buf), &len) == MTP_RX_EMPTY && le32(ring + 4) == 0);
    far_write(ring, bytes, f + 4, 20);
    CHECK(mtp_ring_rx(&r, buf, sizeof(buf), &len) == MTP_RX_EMPTY && le32(ring + 4) == 0);
    far_write(ring, bytes, f + 24, n - 24);
    CHECK(mtp_ring_rx(&r, buf, sizeof(buf), &len) == MTP_RX_FRAME && len == n &&
          !memcmp(buf, f, n) && le32(ring + 4) == n);
    /* Run the ring round several times: every frame wraps somewhere eventually. */
    u32 got = 0, sent = 0;
    for (u32 round = 0; round < 200; round++) {
        u32 k = round % 3 + 1;
        for (u32 j = 0; j < k; j++) {
            msg[0] = (u8)(sent);
            n = hand_frame(f, MTP_CH_SCM_REPORT, (u8)sent, 2, 0, msg, (u16)(1 + sent % 17), true);
            u32 prod = le32(ring), cons = le32(ring + 4);
            u32 used = (prod + r.cap - cons) % r.cap;
            if (used + n + 4 > r.cap)
                break;
            far_write(ring, bytes, f, n);
            sent++;
        }
        int rc;
        while ((rc = mtp_ring_rx(&r, buf, sizeof(buf), &len)) == MTP_RX_FRAME) {
            struct mtp_msg m;
            CHECK(mtp_frame_parse(buf, len, &m) == MTP_PARSE_OK && m.seq == (u8)got &&
                  m.data[0] == (u8)got);
            got++;
        }
        CHECK(rc == MTP_RX_EMPTY);
    }
    CHECK(got == sent && sent > 300);
    free(ring);
    case_result("ring_rx", before);
}

static void s3_ring_rx_refusals(void)
{
    int before = failures;
    const u32 bytes = 128 + 8;
    u8 buf[256], f[64], msg[4] = {1, 2, 3, 4};
    u32 len, n = hand_frame(f, MTP_CH_CMD, 0, 0, 0x80, msg, 4, false);
    for (int which = 0; which < 6; which++) {
        u8 *ring = new_ring(bytes);
        struct mtp_ring r;
        mtp_ring_init(&r, ring, bytes);
        far_write(ring, bytes, f, n);
        int want = MTP_RX_BAD_INDEX;
        switch (which) {
            case 0:
                put32(ring, 128); /* producer == capacity */
                break;
            case 1:
                put32(ring, n + 2); /* unaligned */
                break;
            case 2:
                put32(ring + 4, 4); /* the consumer moved under us */
                break;
            case 3:
                ring[8] = 9; /* hdr_len */
                want = MTP_RX_BAD_FRAME;
                break;
            case 4:
                ring[8 + 2] = 6; /* length not a multiple of 4 */
                want = MTP_RX_BAD_FRAME;
                break;
            case 5:
                ring[8 + 2] = 0x00;
                ring[8 + 3] = 0x01; /* 256: larger than the ring */
                want = MTP_RX_BAD_FRAME;
                break;
        }
        u32 cons = le32(ring + 4);
        CHECK(mtp_ring_rx(&r, buf, sizeof(buf), &len) == want);
        CHECK(le32(ring + 4) == cons && r.pos == 0);
        free(ring);
    }
    case_result("ring_rx_refusals", before);
}

static void s4_oversize(void)
{
    int before = failures;
    const u32 bytes = 1024 + 8;
    u8 *ring = new_ring(bytes);
    struct mtp_ring r;
    mtp_ring_init(&r, ring, bytes);
    u8 f[600], msg[500], small[64], buf[64];
    memset(msg, 0x5a, sizeof(msg));
    u32 big = hand_frame(f, MTP_CH_SCM_REPORT, 0, 1, 0, msg, 500, true);
    far_write(ring, bytes, f, big);
    u32 n = hand_frame(small, MTP_CH_SCM_REPORT, 1, 2, 0, msg, 8, true);
    far_write(ring, bytes, small, n);
    u32 len = 0;
    CHECK(mtp_ring_rx(&r, buf, sizeof(buf), &len) == MTP_RX_SKIPPED && le32(ring + 4) == big);
    CHECK(mtp_ring_rx(&r, buf, sizeof(buf), &len) == MTP_RX_FRAME && len == n);
    free(ring);
    case_result("oversize", before);
}

static void s5_ring_tx(void)
{
    int before = failures;
    const u32 bytes = 96 + 8;
    u8 *ring = new_ring(bytes);
    struct mtp_ring r;
    mtp_ring_init(&r, ring, bytes);
    u8 f[64], out[96], msg[8] = {0xb4, 2};
    u32 n = hand_frame(f, MTP_CH_CMD, 0, 0, 0x80, msg, 2, false); /* 8+8+4+4 = 24 */
    CHECK(n == 24);
    CHECK(mtp_ring_tx(&r, f, n) == MTP_TX_OK && le32(ring) == 24);
    CHECK(mtp_ring_tx(&r, f, n) == MTP_TX_OK && le32(ring) == 48);
    CHECK(mtp_ring_tx(&r, f, n) == MTP_TX_OK && le32(ring) == 72);
    /* 72 used of 96: 24 + 4 does not fit. */
    CHECK(mtp_ring_tx(&r, f, n) == MTP_TX_FULL && le32(ring) == 72);
    /* The far side consumes 48: the next frame wraps byte-wise. */
    put32(ring + 4, 48);
    CHECK(mtp_ring_tx(&r, f, n) == MTP_TX_OK && le32(ring) == 0);
    for (u32 i = 0; i < n; i++)
        out[i] = ring[8 + (72 + i) % 96];
    CHECK(!memcmp(out, f, n));
    CHECK(mtp_ring_tx(&r, f, n - 2) == MTP_TX_BAD_FRAME);
    CHECK(mtp_ring_tx(&r, f, 0) == MTP_TX_BAD_FRAME);
    CHECK(mtp_ring_tx(&r, f, 200) == MTP_TX_BAD_FRAME);
    put32(ring, 4); /* someone else moved our producer */
    CHECK(mtp_ring_tx(&r, f, n) == MTP_TX_BAD_INDEX);
    put32(ring, 0);
    put32(ring + 4, 96); /* consumer out of range */
    CHECK(mtp_ring_tx(&r, f, n) == MTP_TX_BAD_INDEX);
    free(ring);
    case_result("ring_tx", before);
}

static void s6_parse(void)
{
    int before = failures;
    u8 f[128], msg[12];
    struct mtp_msg m;
    for (u32 i = 0; i < sizeof(msg); i++)
        msg[i] = (u8)(0x10 + i);
    u32 n = hand_frame(f, MTP_CH_SCM_REPORT, 3, 2, 0x00, msg, 9, true);
    CHECK(mtp_frame_parse(f, n, &m) == MTP_PARSE_OK && m.channel == MTP_CH_SCM_REPORT &&
          m.iface == 2 && m.seq == 3 && m.len == 9 && !memcmp(m.data, msg, 9) && !m.notify);
    CHECK(mtp_frame_parse(f, n - 4, &m) == MTP_PARSE_SHORT);
    CHECK(mtp_frame_parse(f, 8, &m) == MTP_PARSE_SHORT);
    n = hand_frame(f, MTP_CH_CMD, 3, 0, 0x80, msg, 9, false);
    CHECK(mtp_frame_parse(f, n, &m) == MTP_PARSE_OK && m.len == 9 && m.data == f + 16);
    /* Subheader length past the frame (checksum fixed up). */
    f[10] = 40;
    put32(f + n - 4, 0);
    put32(f + n - 4, 0xffffffffu - mtp_csum(f, n - 4));
    CHECK(mtp_frame_parse(f, n, &m) == MTP_PARSE_SUB);
    /* Channel 0x14 with only the subheader: no room for the timestamp. */
    u8 g[32] = {8, MTP_CH_SCM_REPORT, 8, 0, 0, 2, 0, 0};
    put32(g + 16, 0xffffffffu - mtp_csum(g, 16));
    CHECK(mtp_frame_parse(g, 20, &m) == MTP_PARSE_TS);
    /* Channel 0x11 with a 4-byte payload: no room for the subheader. */
    u8 h[32] = {8, MTP_CH_CMD, 4, 0, 0, 0, 0, 0};
    put32(h + 12, 0xffffffffu - mtp_csum(h, 12));
    CHECK(mtp_frame_parse(h, 16, &m) == MTP_PARSE_SUB);
    /* An empty notification. */
    u8 z[12] = {8, MTP_CH_NOTIFY, 0, 0, 0, 0, 0, 0};
    put32(z + 8, 0xffffffffu - mtp_csum(z, 8));
    CHECK(mtp_frame_parse(z, 12, &m) == MTP_PARSE_OK && m.notify && m.len == 0);
    case_result("parse", before);
}

static void s7_build(void)
{
    int before = failures;
    u8 a[64], b[64], msg[5] = {0xb4, 0x02, 0x99, 0x98, 0x97};
    for (u16 len = 0; len <= 5; len++) {
        u32 n = hand_frame(a, MTP_CH_CMD, 0x21, 0, MTP_FLAGS_SET_FEATURE, msg, len, false);
        u32 m = mtp_frame_build(b, sizeof(b), 0x21, 0, MTP_FLAGS_SET_FEATURE, msg, len);
        CHECK(m == n && !memcmp(a, b, n));
    }
    CHECK(mtp_frame_build(b, 20, 0, 0, 0x80, msg, 5) == 0);
    CHECK(mtp_scm_msg(MTP_SCM_POWER, (u64)MTP_SCM_POWER_ON << 8) == 0x2fd);
    CHECK(mtp_scm_msg(MTP_SCM_AP_FIFO, 0x10040000000ull) == 0x100400000b0ull);
    CHECK(mtp_scm_msg(MTP_SCM_AHT_READY, 0x1234) == 0x12ff);
    case_result("build", before);
}

/* An INIT event: header, then (type, length, payload) blocks. */
static u16 init_event(u8 *out, u8 iface, const char *name, u8 more, const u8 *desc, u16 dlen,
                      bool terminate)
{
    memset(out, 0, MTP_INIT_HDR);
    out[0] = MTP_EV_INIT;
    out[1] = 1;
    out[3] = iface;
    strncpy((char *)out + 4, name, 16);
    out[20] = more;
    u16 n = MTP_INIT_HDR;
    if (desc) {
        out[n] = 0;
        out[n + 1] = 0;
        out[n + 2] = (u8)dlen;
        out[n + 3] = (u8)(dlen >> 8);
        memcpy(out + n + 4, desc, dlen);
        n += 4 + dlen;
    }
    const char product[] = "TP";
    out[n] = 7;
    out[n + 1] = 0;
    out[n + 2] = sizeof(product);
    out[n + 3] = 0;
    memcpy(out + n + 4, product, sizeof(product));
    n += 4 + sizeof(product);
    if (terminate) {
        u8 end[10] = {2, 0, 6, 0, 0, 0, 0, 0, 0, 0};
        memcpy(out + n, end, sizeof(end));
        n += sizeof(end);
    }
    return n;
}

static void s8_init(void)
{
    int before = failures;
    static struct mtp_comm c;
    static u8 ev[4096], desc[3000];
    for (u32 i = 0; i < sizeof(desc); i++)
        desc[i] = (u8)(i * 7);
    mtp_comm_init(&c);
    u8 iface = 0xff;
    u16 n = init_event(ev, 2, "keyboard", 0, desc, 64, true);
    CHECK(mtp_comm_event(&c, ev, n, &iface) == MTP_EVT_INIT && iface == 2);
    CHECK(c.iface[2].seen && c.iface[2].complete && c.iface[2].desc_len == 64 &&
          !memcmp(c.iface[2].desc, desc, 64) && !strcmp(c.iface[2].name, "keyboard"));
    CHECK(mtp_comm_find(&c, "keyboard") == 2 && mtp_comm_find(&c, "multi-touch") == -1);
    /* Two packets: the first says more follow, the descriptor in the second. */
    n = init_event(ev, 1, "multi-touch", 1, NULL, 0, true);
    CHECK(mtp_comm_event(&c, ev, n, &iface) == MTP_EVT_INIT && iface == 1);
    CHECK(c.iface[1].seen && !c.iface[1].complete && !c.iface[1].desc_len);
    n = init_event(ev, 1, "multi-touch", 0, desc, 300, true);
    CHECK(mtp_comm_event(&c, ev, n, &iface) == MTP_EVT_INIT && c.iface[1].complete &&
          c.iface[1].desc_len == 300);
    /* The last packet without a descriptor (ring transports, as Linux waits): last but not
     * complete; a later packet with the descriptor completes it. */
    n = init_event(ev, 6, "keyboard2", 0, NULL, 0, true);
    CHECK(mtp_comm_event(&c, ev, n, &iface) == MTP_EVT_INIT && iface == 6);
    CHECK(c.iface[6].last && !c.iface[6].complete && !c.iface[6].desc_len);
    n = init_event(ev, 6, "keyboard2", 0, desc, 40, true);
    CHECK(mtp_comm_event(&c, ev, n, &iface) == MTP_EVT_INIT && c.iface[6].last &&
          c.iface[6].complete && c.iface[6].desc_len == 40);
    /* Over the cap: kept truncated and marked. */
    n = init_event(ev, 3, "stm", 0, desc, 2500, true);
    CHECK(mtp_comm_event(&c, ev, n, &iface) == MTP_EVT_INIT && c.iface[3].desc_truncated);
    /* An interface out of range. */
    n = init_event(ev, MTP_MAX_IFACES, "x", 0, desc, 8, true);
    CHECK(mtp_comm_event(&c, ev, n, &iface) == MTP_EVT_BAD);
    /* A block that runs past the event: the packet is taken up to it, nothing beyond. */
    n = init_event(ev, 4, "actuator", 0, desc, 32, false);
    ev[MTP_INIT_HDR + 2] = 200;
    CHECK(mtp_comm_event(&c, ev, n, &iface) == MTP_EVT_BAD && c.iface[4].desc_len == 0);
    /* A 16-byte name with no NUL is kept, terminated. */
    n = init_event(ev, 5, "0123456789abcdef", 0, desc, 4, true);
    CHECK(mtp_comm_event(&c, ev, n, &iface) == MTP_EVT_INIT &&
          !strcmp(c.iface[5].name, "0123456789abcdef"));
    /* Short header. */
    CHECK(mtp_comm_event(&c, ev, MTP_INIT_HDR - 1, &iface) == MTP_EVT_BAD);
    case_result("init", before);
}

static void s9_events(void)
{
    int before = failures;
    static struct mtp_comm c;
    mtp_comm_init(&c);
    u8 iface = 0;
    u8 ready[4] = {MTP_EV_READY, 2, 0, 0};
    CHECK(mtp_comm_event(&c, ready, 4, &iface) == MTP_EVT_READY && iface == 2 &&
          c.iface[2].ready);
    u8 ready_bad[2] = {MTP_EV_READY, 9};
    CHECK(mtp_comm_event(&c, ready_bad, 2, &iface) == MTP_EVT_BAD);
    CHECK(mtp_comm_event(&c, ready, 1, &iface) == MTP_EVT_BAD);
    u8 gpio[5] = {MTP_EV_GPIO, 1, 0, 0, 3};
    CHECK(mtp_comm_event(&c, gpio, 5, &iface) == MTP_EVT_GPIO && iface == 1 &&
          c.gpio_requests == 1);
    CHECK(mtp_comm_event(&c, gpio, 4, &iface) == MTP_EVT_BAD);
    u8 ack[32];
    u16 n = mtp_gpio_ack(ack, sizeof(ack), MTP_GPIO_REFUSED, gpio, 5);
    CHECK(n == 10 && ack[0] == MTP_CMD_GPIO_ACK && le32(ack + 1) == MTP_GPIO_REFUSED &&
          !memcmp(ack + 5, gpio, 5));
    CHECK(mtp_gpio_ack(ack, 9, MTP_GPIO_REFUSED, gpio, 5) == 0);
    u8 other[3] = {0x42, 0, 0};
    CHECK(mtp_comm_event(&c, other, 3, &iface) == MTP_EVT_NONE);
    CHECK(mtp_comm_event(&c, other, 0, &iface) == MTP_EVT_NONE);
    case_result("events", before);
}

static void s10_ack(void)
{
    int before = failures;
    u8 f[64], resp[3] = {MTP_CMD_ENABLE, 0, 0};
    struct mtp_msg m;
    struct mtp_cmd c = {.busy = true, .iface = 0, .seq = 5, .flags = 0x80, .first = MTP_CMD_ENABLE};
    struct {
        u8 ch, seq, iface, flags, first;
        u16 len;
        bool ok;
    } t[] = {
        {MTP_CH_CMD, 5, 0, 0x80, MTP_CMD_ENABLE, 1, true},
        {MTP_CH_CMD, 6, 0, 0x80, MTP_CMD_ENABLE, 1, false},
        {MTP_CH_CMD, 5, 1, 0x80, MTP_CMD_ENABLE, 1, false},
        {MTP_CH_CMD, 5, 0, 0x81, MTP_CMD_ENABLE, 1, false},
        {MTP_CH_CMD, 5, 0, 0x80, 0x95, 1, false},
        {MTP_CH_CMD, 5, 0, 0x80, MTP_CMD_ENABLE, 0, false},
        {MTP_CH_SCM_REPORT, 5, 0, 0x80, MTP_CMD_ENABLE, 1, false},
    };
    for (u32 i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        resp[0] = t[i].first;
        u32 n = hand_frame(f, t[i].ch, t[i].seq, t[i].iface, t[i].flags, resp, t[i].len,
                           t[i].ch == MTP_CH_SCM_REPORT);
        put32(f + 12, 0x1234); /* retcode */
        put32(f + n - 4, 0);
        put32(f + n - 4, 0xffffffffu - mtp_csum(f, n - 4));
        CHECK(mtp_frame_parse(f, n, &m) == MTP_PARSE_OK);
        c.busy = true;
        c.done = false;
        CHECK(mtp_cmd_ack(&c, &m) == t[i].ok);
        CHECK(c.done == t[i].ok && c.busy == !t[i].ok);
        if (t[i].ok)
            CHECK(c.retcode == 0x1234);
    }
    c.busy = false;
    CHECK(!mtp_cmd_ack(&c, &m));
    case_result("ack", before);
}

static u64 rng = 0x9e3779b97f4a7c15ull;
static u32 rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (u32)rng;
}

static void s11_fuzz(long iterations)
{
    int before = failures;
    const u32 bytes = 512 + 8;
    u8 *ring = new_ring(bytes);
    static struct mtp_comm c;
    mtp_comm_init(&c);
    u8 buf[300];
    struct mtp_ring r;
    long frames = 0, errors = 0;
    for (long it = 0; it < iterations; it++) {
        if (it % 64 == 0) {
            memset(ring, 0, bytes);
            mtp_ring_init(&r, ring, bytes);
        }
        /* Random bytes, sometimes a real frame, sometimes broken indices. */
        u32 k = rnd() % 8;
        for (u32 i = 0; i < k; i++)
            ring[8 + rnd() % 512] = (u8)rnd();
        if (rnd() % 3 == 0) {
            u8 f[300], msg[256];
            for (u32 i = 0; i < sizeof(msg); i++)
                msg[i] = (u8)rnd();
            if (rnd() % 2)
                msg[0] = rnd() % 2 ? MTP_EV_INIT : MTP_EV_READY;
            u32 n = hand_frame(f, rnd() % 2 ? MTP_CH_SCM_REPORT : MTP_CH_CMD, (u8)rnd(),
                               (u8)(rnd() % 10), (u8)rnd(), msg, (u16)(rnd() % 240), rnd() % 2);
            u32 prod = le32(ring), cons = le32(ring + 4);
            if (prod < 512 && cons < 512 && ((prod - cons + 512) % 512) + n + 4 <= 512)
                far_write(ring, bytes, f, n);
        }
        if (rnd() % 50 == 0)
            put32(ring, rnd());
        if (rnd() % 50 == 0)
            put32(ring + 4, rnd());
        u32 len = 0;
        int rc = mtp_ring_rx(&r, buf, sizeof(buf), &len);
        if (rc == MTP_RX_FRAME) {
            struct mtp_msg m;
            if (mtp_frame_parse(buf, len, &m) == MTP_PARSE_OK) {
                frames++;
                u8 iface;
                mtp_comm_event(&c, m.data, m.len, &iface);
            }
        } else if (rc < 0) {
            errors++;
            memset(ring, 0, bytes);
            mtp_ring_init(&r, ring, bytes);
        }
        /* Raw event bytes too. */
        u8 ev[64];
        for (u32 i = 0; i < sizeof(ev); i++)
            ev[i] = (u8)rnd();
        ev[0] = (u8)(0xf0 + rnd() % 2);
        u8 iface;
        mtp_comm_event(&c, ev, (u16)(rnd() % 65), &iface);
    }
    free(ring);
    printf("{\"case\":\"fuzz\",\"result\":\"%s\",\"iterations\":%ld,\"frames\":%ld,"
           "\"errors\":%ld}\n",
           failures == before ? "passed" : "failed", iterations, frames, errors);
}

int main(int argc, char **argv)
{
    long iterations = argc > 1 ? atol(argv[1]) : 100000;
    s1_checksum();
    s2_ring_rx();
    s3_ring_rx_refusals();
    s4_oversize();
    s5_ring_tx();
    s6_parse();
    s7_build();
    s8_init();
    s9_events();
    s10_ack();
    s11_fuzz(iterations);
    return failures ? 1 : 0;
}
