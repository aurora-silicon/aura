// SPDX-License-Identifier: GPL-2.0-only
// TSO check for the initrd and the running system: real selectors of include/uapi/linux/prctl.h, no libc.
#define PR_GET_MEM_MODEL 0x6d4d444cUL
#define PR_SET_MEM_MODEL 0x4d4d444cUL
static long sc(long n, long a, long b, long c, long d, long e)
{
	register long x8 __asm__("x8") = n, x0 __asm__("x0") = a, x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c, x3 __asm__("x3") = d, x4 __asm__("x4") = e;
	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4) : "memory");
	return x0;
}
static long fd = -1;
static void num(char *p, long v)
{
	char t[24];
	int n = 0;
	if (v < 0) { *p++ = '-'; v = -v; }
	do { t[n++] = '0' + v % 10; v /= 10; } while (v);
	while (n) *p++ = t[--n];
	*p = 0;
}
static void say(const char *a, long v)
{
	char b[128];
	int n = 0;
	while (*a) b[n++] = *a++;
	num(b + n, v);
	while (b[n]) n++;
	b[n++] = '\n';
	sc(64, fd < 0 ? 1 : fd, (long)b, n, 0, 0);
}
void _start(void)
{
	fd = sc(56, -100, (long)"/dev/kmsg", 1, 0, 0);
	long set = sc(167, PR_SET_MEM_MODEL, 1, 0, 0, 0);
	say(set == 0 ? "TSO-PROBE: PR_SET_MEM_MODEL TSO accepted, ret " : "TSO-PROBE: PR_SET_MEM_MODEL TSO REFUSED, ret ", set);
	say("TSO-PROBE: PR_GET_MEM_MODEL (1 = TSO) ", sc(167, PR_GET_MEM_MODEL, 0, 0, 0, 0));
	sc(93, 0, 0, 0, 0, 0);
	for (;;);
}
