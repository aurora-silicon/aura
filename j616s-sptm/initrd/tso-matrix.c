// SPDX-License-Identifier: GPL-2.0-only
// tso-matrix: per-core HW TSO check. For each core C and each role (writer, reader) against a partner:
//   control D/D until NEED events or CAP trials gives the core pair's rate, then T/T for 3x the trials must show 0: P(0 | not ordered) <= e^-90.
//   T/D and D/T show that each side's bit does its own half (store order, load order). Same run for MP with 128-bit accesses (MPV).
//   LB is reported only: 0 events on raw HW in every pair, so it separates nothing here.
// rc 0 all ordered, 1 a T/T violation, 2 no prctl, 3 a pair without enough control events.
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>
#define SET 0x4d4d444c
#define GET 0x6d4d444c
#define BATCH 2048
#define LINE 128
#define NEED 30ULL
#define CAP (1ULL << 29)
#define QCAP (1ULL << 26)
#define PROBE (1ULL << 21)
enum { MP, LB, MPV };
static const char *name[] = { "MP", "LB", "MPV" };
typedef struct { _Alignas(LINE) volatile uint64_t v[2]; } cell;
static cell *X, *Y;
static uint64_t *R0, *R1;
static _Alignas(LINE) volatile int bar;
static _Alignas(LINE) volatile int phase;
static int kind, cpu[2], mode[2];
static uint64_t rounds, bad;
static volatile int run_abort;
/* the logical numbers that really take a task: smp_early can stop early and the guard can offline a core while this runs */
static int cl[64], ncl;
/* macho F33: every pin is named on the panel before and after, straight into the kernel log (one write = one record, no
 * stdio buffer, no pipe): the last line names the core and the step if the machine stops inside sched_setaffinity. */
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
static void kmsg(const char *fmt, ...)
{
	static int fd = -2;
	char b[200];
	va_list ap;
	int n;

	if (fd == -2)
		fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	va_start(ap, fmt);
	n = vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	if (fd >= 0 && n > 0)
		(void)!write(fd, b, n < (int)sizeof(b) ? n : (int)sizeof(b) - 1);
}
static int pin(int c)
{
	cpu_set_t s;
	int r, from = sched_getcpu();
	CPU_ZERO(&s);
	CPU_SET(c, &s);
	kmsg("PIN: tso-matrix tid %d cpu%d -> cpu%d\n", (int)gettid(), from, c);
	r = sched_setaffinity(0, sizeof(s), &s);
	kmsg("PIN: tso-matrix tid %d now cpu%d rc %d\n", (int)gettid(), sched_getcpu(), r);
	return r;
}
static void cpus_scan(void)
{
	cpu_set_t all, on;
	CPU_ZERO(&all);
	for (int c = 0; c < 64; c++)
		CPU_SET(c, &all);
	sched_setaffinity(0, sizeof(all), &all);
	/* only the cores that are online: an offline core is not tried (no -EINVAL noise on the panel) */
	sched_getaffinity(0, sizeof(on), &on);
	ncl = 0;
	for (int c = 0; c < 64; c++)
		if (CPU_ISSET(c, &on) && pin(c) == 0)
			cl[ncl++] = c;
	sched_setaffinity(0, sizeof(all), &all);
	kmsg("PIN: tso-matrix scan done, %d cores\n", ncl);
}
static inline void store(volatile uint64_t *p, uint64_t v) { __asm__ volatile("str %x1, %0" : "=Q"(*p) : "r"(v)); }
static inline uint64_t load(volatile uint64_t *p) { uint64_t r; __asm__ volatile("ldr %x0, %1" : "=r"(r) : "Q"(*p)); return r; }
static inline void vstore(volatile uint64_t *p) { __asm__ volatile("movi v0.16b, #1; str q0, %0" : "=Q"(*p) : : "v0"); }
static inline uint64_t vload(volatile uint64_t *p) { uint64_t r; __asm__ volatile("ldr q0, %1; umov %x0, v0.d[0]" : "=r"(r) : "Q"(*p) : "v0"); return r; }
static void sync2(int *s)
{
	*s = !*s;
	if (__atomic_add_fetch(&bar, 1, __ATOMIC_SEQ_CST) == 2) {
		__atomic_store_n(&bar, 0, __ATOMIC_SEQ_CST);
		__atomic_store_n(&phase, *s, __ATOMIC_SEQ_CST);
	} else
		while (__atomic_load_n(&phase, __ATOMIC_SEQ_CST) != *s && !run_abort)
			;
}
static void *worker(void *a)
{
	long id = (long)a;
	int s = 0;
	if (pin(cpu[id]) || prctl(SET, mode[id], 0, 0, 0)) {
		run_abort = 1;              /* the partner must not spin on the barrier for ever */
		return 0;
	}
	for (uint64_t r = 0; r < rounds && !run_abort; r++) {
		sync2(&s);
		if (id == 0)
			for (int i = 0; i < BATCH; i++) {
				if (kind == MP) { store(&X[i].v[0], 1); store(&Y[i].v[0], 1); }
				else if (kind == LB) { R0[i] = load(&X[i].v[0]); store(&Y[i].v[0], 1); }
				else { vstore(&X[i].v[0]); vstore(&Y[i].v[0]); }
			}
		else
			for (int i = 0; i < BATCH; i++) {
				if (kind == MP) { R1[i] = load(&Y[i].v[0]); R0[i] = load(&X[i].v[0]); }
				else if (kind == LB) { R1[i] = load(&Y[i].v[0]); store(&X[i].v[0], 1); }
				else { R1[i] = vload(&Y[i].v[0]); R0[i] = vload(&X[i].v[0]); }
			}
		sync2(&s);
		if (id == 0) {
			for (int i = 0; i < BATCH; i++) {
				if (kind == LB ? (R0[i] && R1[i]) : (R1[i] && !R0[i]))
					bad++;
				X[i].v[0] = X[i].v[1] = Y[i].v[0] = Y[i].v[1] = 0;
			}
		}
		sync2(&s);
	}
	return 0;
}
static uint64_t run(int k, int w, int r, int mw, int mr, uint64_t trials)
{
	pthread_t t[2];
	kind = k; cpu[0] = w; cpu[1] = r; mode[0] = mw; mode[1] = mr;
	rounds = trials / BATCH + 1; bad = 0; bar = 0; phase = 0; run_abort = 0;
	pthread_create(&t[0], 0, worker, (void *)0);
	pthread_create(&t[1], 0, worker, (void *)1);
	pthread_join(t[0], 0);
	pthread_join(t[1], 0);
	if (run_abort)
		printf("TSO-MX: run w%d r%d aborted, core gone or prctl failed\n", w, r);
	return bad;
}
/* -e: what a core does in EL0 before any long run: the last line names the core. FP across a yield, a timed sleep on this core
 * (its timer FIQ), the counter against CLOCK_MONOTONIC (a differing offset shows here), TSO bit across syscalls, faults and a migration. */
