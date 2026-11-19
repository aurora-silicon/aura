/* SPDX-License-Identifier: MIT */

/*
 * I0 host replay of the carrier glue src/hv/windows/hv_input.c, compiled unchanged (with
 * input_model.c and hv_vdev_spi.c) against stub headers from
 * tests/python/test_windows_t8152_input.py, ASan + UBSan.  Written from that file's failure
 * list (G1..G10) before the glue existed.
 *
 * The machine is fake: the vUART RX is a byte buffer behind iodev_can_read/iodev_read (with a
 * read counter so the per-tick budget is visible), the page hook and the stage-2 leaf walk
 * are recorded, and every hv_t8152_gic_spi_level() call is logged in order.
 *
 * Output: one JSON line per case.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hv.h"
#include "hv_input.h"
#include "hv_mtp_t8152.h"
#include "input_model.h"
#include "iodev.h"
#include "utils.h"

u32 chip_id = 0x8152, board_id = 0x22;
u64 stub_cntpct = 1000, stub_cntfrq = 24000000;

void hv_input_host_new_session(void);

/* ---- the fake machine -------------------------------------------------------------------- */

static u8 rx_buf[1 << 16];
static u32 rx_head, rx_tail;
static int rx_reads;
static u32 rx_last_read;
static bool rx_fail;

ssize_t iodev_can_read(iodev_id_t id)
{
    if (id != IODEV_USB_VUART)
        return 0;
    return (ssize_t)(rx_tail - rx_head);
}

ssize_t iodev_read(iodev_id_t id, void *buf, size_t length)
{
    if (id != IODEV_USB_VUART)
        return -1;
    if (rx_fail)
        return -1;
    /* The real CDC read waits for `length` bytes: asking for more than is buffered would hang. */
    if (length > rx_tail - rx_head) {
        fprintf(stderr, "iodev_read of %zu with %u buffered: would block\n", length,
                rx_tail - rx_head);
        abort();
    }
    memcpy(buf, rx_buf + rx_head, length);
    rx_head += (u32)length;
    rx_reads++;
    rx_last_read = (u32)length;
    return (ssize_t)length;
}

static void rx_push(const void *data, u32 n)
{
    memcpy(rx_buf + rx_tail, data, n);
    rx_tail += n;
}

static void rx_reset(void)
{
    rx_head = rx_tail = 0;
    rx_reads = 0;
    rx_last_read = 0;
    rx_fail = false;
}

static hv_hook_t *hook_fn;
static u64 hook_from, hook_size;
static bool map_fails, foreign_leaf;
static int hooks_mapped;

int hv_map_hook(u64 from, hv_hook_t *hook, u64 size)
{
    if (map_fails)
        return -1;
    hook_fn = hook;
    hook_from = from;
    hook_size = size;
    hooks_mapped++;
    return 0;
}

void hv_pt_for_each_leaf(hv_pt_leaf_fn *fn, void *ctx)
{
    /* Some unrelated leaves, then the hook as one 16 KiB software leaf of type 1. */
    fn(ctx, 0x10000000000UL, 0x4000, 0x10000000000UL | 0x3, 3);
    if (hook_fn)
        fn(ctx, hook_from, hook_size, (1UL << 50) | ((u64)hook_fn & GENMASK(49, 2)), 3);
    if (foreign_leaf)
        fn(ctx, 0xf50000000UL + 0x2000, 0x4000, 0xf50002000UL | 0x3, 3);
}

static int spi_calls;
static bool spi_level_now;
static char spi_log[4096];

void hv_t8152_gic_spi_level(u32 intid, bool level)
{
    size_t n = strlen(spi_log);
    snprintf(spi_log + n, sizeof(spi_log) - n, "%s%u:%d", n ? "," : "", intid, level);
    spi_calls++;
    spi_level_now = level;
}

static bool guest_entered;
bool hv_windows_guest_entered(void)
{
    return guest_entered;
}

/*
 * I1: the MTP glue (hv_mtp_t8152.c) is replaced by a recorder.  Its poll can deliver a scripted
 * internal keyboard state through hv_input_internal_keyboard(), as the real one does from
 * mtp_bringup_poll(); the spin lock stub traps if that ever happens under input_lock.
 */
static int mtp_attaches, mtp_polls, mtp_probes, mtp_reports;
static bool mtp_up, mtp_tp_up, mtp_tp_asked;
static u64 mtp_area, mtp_size;
static bool mtp_fkeys;
static bool script_pending, ptr_pending;
static u8 script_mods, script_keys[20];
static u16 script_cons;
static u8 ptr_buttons;
static s16 ptr_dx;
static s8 ptr_wheel;

bool hv_mtp_t8152_attach(u64 area, u64 size, bool fn_fkeys, bool trackpad)
{
    mtp_attaches++;
    mtp_area = area;
    mtp_size = size;
    mtp_fkeys = fn_fkeys;
    mtp_tp_asked = trackpad;
    return mtp_up;
}

