/* SPDX-License-Identifier: MIT */

/*
 * I0 input device model.  See input_model.h for the contract and windows-m6/INPUT-DESIGN.md
 * for the reasoning.  Written on this branch from the HID 1.11 specification, the HID Usage
 * Tables and the design's register and wire tables; no third-party source.
 */

#include "input_model.h"

#define MEMSET(dst, c, n)                                                                          \
    do {                                                                                           \
        u8 *_d = (u8 *)(dst);                                                                      \
        for (u32 _i = 0; _i < (u32)(n); _i++)                                                      \
            _d[_i] = (u8)(c);                                                                      \
    } while (0)

static void copy(void *dst, const void *src, u32 n)
{
    u8 *d = dst;
    const u8 *s = src;
    for (u32 i = 0; i < n; i++)
        d[i] = s[i];
}

static bool same(const void *a, const void *b, u32 n)
{
    const u8 *x = a, *y = b;
    for (u32 i = 0; i < n; i++)
        if (x[i] != y[i])
            return false;
    return true;
}

/* ---- CRC-32 (zlib) ----------------------------------------------------------------------- */

u32 input_model_crc32(u32 crc, const void *data, u32 len)
{
    /* Reflected polynomial 0xEDB88320, four bits at a time. */
    static const u32 nibble[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4,
        0x4db26158, 0x5005713c, 0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
        0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
    };
    const u8 *p = data;
    crc = ~crc;
    for (u32 i = 0; i < len; i++) {
        crc ^= p[i];
        crc = (crc >> 4) ^ nibble[crc & 15];
        crc = (crc >> 4) ^ nibble[crc & 15];
    }
    return ~crc;
}

/* ---- the queue ----------------------------------------------------------------------------- */

_Static_assert(INPUT_QUEUE_LEN <= 256, "slot numbers are u8");

static u32 q_pos(const struct input_model *m, u32 i)
{
    return (m->q_head + i) % INPUT_QUEUE_LEN;
}

static struct input_report *q_at(struct input_model *m, u32 i)
{
    return &m->q[m->q_order[q_pos(m, i)]];
}

static void q_flush(struct input_model *m)
{
    m->q_head = 0;
    m->q_count = 0;
}

static bool is_pointer(u8 id)
{
    return id == INPUT_RID_ABSOLUTE || id == INPUT_RID_RELATIVE;
}

/*
 * The entry to drop when the queue is full and `incoming` must go in.  Never the head: the
 * driver may be part way through reading it.  Only an entry whose collection has a later state
 * (a later queued entry with the same report ID, or the incoming report) is dropped, so every
 * collection's last state survives; pointer reports go first, oldest first.  257 reports of at
 * most four IDs always contain such an entry among the 255 that are not the head.
 *
 * One pass from the newest entry back, remembering which IDs were seen later: O(n), so a full
 * queue under a stream of host frames costs a bounded few hundred steps per report.
 */
static int q_victim(struct input_model *m, u8 incoming)
{
    int pointer = -1, any = -1;
    u32 seen = BIT(incoming & 31);
    for (u32 i = m->q_count; i-- > 1;) {
        u8 id = q_at(m, i)->id;
        u32 bit = BIT(id & 31);
        if (seen & bit) {
            if (is_pointer(id))
                pointer = (int)i;
            any = (int)i;
        }
        seen |= bit;
    }
    return pointer >= 0 ? pointer : any;
}

/* Drop entry i (not the head): its slot moves to the free end of the ring, the reports stay. */
static void q_remove(struct input_model *m, u32 i)
{
    u8 slot = m->q_order[q_pos(m, i)];
    for (u32 k = i; k + 1 < m->q_count; k++)
        m->q_order[q_pos(m, k)] = m->q_order[q_pos(m, k + 1)];
    m->q_order[q_pos(m, m->q_count - 1)] = slot;
    m->q_count--;
}

