// SPDX-License-Identifier: GPL-2.0-only
// Host console: a RAM log, replayed to the fb and the SoC debug UART (Samsung/Exynos-style, as qemu-sptm models
// uart0) once they are mapped. On HW neither is mapped yet (see main.c), so the log stays in RAM.
#include "mon.h"

#define UART0_PA 0x429200000UL           /* ADT /arm-io/uart0 reg 0x229200000 + arm-io base 0x200000000 */
static u64 uart;
static char ring[1 << 16];
static u64 rn;
static int live;

void con_init(u64 *boot_args)
{
	(void)boot_args;
	uart = hmap(UART0_PA, 1);
}

static void out(char c)
{
	fb_putc(c);
	if (!uart)
		return;
	if (c == '\n')
		out('\r');
	int i = 0;
	while (i < 100000 && !(r32(uart + 0x10) & 2))              /* UTRSTAT.TX buffer empty */
		i++;
	if (i == 100000) {                                          /* no clock/cable: stop waiting per char */
		uart = 0;
		return;
	}
	w32(uart + 0x20, (u8)c);                                    /* UTXH */
}

static volatile u32 g_ring_lock;

void con_putc(char c)
{
	spin_lock(&g_ring_lock);
	ring[rn++ % sizeof(ring)] = c;
	if (live)
		out(c);
	spin_unlock(&g_ring_lock);
}

/* the fault may have hit inside the console */
void con_fault_unlock(void)
{
	spin_unlock(&g_ring_lock);
}

/* the devices are mapped: print the log so far (its last 64 KiB), then go live */
void con_replay(void)
{
	for (u64 i = rn > sizeof(ring) ? rn - sizeof(ring) : 0; i < rn; i++)
		out(ring[i % sizeof(ring)]);
	live = 1;
}

/* the last max bytes of the log, for the guest stub */
u64 con_snapshot(char *dst, u64 max)
{
	u64 n = rn < max ? rn : max;
	for (u64 i = 0; i < n; i++)
		dst[i] = ring[(rn - n + i) % sizeof(ring)];
	return n;
}

void con_puts(const char *s)
{
	while (*s)
		con_putc(*s++);
}

void con_hex(u64 v)
{
	con_puts("0x");
	for (int i = 60; i >= 0; i -= 4)
		con_putc("0123456789abcdef"[(v >> i) & 0xf]);
}