static inline uint64_t cntvct(void) { uint64_t v; __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v)); return v; }
static uint64_t ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000000ULL + t.tv_nsec; }
static int el0_fp(void)
{
	uint64_t a = 0x1122334455667788ULL, b = 0;
	__asm__ volatile("fmov d7, %0" : : "r"(a) : "v7");
	sched_yield();
	__asm__ volatile("fmov %0, d7" : "=r"(b) : : "v7");
	return a == b;
}
static int el0_probe(void)
{
	uint64_t frq;
	int rc = 0;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
	cpus_scan();
	printf("TSO-EL0: %d cores take a task, cntfrq %llu\n", ncl, (unsigned long long)frq);
	for (int i = 0; i < ncl; i++) {
		int c = cl[i];
		printf("TSO-EL0: next cpu%d\n", c);
		if (pin(c)) { printf("TSO-EL0: cpu%d gone\n", c); continue; }
		int on = sched_getcpu(), fp = el0_fp();
		uint64_t n0 = ns(), v0 = cntvct();
		struct timespec d = { 0, 20000000 };
		clock_nanosleep(CLOCK_MONOTONIC, 0, &d, 0);
		uint64_t n1 = ns(), v1 = cntvct(), n2 = n1 - n0, v2 = (v1 - v0) * 1000000000ULL / frq;
		long set = prctl(SET, 1, 0, 0, 0), get = prctl(GET, 0, 0, 0, 0);
		volatile char *p = malloc(1 << 20);
		for (int k = 0; p && k < (1 << 20); k += 16384)
			p[k] = 1;
		free((void *)p);
		long get2 = prctl(GET, 0, 0, 0, 0);
		prctl(SET, 0, 0, 0, 0);
		printf("TSO-EL0: cpu%d on %d fp %d sleep %llu us counter %llu us vct %llx prctl %ld get %ld after faults %ld\n", c, on, fp,
		       (unsigned long long)(n2 / 1000), (unsigned long long)(v2 / 1000), (unsigned long long)v1, set, get, get2);
		if (on != c || !fp || n2 < 20000000 || n2 > 2000000000)
			rc = 1;
	}
	/* one TSO thread through every core: the bit follows it (__switch_to) */
	long set = prctl(SET, 1, 0, 0, 0);
	int moved = 0;
	for (int r = 0; r < 200 && !set; r++) {
		if (pin(cl[r % ncl]))
			continue;
		if (prctl(GET, 0, 0, 0, 0) != 1 || !el0_fp()) { printf("TSO-EL0: bit lost on cpu%d\n", cl[r % ncl]); rc = 1; break; }
		moved++;
	}
	prctl(SET, 0, 0, 0, 0);
	printf("TSO-EL0: migrations %d, done rc=%d\n", moved, rc);
	return rc;
}
static void *probe3(void *a)
{
	(void)a;
	return (void *)(long)prctl(SET, 3, 0, 0, 0);
}
int main(int ac, char **av)
{
	int n, rc = 0, have;

	/* a pipe would hold every line until the buffer fills: F26 showed nothing of this step */
	setvbuf(stdout, 0, _IOLBF, 0);
	kmsg("PIN: tso-matrix %s start on cpu%d\n", ac > 1 ? av[1] : "", sched_getcpu());
	if (ac > 1 && av[1][0] == '-' && av[1][1] == 'e')
		return el0_probe();
	cpus_scan();
	n = ncl;
	printf("TSO-MX: start, %d online\n", n);
	have = prctl(SET, 1, 0, 0, 0) == 0;
	int have1 = have && prctl(SET, 2, 0, 0, 0) == 0;   /* only with the kernel diff K1: ACTLR bit 1 alone, what the muvm guest kernel sets */
	prctl(SET, 0, 0, 0, 0);
	/* mode 3 clears ACTLR bits 9-11 for good in the caller: probe it in a thread that exits */
	pthread_t t3;
	void *r3 = (void *)1L;
	if (have1 && !pthread_create(&t3, 0, probe3, 0))
		pthread_join(t3, &r3);
	int have3 = have1 && !r3;
	X = aligned_alloc(LINE, BATCH * sizeof(cell));
	Y = aligned_alloc(LINE, BATCH * sizeof(cell));
	R0 = calloc(BATCH, 8);
	R1 = calloc(BATCH, 8);
	if (!have)
		printf("TSO-MX: no prctl, control only\n");
	if (n < 2) {
		printf("TSO-MX: one CPU online, nothing to pair\n");
		return 3;
	}
	/* -q: the init slice, one E and two P cores with a short cap. A full MP line takes about 57 s on a poor pair (raw, 11-01), the
	 * pair chosen from a core with many events takes under 2 s, so this gives T/T in seconds. */
	static const int qc[] = { 1, 5, 9 };
	int quick = ac > 1 && av[1][0] == '-' && av[1][1] == 'q', nc = quick ? 3 : n;
	uint64_t cap = quick ? QCAP : CAP;
	for (int ci = 0; ci < nc; ci++)
		for (int role = 0; role < 2; role++) {
			int c = quick ? qc[ci] : cl[ci], there = 0;
			for (int o = 0; o < n; o++)
				there |= cl[o] == c;
			if (!there)
				continue;
			int p = -1;
			uint64_t best = 0;
			/* the rate differs by 10^5 between pairs (raw, 10-31): take the partner that shows the most */
			for (int oi = 0; oi < n; oi++)
				if (cl[oi] != c) {
					int o = cl[oi];
					uint64_t b = run(MP, role ? o : c, role ? c : o, 0, 0, PROBE);
					if (p < 0 || b > best) { p = o; best = b; }
				}
			int w = role ? p : c, r = role ? c : p;
			printf("TSO-MX: cpu%d role %d partner %d shows %llu, prctl %d\n", c, role, p, (unsigned long long)best, have);
			for (int k = 0; k < 3; k++) {
				uint64_t tr = 1ULL << 24, b = 0;
				while ((b = run(k, w, r, 0, 0, tr)) < NEED && tr < cap && k != LB)
					tr <<= 2;
				printf("TSO-MX: %s w%d r%d D/D %llu in %llu", name[k], w, r, (unsigned long long)b, (unsigned long long)tr);
				if (b < NEED && k != LB) { rc = rc ? rc : 3; printf(" CONTROL-LOW"); }
				/* -q has a time limit: a control without events says nothing about T/T, and MPV on a poor pair costs 20 units */
				if (have && !(quick && b < NEED && k != LB)) {
					uint64_t t = run(k, w, r, 1, 1, tr * 3), a = run(k, w, r, 1, 0, tr), e = run(k, w, r, 0, 1, tr);
					printf(" T/T %llu T/D %llu D/T %llu", (unsigned long long)t, (unsigned long long)a, (unsigned long long)e);
					if (t) { rc = 1; printf(" VIOLATION"); }
					if (have1 && k == MP)
						printf(" T1/T1 %llu", (unsigned long long)run(k, w, r, 2, 2, tr * 3));
					if (have3 && k == MP)
						printf(" T3/T3 %llu", (unsigned long long)run(k, w, r, 3, 3, tr * 3));
				}
				printf("\n");
				fflush(stdout);
			}
		}
	if (!have)
		rc = 2;
	printf("TSO-MX: done rc=%d\n", rc);
	return rc;
}