static void q_push(struct input_model *m, const u8 *data, u8 len)
{
    if (!(m->ctrl & INPUT_CTRL_ENABLE))
        return;
    if (m->q_count == INPUT_QUEUE_LEN) {
        int victim = q_victim(m, data[0]);
        m->overflow = true;
        m->dropped++;
        m->stats[INPUT_ST_DROPPED]++;
        if (victim < 0)
            return; /* unreachable (see q_victim); drop the incoming report */
        q_remove(m, (u32)victim);
    }
    struct input_report *r = q_at(m, m->q_count);
    MEMSET(r, 0, sizeof(*r));
    r->seq = ++m->next_seq;
    r->id = data[0];
    r->len = len;
    copy(r->data, data, len);
    m->q_count++;
    m->stats[INPUT_ST_REPORTS]++;
    if (m->q_count > m->stats[INPUT_ST_HIGH_WATER])
        m->stats[INPUT_ST_HIGH_WATER] = m->q_count;
}

/* ---- merged state and reports ------------------------------------------------------------ */

static void build_keyboard(const struct input_model *m, u8 *r)
{
    MEMSET(r, 0, INPUT_LEN_KEYBOARD);
    r[0] = INPUT_RID_KEYBOARD;
    for (u32 s = 0; s < INPUT_SRC_COUNT; s++) {
        r[1] |= m->src[s].mods;
        for (u32 i = 0; i < INPUT_KBD_BITMAP; i++)
            r[2 + i] |= m->src[s].keys[i];
    }
}

static void build_absolute(const struct input_source_state *s, u8 *r, s8 wheel, s8 pan)
{
    r[0] = INPUT_RID_ABSOLUTE;
    r[1] = s->abs_buttons;
    r[2] = (u8)s->abs_x;
    r[3] = (u8)(s->abs_x >> 8);
    r[4] = (u8)s->abs_y;
    r[5] = (u8)(s->abs_y >> 8);
    r[6] = (u8)wheel;
    r[7] = (u8)pan;
}

static u8 rel_buttons(const struct input_model *m)
{
    u8 b = 0;
    for (u32 s = 0; s < INPUT_SRC_COUNT; s++)
        b |= m->src[s].rel_buttons;
    return b;
}

static u16 consumer_usage(const struct input_model *m)
{
    /* One usage at a time (an array of one): the host's, else the internal source's. */
    for (u32 s = 0; s < INPUT_SRC_COUNT; s++)
        if (m->src[s].consumer)
            return m->src[s].consumer;
    return 0;
}

static void emit_keyboard(struct input_model *m, bool force)
{
    u8 r[INPUT_LEN_KEYBOARD];
    build_keyboard(m, r);
    if (!force && m->last_valid && same(r, m->last_kbd, sizeof(r)))
        return;
    copy(m->last_kbd, r, sizeof(r));
    q_push(m, r, INPUT_LEN_KEYBOARD);
}

static void emit_absolute(struct input_model *m, bool force, s8 wheel, s8 pan)
{
    const struct input_source_state *s = &m->src[INPUT_SRC_HOST];
    u8 r[INPUT_LEN_ABSOLUTE];
    if (!s->abs_valid)
        return;
    build_absolute(s, r, 0, 0);
    bool moved = !same(r, m->last_abs, sizeof(r));
    if (!force && m->last_valid && !moved && !wheel && !pan)
        return;
    copy(m->last_abs, r, sizeof(r));
    r[6] = (u8)wheel;
    r[7] = (u8)pan;
    q_push(m, r, INPUT_LEN_ABSOLUTE);
}

static void emit_relative(struct input_model *m, bool force, s16 dx, s16 dy, s8 wheel, s8 pan)
{
    u8 b = rel_buttons(m);
    if (force ? !b : (m->last_valid && b == m->last_rel_buttons && !dx && !dy && !wheel && !pan))
        return;
    u8 r[INPUT_LEN_RELATIVE] = {INPUT_RID_RELATIVE, b,        (u8)dx,     (u8)((u16)dx >> 8),
                                (u8)dy,             (u8)((u16)dy >> 8), (u8)wheel, (u8)pan};
    m->last_rel_buttons = b;
    q_push(m, r, INPUT_LEN_RELATIVE);
}

