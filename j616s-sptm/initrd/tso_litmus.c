// tso_litmus.c
//
// Hardware TSO (total-store-order) correctness gate for Linux-on-Apple-Silicon
// (M4, hostname morpheus). Proves whether the Apple-hardware TSO memory model
// is REALLY enforced by the CPU, not merely claimed by the kernel.
//
// It does three things:
//   (1) Probes the per-thread memory-model prctl (PR_SET_MEM_MODEL /
//       PR_GET_MEM_MODEL) to see whether TSO can be requested at all.
//   (2) Runs the Message-Passing (MP) litmus test -- the CORRECT discriminator
//       between ARM-weak and TSO -- as the authoritative gate.
//   (3) Runs the classic Store-Buffer (SB) litmus test as an informational
//       control / harness-sensitivity check.
//
// Both litmus threads are pinned to two DIFFERENT CPU clusters (one efficiency
// core, one performance core by default); that asymmetry is what actually
// exposes the hardware's native reordering. Each trial uses its own fresh,
// cache-line-separated operand slots, and the two threads free-run through a
// whole batch between barriers so they drift and overlap (a per-trial barrier
// re-synchronizes them so tightly the reorder rate collapses to ~zero).
//
// -------------------------------------------------------------------------
// WHY MP, NOT SB, IS THE TSO GATE  (important -- read this)
// -------------------------------------------------------------------------
// SB (Store Buffer):
//     init x=0 y=0
//     T0: x=1 ; r0=y        T1: y=1 ; r1=x
//     forbidden-looking outcome: r0==0 && r1==0
//   This outcome is the store->load reorder. TSO (x86-style, which is what
//   Apple's TSO mode emulates for Rosetta/FEX) *ALLOWS* store->load reordering
//   via the store buffer -- SB (0,0) is the textbook example of x86 NOT being
//   sequentially consistent. So SB (0,0) is ALLOWED under BOTH ARM-weak AND
//   TSO; it is forbidden only under full sequential consistency. SB therefore
//   does NOT distinguish TSO from weak, and a "SB must be 0 under TSO" gate
//   would FALSELY FAIL on a correctly-working TSO core. SB is kept here only
//   as a harness-sensitivity control (expected nonzero in every mode).
//
// MP (Message Passing):
//     init data=0 flag=0
//     T0 (writer): data=1 ; flag=1          (store -> store)
//     T1 (reader): r_flag=flag ; r_data=data (load  -> load)
//     forbidden-under-TSO outcome: r_flag==1 && r_data==0
//   For the reader to see flag=1 but data=0, either the writer's two stores
//   were reordered (flag published before data) or the reader's two loads were
//   reordered (data read before flag). TSO preserves store->store AND load->load
//   order, so this outcome is FORBIDDEN under TSO. ARM-weak preserves neither,
//   so it IS observed. MP is thus the correct ARM-weak-vs-TSO discriminator and
//   is used for the verdict.  Under genuine hardware TSO the MP forbidden count
//   MUST be exactly 0.
// -------------------------------------------------------------------------
//
// We use __ATOMIC_RELAXED plus a *compiler-only* barrier
// (asm volatile("" ::: "memory")) between the two accesses of each thread.
// That keeps the two instructions in program order in the emitted code (so the
// compiler cannot manufacture the outcome), while leaving the CPU completely
// free to reorder them. Any observed outcome is therefore HARDWARE reordering.
//
// prctl numbers are the real values from morpheus headers
// (uapi/linux/prctl.h and FEXCore/Utils/PrctlUtils.h):
//     PR_SET_MEM_MODEL          0x4d4d444c
//     PR_GET_MEM_MODEL          0x6d4d444c
//     PR_SET_MEM_MODEL_DEFAULT  0
//     PR_SET_MEM_MODEL_TSO      1
//
// Build:  gcc -O2 -pthread -o tso_litmus tso_litmus.c
// Run:    ./tso_litmus [cpuA] [cpuB] [iterations]
//   defaults: cpuA=1 (cluster0/E), cpuB=9 (cluster2/P), iterations=200000000
//   (iterations is rounded down to a multiple of the internal batch size)
//
// SPDX-License-Identifier: MIT

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <sched.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/prctl.h>

