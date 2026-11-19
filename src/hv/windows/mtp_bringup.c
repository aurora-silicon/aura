/* SPDX-License-Identifier: MIT */

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
        b->ops->keyboard(b->ops->ctx, mods, keys, consumer);
    } else if (rc < 0) {
        b->kbd_bad++;
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
 */
static bool send_cmd(struct mtp_bringup *b, const u8 *msg, u16 n, int *fatal)
{
    *fatal = MTP_E_NONE;
    if (b->cmd.busy)
        return false;
    u32 len = mtp_frame_build(b->tx_buf, sizeof(b->tx_buf), b->comm_seq, 0, MTP_FLAGS_SET_FEATURE,
                              msg, n);
    int rc = len ? mtp_ring_tx(&b->tx, b->tx_buf, len) : MTP_TX_BAD_FRAME;
    if (rc == MTP_TX_FULL)
        return false;
    if (rc) {
        *fatal = MTP_E_TX;
        return false;
    }
    b->cmd = (struct mtp_cmd){.busy = true,
                              .iface = 0,
                              .seq = b->comm_seq,
                              .flags = MTP_FLAGS_SET_FEATURE,
                              .first = msg[0]};
    b->comm_seq++;
    b->cmd_sent_us = now(b);
    if (!scm_send_bounded(b, MTP_SCM_AP_DOORBELL, MTP_T_SEND_MS)) {
        *fatal = MTP_E_SCM_SEND;
        return false;
    }
    return true;
}

/* A command whose acknowledgement never came is given up (counted), so others can follow. */
static void cmd_expire(struct mtp_bringup *b)
{
    if (b->cmd.busy && now(b) - b->cmd_sent_us > (u64)MTP_T_CMD_MS * 1000) {
        b->cmd.busy = false;
        b->cmd_timeouts++;
    }
}

static int send_pending(struct mtp_bringup *b)
{
    if (!b->gpio_pending || b->cmd.busy)
        return MTP_E_NONE;
    u8 ack[32];
    u16 n = mtp_gpio_ack(ack, sizeof(ack), MTP_GPIO_REFUSED, b->gpio_req, b->gpio_len);
    int fatal;
    if (send_cmd(b, ack, n, &fatal)) {
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
    return send_pending(b);
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
                {MTP_AREA_POOL, MTP_AREA_POOL_SIZE, &b->pool_dva}};
    for (u32 i = 0; i < 4; i++)
        if (mtp_dart_map(&b->dart, b->area_pa + maps[i].off, maps[i].size, maps[i].dva))
            return MTP_E_MAP;
    b->ops->dart_sync(b->ops->ctx);
    /* Every page, through this side's own walk (never a DART register). */
    for (u32 i = 0; i < 4; i++)
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
        b->ps_before[i] = v;
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

static int wait_init(struct mtp_bringup *b)
{
    u64 deadline = now(b) + (u64)MTP_T_INIT_MS * 1000;
    for (;;) {
        int rc = pump(b);
        if (rc)
            return rc;
        int k = mtp_comm_find(&b->comm, "keyboard");
        if (k >= 0 && b->comm.iface[k].complete) {
            b->kbd_iface = k;
            return MTP_E_NONE;
        }
        if (now(b) >= deadline)
            return MTP_E_INIT_TIMEOUT;
    }
}

static int enable_keyboard(struct mtp_bringup *b)
{
    u8 msg[2] = {MTP_CMD_ENABLE, (u8)b->kbd_iface};
    u64 deadline = now(b) + (u64)MTP_T_CMD_MS * 1000;
    int fatal;
    while (!send_cmd(b, msg, sizeof(msg), &fatal)) {
        if (fatal)
            return fatal;
        int rc = pump(b); /* a GPIO refusal may hold the command slot */
        if (rc)
            return rc;
        if (now(b) >= deadline)
            return MTP_E_KBD_ACK;
    }
    while (b->cmd.busy) {
        int rc = pump(b);
        if (rc)
            return rc;
        if (now(b) >= deadline || !b->cmd.busy)
            break;
    }
    if (!b->cmd.done || b->cmd.first != MTP_CMD_ENABLE)
        return MTP_E_KBD_ACK;
    if (b->cmd.retcode)
        return MTP_E_KBD_ENABLE;
    /* The keyboard is live from the acknowledgement: reports may come before READY. */
    b->kbd_live = true;
    deadline = now(b) + (u64)MTP_T_READY_MS * 1000;
    while (!b->comm.iface[b->kbd_iface].ready) {
        int rc = pump(b);
        if (rc)
            return rc;
        if (now(b) >= deadline)
            return MTP_E_KBD_READY;
    }
    return MTP_E_NONE;
}

int mtp_bringup_attach(struct mtp_bringup *b, const struct mtp_ops *ops, u64 area_pa,
                       void *area, enum kbd_fn_mode fn_mode)
{
    zero(b, sizeof(*b));
    b->ops = ops;
    b->area_pa = area_pa;
    b->area = area;
    b->fn_mode = fn_mode;
    b->kbd_iface = -1;
    b->t_start = now(b);
    mtp_comm_init(&b->comm);
    kbd_state_init(&b->kst);
    reached(b, MTP_ST_IDENTITY); /* the glue checked the ADT, chip and board */

    /* The RUN rule: read only; a running MTP is never touched. */
    b->asc_ctrl = ops->read32(ops->ctx, MTP_T8152_ASC_CPU_CTRL);
    if (b->asc_ctrl & MTP_T8152_RUN)
        return fail(b, MTP_E_RUNNING);

    int rc = check_dart(b);
    if (rc)
        return fail(b, rc);
    reached(b, MTP_ST_DART);
    rc = map_rings(b);
    if (!rc)
        rc = power_on(b);
    if (rc)
        return fail(b, rc);
    reached(b, MTP_ST_POWER);

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
    if (!k->desc_len || k->desc_truncated ||
        kbd_layout_from_desc(&b->kbd, k->desc, k->desc_len) != KBD_OK)
        return fail(b, MTP_E_LAYOUT);

    rc = enable_keyboard(b);
    if (rc)
        return fail(b, rc);
    reached(b, MTP_ST_KBD_READY);
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

/* Latch a run-time failure: the keyboard all up, once; nothing is serviced after it. */
static int latch(struct mtp_bringup *b, int reason)
{
    static const u8 none[KBD_BITMAP_BYTES];
    bool was = b->kbd_live;
    fail(b, reason);
    kbd_release_all(&b->kst);
    if (was)
        b->ops->keyboard(b->ops->ctx, 0, none, 0);
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
    int rc = send_pending(b);
    if (rc)
        return latch(b, rc);
    return n + w;
}