static void emit_consumer(struct input_model *m, bool force)
{
    u16 u = consumer_usage(m);
    if (!force && m->last_valid && u == m->last_consumer)
        return;
    m->last_consumer = u;
    u8 r[INPUT_LEN_CONSUMER] = {INPUT_RID_CONSUMER, (u8)u, (u8)(u >> 8)};
    q_push(m, r, INPUT_LEN_CONSUMER);
}

/*
 * The current state of every collection, once: at ENABLE 0->1 and RESET.  Keyboard and
 * consumer always (all up is harmless); absolute only once a position was given (else the
 * pointer would jump to the corner); relative only while a button is held (motion is not
 * state).  Wheel and pan are never repeated.
 */
static void emit_state(struct input_model *m)
{
    m->last_valid = false;
    emit_keyboard(m, true);
    emit_absolute(m, true, 0, 0);
    m->last_rel_buttons = rel_buttons(m);
    emit_relative(m, true, 0, 0, 0, 0);
    emit_consumer(m, true);
    m->last_valid = true;
}

/* Everything after a state change, with the deltas a frame carried. */
static void emit_changes(struct input_model *m)
{
    emit_keyboard(m, false);
    emit_absolute(m, false, 0, 0);
    emit_relative(m, false, 0, 0, 0, 0);
    emit_consumer(m, false);
}

static bool host_held(const struct input_source_state *s)
{
    if (s->mods || s->abs_buttons || s->rel_buttons || s->consumer)
        return true;
    for (u32 i = 0; i < INPUT_KBD_BITMAP; i++)
        if (s->keys[i])
            return true;
    return false;
}

void input_model_set_keyboard(struct input_model *m, u32 src, u8 mods,
                              const u8 keys[INPUT_KBD_BITMAP])
{
    if (src >= INPUT_SRC_COUNT)
        return;
    m->src[src].mods = mods;
    copy(m->src[src].keys, keys, INPUT_KBD_BITMAP);
    emit_keyboard(m, false);
}

void input_model_release(struct input_model *m, u32 src)
{
    if (src >= INPUT_SRC_COUNT)
        return;
    struct input_source_state *s = &m->src[src];
    s->mods = 0;
    MEMSET(s->keys, 0, INPUT_KBD_BITMAP);
    s->abs_buttons = 0; /* the position stays */
    s->rel_buttons = 0;
    s->consumer = 0;
    emit_changes(m);
}

void input_model_host_restart(struct input_model *m)
{
    m->rx_len = 0;
    m->host_seq_valid = false;
    input_model_release(m, INPUT_SRC_HOST);
}

/* ---- the host feed ----------------------------------------------------------------------- */

static u16 le16(const u8 *p)
{
    return (u16)(p[0] | p[1] << 8);
}

static bool valid_type(u8 type)
{
    return type >= INPUT_HOST_HELLO && type <= INPUT_HOST_RELEASE_ALL;
}

