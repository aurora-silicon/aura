/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#include "../build/build_cfg.h"

#include <stdarg.h>

#include "dockchannel_uart.h"
#include "adt.h"
#include "iodev.h"
#include "types.h"
#include "utils.h"
#include "vsprintf.h"

#ifdef DEBUGUSB_PROXY
#include "stage1_proxy.h"
#endif

#define DATA_TX8      0x4004
#define DATA_TX_FREE  0x4014
#define DATA_RX8      0x401c
#define DATA_RX_COUNT 0x402c

static u64 uart_base = 0;

#ifdef DEBUGUSB_PROXY
/* TX FIFO accounting for dockchannel_uart_host_seen(). */
static u32 tx_free_mask = ~0u;
static u32 tx_free_at_init;
static u64 tx_accepted;
#define TX_ACCEPTED(n) (tx_accepted += (n))
#else
#define TX_ACCEPTED(n) ((void)0)
#endif

#ifndef DOCKCHANNEL_UART_CHANNEL
#define DOCKCHANNEL_UART_CHANNEL 1
#endif

int dockchannel_uart_init(void)
{
    int path[8];

    int node = adt_path_offset_trace(adt, "/arm-io/dockchannel-uart", path);
    if (node < 0)
        return -1;

    if (adt_get_reg(adt, path, "reg", 0, &uart_base, NULL)) {
        printf("!!! Failed to get dockchannel UART reg property!\n");
        return -1;
    }

    int channel = DOCKCHANNEL_UART_CHANNEL;
    int chosen = adt_path_offset(adt, "/chosen");
    u32 chip;
    if (chosen >= 0 && ADT_GETPROP(adt, chosen, "chip-id", &chip) >= 0 && chip == T8140)
        channel = 0;
    uart_base += channel * 0x10000;

#ifdef DEBUGUSB_PROXY
    u32 mask;
    if (ADT_GETPROP(adt, node, "dock-wstat-mask", &mask) >= 0 && mask)
        tx_free_mask = mask;
    tx_free_at_init = read32(uart_base + DATA_TX_FREE) & tx_free_mask;
#endif

    printf("Initialized dockchannel UART channel %d at 0x%lx\n", channel, uart_base);

    return 0;
}

void dockchannel_uart_putbyte(u8 c)
{
    if (!uart_base)
        return;

    /*
     * A stalled or undrained FIFO must not block every printf. Wait up to
     * 1 s, which is long enough for a host that drains in bursts not to
     * lose proxy replies, then drop the byte.
     */
    u64 deadline = mrs(CNTPCT_EL0) + mrs(CNTFRQ_EL0);
    while (read32(uart_base + DATA_TX_FREE) == 0) {
        if ((s64)(mrs(CNTPCT_EL0) - deadline) >= 0)
            return;
    }

    write32(uart_base + DATA_TX8, c);
    TX_ACCEPTED(1);
}

u8 dockchannel_uart_getbyte(void)
{
    if (!uart_base)
        return 0;

    while (read32(uart_base + DATA_RX_COUNT) == 0)
        ;

    return read32(uart_base + DATA_RX8) >> 8;
}

void dockchannel_uart_putchar(u8 c)
{
    if (c == '\n')
        dockchannel_uart_putbyte('\r');

    dockchannel_uart_putbyte(c);
}

u8 dockchannel_uart_getchar(void)
{
    return dockchannel_uart_getbyte();
}

void dockchannel_uart_puts(const char *s)
{
    while (*s)
        dockchannel_uart_putchar(*(s++));

    dockchannel_uart_putchar('\n');
}

void dockchannel_uart_write(const void *buf, size_t count)
{
    const u8 *p = buf;

    while (count--)
        dockchannel_uart_putbyte(*p++);
}

size_t dockchannel_uart_read(void *buf, size_t count)
{
    u8 *p = buf;
    size_t recvd = 0;

    while (count--) {
        *p++ = dockchannel_uart_getbyte();
        recvd++;
    }

    return recvd;
}

int dockchannel_uart_printf(const char *fmt, ...)
{
    va_list args;
    char buffer[512];
    int i;

    va_start(args, fmt);
    i = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    dockchannel_uart_write(buffer, min(i, (int)(sizeof(buffer) - 1)));

    return i;
}

static bool dockchannel_uart_iodev_can_write(void *opaque)
{
    UNUSED(opaque);

    if (!uart_base)
        return false;

    return read32(uart_base + DATA_TX_FREE) > 0;
}

static ssize_t dockchannel_uart_iodev_can_read(void *opaque)
{
    UNUSED(opaque);

    if (!uart_base)
        return 0;

    return read32(uart_base + DATA_RX_COUNT);
}

static ssize_t dockchannel_uart_iodev_read(void *opaque, void *buf, size_t len)
{
    UNUSED(opaque);
    return dockchannel_uart_read(buf, len);
}

static ssize_t dockchannel_uart_iodev_write(void *opaque, const void *buf, size_t len)
{
    UNUSED(opaque);
    dockchannel_uart_write(buf, len);
    return len;
}

static ssize_t dockchannel_uart_iodev_write_nonblocking(void *opaque, const void *buf, size_t len)
{
    UNUSED(opaque);

    if (!uart_base)
        return 0;

    size_t count = min(len, (size_t)read32(uart_base + DATA_TX_FREE));
    const u8 *p = buf;
    for (size_t i = 0; i < count; i++)
        write32(uart_base + DATA_TX8, p[i]);
    TX_ACCEPTED(count);

    return count;
}

static ssize_t dockchannel_uart_iodev_write_atomic(void *opaque, const void *buf, size_t len)
{
    UNUSED(opaque);

    if (!uart_base || read32(uart_base + DATA_TX_FREE) < len)
        return 0;

    const u8 *p = buf;
    for (size_t i = 0; i < len; i++)
        write32(uart_base + DATA_TX8, p[i]);
    TX_ACCEPTED(len);

    return len;
}

static struct iodev_ops iodev_dockchannel_uart_ops = {
    .can_read = dockchannel_uart_iodev_can_read,
    .can_write = dockchannel_uart_iodev_can_write,
    .read = dockchannel_uart_iodev_read,
    .write = dockchannel_uart_iodev_write,
    .write_nonblocking = dockchannel_uart_iodev_write_nonblocking,
    .write_atomic = dockchannel_uart_iodev_write_atomic,
};

struct iodev iodev_dockchannel_uart = {
    .ops = &iodev_dockchannel_uart_ops,
    .usage = USAGE_CONSOLE | USAGE_UARTPROXY,
    .lock = SPINLOCK_INIT,
};

#ifdef DEBUGUSB_PROXY
/*
 * True when a host has read this channel since this stage started, which needs a debugusb link
 * (a VDM `reboot debugusb`) and a reader such as kisd. Two register reads: no wait on a normal
 * boot.
 */
bool dockchannel_uart_host_seen(void)
{
    if (!uart_base)
        return false;
    return stage1_proxy_dockchannel_host(tx_free_at_init, tx_accepted,
                                         read32(uart_base + DATA_TX_FREE) & tx_free_mask,
                                         read32(uart_base + DATA_RX_COUNT));
}
#endif
