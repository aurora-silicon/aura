// SPDX-License-Identifier: GPL-2.0-only
// macho-pinstep CPU STEP...: one piece of what tso-matrix -e does in its first milliseconds, on one core, each step named on the
// panel (kernel log) before it runs and after it returns. Run it over the ACM shell one step at a time; the last PIN line names
// what stopped. Steps, in the order of the test plan:
//   pin      sched_setaffinity to CPU (stopper thread on the source core, the wake of the target)
//   spin     500 ms of EL0 work on CPU, no syscall (EL0 runs there, timer ticks it)
//   yield    sched_yield on CPU
//   fp       a value in d7 across a yield (FP/SIMD state on that core)
//   sleep    clock_nanosleep 20 ms (the core's own timer interrupt wakes the task)
//   cntvct   counter read at EL0 against CLOCK_MONOTONIC (CNTKCTL on that core)
//   prctl    PR_SET_MEM_MODEL TSO, GET, back to default (ACTLR write on that core)
//   fault    1 MiB of first-touch page faults
//   hop      pin to CPU2 (the next argument after hop), then back to CPU: an IPI sent by a secondary
// Build: gcc -O2 -static -o macho-pinstep macho-pinstep.c
#define _GNU_SOURCE
#include <fcntl.h>
#include <sched.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>
#define SET 0x4d4d444c
#define GET 0x6d4d444c

static int kfd = -1;
static void say(const char *fmt, ...)
{
	char b[200];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	if (n <= 0)
		return;
	if (n >= (int)sizeof(b))
		n = sizeof(b) - 1;
	if (kfd >= 0)
		(void)!write(kfd, b, n);
	(void)!write(1, b, n);
}

static int pin(int c)
{
	cpu_set_t s;

	CPU_ZERO(&s);
	CPU_SET(c, &s);
	return sched_setaffinity(0, sizeof(s), &s);
}

static uint64_t ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000000000ULL + t.tv_nsec;
}

static uint64_t cntvct(void)
{
	uint64_t v;

	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
	return v;
}

int main(int ac, char **av)
{
	int cpu, rc = 0;

	kfd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	if (ac < 3) {
		fprintf(stderr, "usage: %s CPU pin|spin|yield|fp|sleep|cntvct|prctl|fault|hop CPU2 ...\n", av[0]);
		return 2;
	}
	cpu = atoi(av[1]);
	say("PIN: pinstep start on cpu%d, target cpu%d\n", sched_getcpu(), cpu);
	for (int i = 2; i < ac; i++) {
		const char *s = av[i];
		long r = 0;

		say("PIN: step %s on cpu%d ->\n", s, sched_getcpu());
		if (!strcmp(s, "pin")) {
			r = pin(cpu);
		} else if (!strcmp(s, "spin")) {
			uint64_t t = ns();
			volatile uint64_t x = 0;
			while (ns() - t < 500000000ULL)
				x++;
			r = x != 0;
		} else if (!strcmp(s, "yield")) {
			r = sched_yield();
		} else if (!strcmp(s, "fp")) {
			uint64_t a = 0x1122334455667788ULL, b = 0;
			__asm__ volatile("fmov d7, %0" : : "r"(a) : "v7");
			sched_yield();
			__asm__ volatile("fmov %0, d7" : "=r"(b) : : "v7");
			r = a == b;
		} else if (!strcmp(s, "sleep")) {
			struct timespec d = { 0, 20000000 };
			uint64_t t = ns();
			r = clock_nanosleep(CLOCK_MONOTONIC, 0, &d, 0);
			say("PIN: sleep took %llu us\n", (unsigned long long)((ns() - t) / 1000));
		} else if (!strcmp(s, "cntvct")) {
			uint64_t frq, v0, n0, v1, n1;
			__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
			v0 = cntvct();
			n0 = ns();
			usleep(10000);
			v1 = cntvct();
			n1 = ns();
			say("PIN: cntfrq %llu counter %llu us monotonic %llu us\n", (unsigned long long)frq,
			    (unsigned long long)((v1 - v0) * 1000000ULL / (frq ? frq : 1)), (unsigned long long)((n1 - n0) / 1000));
		} else if (!strcmp(s, "prctl")) {
			long set = prctl(SET, 1, 0, 0, 0), get = prctl(GET, 0, 0, 0, 0);
			prctl(SET, 0, 0, 0, 0);
			say("PIN: prctl set %ld get %ld\n", set, get);
			r = set;
		} else if (!strcmp(s, "fault")) {
			volatile char *p = malloc(1 << 20);
			for (int k = 0; p && k < (1 << 20); k += 16384)
				p[k] = 1;
			free((void *)p);
			r = p != 0;
		} else if (!strcmp(s, "hop") && i + 1 < ac) {
			int c2 = atoi(av[++i]);
			r = pin(c2);
			say("PIN: hop on cpu%d rc %ld\n", sched_getcpu(), r);
			r = pin(cpu);
		} else {
			say("PIN: unknown step %s\n", s);
			rc = 2;
			break;
		}
		say("PIN: step %s done on cpu%d r %ld\n", s, sched_getcpu(), r);
	}
	say("PIN: pinstep end rc %d\n", rc);
	return rc;
}
