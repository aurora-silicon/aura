/* SPDX-License-Identifier: GPL-2.0-only */

/*
 * I1: the T8152 MTP bring-up and run-time service.  See mtp_bringup.h for the sequence and
 * windows-m6/INPUT-I1I2-BUILD.md for the plan.  Written on this branch; no third-party source.
 */

#include "mtp_bringup.h"

static void zero(void *p, u32 n)
{
    u8 *b = p;
    for (u32 i = 0; i < n; i++)
        b[i] = 0;
}

static u64 now(struct mtp_bringup *b)
{
    return b->ops->now_us(b->ops->ctx);
}

static int fail(struct mtp_bringup *b, int reason)
{
    b->state |= MTP_ST_FAILED;
    b->reason = (u8)reason;
    b->kbd_live = false;
    return reason;
}

static void reached(struct mtp_bringup *b, u8 state)
{
    b->state = state;
    u64 ms = (now(b) - b->t_start) / 1000;
    b->step_ms[state] = ms > 0xffffffffu ? 0xffffffffu : (u32)ms;
}

/* ---- frames ---------------------------------------------------------------------------- */

static void queue_gpio_refusal(struct mtp_bringup *b, const u8 *req, u16 len)
{
    if (b->gpio_pending || len > sizeof(b->gpio_req)) {
        b->gpio_dropped++;
        return;
    }
    for (u32 i = 0; i < len; i++)
        b->gpio_req[i] = req[i];
    b->gpio_len = len;
    b->gpio_pending = true;
}

static void keyboard_report(struct mtp_bringup *b, const struct mtp_msg *m)
{
    if (!b->kbd_live) {
        b->other_frames++;
        return;
    }
    u8 mods, keys[KBD_BITMAP_BYTES];
    u16 consumer;
    int rc = kbd_translate(&b->kbd, &b->kst, b->fn_mode, m->data, m->len, &mods, keys, &consumer);
    if (rc == KBD_T_UPDATE) {
        b->kbd_reports++;
        for (u32 i = 0; i < KBD_BITMAP_BYTES; i++)
            if (keys[i]) {
                tp_note_key(&b->tp, now(b)); /* typing: no taps for a while */
                break;
            }
        b->ops->keyboard(b->ops->ctx, mods, keys, consumer);
    } else if (rc < 0) {
        b->kbd_bad++;
    }
}

static void trackpad_report(struct mtp_bringup *b, const struct mtp_msg *m)
{
    if (!b->tp_live) {
        b->tp_other++;
        return;
    }
    struct tp_report out[TP_MAX_REPORTS];
    int n = tp_frame(&b->tp, m->data, m->len, now(b), out);
    if (n < 0) {
        b->tp_bad++;
        return;
    }
    b->tp_frames++;
    for (int i = 0; i < n; i++) {
        b->tp_reports++;
        b->ops->pointer(b->ops->ctx, &out[i]);
    }
}

static void handle_frame(struct mtp_bringup *b, const u8 *f, u32 len)
{
    struct mtp_msg m;
    int rc = mtp_frame_parse(f, len, &m);
    if (rc == MTP_PARSE_CSUM) {
        b->csum_errors++;
        if (++b->csum_run >= 3)
            b->csum_tripped = true;
        return;
    }
    if (rc) {
        b->parse_errors++;
        return;
    }
    b->csum_run = 0;
    b->frames++;
    if (b->ops->frame)
        b->ops->frame(b->ops->ctx, &m);
    if (m.notify) {
        b->notifications++;
        return;
    }
    if (m.channel == MTP_CH_CMD) {
        if (!mtp_cmd_ack(&b->cmd, &m))
            b->other_frames++;
        return;
    }
    if ((m.channel != MTP_CH_SCM_REPORT && m.channel != MTP_CH_REPORT) ||
        MTP_FLAGS_TYPE(m.flags) != 0) {
        b->other_frames++;
        return;
    }
    if (m.iface == 0) {
        u8 iface = 0;
        if (mtp_comm_event(&b->comm, m.data, m.len, &iface) == MTP_EVT_GPIO)
            queue_gpio_refusal(b, m.data, m.len);
        return;
    }
    if (b->kbd_iface >= 0 && m.iface == (u8)b->kbd_iface) {
        keyboard_report(b, &m);
        return;
    }
    if (b->tp_iface >= 0 && m.iface == (u8)b->tp_iface) {
        trackpad_report(b, &m);
        return;
    }
    b->other_frames++;
}