/* One frame with a good CRC: false when the payload is invalid for its type (no effect). */
static bool host_frame(struct input_model *m, u8 type, const u8 *p, u8 len)
{
    struct input_source_state *s = &m->src[INPUT_SRC_HOST];

    switch (type) {
        case INPUT_HOST_HELLO:
            if (len != 4 || le16(p) != INPUT_HOST_PROTO || le16(p + 2) != 0)
                return false;
            m->stats[INPUT_ST_HELLOS]++;
            input_model_release(m, INPUT_SRC_HOST);
            return true;
        case INPUT_HOST_KEYS: {
            if (len < 2 || p[1] > INPUT_HOST_MAX_KEYS || len != 2 + p[1])
                return false;
            u8 keys[INPUT_KBD_BITMAP] = {0};
            for (u32 i = 0; i < p[1]; i++) {
                u8 u = p[2 + i];
                if (u < INPUT_HOST_USAGE_MIN || u > INPUT_HOST_USAGE_MAX)
                    return false;
                keys[u / 8] |= (u8)(1u << (u % 8));
            }
            input_model_set_keyboard(m, INPUT_SRC_HOST, p[0], keys);
            return true;
        }
        case INPUT_HOST_ABS: {
            if (len != 7)
                return false;
            u16 x = le16(p + 1), y = le16(p + 3);
            s8 wheel = (s8)p[5], pan = (s8)p[6];
            if (p[0] & ~INPUT_BUTTONS_MASK || x > INPUT_ABS_MAX || y > INPUT_ABS_MAX ||
                wheel < -INPUT_WHEEL_MAX || pan < -INPUT_WHEEL_MAX)
                return false;
            s->abs_buttons = p[0];
            s->abs_x = x;
            s->abs_y = y;
            s->abs_valid = true;
            emit_absolute(m, false, wheel, pan);
            return true;
        }
        case INPUT_HOST_REL: {
            if (len != 7)
                return false;
            s16 dx = (s16)le16(p + 1), dy = (s16)le16(p + 3);
            s8 wheel = (s8)p[5], pan = (s8)p[6];
            if (p[0] & ~INPUT_BUTTONS_MASK || dx < -INPUT_REL_MAX || dy < -INPUT_REL_MAX ||
                wheel < -INPUT_WHEEL_MAX || pan < -INPUT_WHEEL_MAX)
                return false;
            s->rel_buttons = p[0];
            emit_relative(m, false, dx, dy, wheel, pan);
            return true;
        }
        case INPUT_HOST_CONSUMER:
            if (len != 2 || le16(p) > INPUT_CONSUMER_MAX)
                return false;
            s->consumer = le16(p);
            emit_consumer(m, false);
            return true;
        case INPUT_HOST_KEEPALIVE:
            return len == 0;
        case INPUT_HOST_RELEASE_ALL:
            if (len != 0)
                return false;
            input_model_release(m, INPUT_SRC_HOST);
            return true;
        default:
            return false;
    }
}

static void rx_drop(struct input_model *m, u32 n)
{
    for (u32 i = n; i < m->rx_len; i++)
        m->rx[i - n] = m->rx[i];
    m->rx_len -= n;
}

/*
 * Parse what is buffered.  A failed frame (bad header or CRC) gives up only its first byte,
 * so the scan resumes at the byte after its sync and a frame inside it is still found.
 */
static void rx_parse(struct input_model *m, u64 now)
{
    for (;;) {
        if (!m->rx_len)
            return;
        if (m->rx[0] != INPUT_HOST_SYNC0) {
            m->stats[INPUT_ST_SKIPPED]++;
            rx_drop(m, 1);
            continue;
        }
        if (m->rx_len < 2)
            return;
        if (m->rx[1] != INPUT_HOST_SYNC1) {
            m->stats[INPUT_ST_SKIPPED]++;
            rx_drop(m, 1);
            continue;
        }
        if (m->rx_len < 3)
            return;
        if (!valid_type(m->rx[2])) {
            m->stats[INPUT_ST_BAD_HEADER]++;
            rx_drop(m, 1);
            continue;
        }
        if (m->rx_len < 4)
            return;
        u8 len = m->rx[3];
        if (len > INPUT_HOST_MAX_PAYLOAD) {
            m->stats[INPUT_ST_BAD_HEADER]++;
            rx_drop(m, 1);
            continue;
        }
        u32 total = INPUT_HOST_HDR + len + INPUT_HOST_CRC;
        if (m->rx_len < total)
            return;
        const u8 *c = m->rx + INPUT_HOST_HDR + len;
        u32 want = (u32)c[0] | (u32)c[1] << 8 | (u32)c[2] << 16 | (u32)c[3] << 24;
        if (input_model_crc32(0, m->rx + 2, 4u + len) != want) {
            m->stats[INPUT_ST_BAD_CRC]++;
            rx_drop(m, 1);
            continue;
        }
        u8 type = m->rx[2];
        u16 seq = le16(m->rx + 4);
        if (!host_frame(m, type, m->rx + INPUT_HOST_HDR, len)) {
            m->stats[INPUT_ST_BAD_PAYLOAD]++;
        } else {
            m->stats[INPUT_ST_FRAMES_OK]++;
            if (type != INPUT_HOST_HELLO && m->host_seq_valid && seq != (u16)(m->host_seq + 1))
                m->stats[INPUT_ST_SEQ_GAPS]++;
            m->host_seq = seq;
            m->host_seq_valid = true;
            m->host_last = now;
        }
        rx_drop(m, total);
    }
}