bool hv_mtp_t8152_trackpad_up(void)
{
    return mtp_tp_up;
}

bool hv_mtp_t8152_live(void)
{
    return mtp_up;
}

void hv_mtp_t8152_poll(void)
{
    mtp_polls++;
    if (script_pending) {
        script_pending = false;
        hv_input_internal_keyboard(script_mods, script_keys, script_cons);
    }
    if (ptr_pending) {
        ptr_pending = false;
        hv_input_internal_pointer(ptr_buttons, ptr_dx, 0, ptr_wheel, 0);
    }
}

u64 hv_mtp_t8152_status(u32 index)
{
    return 0x100 + index;
}

s64 hv_mtp_t8152_probe(u32 ms)
{
    mtp_probes++;
    return ms;
}

void hv_mtp_t8152_report(void)
{
    mtp_reports++;
}

void hv_mtp_t8152_fatal_report(void)
{
}

static void script(u8 mods, u8 usage, u16 consumer)
{
    memset(script_keys, 0, sizeof(script_keys));
    if (usage)
        script_keys[usage / 8] |= (u8)(1u << (usage % 8));
    script_mods = mods;
    script_cons = consumer;
    script_pending = true;
}

/* ---- harness ----------------------------------------------------------------------------- */

static int failures, total_failures;
static const char *current;
static char extra[4096];

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

static void begin(const char *name)
{
    current = name;
    failures = 0;
    extra[0] = 0;
    hv_input_host_new_session();
    rx_reset();
    hook_fn = NULL;
    map_fails = foreign_leaf = false;
    hooks_mapped = 0;
    spi_calls = 0;
    spi_level_now = false;
    spi_log[0] = 0;
    guest_entered = false;
    chip_id = 0x8152;
    board_id = 0x22;
    stub_cntpct = 1000;
    mtp_attaches = mtp_polls = mtp_probes = mtp_reports = 0;
    mtp_up = true;
    mtp_tp_up = true;
    mtp_tp_asked = false;
    mtp_area = mtp_size = 0;
    mtp_fkeys = false;
    script_pending = ptr_pending = false;
}

static void end(void)
{
    printf("{\"case\":\"%s\",\"result\":\"%s\",\"spi\":\"%s\"%s}\n", current,
           failures ? "failed" : "passed", spi_log, extra);
    total_failures += failures;
}

static s64 attach(u64 mask, u64 a2, u64 a3)
{
    return (s64)hv_input_proxy(HV_INPUT_OP_ATTACH, mask, a2, a3);
}

/* The guest's accesses go through the recorded hook, as the stage-2 fault path would. */
static u64 g_rd(u32 off, int width)
{
    u64 v[2] = {0x1111, 0x2222};
    CHECK(hook_fn);
    if (hook_fn)
        CHECK(hook_fn(NULL, 0xf50000000UL + off, v, false, width));
    return v[0];
}

static void g_wr(u32 off, u64 val, int width)
{
    u64 v[2] = {val, 0};
    CHECK(hook_fn);
    if (hook_fn)
        CHECK(hook_fn(NULL, 0xf50000000UL + off, v, true, width));
}

static u16 seq;

static u32 frame(u8 *buf, u8 type, const u8 *payload, u8 len)
{
    buf[0] = 'M';
    buf[1] = '6';
    buf[2] = type;
    buf[3] = len;
    buf[4] = (u8)seq;
    buf[5] = (u8)(seq >> 8);
    seq++;
    memcpy(buf + 6, payload, len);
    u32 crc = input_model_crc32(0, buf + 2, 4u + len);
    for (int i = 0; i < 4; i++)
        buf[6 + len + i] = (u8)(crc >> (8 * i));
    return 10u + len;
}

static void push_keys(u8 mods, u8 usage)
{
    u8 buf[32], p[3] = {mods, usage ? 1 : 0, usage};
    rx_push(buf, frame(buf, INPUT_HOST_KEYS, p, usage ? 3 : 2));
}

static u64 st(u64 index)
{
    return hv_input_proxy(HV_INPUT_OP_STATUS, index, 0, 0);
}

static void enter(void)
{
    CHECK(hv_input_verify() == 0);
    guest_entered = true;
    hv_input_guest_entered();
}

/* ---- cases ------------------------------------------------------------------------------- */