/* Up to `max` frames from one ring.  < 0 on a ring error (indices or framing). */
static int drain(struct mtp_bringup *b, struct mtp_ring *r, u32 max)
{
    u32 n = 0;
    while (n < max) {
        u32 len;
        int rc = mtp_ring_rx(r, b->frame_buf, sizeof(b->frame_buf), &len);
        if (rc == MTP_RX_EMPTY)
            break;
        if (rc < 0) {
            b->ring_errors++;
            return -1;
        }
        n++;
        if (rc == MTP_RX_SKIPPED) {
            b->oversize++;
            continue;
        }
        handle_frame(b, b->frame_buf, len);
    }
    return (int)n;
}

/* ---- commands on interface 0 ------------------------------------------------------------ */

static bool scm_send_bounded(struct mtp_bringup *b, u64 msg, u32 ms)
{
    u64 deadline = now(b) + (u64)ms * 1000;
    for (;;) {
        if (b->ops->scm_send(b->ops->ctx, msg)) {
            b->scm_sends++;
            return true;
        }
        b->scm_busy++;
        if (now(b) >= deadline)
            return false;
    }
}

/*
 * Put one command on the TX ring and ring the doorbell.  One command in flight: false when
 * one is still waiting, or the ring or the mailbox refused it (`*fatal` then says why).
 * Interface 0 takes SET_REPORT commands; a GET_REPORT goes to the interface it reads.
 * `send_ms` bounds the doorbell: the bring-up waits up to MTP_T_SEND_MS for a full mailbox; the
 * run time (0) tries once and, when the mailbox is full, owes the doorbell to the next poll
 * (ring_owed_doorbell()), so a poll never spins.
 */
static bool send_to(struct mtp_bringup *b, u8 iface, u8 flags, const u8 *msg, u16 n, int *fatal,
                    u32 send_ms)
{
    *fatal = MTP_E_NONE;
    if (b->cmd.busy)
        return false;
    u8 seq = b->seq[iface % MTP_MAX_IFACES];
    u32 len = mtp_frame_build(b->tx_buf, sizeof(b->tx_buf), seq, iface, flags, msg, n);
    int rc = len ? mtp_ring_tx(&b->tx, b->tx_buf, len) : MTP_TX_BAD_FRAME;
    if (rc == MTP_TX_FULL)
        return false;
    if (rc) {
        *fatal = MTP_E_TX;
        return false;
    }
    b->cmd = (struct mtp_cmd){.busy = true, .iface = iface, .seq = seq, .flags = flags,
                              .first = msg[0]};
    b->seq[iface % MTP_MAX_IFACES]++;
    b->cmd_sent_us = now(b);
    if (!scm_send_bounded(b, MTP_SCM_AP_DOORBELL, send_ms)) {
        if (send_ms) {
            *fatal = MTP_E_SCM_SEND;
            return false;
        }
        b->doorbell_owed = true;
        b->doorbell_since = b->cmd_sent_us;
        b->doorbells_owed++;
    }
    return true;
}

/* The run time's doorbell that found the mailbox full: one try per call; MTP_E_SCM_SEND once
 * it has been owed for MTP_T_SEND_MS. */
static int ring_owed_doorbell(struct mtp_bringup *b)
{
    if (!b->doorbell_owed)
        return MTP_E_NONE;
    if (b->ops->scm_send(b->ops->ctx, MTP_SCM_AP_DOORBELL)) {
        b->scm_sends++;
        b->doorbell_owed = false;
        return MTP_E_NONE;
    }
    b->scm_busy++;
    return now(b) - b->doorbell_since > (u64)MTP_T_SEND_MS * 1000 ? MTP_E_SCM_SEND : MTP_E_NONE;
}

/* A command whose acknowledgement never came is given up (counted), so others can follow. */
static void cmd_expire(struct mtp_bringup *b)
{
    if (b->cmd.busy && now(b) - b->cmd_sent_us > (u64)MTP_T_CMD_MS * 1000) {
        b->cmd.busy = false;
        b->cmd_timeouts++;
    }
}