/* ---- prctl memory-model constants (real values from morpheus headers) ---- */
#ifndef PR_SET_MEM_MODEL
#define PR_SET_MEM_MODEL 0x4d4d444c
#endif
#ifndef PR_GET_MEM_MODEL
#define PR_GET_MEM_MODEL 0x6d4d444c
#endif
#ifndef PR_SET_MEM_MODEL_DEFAULT
#define PR_SET_MEM_MODEL_DEFAULT 0
#endif
#ifndef PR_SET_MEM_MODEL_TSO
#define PR_SET_MEM_MODEL_TSO 1
#endif

/* Apple Silicon has 128-byte cache lines; separate everything that races. */
#define CACHELINE 128

/* test kinds */
#define TEST_SB 0
#define TEST_MP 1

/* One operand on its own cache line, so slot[k] and its neighbours never
 * false-share (false sharing would serialize them and hide the reorder). */
typedef struct {
    _Alignas(CACHELINE) volatile int v;
    char pad[CACHELINE - sizeof(int)];
} cell_t;

/* trials per barrier round: threads free-run through this many fresh slots */
#define BATCH 2048

/* ---- shared state ---- */
typedef struct {
    /* 2-thread sense-reversing barrier (each field on its own line) */
    _Alignas(CACHELINE) volatile int bar_count;
    char _p0[CACHELINE - sizeof(int)];
    _Alignas(CACHELINE) volatile int bar_sense;
    char _p1[CACHELINE - sizeof(int)];
} shared_t;

static shared_t *S;
static cell_t *X;             /* BATCH slots: SB operand x / MP "data"  */
static cell_t *Y;             /* BATCH slots: SB operand y / MP "flag"  */
static int     *R0;           /* BATCH: SB r0 (T0 load y) / MP r_data   */
static int     *R1;           /* BATCH: SB r1 (T1 load x) / MP r_flag   */

static unsigned long long g_iters;   /* effective total trials            */
static unsigned long long g_rounds;  /* g_iters / BATCH                   */
static int g_mode;            /* PR_SET_MEM_MODEL_{DEFAULT,TSO} for workers */
static int g_test;            /* TEST_SB | TEST_MP                          */
static int g_cpu[2];

/* per-worker report of its own prctl(PR_SET_MEM_MODEL, g_mode) call */
static volatile int worker_prctl_ret[2];
static volatile int worker_prctl_errno[2];

/* forbidden-outcome counter (written only by the counting thread each round) */
static volatile unsigned long long g_bad;

/* --- 2-thread sense-reversing barrier built on SEQ_CST atomics ---
 * Ordering here is irrelevant to the test: the test operands use RELAXED and
 * each trial's window lives strictly *between* two barrier calls. The barrier
 * only guarantees both threads start and finish each batch together. */
static inline void bar_wait(int *local_sense)
{
    *local_sense = !*local_sense;
    if (__atomic_add_fetch(&S->bar_count, 1, __ATOMIC_SEQ_CST) == 2) {
        __atomic_store_n(&S->bar_count, 0, __ATOMIC_SEQ_CST);
        __atomic_store_n(&S->bar_sense, *local_sense, __ATOMIC_SEQ_CST);
    } else {
        while (__atomic_load_n(&S->bar_sense, __ATOMIC_SEQ_CST) != *local_sense)
            ; /* spin */
    }
}

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
static int pin_to_cpu(int cpu)
{
    cpu_set_t set;
    int r, from = sched_getcpu();
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    kmsg("PIN: litmus tid %d cpu%d -> cpu%d\n", (int)gettid(), from, cpu);
    r = sched_setaffinity(0, sizeof(set), &set);
    kmsg("PIN: litmus tid %d now cpu%d rc %d\n", (int)gettid(), sched_getcpu(), r);
    return r;
}

