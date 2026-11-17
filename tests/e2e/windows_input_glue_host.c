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
    EQ(attach(HV_INPUT_SRC_HOST | HV_INPUT_SRC_KEYBOARD, 0, 0), (u64)(s64)HV_INPUT_E_SOURCES);
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
    EQ(hv_input_proxy(HV_INPUT_OP_QUERY, HV_INPUT_Q_SOURCES, 0, 0), 1);
    EQ(hv_input_proxy(HV_INPUT_OP_QUERY, 6, 0, 0), HV_INPUT_ERROR);
    EQ(hv_input_proxy(7, 0, 0, 0), HV_INPUT_ERROR);
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
    for (int i = 0; i < 100; i++)
        push_keys(0, (u8)(0x04 + (i % 20))); /* 13 bytes each: 1300 bytes */
    hv_input_poll(); /* before entry: nothing read */
    EQ(rx_reads, 0);
    enter();
    int polls = 0;
    while (rx_head != rx_tail && polls < 100) {
        u32 before = rx_head;
        hv_input_poll();
        CHECK(rx_head - before <= HV_INPUT_RX_BUDGET);
        polls++;
    }
    EQ(polls, (1300 + 255) / 256);
    EQ(st(INPUT_ST_FRAMES_OK), 100);
    EQ(st(INPUT_ST_RX_BYTES), 1300);
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
    hv_input_guest_exited(2);
    EQ(st(HV_INPUT_S_DEPTH), 1);
    EQ(g_rd(INPUT_REG_REPORT, 3) & 0xffff, 0x0001);
    CHECK(hv_input_owns_vuart_rx()); /* until the carrier is reloaded: no guest reads it */
    EQ(st(INPUT_ST_FRAMES_OK), 1);
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
    EQ(st(23), HV_INPUT_ERROR);
    hv_input_report();
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
    case_status();
    return total_failures ? 1 : 0;
}