void input_model_host_rx(struct input_model *m, const u8 *data, u32 len, u64 now)
{
    m->stats[INPUT_ST_RX_BYTES] += len;
    if (len)
        m->rx_last = now;
    for (u32 i = 0; i < len; i++) {
        if (m->rx_len == sizeof(m->rx)) {
            /* Unreachable: rx_parse() never leaves a full buffer.  Fail safe anyway. */
            m->stats[INPUT_ST_SKIPPED]++;
            rx_drop(m, 1);
        }
        m->rx[m->rx_len++] = data[i];
        rx_parse(m, now);
    }
}

void input_model_poll(struct input_model *m, u64 now)
{
    struct input_source_state *s = &m->src[INPUT_SRC_HOST];

    /* What is still buffered is a partial frame (rx_parse consumed everything else). */
    if (m->rx_len && now - m->rx_last >= m->watchdog_ticks / 40) {
        while (m->rx_len) {
            m->stats[INPUT_ST_SKIPPED]++;
            rx_drop(m, 1);
            rx_parse(m, m->rx_last);
        }
    }
    if (!host_held(s) || now - m->host_last < m->watchdog_ticks)
        return;
    m->stats[INPUT_ST_WATCHDOG]++;
    input_model_release(m, INPUT_SRC_HOST);
}

/* ---- registers --------------------------------------------------------------------------- */

static void write_ctrl(struct input_model *m, u32 val)
{
    u32 old = m->ctrl;
    m->ctrl = val & INPUT_CTRL_RW;
    bool was = old & INPUT_CTRL_ENABLE, on = m->ctrl & INPUT_CTRL_ENABLE;

    if (was && !on) {
        q_flush(m);
    } else if (!was && on) {
        q_flush(m);
        m->dropped = 0;
        m->overflow = false;
        emit_state(m);
        return; /* a RESET in the same write has nothing more to do */
    }
    if (val & INPUT_CTRL_RESET) {
        q_flush(m);
        m->overflow = false;
        if (on)
            emit_state(m);
    }
}

static void write_out(struct input_model *m, u32 val)
{
    u32 len = val & 0xff, id = (val >> 8) & 0xff;
    bool feature = val & INPUT_OUT_FEATURE;
    /* v1 takes the keyboard LED output report: ID 1, two bytes, OUT_DATA[0] = ID. */
    if (feature || id != INPUT_RID_KEYBOARD || len != 2 || m->out_data[0] != id) {
        m->stats[INPUT_ST_OUT_BAD]++;
        return;
    }
    m->leds = m->out_data[1] & INPUT_LED_MASK;
    m->stats[INPUT_ST_OUT_OK]++;
}