/*
 * A pending GPIO refusal takes the command slot only when it is free and no command() is
 * still reading its own acknowledgement (cmd_waiting): an acknowledgement and a GPIO request
 * drained in the same pump must not replace the command before command() has seen its result.
 */
static int send_pending(struct mtp_bringup *b, u32 send_ms)
{
    if (!b->gpio_pending || b->cmd.busy || b->cmd_waiting)
        return MTP_E_NONE;
    u8 ack[32];
    u16 n = mtp_gpio_ack(ack, sizeof(ack), MTP_GPIO_REFUSED, b->gpio_req, b->gpio_len);
    int fatal;
    if (send_to(b, 0, MTP_FLAGS_SET_FEATURE, ack, n, &fatal, send_ms)) {
        b->gpio_pending = false;
        b->gpio_refused++;
    }
    return fatal;
}

/* One round of servicing during the bring-up: RTKit, both rings, a pending GPIO refusal. */
static int pump(struct mtp_bringup *b)
{
    if (b->ops->rtkit_service(b->ops->ctx, 4) < 0)
        return MTP_E_RTKIT;
    if (drain(b, &b->rx, 32) < 0 || drain(b, &b->wake, 32) < 0)
        return MTP_E_RING;
    cmd_expire(b);
    int rc = ring_owed_doorbell(b);
    if (rc)
        return rc;
    return send_pending(b, MTP_T_SEND_MS);
}

/* ---- the bring-up ----------------------------------------------------------------------- */

static int check_dart(struct mtp_bringup *b)
{
    b->ttbr = b->ops->read32(b->ops->ctx, MTP_T8152_DART_TTBR);
    b->tcr = b->ops->read32(b->ops->ctx, MTP_T8152_DART_TCR);
    if (!(b->ttbr & MTP_T8152_TTBR_VALID) ||
        (b->ttbr & ~(MTP_T8152_TTBR_ADDR | MTP_T8152_TTBR_VALID)))
        return MTP_E_TTBR;
    if (b->tcr != MTP_T8152_TCR)
        return MTP_E_TCR;
    b->root = (u64)(b->ttbr & MTP_T8152_TTBR_ADDR) << 12;
    if (!b->ops->root_ok(b->ops->ctx, b->root))
        return MTP_E_ROOT;
    return MTP_E_NONE;
}

static int map_rings(struct mtp_bringup *b)
{
    u64 tables[MTP_DART_MAX_TABLES];
    for (u32 i = 0; i < MTP_DART_MAX_TABLES; i++)
        tables[i] = b->area_pa + MTP_AREA_TABLES + i * MTP_DART_PAGE;
    mtp_dart_init(&b->dart, &b->ops->dart_mem, b->root, MTP_T8152_DVA_BASE,
                  MTP_T8152_DVA_BASE + MTP_T8152_DVA_WINDOW, tables, MTP_DART_MAX_TABLES);
    const struct {
        u32 off, size;
        u64 *dva;
    } maps[] = {{MTP_AREA_TX, MTP_RING_BYTES, &b->tx_dva},
                {MTP_AREA_RX, MTP_RING_BYTES, &b->rx_dva},
                {MTP_AREA_WAKE, MTP_WAKE_BYTES, &b->wake_dva},
                {MTP_AREA_POOL, MTP_AREA_POOL_SIZE, &b->pool_dva},
                {MTP_AREA_FW, MTP_AREA_FW_SIZE, &b->fw_dva}};
    /* The firmware buffer (I2) after the pool, only when the trackpad is wanted. */
    u32 count = b->want_tp ? 5 : 4;
    for (u32 i = 0; i < count; i++)
        if (mtp_dart_map(&b->dart, b->area_pa + maps[i].off, maps[i].size, maps[i].dva))
            return MTP_E_MAP;
    b->ops->dart_sync(b->ops->ctx);
    /* Every page, through this side's own walk (never a DART register). */
    for (u32 i = 0; i < count; i++)
        for (u64 off = 0; off < maps[i].size; off += MTP_DART_PAGE) {
            u64 pa;
            if (!mtp_dart_lookup(&b->dart, *maps[i].dva + off, &pa) ||
                pa != b->area_pa + maps[i].off + off)
                return MTP_E_MAP;
        }
    return MTP_E_NONE;
}