static void *worker(void *arg)
{
    long id = (long)arg;
    int ls = 0;

    if (pin_to_cpu(g_cpu[id]) != 0)
        fprintf(stderr, "WARN: thread %ld could not pin to cpu %d: %s\n",
                id, g_cpu[id], strerror(errno));

    /* Each thread sets its OWN memory model (prctl is per-thread). */
    errno = 0;
    int ret = prctl(PR_SET_MEM_MODEL, (unsigned long)g_mode, 0, 0, 0);
    worker_prctl_ret[id]   = ret;
    worker_prctl_errno[id] = (ret != 0) ? errno : 0;

    for (unsigned long long round = 0; round < g_rounds; round++) {
        /* ---- reset: each thread clears one operand array + its results ---- */
        if (id == 0) {
            for (int k = 0; k < BATCH; k++) { X[k].v = 0; R0[k] = 0; }
        } else {
            for (int k = 0; k < BATCH; k++) { Y[k].v = 0; R1[k] = 0; }
        }

        bar_wait(&ls);  /* barrier A: resets globally visible, aligned start */

        /* ---- the litmus batch: free-run, one fresh slot per trial ---- */
        if (g_test == TEST_SB) {
            if (id == 0) {
                for (int k = 0; k < BATCH; k++) {
                    __atomic_store_n(&X[k].v, 1, __ATOMIC_RELAXED);   /* x=1 */
                    asm volatile("" ::: "memory");
                    R0[k] = __atomic_load_n(&Y[k].v, __ATOMIC_RELAXED); /* r0=y */
                }
            } else {
                for (int k = 0; k < BATCH; k++) {
                    __atomic_store_n(&Y[k].v, 1, __ATOMIC_RELAXED);   /* y=1 */
                    asm volatile("" ::: "memory");
                    R1[k] = __atomic_load_n(&X[k].v, __ATOMIC_RELAXED); /* r1=x */
                }
            }
        } else { /* TEST_MP: X=data, Y=flag */
            if (id == 0) { /* writer: data=1 then flag=1 */
                for (int k = 0; k < BATCH; k++) {
                    __atomic_store_n(&X[k].v, 1, __ATOMIC_RELAXED);   /* data=1 */
                    asm volatile("" ::: "memory");
                    __atomic_store_n(&Y[k].v, 1, __ATOMIC_RELAXED);   /* flag=1 */
                }
            } else { /* reader: r_flag=flag then r_data=data */
                for (int k = 0; k < BATCH; k++) {
                    R1[k] = __atomic_load_n(&Y[k].v, __ATOMIC_RELAXED); /* flag */
                    asm volatile("" ::: "memory");
                    R0[k] = __atomic_load_n(&X[k].v, __ATOMIC_RELAXED); /* data */
                }
            }
        }

        bar_wait(&ls);  /* barrier B: whole batch done on both threads */

        /* ---- tally (single writer: T0) ---- */
        if (id == 0) {
            if (g_test == TEST_SB) {
                for (int k = 0; k < BATCH; k++)
                    if (R0[k] == 0 && R1[k] == 0)      /* SB: both read 0 */
                        g_bad++;
            } else {
                for (int k = 0; k < BATCH; k++)
                    if (R1[k] == 1 && R0[k] == 0)      /* MP: flag seen, data not */
                        g_bad++;
            }
        }

        bar_wait(&ls);  /* barrier C: T1 must not reset ahead of T0's tally */
    }
    return NULL;
}

/* Run one full experiment; returns forbidden-outcome count and fills prctl_ok
 * (1 if BOTH workers set the requested model successfully). */