static u32 read32(struct input_model *m, u32 off)
{
    if (off >= INPUT_REG_REPORT && off < INPUT_REG_REPORT + INPUT_REPORT_MAX) {
        if (!m->q_count)
            return 0;
        const u8 *d = q_at(m, 0)->data + (off - INPUT_REG_REPORT);
        return (u32)d[0] | (u32)d[1] << 8 | (u32)d[2] << 16 | (u32)d[3] << 24;
    }
    if (off >= INPUT_REG_STATS && off < INPUT_REG_STATS + 4 * INPUT_ST_COUNT)
        return m->stats[(off - INPUT_REG_STATS) / 4];
    switch (off) {
        case INPUT_REG_ID:
            return INPUT_ID_VALUE;
        case INPUT_REG_VERSION:
            return INPUT_VERSION;
        case INPUT_REG_FEATURES:
            return m->features;
        case INPUT_REG_CTRL:
            return m->ctrl;
        case INPUT_REG_STATUS:
            return (m->q_count ? INPUT_STATUS_PENDING : 0) |
                   (m->overflow ? INPUT_STATUS_OVERFLOW : 0) |
                   m->q_count << INPUT_STATUS_DEPTH_SHIFT;
        case INPUT_REG_IRQ_STATUS:
            return input_model_line(m) ? 1 : 0;
        case INPUT_REG_SEQ:
            return m->q_count ? q_at(m, 0)->seq : 0;
        case INPUT_REG_DROPPED:
            return m->dropped;
        case INPUT_REG_REPORT_LEN:
            return m->q_count ? (u32)q_at(m, 0)->len | (u32)q_at(m, 0)->id << 8 : 0;
        default:
            return 0; /* WO registers and holes */
    }
}

static void write32(struct input_model *m, u32 off, u32 val)
{
    if (off >= INPUT_REG_OUT_DATA && off < INPUT_REG_OUT_DATA + INPUT_REPORT_MAX) {
        u8 *d = m->out_data + (off - INPUT_REG_OUT_DATA);
        for (int i = 0; i < 4; i++)
            d[i] = (u8)(val >> (8 * i));
        return;
    }
    switch (off) {
        case INPUT_REG_CTRL:
            write_ctrl(m, val);
            break;
        case INPUT_REG_POP:
            if (val == 1 && m->q_count) {
                m->q_head = (m->q_head + 1) % INPUT_QUEUE_LEN;
                m->q_count--;
            }
            break;
        case INPUT_REG_OUT_LEN:
            write_out(m, val);
            break;
        default:
            break; /* RO registers and holes */
    }
}

bool input_model_mmio(struct input_model *m, u64 off, int width, u64 *val, bool write)
{
    bool ok = (width == 2 && !(off & 3)) || (width == 3 && !(off & 7));
    if (!ok || off >= INPUT_PAGE_SIZE) {
        m->stats[INPUT_ST_BAD_ACCESS]++;
        if (!write) {
            val[0] = 0;
            if (width == 4)
                val[1] = 0;
        }
        return false;
    }
    if (off >= INPUT_REG_END) {
        if (!write)
            val[0] = 0;
        return true;
    }
    u32 o = (u32)off;
    if (write) {
        write32(m, o, (u32)*val);
        if (width == 3)
            write32(m, o + 4, (u32)(*val >> 32));
    } else {
        u64 v = read32(m, o);
        if (width == 3)
            v |= (u64)read32(m, o + 4) << 32;
        *val = v;
    }
    return true;
}

bool input_model_line(const struct input_model *m)
{
    return (m->ctrl & INPUT_CTRL_ENABLE) && (m->ctrl & INPUT_CTRL_IRQ_EN) && m->q_count;
}

u32 input_model_depth(const struct input_model *m)
{
    return m->q_count;
}

void input_model_init(struct input_model *m, u32 sources, u64 watchdog_ticks)
{
    MEMSET(m, 0, sizeof(*m));
    for (u32 i = 0; i < INPUT_QUEUE_LEN; i++)
        m->q_order[i] = (u8)i;
    m->features = INPUT_FEAT_REPORTS |
                  (sources & (INPUT_FEAT_SRC_HOST | INPUT_FEAT_SRC_KBD | INPUT_FEAT_SRC_TP));
    m->watchdog_ticks = watchdog_ticks;
}