static int power_on(struct mtp_bringup *b)
{
    for (u32 i = 0; i < MTP_T8152_PMGR_COUNT; i++) {
        u64 reg = MTP_T8152_PMGR_PS + (u64)i * MTP_T8152_PMGR_STRIDE;
        u32 v = b->ops->read32(b->ops->ctx, reg);
        if ((v & MTP_PS_TARGET) != MTP_PS_ON) {
            b->ops->write32(b->ops->ctx, reg, (v & MTP_PS_KEEP) | MTP_PS_ON);
            b->pmgr_writes++;
        }
        u64 deadline = now(b) + (u64)MTP_T_PMGR_MS * 1000;
        while (MTP_PS_ACTUAL(b->ops->read32(b->ops->ctx, reg)) != MTP_PS_ON)
            if (now(b) >= deadline)
                return MTP_E_POWER;
    }
    return MTP_E_NONE;
}

/*
 * The RUN rule, with ASC_CPU_CONTROL read only while the MTP's domains are on (an unpowered
 * ASC block faults or reads all ones).  The 13 states are read first (PMGR, always powered).
 * All on: RUN is read before anything is written, and a running MTP is refused untouched.
 * Any off: the MTP cannot be running (it needs every one of them), so they are switched on
 * parent first (the only writes) and RUN is read after; set then, it is still refused.
 */
static int power_and_run_rule(struct mtp_bringup *b)
{
    bool all_on = true;
    for (u32 i = 0; i < MTP_T8152_PMGR_COUNT; i++) {
        u64 reg = MTP_T8152_PMGR_PS + (u64)i * MTP_T8152_PMGR_STRIDE;
        b->ps_before[i] = b->ops->read32(b->ops->ctx, reg);
        if (MTP_PS_ACTUAL(b->ps_before[i]) != MTP_PS_ON)
            all_on = false;
    }
    b->run_read_powered = all_on;
    if (all_on) {
        b->asc_ctrl = b->ops->read32(b->ops->ctx, MTP_T8152_ASC_CPU_CTRL);
        if (b->asc_ctrl & MTP_T8152_RUN)
            return MTP_E_RUNNING;
    }
    int rc = power_on(b);
    if (rc)
        return rc;
    if (!all_on) {
        b->asc_ctrl = b->ops->read32(b->ops->ctx, MTP_T8152_ASC_CPU_CTRL);
        if (b->asc_ctrl & MTP_T8152_RUN)
            return MTP_E_RUNNING;
    }
    return MTP_E_NONE;
}

static int start_scm(struct mtp_bringup *b)
{
    mtp_ring_init(&b->tx, b->area + MTP_AREA_TX, MTP_RING_BYTES);
    mtp_ring_init(&b->rx, b->area + MTP_AREA_RX, MTP_RING_BYTES);
    mtp_ring_init(&b->wake, b->area + MTP_AREA_WAKE, MTP_WAKE_USED_BYTES);
    if (!scm_send_bounded(b, mtp_scm_msg(MTP_SCM_POWER, (u64)MTP_SCM_POWER_ON << 8), MTP_T_SEND_MS))
        return MTP_E_SCM_SEND;
    u64 deadline = now(b) + (u64)MTP_T_SCM_MS * 1000;
    while (!b->iop_ready) {
        if (b->ops->rtkit_service(b->ops->ctx, 4) < 0)
            return MTP_E_RTKIT;
        if (now(b) >= deadline)
            return MTP_E_SCM_TIMEOUT;
    }
    const u64 msgs[4] = {mtp_scm_msg(MTP_SCM_AP_FIFO, b->tx_dva),
                         mtp_scm_msg(MTP_SCM_IOP_FIFO, b->rx_dva),
                         mtp_scm_msg(MTP_SCM_WAKE_FIFO, b->wake_dva),
                         mtp_scm_msg(MTP_SCM_AHT_READY, 0)};
    for (u32 i = 0; i < 4; i++)
        if (!scm_send_bounded(b, msgs[i], MTP_T_SEND_MS))
            return MTP_E_SCM_SEND;
    return MTP_E_NONE;
}