static unsigned long long run_litmus(int mode, int test, int *prctl_ok)
{
    g_mode = mode;
    g_test = test;
    g_bad = 0;
    S->bar_count = 0;
    S->bar_sense = 0;
    for (int k = 0; k < BATCH; k++) { X[k].v = Y[k].v = 0; R0[k] = R1[k] = 0; }
    worker_prctl_ret[0] = worker_prctl_ret[1] = 0;
    worker_prctl_errno[0] = worker_prctl_errno[1] = 0;

    pthread_t t0, t1;
    pthread_create(&t1, NULL, worker, (void *)1L);
    pthread_create(&t0, NULL, worker, (void *)0L);
    pthread_join(t0, NULL);
    pthread_join(t1, NULL);

    *prctl_ok = (worker_prctl_ret[0] == 0 && worker_prctl_ret[1] == 0);
    return g_bad;
}

int main(int argc, char **argv)
{
    g_cpu[0] = (argc > 1) ? atoi(argv[1]) : 1;   /* cluster0 (E) by default */
    g_cpu[1] = (argc > 2) ? atoi(argv[2]) : 9;   /* cluster2 (P) by default */
    g_iters  = (argc > 3) ? strtoull(argv[3], NULL, 0) : 200000000ULL;

    g_rounds = g_iters / BATCH;
    if (g_rounds == 0) g_rounds = 1;
    g_iters  = g_rounds * (unsigned long long)BATCH;   /* effective total */

    if (posix_memalign((void **)&S, CACHELINE, sizeof(shared_t)) != 0 || !S) {
        fprintf(stderr, "alloc failed\n"); return 2;
    }
    memset(S, 0, sizeof(*S));
    if (posix_memalign((void **)&X, CACHELINE, BATCH * sizeof(cell_t)) != 0 ||
        posix_memalign((void **)&Y, CACHELINE, BATCH * sizeof(cell_t)) != 0 ||
        !X || !Y) {
        fprintf(stderr, "alloc failed\n"); return 2;
    }
    R0 = calloc(BATCH, sizeof(int));
    R1 = calloc(BATCH, sizeof(int));
    if (!R0 || !R1) { fprintf(stderr, "alloc failed\n"); return 2; }
    memset(X, 0, BATCH * sizeof(cell_t));
    memset(Y, 0, BATCH * sizeof(cell_t));

    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);

    printf("==== Apple M4 hardware-TSO litmus (MP gate + SB control) ====\n");
    printf("host CPUs online : %ld\n", ncpu);
    printf("pinned CPUs      : T0=cpu%d  T1=cpu%d\n", g_cpu[0], g_cpu[1]);
    printf("iterations/test  : %llu  (%llu rounds x %d batch)\n",
           g_iters, g_rounds, BATCH);
    printf("prctl numbers    : SET=0x%x GET=0x%x  DEFAULT=%d TSO=%d\n",
           PR_SET_MEM_MODEL, PR_GET_MEM_MODEL,
           PR_SET_MEM_MODEL_DEFAULT, PR_SET_MEM_MODEL_TSO);
    printf("\n");

    /* ---- (1) probe: can this thread request TSO at all? ---- */
    errno = 0;
    int set_tso = prctl(PR_SET_MEM_MODEL, (unsigned long)PR_SET_MEM_MODEL_TSO,
                        0, 0, 0);
    int set_tso_errno = errno;
    int tso_available = (set_tso == 0);

    printf("[probe] prctl(PR_SET_MEM_MODEL, TSO) = %d", set_tso);
    if (set_tso != 0)
        printf("  (errno=%d %s)", set_tso_errno, strerror(set_tso_errno));
    printf("\n");

    errno = 0;
    int getm = prctl(PR_GET_MEM_MODEL, 0, 0, 0, 0);
    if (getm < 0)
        printf("[probe] prctl(PR_GET_MEM_MODEL)      = %d (errno=%d %s)\n",
               getm, errno, strerror(errno));
    else
        printf("[probe] prctl(PR_GET_MEM_MODEL)      = %d (%s)\n",
               getm, getm == PR_SET_MEM_MODEL_TSO ? "TSO" :
                     getm == PR_SET_MEM_MODEL_DEFAULT ? "DEFAULT" : "?");
    int get_confirms_tso = (getm == PR_SET_MEM_MODEL_TSO);

    /* restore main thread to default (workers set their own anyway) */
    prctl(PR_SET_MEM_MODEL, (unsigned long)PR_SET_MEM_MODEL_DEFAULT, 0, 0, 0);
    printf("\n");

    int dummy;

    /* ---- (2) DEFAULT (weak) baseline / sanity controls ---- */
    printf("[run ] DEFAULT (weak) mode ...\n");
    unsigned long long def_mp = run_litmus(PR_SET_MEM_MODEL_DEFAULT, TEST_MP, &dummy);
    printf("       MP forbidden (r_flag==1 && r_data==0) : %llu / %llu\n",
           def_mp, g_iters);
    unsigned long long def_sb = run_litmus(PR_SET_MEM_MODEL_DEFAULT, TEST_SB, &dummy);
    printf("       SB outcome   (r0==0 && r1==0)         : %llu / %llu  (control)\n",
           def_sb, g_iters);
    printf("\n");

    /* ---- (3) TSO mode (only if available) ---- */
    int tso_mp_prctl_ok = 0, tso_sb_prctl_ok = 0, tso_ran = 0;
    unsigned long long tso_mp = 0, tso_sb = 0;
    if (tso_available) {
        printf("[run ] TSO mode ...\n");
        tso_mp = run_litmus(PR_SET_MEM_MODEL_TSO, TEST_MP, &tso_mp_prctl_ok);
        printf("       MP forbidden (r_flag==1 && r_data==0) : %llu / %llu  <== GATE\n",
               tso_mp, g_iters);
        tso_sb = run_litmus(PR_SET_MEM_MODEL_TSO, TEST_SB, &tso_sb_prctl_ok);
        printf("       SB outcome   (r0==0 && r1==0)         : %llu / %llu  (control; nonzero is OK under TSO)\n",
               tso_sb, g_iters);
        printf("       workers' prctl(TSO) ok: MP-run=%s SB-run=%s\n",
               tso_mp_prctl_ok ? "yes" : "no",
               tso_sb_prctl_ok ? "yes" : "no");
        tso_ran = 1;
    } else {
        printf("[run ] TSO mode SKIPPED: prctl(TSO) not available on this kernel\n");
    }
    printf("\n");

    /* ---- (3b) macho F34: XNU's Rosetta thread state, prctl value 3 = ACTLR 0x12 (EnTSO|EnAPFLG) with bits 9-11 cleared
     * (XNU arm_init_cpu). Measured in the same run as bit 1 alone, so one HW trip says which state orders on T6040. ---- */
    int xnu_mp_prctl_ok = 0, xnu_sb_prctl_ok = 0, xnu_ran = 0;
    unsigned long long xnu_mp = 0, xnu_sb = 0;
    if (tso_available && prctl(PR_SET_MEM_MODEL, 3UL, 0, 0, 0) == 0) {
        prctl(PR_SET_MEM_MODEL, (unsigned long)PR_SET_MEM_MODEL_DEFAULT, 0, 0, 0);
        printf("[run ] XNU state (0x12, ~0xe00) ...\n");
        xnu_mp = run_litmus(3, TEST_MP, &xnu_mp_prctl_ok);
        printf("       MP forbidden (r_flag==1 && r_data==0) : %llu / %llu  <== GATE (XNU state)\n",
               xnu_mp, g_iters);
        xnu_sb = run_litmus(3, TEST_SB, &xnu_sb_prctl_ok);
        printf("       SB outcome   (r0==0 && r1==0)         : %llu / %llu  (control)\n",
               xnu_sb, g_iters);
        xnu_ran = 1;
    } else {
        printf("[run ] XNU state SKIPPED: prctl(3) not available on this kernel\n");
    }
    printf("\n");

    /* ---- verdict ---- */
    printf("==== VERDICT ====\n");
    printf("probe: TSO %s", tso_available ? "AVAILABLE" : "NOT available");
    if (tso_available)
        printf(" (GET read-back: %s)", get_confirms_tso ? "TSO confirmed"
                                                         : "did NOT confirm TSO");
    printf("\n");
    printf("DEFAULT MP reorder : %llu  (expect > 0: harness sees weak ordering)\n",
           def_mp);
    printf("DEFAULT SB reorder : %llu  (control; store->load reorder)\n", def_sb);
    if (tso_ran) {
        printf("TSO     MP reorder : %llu  (MUST be 0 for real TSO)  <== GATE\n",
               tso_mp);
        printf("TSO     SB reorder : %llu  (expected > 0 even under real TSO)\n",
               tso_sb);
    }
    if (xnu_ran) {
        printf("XNU     MP reorder : %llu  (bit 1|4, 9-11 clear; MUST be 0 if that state orders)\n",
               xnu_mp);
        printf("XNU     SB reorder : %llu  (control)\n", xnu_sb);
    }

    int rc;
    if (!tso_available) {
        printf("\n>>> TSO NOT AVAILABLE <<<\n");
        if (def_mp > 0)
            printf("    Harness VALIDATED: DEFAULT mode observed %llu MP reorderings\n"
                   "    (and %llu SB). The MP gate will be trustworthy once the\n"
                   "    kernel exposes TSO -- re-run then.\n", def_mp, def_sb);
        else
            printf("    WARNING: DEFAULT mode saw ZERO MP reordering -- harness did\n"
                   "    NOT observe weak ordering; increase iterations or pick CPUs\n"
                   "    in two different clusters (one E-core, cpu1..4) before\n"
                   "    trusting any future TSO MP==0 result.\n");
        rc = 2;
    } else if (tso_mp_prctl_ok && get_confirms_tso && tso_mp == 0) {
        printf("\n>>> HW TSO CONFIRMED <<<\n");
        printf("    prctl(TSO) succeeded, read-back confirmed TSO, and the MP\n");
        printf("    forbidden outcome was NEVER observed over %llu iterations.\n",
               g_iters);
        printf("    (SB nonzero under TSO is expected and correct: TSO permits\n");
        printf("     store->load reordering.)\n");
        if (def_mp == 0)
            printf("    CAUTION: DEFAULT MP baseline was also 0 -- harness sensitivity\n"
                   "    unproven this run; re-run with more iterations to be sure.\n");
        rc = 0;
    } else if (tso_mp_prctl_ok && tso_mp > 0 && xnu_ran && xnu_mp_prctl_ok && xnu_mp == 0 && def_mp > 0) {
        printf("\n>>> ONLY THE XNU STATE ORDERS (bit 1 alone does not) <<<\n");
        printf("    bit 1 alone saw %llu MP reorderings, ACTLR 0x12 with bits 9-11 clear saw 0 over %llu.\n",
               tso_mp, g_iters);
        printf("    Switch the kernel: echo 0x12 > /sys/kernel/macho_tso/set; echo 0xe00 > /sys/kernel/macho_tso/clear\n");
        rc = 4;
    } else if (tso_mp_prctl_ok && tso_mp > 0) {
        printf("\n>>> TSO CLAIMED BUT NOT ENFORCED (DANGER) <<<\n");
        printf("    prctl(TSO) succeeded but the hardware STILL produced the MP\n");
        printf("    forbidden outcome %llu times. Store->store or load->load order\n",
               tso_mp);
        printf("    is NOT enforced -- do NOT trust x86 emulation.\n");
        rc = 1;
    } else {
        printf("\n>>> INCONCLUSIVE <<<\n");
        printf("    prctl probe said available but a worker set or the GET read-back\n");
        printf("    did not confirm TSO. Investigate before trusting.\n");
        rc = 3;
    }

    free(S); free(X); free(Y); free(R0); free(R1);
    return rc;
}