/* G1: every refusal changes nothing; QUERY works before attach. */
static void case_attach_refusals(void)
{
    begin("attach_refusals");
    EQ(attach(HV_INPUT_SRC_HOST, 0, 0), (u64)(s64)HV_INPUT_E_STATE); /* not configured */
    hv_input_enable();
    chip_id = 0x8142;
    EQ(attach(HV_INPUT_SRC_HOST, 0, 0), (u64)(s64)HV_INPUT_E_STATE);
    chip_id = 0x8152;
    board_id = 0x24;
    EQ(attach(HV_INPUT_SRC_HOST, 0, 0), (u64)(s64)HV_INPUT_E_STATE);
    board_id = 0x22;
    EQ(attach(0, 0, 0), (u64)(s64)HV_INPUT_E_SOURCES);
    EQ(attach(0x8, 0, 0), (u64)(s64)HV_INPUT_E_SOURCES);
    /* I1/I2: the keyboard needs the area and the host source; the trackpad the keyboard. */
    EQ(attach(HV_INPUT_SRC_HOST | HV_INPUT_SRC_KEYBOARD, 0, 0), (u64)(s64)HV_INPUT_E_AREA);
    EQ(attach(HV_INPUT_SRC_HOST | HV_INPUT_SRC_KEYBOARD | HV_INPUT_SRC_TRACKPAD, 0, 0),
       (u64)(s64)HV_INPUT_E_AREA);
    EQ(attach(HV_INPUT_SRC_KEYBOARD, 0x10f00000000UL, 0x800000), (u64)(s64)HV_INPUT_E_SOURCES);
    EQ(attach(HV_INPUT_SRC_HOST | HV_INPUT_SRC_TRACKPAD, 0x10f00000000UL, 0x800000),
       (u64)(s64)HV_INPUT_E_SOURCES);
    EQ(attach(HV_INPUT_SRC_KEYBOARD | HV_INPUT_SRC_TRACKPAD, 0x10f00000000UL, 0x800000),
       (u64)(s64)HV_INPUT_E_SOURCES);
    EQ(attach(HV_INPUT_SRC_HOST | HV_INPUT_OPT_FN_FKEYS, 0, 0), (u64)(s64)HV_INPUT_E_SOURCES);
    EQ(attach(HV_INPUT_SRC_HOST | HV_INPUT_SRC_KEYBOARD | BIT(9), 0x10f00000000UL, 0x800000),
       (u64)(s64)HV_INPUT_E_SOURCES);
    EQ(attach(HV_INPUT_SRC_TRACKPAD, 0, 0), (u64)(s64)HV_INPUT_E_SOURCES);
    EQ(attach(HV_INPUT_SRC_HOST, 0x10000000000UL, 0), (u64)(s64)HV_INPUT_E_AREA);
    EQ(attach(HV_INPUT_SRC_HOST, 0, 0x4000), (u64)(s64)HV_INPUT_E_AREA);
    guest_entered = true;
    EQ(attach(HV_INPUT_SRC_HOST, 0, 0), (u64)(s64)HV_INPUT_E_STATE);
    guest_entered = false;
    EQ(hooks_mapped, 0);
    CHECK(!hv_input_owns_vuart_rx());
    EQ(st(HV_INPUT_S_ATTACHED), 0);
    /* hv_start refuses with the flag and no attach. */
    EQ(hv_input_verify(), -1);
    /* QUERY: the contract. */
    EQ(hv_input_proxy(HV_INPUT_OP_QUERY, HV_INPUT_Q_VERSION, 0, 0), 1);
    EQ(hv_input_proxy(HV_INPUT_OP_QUERY, HV_INPUT_Q_BASE, 0, 0), 0xf50000000UL);
    EQ(hv_input_proxy(HV_INPUT_OP_QUERY, HV_INPUT_Q_SIZE, 0, 0), 0x4000);
    EQ(hv_input_proxy(HV_INPUT_OP_QUERY, HV_INPUT_Q_INTID, 0, 0), 991);
    EQ(hv_input_proxy(HV_INPUT_OP_QUERY, HV_INPUT_Q_FEATURES, 0, 0), 0x1000f);
    EQ(hv_input_proxy(HV_INPUT_OP_QUERY, HV_INPUT_Q_SOURCES, 0, 0), 7);
    EQ(hv_input_proxy(HV_INPUT_OP_QUERY, 6, 0, 0), HV_INPUT_ERROR);
    EQ(hv_input_proxy(7, 0, 0, 0), HV_INPUT_ERROR);
    EQ(mtp_attaches, 0);
    /* A hook failure uses the session up and attaches nothing. */
    map_fails = true;
    EQ(attach(HV_INPUT_SRC_HOST, 0, 0), (u64)(s64)HV_INPUT_E_HOOK);
    map_fails = false;
    CHECK(!hv_input_owns_vuart_rx());
    EQ(attach(HV_INPUT_SRC_HOST, 0, 0), (u64)(s64)HV_INPUT_E_STATE);
    EQ(hv_input_verify(), -1);
    end();
}

