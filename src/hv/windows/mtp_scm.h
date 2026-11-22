/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef MTP_SCM_H
#define MTP_SCM_H

#include "types.h"

/*
 * I1: the T8152 (J804) MTP's SCM transport, pure (no MMIO, no locks, no allocation).
 * hv_mtp_t8152.c is the hardware glue; mtp_bringup.c drives the protocol;
 * tests/e2e/windows_mtp_scm_host.c replays it on the host.
 *
 * Facts (windows-m6/INPUT-I1I2-BUILD.md section 1; measured by our own Linux driver,
 * linux-j804/patches/t8152-mtp-hid.py, and the in-tree MIT proxyclient/aura/fw/mtp.py):
 *
 *  - RTKit application endpoint 0x20 carries 64-bit messages, ID in bits 7:0.  The host sends
 *    0xfd | 2 << 8 (transport power on), the MTP answers 0xfe; the host registers its rings
 *    (address = message & ~0xfff): 0xb0 AP->IOP, 0xa0 IOP->AP, 0xa1 IOP->AP wake ring; then
 *    0xff.  0x01 is the MTP's doorbell, 0x02 the host's.
 *  - Rings: an 8-byte header (producer u32, consumer u32), then a byte ring of DockChannel-HID
 *    frames (2 MiB rings; the wake ring is 16 KiB of which the firmware uses 4 KiB).  The
 *    producer is 4-byte aligned and below the capacity; a frame may wrap byte-wise.
 *  - A frame: hdr_len (8), channel, u16 length, seq, iface, u16 pad; then `length` bytes; then
 *    a u32 checksum chosen so that the little-endian u32 sum of the whole frame is 0xFFFFFFFF.
 *    `length` is a multiple of 4.
 *  - The payload starts with an 8-byte subheader: flags (bits 7:6 HID report type, 5:0
 *    request), a pad byte, u16 message length, u32 return code.  MTP reports arrive on
 *    channel 0x14 with an 8-byte timestamp between the subheader and the message (the
 *    subheader length counts the message only); command replies arrive on 0x11 without one.
 *  - Interface 0 ("comm") reports are events: 0xf0 INIT (interface table and HID descriptor),
 *    0xf1 READY, 0xa0 GPIO request.  Commands go to interface 0 as feature SET_REPORTs
 *    (flags 0x80): 0xb4 enable interface, 0xa1 GPIO acknowledgement.  An acknowledgement
 *    repeats the command's seq, flags and first byte.
 */

/* ---- SCM mailbox messages (RTKit endpoint 0x20) ------------------------------------------- */

#define MTP_SCM_EP           0x20u
#define MTP_SCM_IOP_DOORBELL 0x01u
#define MTP_SCM_AP_DOORBELL  0x02u
#define MTP_SCM_IOP_FIFO     0xa0u
#define MTP_SCM_WAKE_FIFO    0xa1u
#define MTP_SCM_AP_FIFO      0xb0u
#define MTP_SCM_POWER        0xfdu
#define MTP_SCM_IOP_READY    0xfeu
#define MTP_SCM_AHT_READY    0xffu
#define MTP_SCM_POWER_ON     2u

/* id | payload with its low byte cleared (the ring addresses are 4 KiB aligned). */
u64 mtp_scm_msg(u8 id, u64 payload);

/* ---- rings ----------------------------------------------------------------------------- */

#define MTP_RING_BYTES      0x200000u /* TX and RX */
#define MTP_RING_HDR        8u
#define MTP_WAKE_BYTES      0x4000u /* allocated and mapped */
#define MTP_WAKE_USED_BYTES 0x1000u /* what the firmware uses (the ring's capacity + 8) */

struct mtp_ring {
    volatile u32 *idx; /* [0] producer, [1] consumer */
    volatile u8 *data;
    u32 cap; /* data bytes */
    u32 pos; /* RX: the consumer this side last published; TX: the producer */
};

/* `bytes` includes the 8-byte header.  The memory must already be zero (indices 0). */
void mtp_ring_init(struct mtp_ring *r, void *base, u32 bytes);

enum mtp_rx_result {
    MTP_RX_EMPTY = 0,      /* no complete frame */
    MTP_RX_FRAME = 1,      /* a frame of *len bytes is in buf, and consumed */
    MTP_RX_SKIPPED = 2,    /* a well-framed frame larger than buf was consumed unread */
    MTP_RX_BAD_INDEX = -1, /* producer out of range or unaligned, or the consumer moved */
    MTP_RX_BAD_FRAME = -2, /* hdr_len != 8, length not a multiple of 4, or larger than the ring:
                              the framing is lost; nothing is consumed */
};

/* Take the next frame (header to checksum).  Frames are never partially consumed. */
int mtp_ring_rx(struct mtp_ring *r, u8 *buf, u32 buflen, u32 *len);

enum mtp_tx_result {
    MTP_TX_OK = 0,
    MTP_TX_FULL = 1,       /* no room now (the frame plus 4 bytes must fit) */
    MTP_TX_BAD_INDEX = -1, /* the producer is not ours, or the consumer is out of range */
    MTP_TX_BAD_FRAME = -2, /* empty, unaligned or larger than the ring */
};

/* Append one whole frame and publish the producer (the caller rings doorbell 0x02). */
int mtp_ring_tx(struct mtp_ring *r, const u8 *frame, u32 len);

/* ---- frames ---------------------------------------------------------------------------- */

#define MTP_FRAME_HDR  8u
#define MTP_FRAME_SUB  8u
#define MTP_FRAME_TS   8u
#define MTP_FRAME_CSUM 4u
#define MTP_FRAME_MAX  (MTP_FRAME_HDR + 0xfffcu + MTP_FRAME_CSUM) /* the largest frame (as Linux) */