/* INIT for the keyboard (and, I2, multi-touch).  A missing multi-touch INIT costs only the
 * trackpad (tp_iface stays -1). */
static int wait_init(struct mtp_bringup *b)
{
    u64 deadline = now(b) + (u64)MTP_T_INIT_MS * 1000;
    for (;;) {
        int rc = pump(b);
        if (rc)
            return rc;
        int k = mtp_comm_find(&b->comm, "keyboard");
        int t = b->want_tp ? mtp_comm_find(&b->comm, "multi-touch") : -1;
        /* complete: the last packet came and a descriptor with it or before it (mtp_scm.c,
         * as Linux waits on ring transports): an interface whose last packet had none is
         * waited for until the deadline, then refused as a layout failure. */
        bool kd = k >= 0 && b->comm.iface[k].complete;
        bool td = t >= 0 && b->comm.iface[t].complete;
        if (kd && (td || !b->want_tp)) {
            b->kbd_iface = k;
            b->tp_iface = td ? t : -1;
            return MTP_E_NONE;
        }
        if (now(b) >= deadline) {
            if (k < 0)
                return MTP_E_INIT_TIMEOUT;
            b->kbd_iface = k; /* seen: incomplete or without a descriptor is MTP_E_LAYOUT */
            b->tp_iface = td ? t : -1;
            return MTP_E_NONE;
        }
    }
}

/*
 * One command and its acknowledgement within `ms`: MTP_E_NONE when it was acknowledged with
 * return code 0; `e_ack` without an acknowledgement, `e_ret` with another return code; the
 * transport's own reason when the ring, the mailbox or RTKit failed.
 */
static int command(struct mtp_bringup *b, u8 iface, u8 flags, const u8 *msg, u16 n, u32 ms,
                   int e_ack, int e_ret)
{
    u64 deadline = now(b) + (u64)ms * 1000;
    int fatal;
    while (!send_to(b, iface, flags, msg, n, &fatal, MTP_T_SEND_MS)) {
        if (fatal)
            return fatal;
        int rc = pump(b); /* a GPIO refusal may hold the command slot */
        if (rc)
            return rc;
        if (now(b) >= deadline)
            return e_ack;
    }
    b->cmd_waiting = true; /* no GPIO refusal takes the slot until the result is read */
    int rc = MTP_E_NONE;
    while (b->cmd.busy) {
        rc = pump(b);
        if (rc || now(b) >= deadline)
            break;
    }
    b->cmd_waiting = false;
    if (rc)
        return rc;
    if (b->cmd.busy || !b->cmd.done || b->cmd.first != msg[0] || b->cmd.iface != iface)
        return e_ack;
    return b->cmd.retcode ? e_ret : MTP_E_NONE;
}

static int wait_ready(struct mtp_bringup *b, int iface, u32 ms, int e)
{
    u64 deadline = now(b) + (u64)ms * 1000;
    while (!b->comm.iface[iface].ready) {
        int rc = pump(b);
        if (rc)
            return rc;
        if (now(b) >= deadline)
            return e;
    }
    return MTP_E_NONE;
}

/* Keep servicing for `ms` (the AFE settle times). */
static int settle(struct mtp_bringup *b, u32 ms)
{
    u64 deadline = now(b) + (u64)ms * 1000;
    while (now(b) < deadline) {
        int rc = pump(b);
        if (rc)
            return rc;
    }
    return MTP_E_NONE;
}

static int enable_keyboard(struct mtp_bringup *b)
{
    u8 msg[2] = {MTP_CMD_ENABLE, (u8)b->kbd_iface};
    int rc = command(b, 0, MTP_FLAGS_SET_FEATURE, msg, sizeof(msg), MTP_T_CMD_MS, MTP_E_KBD_ACK,
                     MTP_E_KBD_ENABLE);
    if (rc)
        return rc;
    /* The keyboard is live from the acknowledgement: reports may come before READY. */
    b->kbd_live = true;
    return wait_ready(b, b->kbd_iface, MTP_T_READY_MS, MTP_E_KBD_READY);
}

