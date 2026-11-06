/* SPDX-License-Identifier: MIT */
/* Failure-first UART -> I/O -> production CDC queue replay, no physical USB. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#define AURA_WINDOWS
#define TYPES_H
#define UTILS_H
#define MEMORY_H
#define HV_H
#define __EXCEPTION_H__
#define USB_H
#define AIC_H
#define UART_H
#define MALLOC_H
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef int64_t s64;
#define UNUSED(x)        ((void)(x))
#define BIT(x)           (1ULL << (x))
#define GENMASK(h, l)    (((1ULL << ((h) - (l) + 1)) - 1) << (l))
#define FIELD_GET(f, v)  (((v) & (f)) / ((f) & -(f)))
#define FIELD_PREP(f, v) (((v) * ((f) & -(f))) & (f))
#define min(a, b)        ((a) < (b) ? (a) : (b))
#define max(a, b)        ((a) > (b) ? (a) : (b))
#define ARRAY_SIZE(a)    (sizeof(a) / sizeof((a)[0]))
#define SPINLOCK_INIT    0
typedef int spinlock_t;
#define DECLARE_SPINLOCK(name) spinlock_t name = SPINLOCK_INIT
/* Aura locks are recursive on one CPU (utils.c); retain depth in this model. */
static void spin_lock(int *lock)
{
    ++*lock;
}
static void spin_unlock(int *lock)
{
    assert(*lock > 0);
    --*lock;
}
static bool mmu_active(void)
{
    return true;
}
static bool is_boot_cpu(void)
{
    return true;
}
static void usb_init(void)
{
}
static void usb_iodev_init(void)
{
}
static u64 get_ticks(void)
{
    return 0;
}
static u64 ticks_to_msecs(u64 ticks)
{
    return ticks;
}
static void write8(u64 a, u8 v)
{
    (void)a;
    (void)v;
}
bool monitor_active;
static u8 host_bytes[4096], capture_bytes[4096];
static size_t host_count, capture_count;
static unsigned polls, kicks;
static bool draining;
static bool hv_windows_serial_capture_enabled(void)
{
    return true;
}
void hv_windows_serial_putc(u8 byte)
{
    assert(capture_count < sizeof(capture_bytes));
    capture_bytes[capture_count++] = byte;
}
#include "../../src/ringbuffer.c"
#define CDC_ACM_PIPE_1 1
typedef struct {
    bool failed;
    struct {
        bool ready;
        ringbuffer_t *device2host;
        u8 ep_in;
    } pipe[2];
} dwc3_dev_t;
static void usb_dwc3_handle_events(dwc3_dev_t *dev)
{
    (void)dev;
    ++polls;
}
static void usb_dwc3_cdc_start_bulk_in_xfer(dwc3_dev_t *dev, u8 ep)
{
    ++kicks;
    assert(ep == 7);
    if (draining && dev->pipe[1].ready)
        host_count += ringbuffer_read(host_bytes + host_count, sizeof(host_bytes) - host_count,
                                      dev->pipe[1].device2host);
}
#include "../../src/iodev.h"
#include "../../src/usb_dwc3_vuart.inc"
static bool secondary_ready(void *dev)
{
    return usb_dwc3_vuart_capacity(dev) != 0;
}
static ssize_t secondary_write(void *dev, const void *buf, size_t count)
{
    unsigned before_polls = polls, before_kicks = kicks;
    size_t wrote = usb_dwc3_vuart_write(dev, buf, count);
    assert(polls == before_polls && kicks == before_kicks);
    return wrote;
}
static void secondary_poll(void *dev)
{
    usb_dwc3_vuart_poll(dev);
}
static ssize_t no_read(void *dev)
{
    (void)dev;
    return 0;
}
static ssize_t forbidden_write(void *dev, const void *buf, size_t size)
{
    (void)dev;
    (void)buf;
    (void)size;
    assert(!"blocking write reached");
    return -1;
}
static bool other_ready(void *dev)
{
    (void)dev;
    return true;
}
static const struct iodev_ops secondary_ops = {.can_read = no_read,
                                               .can_write = secondary_ready,
                                               .write = secondary_write,
                                               .handle_events = secondary_poll};
static const struct iodev_ops other_ops = {.can_write = other_ready, .write = forbidden_write};
struct iodev iodev_uart = {.ops = &other_ops}, iodev_fb = {.ops = &other_ops},
             iodev_log = {.ops = &other_ops}, iodev_dockchannel_uart = {.ops = &other_ops},
             iodev_usb_vuart = {.ops = &secondary_ops};