#define MTP_CH_NOTIFY     0x00u
#define MTP_CH_CMD        0x11u
#define MTP_CH_REPORT     0x12u
#define MTP_CH_SCM_REPORT 0x14u

#define MTP_FLAGS_INPUT       0x00u /* report type 0 (input), request 0 */
#define MTP_FLAGS_SET_FEATURE 0x80u /* report type 2 (feature), request 0 (SET_REPORT) */
#define MTP_FLAGS_GET_FEATURE 0x81u /* report type 2 (feature), request 1 (GET_REPORT) */
#define MTP_FLAGS_TYPE(f)     ((u8)((f) >> 6))

/* The little-endian u32 sum of `len` bytes (len a multiple of 4). */
u32 mtp_csum(const u8 *p, u32 len);

struct mtp_msg {
    u8 channel, seq, iface;
    bool notify; /* channel 0 with no payload: a FIFO notification, nothing else */
    u8 flags;
    u32 retcode;
    const u8 *data; /* the message (timestamp stripped on channel 0x14) */
    u16 len;        /* the subheader's message length */
};

enum mtp_parse_result {
    MTP_PARSE_OK = 0,
    MTP_PARSE_SHORT = -1, /* shorter than its header says, or than a header */
    MTP_PARSE_CSUM = -2,
    MTP_PARSE_SUB = -3,   /* no room for the subheader, or its length runs past the frame */
    MTP_PARSE_TS = -4,    /* a channel 0x14 frame shorter than subheader + timestamp */
};

int mtp_frame_parse(const u8 *frame, u32 len, struct mtp_msg *m);

/*
 * A host frame: channel 0x11, the subheader (flags, length n), the message padded to 4
 * bytes, the checksum.  Returns its length, or 0 when it does not fit `outlen` or n is too
 * long for the 16-bit length fields.
 */
u32 mtp_frame_build(u8 *out, u32 outlen, u8 seq, u8 iface, u8 flags, const u8 *msg, u16 n);

/* ---- interface 0 events ------------------------------------------------------------------ */

#define MTP_EV_GPIO  0xa0u
#define MTP_EV_INIT  0xf0u
#define MTP_EV_READY 0xf1u

#define MTP_CMD_GPIO_ACK  0xa1u
#define MTP_CMD_ENABLE    0xb4u
/* I2: {0x95, 2, 0, iface, u64 DVA, u32 size}: the multi-touch firmware, from a DMA buffer. */
#define MTP_CMD_FIRMWARE  0x95u
#define MTP_FIRMWARE_LEN  16u
/* I2: power method 2 {0x40, 2, iface, state, has_changed, 0, 0, 0, 0}: announce, then confirm. */
#define MTP_CMD_POWER     0x40u
#define MTP_POWER_LEN     9u
#define MTP_POWER_METHOD2 2u
#define MTP_POWER_ON      2u
#define MTP_GPIO_REFUSED  0xe000f00du /* the return code a refused GPIO request is acked with */

#define MTP_MAX_IFACES 8u
#define MTP_DESC_MAX   2048u
#define MTP_NAME_LEN   16u
#define MTP_INIT_HDR   22u /* type, 2 bytes, iface, name[16], more_packets, pad */

#define MTP_INIT_BLK_DESC  0u
#define MTP_INIT_BLK_GPIO  1u
#define MTP_INIT_BLK_END   2u
#define MTP_INIT_BLK_NAME  7u

struct mtp_iface_info {
    bool seen;      /* an INIT packet arrived */
    bool last;      /* its last INIT packet (more_packets == 0) arrived */
    bool complete;  /* last, and a HID descriptor came with it or before it */
    bool ready;     /* READY arrived */
    bool desc_truncated;
    bool gpio;      /* it announced a GPIO */
    char name[MTP_NAME_LEN + 1];
    u16 desc_len;
    u8 desc[MTP_DESC_MAX];
};

struct mtp_comm {
    struct mtp_iface_info iface[MTP_MAX_IFACES];
    u32 inits, readies, gpio_requests, bad_events, unknown_blocks;
};

enum mtp_event {
    MTP_EVT_NONE = 0,  /* empty, or an event this side ignores */
    MTP_EVT_INIT = 1,  /* *iface: one INIT packet taken */
    MTP_EVT_READY = 2, /* *iface is ready */
    MTP_EVT_GPIO = 3,  /* a GPIO request (refuse it with mtp_gpio_ack()) */
    MTP_EVT_BAD = -1,  /* malformed or for an interface out of range: counted */
};

void mtp_comm_init(struct mtp_comm *c);
int mtp_comm_event(struct mtp_comm *c, const u8 *p, u16 len, u8 *iface);
/* The interface whose INIT name is `name`, or -1. */
int mtp_comm_find(const struct mtp_comm *c, const char *name);

/* The GPIO acknowledgement message: 0xa1, u32 retcode, the request echoed.  Its length or 0. */
u16 mtp_gpio_ack(u8 *out, u16 outlen, u32 retcode, const u8 *req, u16 req_len);

/* ---- one command in flight per interface -------------------------------------------------- */

#define MTP_CMD_RESP_MAX 32u

struct mtp_cmd {
    bool busy, done;
    u8 iface, seq, flags, first; /* what the acknowledgement must repeat */
    u32 retcode;
    u16 resp_len;
    u8 resp[MTP_CMD_RESP_MAX]; /* the acknowledgement's first bytes (a GET_REPORT's answer) */
};

/*
 * Is `m` (a channel 0x11 frame) the acknowledgement of `c`?  Completes `c` when it is, keeping
 * up to MTP_CMD_RESP_MAX bytes of its message (byte 0 repeats the command or report ID).
 */
bool mtp_cmd_ack(struct mtp_cmd *c, const struct mtp_msg *m);

#endif