/* ---- I2: the parameter page and the trackpad ---------------------------------------------- */

static u32 rd32(const u8 *p)
{
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static u16 rd16(const u8 *p)
{
    return (u16)(p[0] | p[1] << 8);
}

u32 mtp_crc32(u32 crc, const u8 *p, u32 len)
{
    crc = ~crc;
    for (u32 i = 0; i < len; i++) {
        crc ^= p[i];
        for (u32 k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

int mtp_params_parse(const u8 *pg, struct mtp_params *o)
{
    for (u32 i = 0; i < 8; i++)
        if (pg[i] != (u8)MTP_PARAMS_MAGIC[i])
            return MTP_E_TP_PARAMS;
    if (rd32(pg + 8) != MTP_PARAMS_VERSION || rd32(pg + 12) != MTP_PARAMS_LEN ||
        mtp_crc32(0, pg, 60) != rd32(pg + 60) || rd32(pg + 16) != MTP_AREA_FW ||
        rd32(pg + 56))
        return MTP_E_TP_PARAMS;
    u32 size = rd32(pg + 20), iface_off = rd32(pg + 28), flags = rd32(pg + 32);
    if (!size || size > MTP_AREA_FW_SIZE || iface_off >= size ||
        (flags & ~(u32)(MTP_PARAMS_TAP | MTP_PARAMS_NATURAL)))
        return MTP_E_TP_PARAMS;
    struct tp_config c;
    tp_config_default(&c);
    c.gain_q8 = rd16(pg + 36);
    c.accel_q8 = rd16(pg + 38);
    c.accel_lo = rd16(pg + 40);
    c.accel_hi = rd16(pg + 42);
    c.scroll_um = rd16(pg + 44);
    c.tap_ms = rd16(pg + 46);
    c.tap_travel_um = rd16(pg + 48);
    c.tap_key_quiet_ms = rd16(pg + 50);
    c.click_settle_ms = rd16(pg + 52);
    c.palm_major = rd16(pg + 54);
    c.tap = flags & MTP_PARAMS_TAP;
    c.natural = flags & MTP_PARAMS_NATURAL;
    if (c.gain_q8 < 16 || (c.accel_q8 && c.accel_hi <= c.accel_lo) || c.scroll_um < 500 ||
        c.tap_ms > 1000 || c.tap_key_quiet_ms > 5000 || c.click_settle_ms > 500)
        return MTP_E_TP_PARAMS;
    o->fw_size = size;
    o->fw_crc = rd32(pg + 24);
    o->fw_iface_off = iface_off;
    o->tp = c;
    return MTP_E_NONE;
}

static bool tp_reason_code(int rc)
{
    return rc >= MTP_E_TP_PARAMS && rc <= MTP_E_TP_IDENTITY;
}

#define GEOMETRY_MISSING (-1)

static int bring_up_trackpad(struct mtp_bringup *b)
{
    if (b->tp_iface < 0)
        return MTP_E_TP_INIT;
    u8 i = (u8)b->tp_iface;

    b->tp_step = 1;
    u8 en[2] = {MTP_CMD_ENABLE, i};
    int rc = command(b, 0, MTP_FLAGS_SET_FEATURE, en, sizeof(en), MTP_T_CMD_MS, MTP_E_TP_ENABLE,
                     MTP_E_TP_ENABLE);
    if (rc)
        return rc;

    /* The firmware takes its interface number at iface_offset, then goes to the PoC. */
    if (b->params.fw_iface_off)
        b->area[MTP_AREA_FW + b->params.fw_iface_off] = i;
    b->ops->dma_sync(b->ops->ctx, b->area_pa + MTP_AREA_FW, b->params.fw_size);

    b->tp_step = 2;
    b->afe_writes++;
    if (!b->ops->afe_reset(b->ops->ctx, true)) {
        b->afe_failed++;
        return MTP_E_TP_SMC;
    }
    rc = settle(b, MTP_T_AFE_MS);
    if (rc)
        return rc;

    b->tp_step = 3;
    u8 fw[MTP_FIRMWARE_LEN] = {MTP_CMD_FIRMWARE, 2, 0, i};
    for (u32 k = 0; k < 8; k++)
        fw[4 + k] = (u8)(b->fw_dva >> (8 * k));
    for (u32 k = 0; k < 4; k++)
        fw[12 + k] = (u8)(b->params.fw_size >> (8 * k));
    rc = command(b, 0, MTP_FLAGS_SET_FEATURE, fw, sizeof(fw), MTP_T_CMD_MS, MTP_E_TP_FW,
                 MTP_E_TP_FW);
    if (rc)
        return rc;

    /* Power method 2, on: announce, release the AFE reset, settle, confirm. */
    b->tp_step = 4;
    u8 pw[MTP_POWER_LEN] = {MTP_CMD_POWER, MTP_POWER_METHOD2, i, MTP_POWER_ON, 0};
    rc = command(b, 0, MTP_FLAGS_SET_FEATURE, pw, sizeof(pw), MTP_T_CMD_MS, MTP_E_TP_POWER,
                 MTP_E_TP_POWER);
    if (rc)
        return rc;
    b->tp_step = 5;
    b->afe_writes++;
    if (!b->ops->afe_reset(b->ops->ctx, false)) {
        b->afe_failed++;
        return MTP_E_TP_SMC;
    }
    rc = settle(b, MTP_T_AFE_MS);
    if (rc)
        return rc;
    b->tp_step = 6;
    pw[4] = 1;
    rc = command(b, 0, MTP_FLAGS_SET_FEATURE, pw, sizeof(pw), MTP_T_CMD_MS, MTP_E_TP_POWER,
                 MTP_E_TP_POWER);
    if (rc)
        return rc;

    /* Live from the confirmation: frames may come before READY. */
    b->tp_live = true;
    b->tp_step = 7;
    rc = wait_ready(b, i, MTP_T_READY_MS, MTP_E_TP_READY);
    if (rc)
        return rc;

    /* Optional: the device's geometry (0xD9); without it the defaults stay. */
    b->tp_step = 8;
    u8 q = TP_FEATURE_GEOMETRY;
    rc = command(b, i, MTP_FLAGS_GET_FEATURE, &q, 1, MTP_T_GEOM_MS, GEOMETRY_MISSING,
                 GEOMETRY_MISSING);
    if (rc && rc != GEOMETRY_MISSING)
        return rc;
    struct tp_geometry g;
    u32 n = b->cmd.resp_len < MTP_CMD_RESP_MAX ? b->cmd.resp_len : MTP_CMD_RESP_MAX;
    if (!rc && tp_parse_geometry(b->cmd.resp, n, &g) && tp_set_geometry(&b->tp, &g))
        b->tp_geometry_device = true;
    return MTP_E_NONE;
}

static int latch(struct mtp_bringup *b, int reason);

static void tp_down(struct mtp_bringup *b, int reason)
{
    struct tp_report r;
    bool was = b->tp_live;
    b->tp_live = false;
    b->tp_reason = (u8)reason;
    tp_release(&b->tp, &r);
    if (was)
        b->ops->pointer(b->ops->ctx, &r);
}

int mtp_bringup_attach(struct mtp_bringup *b, const struct mtp_ops *ops, u64 area_pa,
                       void *area, enum kbd_fn_mode fn_mode, bool want_tp)
{
    zero(b, sizeof(*b));
    b->ops = ops;
    b->area_pa = area_pa;
    b->area = area;
    b->fn_mode = fn_mode;
    b->kbd_iface = -1;
    b->tp_iface = -1;
    b->t_start = now(b);
    mtp_comm_init(&b->comm);
    kbd_state_init(&b->kst);
    struct tp_config defaults;
    tp_config_default(&defaults);
    tp_init(&b->tp, &defaults);
    reached(b, MTP_ST_IDENTITY); /* the glue checked the ADT, chip and board */

    /* Power (only where a domain is off), with the RUN rule: a running MTP is never touched. */
    int rc = power_and_run_rule(b);
    if (rc)
        return fail(b, rc);
    reached(b, MTP_ST_POWER);

    /* I2: the launcher's page and the firmware's CRC, before anything is mapped. */
    if (want_tp) {
        const u8 *a = area;
        if (mtp_params_parse(a + MTP_AREA_PARAMS, &b->params) ||
            mtp_crc32(0, a + MTP_AREA_FW, b->params.fw_size) != b->params.fw_crc) {
            b->tp_reason = MTP_E_TP_PARAMS;
        } else {
            b->want_tp = true;
            tp_init(&b->tp, &b->params.tp);
        }
    }

    rc = check_dart(b);
    if (!rc)
        rc = map_rings(b);
    if (rc)
        return fail(b, rc);
    reached(b, MTP_ST_DART);

    if (!ops->rtkit_start(ops->ctx, b->pool_dva, MTP_T_RTKIT_MS))
        return fail(b, MTP_E_RTKIT);
    reached(b, MTP_ST_RTKIT);

    rc = start_scm(b);
    if (rc)
        return fail(b, rc);
    reached(b, MTP_ST_SCM);

    rc = wait_init(b);
    if (rc)
        return fail(b, rc);
    reached(b, MTP_ST_INIT);
    const struct mtp_iface_info *k = &b->comm.iface[b->kbd_iface];
    if (!k->complete || !k->desc_len || k->desc_truncated ||
        kbd_layout_from_desc(&b->kbd, k->desc, k->desc_len) != KBD_OK)
        return fail(b, MTP_E_LAYOUT);

    rc = enable_keyboard(b);
    if (rc)
        return fail(b, rc);
    reached(b, MTP_ST_KBD_READY);

    if (b->want_tp) {
        rc = bring_up_trackpad(b);
        if (tp_reason_code(rc)) {
            tp_down(b, rc); /* the keyboard stays */
        } else if (rc) {
            latch(b, rc); /* the transport failed: everything stops, all up */
            b->tp_reason = (u8)rc;
            return rc;
        } else {
            reached(b, MTP_ST_TP_READY);
        }
    }
    return MTP_E_NONE;
}

void mtp_bringup_app_msg(struct mtp_bringup *b, u8 ep, u64 msg)
{
    if (ep != MTP_SCM_EP) {
        b->app_other++;
        return;
    }
    switch (msg & 0xff) {
        case MTP_SCM_IOP_READY:
            b->iop_ready = true;
            break;
        case MTP_SCM_IOP_DOORBELL:
            b->doorbells++;
            break;
        default:
            b->app_other++;
            break;
    }
}

/* Latch a run-time failure: the keyboard and the pointer all up, once; nothing is serviced
 * after it. */
static int latch(struct mtp_bringup *b, int reason)
{
    static const u8 none[KBD_BITMAP_BYTES];
    bool was = b->kbd_live;
    fail(b, reason);
    kbd_release_all(&b->kst);
    if (was)
        b->ops->keyboard(b->ops->ctx, 0, none, 0);
    if (b->tp_live)
        tp_down(b, MTP_E_NONE); /* the trackpad itself did not fail: the reason says why */
    return -1;
}

int mtp_bringup_poll(struct mtp_bringup *b, u32 max_frames)
{
    if (!b->ops || !b->kbd_live || (b->state & MTP_ST_FAILED))
        return -1;
    b->polls++;
    if (b->ops->rtkit_service(b->ops->ctx, 4) < 0)
        return latch(b, MTP_E_RTKIT_FAILED);
    int n = drain(b, &b->rx, max_frames);
    if (n < 0)
        return latch(b, MTP_E_RING);
    int w = drain(b, &b->wake, max_frames - (u32)n);
    if (w < 0)
        return latch(b, MTP_E_RING);
    if (b->csum_tripped)
        return latch(b, MTP_E_CSUM);
    cmd_expire(b);
    int rc = ring_owed_doorbell(b);
    if (!rc)
        rc = send_pending(b, 0); /* one try: a full mailbox owes the doorbell to the next poll */
    if (rc)
        return latch(b, rc);
    if (b->tp_live) {
        struct tp_report r[TP_MAX_REPORTS];
        int k = tp_idle(&b->tp, now(b), r); /* silence: drop held contacts and the button */
        for (int i = 0; i < k; i++) {
            b->tp_reports++;
            b->ops->pointer(b->ops->ctx, &r[i]);
        }
    }
    return n + w;
}
