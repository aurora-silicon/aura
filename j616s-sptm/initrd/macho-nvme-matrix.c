// SPDX-License-Identifier: GPL-2.0-only
// macho-nvme-matrix: read-only O_DIRECT tests that separate the classes of the NVMe stop under the monitor.
// Every read is compared with a 4 KiB reference read of the same blocks. A step that does not return in STEP_S ends the run: the kernel is in a reset.
// usage: macho-nvme-matrix [-k] [DEV]   -k also writes the lines to /dev/kmsg
#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#define LBA 4096UL
#define FRAME 16384UL
#define STEP_S 60
#define LBA0 17708544UL                 /* == 0 mod 4 */
#define LBA1 17708665UL                 /* == 1 mod 4, F17 stop; p3 starts == 1 mod 4 */

static int fd, kmsg = -1;

static void say(const char *f, ...)
{
	char b[256];
	va_list a;
	va_start(a, f);
	int n = vsnprintf(b, sizeof(b) - 1, f, a);
	va_end(a);
	b[n++] = '\n';
	if (write(1, b, n) < 0) {}
	if (kmsg >= 0 && write(kmsg, b, n) < 0) {}
}

static uint8_t *map_pages(size_t n, int shared)
{
	uint8_t *p = mmap(0, n, PROT_READ | PROT_WRITE, (shared ? MAP_SHARED : MAP_PRIVATE) | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
	if (p == MAP_FAILED)
		_exit(90);
	memset(p, 0xa5, n);
	return p;
}

/* compares [buf, buf + n) with single 4 KiB reads; returns the number of differing blocks, -1 on a read error */
static int verify(const uint8_t *buf, uint64_t lba, size_t n)
{
	static uint8_t *one;
	int bad = 0;
	if (!one)
		one = map_pages(FRAME, 0);
	for (size_t o = 0; o < n; o += LBA)
		if (pread(fd, one, LBA, (lba * LBA) + o) != (ssize_t)LBA)
			return -1;
		else if (memcmp(one, buf + o, LBA))
			bad++;
	return bad;
}

static int check(const char *what, const uint8_t *buf, uint64_t lba, size_t n, ssize_t got)
{
	if (got != (ssize_t)n) {
		say("NVME-MX: %s lba %lu read %zd of %zu", what, (unsigned long)lba, got, n);
		return 1;
	}
	int v = verify(buf, lba, n);
	if (v) {
		say("NVME-MX: %s lba %lu verify %d blocks differ", what, (unsigned long)lba, v);
		return 1;
	}
	return 0;
}

static uint64_t pfn_of(const void *v)
{
	static int pm = -2;
	uint64_t e = 0;
	if (pm == -2)
		pm = open("/proc/self/pagemap", O_RDONLY);
	if (pm < 0 || pread(pm, &e, 8, ((uintptr_t)v / sysconf(_SC_PAGESIZE)) * 8) != 8)
		return 0;
	return e & ((1ULL << 55) - 1);
}

/* T8: a buffer above 16 GiB of the guest RAM. Occupies 20 GiB first so the buddy allocator gives high frames, then reads into frames it reports. */
static int segments(const char *t, uint64_t lba, size_t l0, size_t l1, size_t l2);
static int t8_high(void)
{
	size_t ps = sysconf(_SC_PAGESIZE), big = 20UL << 30;
	uint8_t *hog = mmap(0, big, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
	if (hog == MAP_FAILED) {
		say("NVME-MX: T8 no 20 GiB to occupy");
		return 1;
	}
	uint8_t *b = map_pages(64 * ps, 0);
	uint64_t lo = ~0ULL, hi = 0;
	for (int i = 0; i < 64; i++) {
		uint64_t p = pfn_of(b + i * ps) * ps;
		if (p < lo) lo = p;
		if (p > hi) hi = p;
	}
	say("NVME-MX: T8 buffer pa %lx .. %lx (0 = no pagemap)", (long)lo, (long)hi);
	int r = check("T8", b, LBA0, 64 * 1024, pread(fd, b, 64 * 1024, LBA0 * LBA));
	r |= segments("T8s", LBA1, 16384, 16384, 16384);
	munmap(hog, big);
	return r;
}

static int t1_wrap(void)
{
	uint8_t *b = map_pages(FRAME, 0);
	for (int i = 0; i < 200; i++)
		if (pread(fd, b, LBA, (LBA0 + i) * LBA) != (ssize_t)LBA) {
			say("NVME-MX: T1 read %d failed", i);
			return 1;
		}
	return 0;
}

static int contiguous(const char *t, uint64_t lba)
{
	uint8_t *b = map_pages(64 * 1024, 0);
	return check(t, b, lba, 64 * 1024, pread(fd, b, 64 * 1024, lba * LBA));
}

/* len0 + len1 + len2 bytes into three frames that are not next to each other physically */
static int segments(const char *t, uint64_t lba, size_t l0, size_t l1, size_t l2)
{
	size_t ps = sysconf(_SC_PAGESIZE), n = 64 * ps;
	uint8_t *b = map_pages(n, 0), *f[3] = { 0 }, *out = map_pages(l0 + l1 + l2 + ps, 0);
	int k = 0;
	for (size_t o = 0; o < n && k < 3; o += ps) {
		uint64_t p = pfn_of(b + o);
		if (k == 0 || (p != pfn_of(f[k - 1]) + 1 && p + 1 != pfn_of(f[k - 1])))
			f[k++] = b + o;
	}
	if (k < 3 || !pfn_of(f[0])) {         /* no PFNs (pagemap off or unprivileged): far apart and out of order */
		f[0] = b + 40 * ps;
		f[1] = b + 10 * ps;
		f[2] = b + 25 * ps;
	}
	struct iovec v[3] = { { f[0], l0 }, { f[1], l1 }, { f[2], l2 } };
	ssize_t got = preadv(fd, v, 3, lba * LBA);
	memcpy(out, f[0], l0);
	memcpy(out + l0, f[1], l1);
	memcpy(out + l0 + l1, f[2], l2);
	say("NVME-MX: %s pfn %lx %lx %lx", t, (long)pfn_of(f[0]), (long)pfn_of(f[1]), (long)pfn_of(f[2]));
	return check(t, out, lba, l0 + l1 + l2, got);
}

static int both(int (*fn)(int))
{
	pid_t c = fork();
	if (c == 0)
		_exit(fn(1));
	int r = fn(0), st = 1;
	waitpid(c, &st, 0);
	return r || !WIFEXITED(st) || WEXITSTATUS(st);
}

static int t6_one(int who)
{
	uint8_t *b = map_pages(64 * 1024, 0);
	for (int i = 0; i < 100; i++) {
		uint64_t lba = (who ? LBA1 : LBA0) + i * 16 + who * 4096;
		if (pread(fd, b, 32 * 1024, lba * LBA) != 32 * 1024) {
			say("NVME-MX: T6 proc %d read %d failed", who, i);
			return 1;
		}
	}
	return 0;
}

static uint8_t *shared_frame;
static int t7_one(int who)
{
	for (int i = 0; i < 300; i++) {
		uint64_t lba = LBA1 + i * 2 + who;
		if (pread(fd, shared_frame + who * LBA, LBA, lba * LBA) != (ssize_t)LBA) {
			say("NVME-MX: T7 proc %d read %d failed", who, i);
			return 1;
		}
	}
	return 0;
}

static int step_t1(void) { return t1_wrap(); }
static int step_t2(void) { return contiguous("T2", LBA0); }
static int step_t3(void) { return contiguous("T3", LBA1); }
static int step_t4(void) { return segments("T4", LBA0, 16384, 16384, 16384); }
static int step_t5(void) { return segments("T5", LBA1, 16384, 16384, 16384); }
static int step_t4b(void) { return segments("T4b", LBA0, 4096, 8192, 4096); }
static int step_t5b(void) { return segments("T5b", LBA1, 4096, 8192, 4096); }
static int step_t6(void) { return both(t6_one); }
static int step_t8(void) { return t8_high(); }
/* T9: more readers than tags (63), every CPU that is online, random offsets in p3, content of every 8th read checked */
static int t9_one(int who)
{
	uint8_t *b = map_pages(64 * 1024, 0);
	uint64_t x = 0x9e3779b97f4a7c15ULL * (who + 1);
	for (int i = 0; i < 64; i++) {
		x ^= x << 13; x ^= x >> 7; x ^= x << 17;
		uint64_t lba = LBA1 + (x % (8UL << 20)) * 16;      /* at most 134 M blocks above LBA1, inside p3 (ends at 240802351) */
		size_t n = (1 + x % 16) * LBA;
		if (pread(fd, b, n, lba * LBA) != (ssize_t)n)
			return 1;
		if (!(i & 7) && verify(b, lba, n))
			return 1;
	}
	return 0;
}
static int step_t9(void)
{
	pid_t c[70];
	int bad = 0, st;
	for (int i = 0; i < 70; i++) {
		c[i] = fork();
		if (c[i] == 0)
			_exit(t9_one(i));
	}
	for (int i = 0; i < 70; i++)
		if (waitpid(c[i], &st, 0) < 0 || !WIFEXITED(st) || WEXITSTATUS(st))
			bad++;
	if (bad)
		say("NVME-MX: T9 %d of 70 readers failed", bad);
	return !!bad;
}
static int step_t7(void) { shared_frame = map_pages(FRAME, 1); return both(t7_one); }

static const struct { const char *name; int (*fn)(void); } steps[] = {
	{ "T1 200x4K", step_t1 }, { "T2 64K lba0", step_t2 }, { "T3 64K lba1", step_t3 },
	{ "T4 3seg48K lba0", step_t4 }, { "T5 3seg48K lba1", step_t5 }, { "T4b 3seg16K lba0", step_t4b }, { "T5b 3seg16K lba1", step_t5b },
	{ "T6 2proc", step_t6 }, { "T7 2proc same frame", step_t7 }, { "T8 high frames", step_t8 }, { "T9 70 readers", step_t9 },
};

int main(int ac, char **av)
{
	const char *dev = "/dev/nvme0n1";
	for (int i = 1; i < ac; i++) {
		if (!strcmp(av[i], "-k"))
			kmsg = open("/dev/kmsg", O_WRONLY);
		else
			dev = av[i];
	}
	fd = open(dev, O_RDONLY | O_DIRECT);
	if (fd < 0) {
		say("NVME-MX: cannot open %s", dev);
		return 2;
	}
	for (unsigned s = 0; s < sizeof(steps) / sizeof(steps[0]); s++) {
		pid_t c = fork();
		if (c == 0)
			_exit(steps[s].fn());
		int st = 0, done = 0;
		for (int t = 0; t < STEP_S * 10 && !done; t++) {
			done = waitpid(c, &st, WNOHANG) == c;
			if (!done)
				usleep(100000);
		}
		if (!done) {
			say("NVME-MX: %s HANG after %d s, stop", steps[s].name, STEP_S);
			kill(c, SIGKILL);
			return 3;
		}
		say("NVME-MX: %s %s", steps[s].name, WIFEXITED(st) && !WEXITSTATUS(st) ? "ok" : "FAIL");
	}
	say("NVME-MX: done");
	return 0;
}