/* G2/G3/G8: one ATTACH per session; RX ownership; verify. */
static void case_attach_once(void)
{
    begin("attach_once");
    EQ(hv_input_verify(), 0); /* not configured: nothing to check */
    hv_input_enable();
    CHECK(!hv_input_owns_vuart_rx());
    EQ(attach(HV_INPUT_SRC_HOST, 0, 0), 0);
    EQ(hooks_mapped, 1);
    EQ(hook_from, 0xf50000000UL);
    EQ(hook_size, 0x4000);
    CHECK(hv_input_owns_vuart_rx());
    EQ(st(HV_INPUT_S_ATTACHED), 1);
    EQ(st(HV_INPUT_S_OWNS_RX), 1);
    EQ(attach(HV_INPUT_SRC_HOST, 0, 0), (u64)(s64)HV_INPUT_E_STATE);
    EQ(hooks_mapped, 1);
    EQ(hv_input_verify(), 0);
    /* A foreign leaf inside the page: refuse. */
    foreign_leaf = true;
    EQ(hv_input_verify(), -1);
    foreign_leaf = false;
    /* DETACH before entry: RX released, verify refuses (fail closed). */
    EQ(hv_input_proxy(HV_INPUT_OP_DETACH, 0, 0, 0), 0);
    CHECK(!hv_input_owns_vuart_rx());
    EQ(hv_input_verify(), -1);
    EQ(st(HV_INPUT_S_ATTACHED), 0);
    end();
}

/* G4: the tick drains at most 256 bytes, never blocks, only while the guest runs. */
static void case_poll_budget(void)
{
    begin("poll_budget");
    hv_input_enable();
    EQ(attach(HV_INPUT_SRC_HOST, 0, 0), 0);
    push_keys(0, 0x04);
    hv_input_poll(); /* before entry: nothing read */
    EQ(rx_reads, 0);
    enter(); /* discards the pre-entry frame (G11) */
    EQ(rx_head, rx_tail);
    for (int i = 0; i < 100; i++)
        push_keys(0, (u8)(0x04 + (i % 20))); /* 13 bytes each: 1300 bytes */
    int polls = 0;
    while (rx_head != rx_tail && polls < 100) {
        u32 before = rx_head;
        hv_input_poll();
        CHECK(rx_head - before <= HV_INPUT_RX_BUDGET);
        polls++;
    }
    EQ(polls, (1300 + 255) / 256);
    EQ(st(INPUT_ST_FRAMES_OK), 100);
    EQ(st(INPUT_ST_RX_BYTES), 1300); /* the 13 discarded bytes never reached the parser */
    /* Nothing buffered: no read at all. */
    int reads = rx_reads;
    hv_input_poll();
    EQ(rx_reads, reads);
    /* A read failure is survived and logged once. */
    push_keys(0, 0x05);
    rx_fail = true;
    hv_input_poll();
    hv_input_poll();
    rx_fail = false;
    hv_input_poll();
    EQ(st(INPUT_ST_FRAMES_OK), 101);
    /* After exit the feed is not read. */
    hv_input_guest_exited(1);
    push_keys(0, 0x06);
    reads = rx_reads;
    hv_input_poll();
    EQ(rx_reads, reads);
    end();
}

/* G5: the SPI follows the model's line, on changes only. */
static void case_line_sync(void)
{
    begin("line_sync");
    hv_input_enable();
    EQ(attach(HV_INPUT_SRC_HOST, 0, 0), 0);
    enter();
    push_keys(0x02, 0x04);
    hv_input_poll();
    EQ(spi_calls, 0); /* not enabled: state only */
    g_wr(INPUT_REG_CTRL, INPUT_CTRL_ENABLE, 2);
    EQ(spi_calls, 0); /* pending, but IRQ_EN clear */
    g_wr(INPUT_REG_CTRL, INPUT_CTRL_ENABLE | INPUT_CTRL_IRQ_EN, 2);
    EQ(spi_calls, 1);
    CHECK(spi_level_now);
    EQ(st(HV_INPUT_S_LINE), 1);
    EQ(st(HV_INPUT_S_DEPTH), 2);
    /* The ISR's read and mask, the DPC's reads and POPs. */
    EQ(g_rd(INPUT_REG_IRQ_STATUS, 2), 1);
    g_wr(INPUT_REG_CTRL, INPUT_CTRL_ENABLE, 2);
    EQ(spi_calls, 2);
    CHECK(!spi_level_now);
    EQ(g_rd(INPUT_REG_REPORT_LEN, 2), 22 | 1 << 8);
    u64 r0 = g_rd(INPUT_REG_REPORT, 3);
    EQ(r0 & 0xffffff, 0x100201); /* id 1, LeftShift, usage 0x04 */
    g_wr(INPUT_REG_POP, 1, 2);
    g_wr(INPUT_REG_POP, 1, 2);
    g_wr(INPUT_REG_CTRL, INPUT_CTRL_ENABLE | INPUT_CTRL_IRQ_EN, 2);
    EQ(spi_calls, 2); /* empty: stays low */
    push_keys(0, 0);
    hv_input_poll();
    EQ(spi_calls, 3);
    CHECK(spi_level_now);
    g_wr(INPUT_REG_POP, 1, 2);
    EQ(spi_calls, 4);
    CHECK(!spi_level_now);
    /* Only INTID 991 is ever driven. */
    CHECK(!strstr(spi_log, "990"));
    end();
}

