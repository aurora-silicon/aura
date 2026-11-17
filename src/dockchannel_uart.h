/* SPDX-License-Identifier: MIT */

#ifndef DOCKCHANNEL_UART_H
#define DOCKCHANNEL_UART_H

#include "types.h"

int dockchannel_uart_init(void);

/* Free bytes in the TX FIFO, 0 without a DockChannel. */
u32 dockchannel_uart_tx_free(void);
void dockchannel_uart_set_tx_timeout(u32 ms);
void dockchannel_uart_putbyte(u8 c);
u8 dockchannel_uart_getbyte(void);

void dockchannel_uart_putchar(u8 c);
u8 dockchannel_uart_getchar(void);

void dockchannel_uart_write(const void *buf, size_t count);
size_t dockchannel_uart_read(void *buf, size_t count);

void dockchannel_uart_puts(const char *s);
bool dockchannel_uart_try_puts(const char *s);
int dockchannel_uart_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* DEBUGUSB_PROXY builds only. */
bool dockchannel_uart_host_seen(void);

#endif
