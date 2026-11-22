/* SPDX-License-Identifier: GPL-2.0-only */

/*
 * I1: the T8152 MTP SCM transport, pure.  See mtp_scm.h for the contract and the facts it rests
 * on, and windows-m6/INPUT-I1I2-BUILD.md for the plan.  Written on this branch from those facts;
 * no third-party source.
 */

#include "mtp_scm.h"

#ifdef MTP_HOST_TEST
#define mtp_dma_rmb() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define mtp_dma_mb()  __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define mtp_dma_wmb() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#else
#include "utils.h"
/* The rings are Normal-NC in the carrier (hv_mtp_t8152.c); the MTP is an outer observer. */
#define mtp_dma_rmb() dma_rmb()
#define mtp_dma_mb()  dma_mb()
#define mtp_dma_wmb() dma_wmb()
#endif

static u16 rd16(const u8 *p)
{
    return (u16)(p[0] | p[1] << 8);
}

static u32 rd32(const u8 *p)
{
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static void wr16(u8 *p, u16 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static void wr32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

u64 mtp_scm_msg(u8 id, u64 payload)
{
    return id | (payload & ~(u64)0xff);
}

/* ---- rings ----------------------------------------------------------------------------- */

void mtp_ring_init(struct mtp_ring *r, void *base, u32 bytes)
{
    r->idx = (volatile u32 *)base;
    r->data = (volatile u8 *)base + MTP_RING_HDR;
    r->cap = bytes - MTP_RING_HDR;
    r->pos = 0;
}

static void ring_copy_out(const struct mtp_ring *r, u8 *dst, u32 off, u32 len)
{
    for (u32 i = 0; i < len; i++) {
        u32 at = off + i;
        if (at >= r->cap)
            at -= r->cap;
        dst[i] = r->data[at];
    }
}

static void ring_copy_in(struct mtp_ring *r, u32 off, const u8 *src, u32 len)
{
    for (u32 i = 0; i < len; i++) {
        u32 at = off + i;
        if (at >= r->cap)
            at -= r->cap;
        r->data[at] = src[i];
    }
}

int mtp_ring_rx(struct mtp_ring *r, u8 *buf, u32 buflen, u32 *len)
{
    u32 prod = r->idx[0], cons = r->idx[1];
    *len = 0;
    if (prod >= r->cap || (prod & 3) || cons != r->pos)
        return MTP_RX_BAD_INDEX;
    /* Read only what this producer snapshot published. */
    mtp_dma_rmb();
    u32 avail = prod >= cons ? prod - cons : r->cap - cons + prod;
    if (avail < MTP_FRAME_HDR)
        return MTP_RX_EMPTY;
    u8 hdr[MTP_FRAME_HDR];
    ring_copy_out(r, hdr, cons, MTP_FRAME_HDR);
    u32 length = rd16(hdr + 2);
    if (hdr[0] != MTP_FRAME_HDR || (length & 3))
        return MTP_RX_BAD_FRAME;
    u32 size = MTP_FRAME_HDR + length + MTP_FRAME_CSUM;
    if (size > r->cap)
        return MTP_RX_BAD_FRAME;
    if (avail < size)
        return MTP_RX_EMPTY;
    int rc = MTP_RX_SKIPPED;
    if (size <= buflen) {
        ring_copy_out(r, buf, cons, size);
        *len = size;
        rc = MTP_RX_FRAME;
    }
    cons += size;
    if (cons >= r->cap)
        cons -= r->cap;
    r->pos = cons;
    /* Every read of the frame completes before the producer may reuse its bytes. */
    mtp_dma_mb();
    r->idx[1] = cons;
    return rc;
}

int mtp_ring_tx(struct mtp_ring *r, const u8 *frame, u32 len)
{
    if (!len || (len & 3) || len + 4 > r->cap)
        return MTP_TX_BAD_FRAME;
    u32 prod = r->idx[0], cons = r->idx[1];
    if (prod != r->pos || prod >= r->cap || cons >= r->cap || (prod & 3))
        return MTP_TX_BAD_INDEX;
    u32 used = prod >= cons ? prod - cons : r->cap - cons + prod;
    if (len + 4 > r->cap - used)
        return MTP_TX_FULL;
    ring_copy_in(r, prod, frame, len);
    prod += len;
    if (prod >= r->cap)
        prod -= r->cap;
    r->pos = prod;
    /* The frame is visible before its producer index. */
    mtp_dma_wmb();
    r->idx[0] = prod;
    mtp_dma_mb();
    return MTP_TX_OK;
}

/* ---- frames ---------------------------------------------------------------------------- */

u32 mtp_csum(const u8 *p, u32 len)
{
    u32 sum = 0;
    for (u32 i = 0; i + 4 <= len; i += 4)
        sum += rd32(p + i);
    return sum;
}

int mtp_frame_parse(const u8 *f, u32 len, struct mtp_msg *m)
{
    if (len < MTP_FRAME_HDR + MTP_FRAME_CSUM)
        return MTP_PARSE_SHORT;
    u32 length = rd16(f + 2);
    if (f[0] != MTP_FRAME_HDR || (length & 3) ||
        len != MTP_FRAME_HDR + length + MTP_FRAME_CSUM)
        return MTP_PARSE_SHORT;
    if (mtp_csum(f, len) != 0xffffffffu)
        return MTP_PARSE_CSUM;
    m->channel = f[1];
    m->seq = f[4];
    m->iface = f[5];
    m->notify = false;
    m->flags = 0;
    m->retcode = 0;
    m->data = f + MTP_FRAME_HDR;
    m->len = 0;
    if (m->channel == MTP_CH_NOTIFY && !length) {
        m->notify = true;
        return MTP_PARSE_OK;
    }
    if (length < MTP_FRAME_SUB)
        return MTP_PARSE_SUB;
    const u8 *sub = f + MTP_FRAME_HDR;
    u32 room = length - MTP_FRAME_SUB;
    const u8 *data = sub + MTP_FRAME_SUB;
    if (m->channel == MTP_CH_SCM_REPORT) {
        if (room < MTP_FRAME_TS)
            return MTP_PARSE_TS;
        room -= MTP_FRAME_TS;
        data += MTP_FRAME_TS;
    }
    u16 n = rd16(sub + 2);
    if (n > room)
        return MTP_PARSE_SUB;
    m->flags = sub[0];
    m->retcode = rd32(sub + 4);
    m->data = data;
    m->len = n;
    return MTP_PARSE_OK;
}

u32 mtp_frame_build(u8 *out, u32 outlen, u8 seq, u8 iface, u8 flags, const u8 *msg, u16 n)
{
    u32 padded = ((u32)n + 3u) & ~3u;
    u32 length = MTP_FRAME_SUB + padded;
    u32 total = MTP_FRAME_HDR + length + MTP_FRAME_CSUM;
    if (length > 0xfffcu || total > outlen)
        return 0;
    for (u32 i = 0; i < total; i++)
        out[i] = 0;
    out[0] = MTP_FRAME_HDR;
    out[1] = MTP_CH_CMD;
    wr16(out + 2, (u16)length);
    out[4] = seq;
    out[5] = iface;
    out[8] = flags;
    wr16(out + 10, n);
    for (u32 i = 0; i < n; i++)
        out[16 + i] = msg[i];
    wr32(out + total - 4, 0xffffffffu - mtp_csum(out, total - 4));
    return total;
}

/* ---- interface 0 events ------------------------------------------------------------------ */

void mtp_comm_init(struct mtp_comm *c)
{
    u8 *p = (u8 *)c;
    for (u32 i = 0; i < sizeof(*c); i++)
        p[i] = 0;
}

static bool name_is(const char *a, const char *b)
{
    for (u32 i = 0;; i++) {
        if (a[i] != b[i])
            return false;
        if (!a[i])
            return true;
    }
}

int mtp_comm_find(const struct mtp_comm *c, const char *name)
{
    for (u32 i = 0; i < MTP_MAX_IFACES; i++)
        if (c->iface[i].seen && name_is(c->iface[i].name, name))
            return (int)i;
    return -1;
}

static int init_event(struct mtp_comm *c, const u8 *p, u16 len, u8 *out)
{
    if (len < MTP_INIT_HDR || p[3] >= MTP_MAX_IFACES) {
        c->bad_events++;
        return MTP_EVT_BAD;
    }
    struct mtp_iface_info *f = &c->iface[p[3]];
    *out = p[3];
    f->seen = true;
    for (u32 i = 0; i < MTP_NAME_LEN; i++)
        f->name[i] = (char)p[4 + i];
    f->name[MTP_NAME_LEN] = 0;
    c->inits++;
    u32 at = MTP_INIT_HDR;
    bool bad = false;
    while (at + 4 <= len) {
        u16 type = rd16(p + at), blen = rd16(p + at + 2);
        at += 4;
        if (blen > len - at) {
            bad = true;
            break;
        }
        switch (type) {
            case MTP_INIT_BLK_DESC:
                if (!f->desc_len) {
                    u16 keep = blen > MTP_DESC_MAX ? MTP_DESC_MAX : blen;
                    for (u32 i = 0; i < keep; i++)
                        f->desc[i] = p[at + i];
                    f->desc_len = keep;
                    f->desc_truncated = keep != blen;
                }
                break;
            case MTP_INIT_BLK_GPIO:
                f->gpio = true;
                break;
            case MTP_INIT_BLK_END:
            case MTP_INIT_BLK_NAME:
                break;
            default:
                c->unknown_blocks++;
                break;
        }
        at += blen;
        if (type == MTP_INIT_BLK_END)
            break;
    }
    if (bad) {
        c->bad_events++;
        return MTP_EVT_BAD;
    }
    /* As Linux on ring transports: the interface is complete only with its descriptor. */
    if (!p[20])
        f->last = true;
    f->complete = f->last && f->desc_len;
    return MTP_EVT_INIT;
}

int mtp_comm_event(struct mtp_comm *c, const u8 *p, u16 len, u8 *iface)
{
    if (!len)
        return MTP_EVT_NONE;
    switch (p[0]) {
        case MTP_EV_INIT:
            return init_event(c, p, len, iface);
        case MTP_EV_READY:
            if (len < 2 || p[1] >= MTP_MAX_IFACES) {
                c->bad_events++;
                return MTP_EVT_BAD;
            }
            *iface = p[1];
            c->iface[p[1]].ready = true;
            c->readies++;
            return MTP_EVT_READY;
        case MTP_EV_GPIO:
            /* type, iface, gpio, pad, cmd */
            if (len < 5) {
                c->bad_events++;
                return MTP_EVT_BAD;
            }
            *iface = p[1];
            c->gpio_requests++;
            return MTP_EVT_GPIO;
        default:
            return MTP_EVT_NONE;
    }
}

u16 mtp_gpio_ack(u8 *out, u16 outlen, u32 retcode, const u8 *req, u16 req_len)
{
    if ((u32)req_len + 5 > outlen)
        return 0;
    out[0] = MTP_CMD_GPIO_ACK;
    wr32(out + 1, retcode);
    for (u32 i = 0; i < req_len; i++)
        out[5 + i] = req[i];
    return (u16)(req_len + 5);
}

bool mtp_cmd_ack(struct mtp_cmd *c, const struct mtp_msg *m)
{
    if (!c->busy || m->channel != MTP_CH_CMD || m->iface != c->iface || m->seq != c->seq ||
        m->flags != c->flags || m->len < 1 || m->data[0] != c->first)
        return false;
    c->busy = false;
    c->done = true;
    c->retcode = m->retcode;
    c->resp_len = m->len;
    for (u32 i = 0; i < MTP_CMD_RESP_MAX; i++)
        c->resp[i] = i < m->len ? m->data[i] : 0;
    return true;
}