/* G6: bad widths are RAZ/WI through the hook, logged once, never a guest abort. */
static void case_bad_width(void)
{
    begin("bad_width");
    hv_input_enable();
    EQ(attach(HV_INPUT_SRC_HOST, 0, 0), 0);
    enter();
    u64 v[4] = {1, 2, 3, 4};
    CHECK(hook_fn(NULL, 0xf50000000UL, v, false, 4)); /* LDP of two X registers */
    EQ(v[0], 0);
    EQ(v[1], 0);
    v[0] = v[1] = v[2] = v[3] = 9;
    CHECK(hook_fn(NULL, 0xf50000000UL, v, false, 5)); /* wider still */
    EQ(v[0] | v[1] | v[2] | v[3], 0);
    v[0] = INPUT_CTRL_ENABLE;
    CHECK(hook_fn(NULL, 0xf50000000UL + INPUT_REG_CTRL, v, true, 0));
    EQ(g_rd(INPUT_REG_CTRL, 2), 0);
    EQ(st(INPUT_ST_BAD_ACCESS), 3);
    EQ(g_rd(INPUT_REG_ID, 2), 0x4e49364d);
    end();
}

/* G7: the held-key watchdog runs on the tick with the system counter. */
static void case_watchdog(void)
{
    begin("watchdog");
    hv_input_enable();
    EQ(attach(HV_INPUT_SRC_HOST, 0, 0), 0);
    enter();
    g_wr(INPUT_REG_CTRL, INPUT_CTRL_ENABLE | INPUT_CTRL_IRQ_EN, 2);
    g_wr(INPUT_REG_POP, 1, 2);
    g_wr(INPUT_REG_POP, 1, 2);
    push_keys(0x01, 0x06); /* Ctrl+C held */
    hv_input_poll();
    g_wr(INPUT_REG_POP, 1, 2);
    EQ(st(HV_INPUT_S_DEPTH), 0);
    stub_cntpct += 2 * stub_cntfrq - 1;
    hv_input_poll();
    EQ(st(HV_INPUT_S_DEPTH), 0);
    stub_cntpct += 1;
    hv_input_poll();
    EQ(st(INPUT_ST_WATCHDOG), 1);
    EQ(st(HV_INPUT_S_DEPTH), 1);
    u64 r = g_rd(INPUT_REG_REPORT, 3);
    EQ(r & 0xffffff, 0x000001); /* id 1, nothing held */
    end();
}

/* G9: exit releases every host key; the report prints; DETACH after entry is refused. */
static void case_exit(void)
{
    begin("exit");
    hv_input_enable();
    EQ(attach(HV_INPUT_SRC_HOST, 0, 0), 0);
    enter();
    g_wr(INPUT_REG_CTRL, INPUT_CTRL_ENABLE | INPUT_CTRL_IRQ_EN, 2);
    g_wr(INPUT_REG_POP, 1, 2);
    g_wr(INPUT_REG_POP, 1, 2);
    push_keys(0x08, 0x15); /* Win+R held */
    hv_input_poll();
    g_wr(INPUT_REG_POP, 1, 2);
    EQ(hv_input_proxy(HV_INPUT_OP_DETACH, 0, 0, 0), HV_INPUT_ERROR);
    /* ENABLE|IRQ_EN with an empty queue: the release's report would raise the line. */
    EQ(st(HV_INPUT_S_LINE), 0);
    int calls = spi_calls;
    hv_input_guest_exited(2);
    EQ(spi_calls, calls); /* state only: the vSPI is never driven after exit */
    EQ(st(HV_INPUT_S_DEPTH), 1);
    EQ(g_rd(INPUT_REG_REPORT, 3) & 0xffff, 0x0001);
    g_wr(INPUT_REG_POP, 1, 2);
    EQ(spi_calls, calls);
    CHECK(hv_input_owns_vuart_rx()); /* until the carrier is reloaded: no guest reads it */
    EQ(st(INPUT_ST_FRAMES_OK), 1);
    end();
}

/*
 * G11: host bytes sent between ATTACH and entry are discarded at entry, so no key, button or
 * position from before the guest's session reaches the model; a partial frame is forgotten.
 */