#include "../../src/iodev.c"
struct exc_info;
static void uart_clear_irqs(void)
{
    assert(!"physical IRQ touched");
}
static void aic_set_sw(int irq, bool on)
{
    (void)irq;
    (void)on;
}
static void usb_iodev_vuart_setup(iodev_id_t id)
{
    (void)id;
}
static int hv_map_hook(u64 base, bool (*fn)(struct exc_info *, u64, u64 *, bool, int), u64 size)
{
    (void)base;
    (void)fn;
    (void)size;
    return 0;
}
#include "../../src/hv/hv_vuart.c"
static void put_byte(u8 b)
{
    u64 v = b;
    assert(handle_vuart(NULL, UTXH, &v, true, 2));
}
static u64 read_reg(u64 reg)
{
    u64 v = 0;
    assert(handle_vuart(NULL, reg, &v, false, 2));
    return v;
}
static void check_ready(bool ready)
{
    assert(!!(read_reg(UTRSTAT) & UTRSTAT_TXBE) == ready);
    assert(!!(read_reg(UFSTAT) & UFSTAT_TXFULL) == !ready);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    dwc3_dev_t dev = {0};
    dev.pipe[1].ep_in = 7;
    dev.pipe[1].device2host = ringbuffer_alloc(8); /* Seven usable slots. */
    assert(dev.pipe[1].device2host);
    /* Debug polling must not flush another console's blocking backlog. */
    iodev_uart.usage = USAGE_CONSOLE;
    con_buf[0] = 'B';
    con_wp = 1;
    check_ready(true);
    put_byte('U');
    assert(vuart_tx_overruns == 0); /* Unbound. */
    iodev_usb_vuart.opaque = &dev;
    check_ready(true);
    put_byte('D');
    assert(host_count == 0); /* No DTR. */
    dev.pipe[1].ready = true;
    assert(usb_dwc3_vuart_capacity(&dev) == 7);
    for (u8 b = 'a'; b <= 'f'; ++b) {
        check_ready(true);
        put_byte(b);
    }
    assert(usb_dwc3_vuart_capacity(&dev) == 1);
    assert(usb_dwc3_vuart_write(&dev, "XY", 2) == 0); /* No partial enqueue. */
    assert(usb_dwc3_vuart_capacity(&dev) == 1);
    put_byte('g');
    check_ready(false);
    assert(usb_dwc3_vuart_capacity(&dev) == 0);
    put_byte('!');
    put_byte('?');
    check_ready(false);
    assert(vuart_tx_overruns == 2);
    assert(capture_count == 11 && !memcmp(capture_bytes, "UDabcdefg!?", 11));
    draining = true;
    check_ready(true);
    assert(host_count == 7 && !memcmp(host_bytes, "abcdefg", 7));
    draining = false;
    for (u8 b = 'h'; b <= 'n'; ++b)
        put_byte(b); /* Ring wraparound. */
    check_ready(false);
    dev.pipe[1].ready = false;
    check_ready(true);
    put_byte('C');
    assert(vuart_tx_overruns == 2); /* Closed DTR still discards. */
    dev.pipe[1].ready = true;
    draining = true;
    check_ready(true);
    assert(host_count == 14 && !memcmp(host_bytes, "abcdefghijklmn", 14));
    draining = false;
    put_byte('z'); /* Last byte must drain from poll alone. */
    assert(host_count == 14);
    draining = true;
    usb_dwc3_vuart_poll(&dev);
    assert(host_count == 15 && host_bytes[14] == 'z');
    dev.failed = true;
    check_ready(true);
    put_byte('F');
    assert(vuart_tx_overruns == 2);
    assert(console_lock == 0 && iodev_usb_vuart.lock == 0 && in_iodev == 0);
    FILE *f = fopen(argv[1], "wb");
    assert(f);
    assert(fwrite(host_bytes, 1, host_count, f) == host_count);
    fclose(f);
    ringbuffer_free(dev.pipe[1].device2host);
    printf("{\"passed\":true,\"host_bytes\":%zu,\"capture_bytes\":%zu,\"overruns\":%llu,\"polls\":%"
           "u}\n",
           host_count, capture_count, (unsigned long long)vuart_tx_overruns, polls);
}
