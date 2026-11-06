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
static bool hv_windows_serial_cdc_enabled(void)
{
    return false;
}
void hv_windows_serial_putc(u8 byte)
{
    assert(capture_count < sizeof(capture_bytes));
    capture_bytes[capture_count++] = byte;
}
#include "../../src/ringbuffer.c"
#define CDC_ACM_PIPE_1 1
#define SZ_16K         16384
#include "../../src/usb_cdc_bulk.h"
#if USB_DWC3_SUPERSPEED
#define XFER_SIZE                 CDC_BULK_CHAIN_BYTES
#define CDC_LOOP_READY(dev, pipe) ((dev)->pipe[(pipe)].ready)
#else
#define XFER_SIZE                 SZ_16K
#define CDC_LOOP_READY(dev, pipe) true
#endif
#define CDC_IO_TIMEOUT_MS      3000
#define USB_LEP_CDC_BULK_OUT   4
#define USB_LEP_CDC_BULK_OUT_2 8
typedef int cdc_acm_pipe_id_t;
typedef struct {
    bool failed;
    struct {
        bool ready;
        ringbuffer_t *device2host, *host2device;
        u8 ep_in, ep_out;
    } pipe[2];
    struct {
        bool xfer_in_progress;
        u8 *xfer_buffer;
    } endpoints[16];
} dwc3_dev_t;
static unsigned out_starts, not_ready_events, received_bytes;
static bool out_trb_available = true, pending_not_ready, pending_host_byte;
static size_t pending_host_length = 1;
static bool usb_dwc3_trb_available(dwc3_dev_t *dev, u8 ep)
{
    (void)dev;
    assert(ep == USB_LEP_CDC_BULK_OUT_2);
    return out_trb_available;
}
static ringbuffer_t *usb_dwc3_cdc_get_ringbuffer(dwc3_dev_t *dev, u8 ep)
{
    assert(ep == USB_LEP_CDC_BULK_OUT_2);
    return dev->pipe[1].host2device;
}
static int usb_dwc3_start_bulk_transfer(dwc3_dev_t *dev, u8 ep, size_t count)
{
    assert(ep == USB_LEP_CDC_BULK_OUT_2 && count == XFER_SIZE);
    assert(dev->pipe[1].ready && !dev->failed);
    assert(!dev->endpoints[ep].xfer_in_progress);
    dev->endpoints[ep].xfer_in_progress = true;
    ++out_starts;
    return 0;
}
static void usb_dwc3_handle_events(dwc3_dev_t *dev);
/* Whole production OUT starter, can_read and read functions, extracted by runner. */
#include "production_rx.inc"
static void usb_dwc3_handle_events(dwc3_dev_t *dev)
{
    ++polls;
    if (pending_not_ready) {
        pending_not_ready = false;
        ++not_ready_events;
        /* The real XFERNOTREADY branch consumes the event and calls this. */
        usb_dwc3_cdc_start_bulk_out_xfer(dev, USB_LEP_CDC_BULK_OUT_2);
    }
    if (pending_host_byte && dev->pipe[1].ready &&
        dev->endpoints[USB_LEP_CDC_BULK_OUT_2].xfer_in_progress) {
        /* Model one successful short OUT DMA completion, without another NRDY. */
        const u8 command = 'R';
        const u8 *bytes = pending_host_length == 1 ? &command : dev->endpoints[8].xfer_buffer;
        assert(ringbuffer_write(bytes, pending_host_length, dev->pipe[1].host2device) ==
               pending_host_length);
        dev->endpoints[USB_LEP_CDC_BULK_OUT_2].xfer_in_progress = false;
        pending_host_byte = false;
        received_bytes += pending_host_length;
    }
}
static void usb_dwc3_cdc_start_bulk_in_xfer(dwc3_dev_t *dev, u8 ep)
{
    ++kicks;
    assert(ep == 9);
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
static ssize_t secondary_can_read(void *dev)
{
    return usb_dwc3_can_read(dev, CDC_ACM_PIPE_1);
}
static ssize_t secondary_read(void *dev, void *buf, size_t count)
{
    return usb_dwc3_read(dev, CDC_ACM_PIPE_1, buf, count);
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
static const struct iodev_ops secondary_ops = {.can_read = secondary_can_read,
                                               .can_write = secondary_ready,
                                               .read = secondary_read,
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
static bool check_receive_rearm(dwc3_dev_t *dev)
{
    u8 ep = dev->pipe[1].ep_out;
    /* Model a closed pipe whose prior transfer has completed or been ended. */
    dev->pipe[1].ready = false;
    dev->endpoints[ep].xfer_in_progress = false;
    unsigned starts = out_starts;
    pending_not_ready = pending_host_byte = true;
    usb_dwc3_vuart_poll(dev);
    assert(not_ready_events == 1 && !pending_not_ready && out_starts == starts);
    assert(!usb_dwc3_can_read(dev, CDC_ACM_PIPE_1));

    /* No second NRDY: the host's byte can complete only after polling primes OUT. */
    dev->pipe[1].ready = true;
    read_reg(UTRSTAT);
    u64 status = read_reg(UTRSTAT);
#ifdef EXPECT_UNFIXED
    assert(!(status & UTRSTAT_RXD) && out_starts == starts && pending_host_byte);
    return false;
#else
    assert((status & UTRSTAT_RXD) && received_bytes == 1 && !pending_host_byte);
    assert(read_reg(URXH) == 'R' && !usb_dwc3_can_read(dev, CDC_ACM_PIPE_1));
    starts = out_starts;
    for (int i = 0; i < 3; i++)
        usb_dwc3_vuart_poll(dev);
    assert(out_starts == starts); /* Already active: no duplicate command. */

    dev->endpoints[ep].xfer_in_progress = false;
    out_trb_available = false;
    usb_dwc3_vuart_poll(dev);
    assert(out_starts == starts);
    out_trb_available = true;
    ringbuffer_t *rx = dev->pipe[1].host2device;
    dev->pipe[1].host2device = NULL;
    usb_dwc3_vuart_poll(dev);
    assert(out_starts == starts);
    dev->pipe[1].host2device = rx;
    dev->failed = true;
    usb_dwc3_vuart_poll(dev);
    assert(out_starts == starts);
    dev->failed = false;

    u8 fill[XFER_SIZE + 1];
    memset(fill, 'A', sizeof(fill));
    assert(ringbuffer_write(fill, sizeof(fill), rx) == sizeof(fill));
    assert(ringbuffer_get_free(rx) == XFER_SIZE - 1);
    usb_dwc3_vuart_poll(dev);
    assert(out_starts == starts); /* RX queue cannot hold a complete transfer. */
    assert(read_reg(URXH) == 'A');
    assert(out_starts == starts); /* Exactly XFER_SIZE free includes the reserved slot. */
    assert(read_reg(URXH) == 'A');
    assert(out_starts == starts + 1); /* XFER_SIZE usable slots now exist. */
    for (size_t i = 0; i < XFER_SIZE; ++i)
        dev->endpoints[ep].xfer_buffer[i] = (u8)i;
    pending_host_length = XFER_SIZE;
    pending_host_byte = true;
    usb_dwc3_vuart_poll(dev); /* Complete every byte of a maximum-size OUT transfer. */
    assert(usb_dwc3_can_read(dev, CDC_ACM_PIPE_1) == 2 * XFER_SIZE - 1);
    u8 received[2 * XFER_SIZE - 1];
    assert(usb_dwc3_read(dev, CDC_ACM_PIPE_1, received, sizeof(received)) == sizeof(received));
    for (size_t i = 0; i < XFER_SIZE - 1; ++i)
        assert(received[i] == 'A');
    for (size_t i = 0; i < XFER_SIZE; ++i)
        assert(received[XFER_SIZE - 1 + i] == (u8)i);
    assert(not_ready_events == 1); /* All progress after reopen was independent of NRDY. */
    return true;
#endif
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    dwc3_dev_t dev = {0};
    dev.pipe[1].ep_in = 9;
    dev.pipe[1].ep_out = USB_LEP_CDC_BULK_OUT_2;
    u8 rx_dma[XFER_SIZE];
    dev.endpoints[USB_LEP_CDC_BULK_OUT_2].xfer_buffer = rx_dma;
    dev.pipe[1].device2host = ringbuffer_alloc(8); /* Seven usable slots. */
    assert(dev.pipe[1].device2host);
    dev.pipe[1].host2device = ringbuffer_alloc(2 * XFER_SIZE);
    assert(dev.pipe[1].host2device);
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
    bool rx_rearmed = check_receive_rearm(&dev);
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
    ringbuffer_free(dev.pipe[1].host2device);
    printf(
        "{\"passed\":true,\"host_bytes\":%zu,\"capture_bytes\":%zu,\"overruns\":%llu,\"polls\":%u,"
        "\"rx_rearmed\":%s,\"rx_starts\":%u,\"received_bytes\":%u,\"not_ready_events\":%u}\n",
        host_count, capture_count, (unsigned long long)vuart_tx_overruns, polls,
        rx_rearmed ? "true" : "false", out_starts, received_bytes, not_ready_events);
}