static void case_pre_entry(void)
{
    begin("pre_entry");
    hv_input_enable();
    EQ(attach(HV_INPUT_SRC_HOST, 0, 0), 0);
    CHECK(hv_input_owns_vuart_rx());
    for (int i = 0; i < 40; i++)
        push_keys(0x01, 0x06); /* Ctrl+C held, 40 times: 520 bytes, more than one budget */
    u8 abs[32], p[7] = {0x01, 0x00, 0x40, 0x00, 0x40, 0, 0};
    rx_push(abs, frame(abs, INPUT_HOST_ABS, p, 7));
    u8 half[32];
    u32 n = frame(half, INPUT_HOST_KEYS, (const u8[]){0x02, 1, 0x04}, 3);
    rx_push(half, n / 2); /* a frame cut short before entry */
    hv_input_poll();
    EQ(rx_reads, 0);
    enter();
    EQ(rx_head, rx_tail);
    EQ(st(INPUT_ST_RX_BYTES), 0);
    EQ(st(INPUT_ST_FRAMES_OK), 0);
    /* The rest of the cut frame arrives after entry: resynchronised, not applied. */
    rx_push(half + n / 2, n - n / 2);
    push_keys(0, 0x05);
    hv_input_poll();
    EQ(st(INPUT_ST_FRAMES_OK), 1);
    EQ(st(INPUT_ST_SEQ_GAPS), 0); /* the sequence restarted at entry */
    g_wr(INPUT_REG_CTRL, INPUT_CTRL_ENABLE | INPUT_CTRL_IRQ_EN, 2);
    /* ENABLE emits the current state: only the post-entry key, no Ctrl, no ABS position. */
    EQ(st(HV_INPUT_S_DEPTH), 2);
    u64 r = g_rd(INPUT_REG_REPORT, 3);
    EQ(r & 0xffff, 0x0001);     /* id 1, no modifiers */
    EQ((r >> 16) & 0xff, 0x20);     /* usage 0x05 only (bit 5 of bitmap byte 0) */
    g_wr(INPUT_REG_POP, 1, 2);
    EQ(g_rd(INPUT_REG_REPORT_LEN, 2) >> 8 & 0xff, 4); /* consumer; no absolute report */
    end();
}

/* G10: STATUS indices and the counters through the op. */
static void case_status(void)
{
    begin("status");
    hv_input_enable();
    EQ(attach(HV_INPUT_SRC_HOST, 0, 0), 0);
    enter();
    u8 junk[] = {1, 2, 3};
    rx_push(junk, 3);
    push_keys(0, 0x04);
    u8 bad[32];
    u8 p[2] = {0, 0};
    u32 n = frame(bad, INPUT_HOST_KEYS, p, 2);
    bad[n - 1] ^= 1;
    rx_push(bad, n);
    hv_input_poll();
    EQ(st(INPUT_ST_FRAMES_OK), 1);
    EQ(st(INPUT_ST_BAD_CRC), 1);
    EQ(st(INPUT_ST_SKIPPED) >= 3, 1);
    g_wr(INPUT_REG_OUT_DATA, 0x0201, 2);
    g_wr(INPUT_REG_OUT_LEN, 2 | 1 << 8, 2);
    EQ(st(HV_INPUT_S_LEDS), 2);
    EQ(st(HV_INPUT_S_CTRL), 0);
    EQ(st(HV_INPUT_S_DROPPED), 0);
    EQ(st(HV_INPUT_S_MTP_STATE), 0); /* host only: no MTP */
    EQ(st(HV_INPUT_S_MTP_GPIO), 0);
    EQ(st(32), HV_INPUT_ERROR);
    hv_input_report();
    EQ(mtp_reports, 0);
    end();
}

/* I1 G12: a host-only session never calls the MTP glue (attach, tick, idle, exit). */
static void case_host_only_no_mtp(void)
{
    begin("host_only_no_mtp");
    hv_input_enable();
    EQ(attach(HV_INPUT_SRC_HOST, 0, 0), 0);
    hv_input_idle_poll();
    enter();
    push_keys(0, 0x04);
    hv_input_poll();
    hv_input_idle_poll();
    stub_cntpct += 3 * stub_cntfrq;
    hv_input_report();
    EQ(hv_input_proxy(HV_INPUT_OP_PROBE, 100, 0, 0), HV_INPUT_ERROR);
    hv_input_guest_exited(0);
    EQ(mtp_attaches + mtp_polls + mtp_probes + mtp_reports, 0);
    EQ(hv_input_proxy(HV_INPUT_OP_QUERY, HV_INPUT_Q_FEATURES, 0, 0), 0x1000f);
    end();
}

/* I1 G13: ATTACH with the keyboard: the area and the Fn option reach the MTP glue; FEATURES
 * bit 17 only when it came up; a failed MTP keeps the host source. */
static void case_kbd_attach(void)
{
    begin("kbd_attach");
    hv_input_enable();
    EQ(attach(HV_INPUT_SRC_HOST | HV_INPUT_SRC_KEYBOARD | HV_INPUT_OPT_FN_FKEYS, 0x10f00000000UL,
              0x800000),
       0);
    EQ(mtp_attaches, 1);
    EQ(mtp_area, 0x10f00000000UL);
    EQ(mtp_size, 0x800000);
    CHECK(mtp_fkeys);
    EQ(st(HV_INPUT_S_ATTACHED), 3);
    EQ(hv_input_proxy(HV_INPUT_OP_QUERY, HV_INPUT_Q_FEATURES, 0, 0), 0x3000f);
    EQ(st(HV_INPUT_S_MTP_STATE), 0x100 + 23);
    EQ(st(HV_INPUT_S_MTP_GPIO), 0x100 + 31);
    CHECK(hv_input_owns_vuart_rx());
    EQ(hv_input_verify(), 0);
    /* PROBE before entry only. */
    EQ(hv_input_proxy(HV_INPUT_OP_PROBE, 1500, 0, 0), 1500);
    enter();
    EQ(hv_input_proxy(HV_INPUT_OP_PROBE, 1500, 0, 0), HV_INPUT_ERROR);
    EQ(mtp_probes, 1);
    end();

    begin("kbd_attach_down");
    hv_input_enable();
    mtp_up = false;
    EQ(attach(HV_INPUT_SRC_HOST | HV_INPUT_SRC_KEYBOARD, 0x10f00000000UL, 0x800000), 0);
    CHECK(!mtp_fkeys);
    EQ(hv_input_proxy(HV_INPUT_OP_QUERY, HV_INPUT_Q_FEATURES, 0, 0), 0x1000f);
    CHECK(hv_input_owns_vuart_rx());
    EQ(attach(HV_INPUT_SRC_HOST | HV_INPUT_SRC_KEYBOARD, 0x10f00000000UL, 0x800000),
       (u64)(s64)HV_INPUT_E_STATE);
    EQ(mtp_attaches, 1);
    end();
}

/* I1 G14: the internal keyboard and the host merge; the idle path services but never drives
 * the SPI; the tick does; nothing is serviced before entry or after exit. */
static void case_kbd_merge(void)
{
    begin("kbd_merge");
    hv_input_enable();
    EQ(attach(HV_INPUT_SRC_HOST | HV_INPUT_SRC_KEYBOARD, 0x10f00000000UL, 0x800000), 0);
    hv_input_idle_poll();
    hv_input_poll();
    EQ(mtp_polls, 0);
    /* A key the internal keyboard held before entry is forgotten at entry. */
    script(0, 0x2c, 0);
    enter();
    g_wr(INPUT_REG_CTRL, INPUT_CTRL_ENABLE | INPUT_CTRL_IRQ_EN, 2);
    g_wr(INPUT_REG_POP, 1, 2);
    g_wr(INPUT_REG_POP, 1, 2);
    EQ(st(HV_INPUT_S_DEPTH), 0);
    int calls = spi_calls;
    hv_input_idle_poll(); /* delivers the scripted state: queued, line not driven here */
    EQ(mtp_polls, 1);
    EQ(st(HV_INPUT_S_DEPTH), 1);
    EQ(spi_calls, calls);
    push_keys(0x02, 0x04);
    script(0x10, 0x2c, 0xe9);
    hv_input_poll(); /* the tick: MTP first, then the host feed, then the SPI */
    EQ(mtp_polls, 2);
    EQ(spi_calls, calls + 1);
    CHECK(spi_level_now);
    g_wr(INPUT_REG_POP, 1, 2); /* the idle path's report */
    /* Merged keyboard: both sources' modifiers and keys. */
    bool merged = false, volume = false;
    while (g_rd(INPUT_REG_REPORT_LEN, 2)) {
        u64 r = g_rd(INPUT_REG_REPORT, 3);
        /* id 1, Shift (host) | GUI (internal), usage 0x04 (host) and 0x2c (internal). */
        if ((r & 0xff) == 1 && ((r >> 8) & 0xff) == 0x12 && ((r >> 16) & 0x10) &&
            ((r >> 56) & 0x10))
            merged = true;
        if ((r & 0xff) == 4 && ((r >> 8) & 0xffff) == 0xe9)
            volume = true;
        g_wr(INPUT_REG_POP, 1, 2);
    }
    CHECK(merged && volume);
    EQ(st(INPUT_ST_MTP) >= 2, 1);
    /* Exit: both released, state only; no MTP service after it. */
    calls = spi_calls;
    hv_input_guest_exited(0);
    int polls = mtp_polls;
    hv_input_poll();
    hv_input_idle_poll();
    EQ(mtp_polls, polls);
    /* Host released (internal GUI + 0x2c left), then internal: all up, then consumer 0. */
    EQ(st(HV_INPUT_S_DEPTH), 3);
    g_wr(INPUT_REG_POP, 1, 2);
    u64 r = g_rd(INPUT_REG_REPORT, 3);
    EQ(r & 0xffffff, 0x000001);
    g_wr(INPUT_REG_POP, 1, 2);
    EQ(g_rd(INPUT_REG_REPORT, 2) & 0xffffff, 0x000004);
    EQ(spi_calls, calls); /* never driven after exit */
    end();
}

/* I2 G15: ATTACH with the trackpad: it reaches the MTP glue; FEATURES bit 18 only when the
 * trackpad came up (bit 17 alone when only the keyboard did); its pointer reports merge with
 * the host's relative buttons; entry and exit release it. */
static void case_tp_attach(void)
{
    begin("tp_attach");
    hv_input_enable();
    EQ(attach(HV_INPUT_SRC_HOST | HV_INPUT_SRC_KEYBOARD | HV_INPUT_SRC_TRACKPAD, 0x10f00000000UL,
              0x800000),
       0);
    CHECK(mtp_tp_asked);
    EQ(st(HV_INPUT_S_ATTACHED), 7);
    EQ(hv_input_proxy(HV_INPUT_OP_QUERY, HV_INPUT_Q_FEATURES, 0, 0), 0x7000f);
    /* A trackpad button held before entry is forgotten at entry. */
    ptr_buttons = 1;
    ptr_dx = 0;
    ptr_wheel = 0;
    ptr_pending = true;
    hv_mtp_t8152_poll();
    enter();
    g_wr(INPUT_REG_CTRL, INPUT_CTRL_ENABLE | INPUT_CTRL_IRQ_EN, 2);
    while (g_rd(INPUT_REG_REPORT_LEN, 2)) {
        u64 r = g_rd(INPUT_REG_REPORT, 3);
        CHECK((r & 0xff) != INPUT_RID_RELATIVE || !((r >> 8) & 0xff));
        g_wr(INPUT_REG_POP, 1, 2);
    }
    /* Motion with the right button and a wheel detent: report ID 3. */
    ptr_buttons = 2;
    ptr_dx = -5;
    ptr_wheel = -1;
    ptr_pending = true;
    hv_input_idle_poll();
    EQ(g_rd(INPUT_REG_REPORT_LEN, 2), 0x0308);
    u64 r = g_rd(INPUT_REG_REPORT, 3);
    EQ(r & 0xff, 3);
    EQ((r >> 8) & 0xff, 2);
    EQ((r >> 16) & 0xffff, 0xfffb);
    EQ((r >> 48) & 0xff, 0xff);
    g_wr(INPUT_REG_POP, 1, 2);
    /* Exit: released (state only). */
    hv_input_guest_exited(0);
    bool released = false;
    while (g_rd(INPUT_REG_REPORT_LEN, 2)) {
        r = g_rd(INPUT_REG_REPORT, 3);
        if ((r & 0xff) == 3 && !((r >> 8) & 0xff))
            released = true;
        g_wr(INPUT_REG_POP, 1, 2);
    }
    CHECK(released);
    end();

    begin("tp_attach_kbd_only");
    hv_input_enable();
    mtp_tp_up = false; /* the keyboard came up, the trackpad did not */
    EQ(attach(HV_INPUT_SRC_HOST | HV_INPUT_SRC_KEYBOARD | HV_INPUT_SRC_TRACKPAD, 0x10f00000000UL,
              0x800000),
       0);
    EQ(hv_input_proxy(HV_INPUT_OP_QUERY, HV_INPUT_Q_FEATURES, 0, 0), 0x3000f);
    end();
    begin("tp_attach_none");
    hv_input_enable();
    mtp_up = false; /* nothing came up: the trackpad's flag is not believed either */
    EQ(attach(HV_INPUT_SRC_HOST | HV_INPUT_SRC_KEYBOARD | HV_INPUT_SRC_TRACKPAD, 0x10f00000000UL,
              0x800000),
       0);
    EQ(hv_input_proxy(HV_INPUT_OP_QUERY, HV_INPUT_Q_FEATURES, 0, 0), 0x1000f);
    end();
    /* The keyboard alone never asks for the trackpad. */
    begin("kbd_attach_no_tp");
    hv_input_enable();
    EQ(attach(HV_INPUT_SRC_HOST | HV_INPUT_SRC_KEYBOARD, 0x10f00000000UL, 0x800000), 0);
    CHECK(!mtp_tp_asked);
    EQ(hv_input_proxy(HV_INPUT_OP_QUERY, HV_INPUT_Q_FEATURES, 0, 0), 0x3000f);
    end();
}

int main(void)
{
    case_attach_refusals();
    case_attach_once();
    case_poll_budget();
    case_line_sync();
    case_bad_width();
    case_watchdog();
    case_exit();
    case_pre_entry();
    case_status();
    case_host_only_no_mtp();
    case_kbd_attach();
    case_kbd_merge();
    case_tp_attach();
    return total_failures ? 1 : 0;
}
