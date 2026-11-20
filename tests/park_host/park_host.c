/* SPDX-License-Identifier: MIT */
/*
 * src/sptm_virt_park.c with its cores as threads (tests/python/test_sptm_virt_park_host.py).
 *
 * Each core loops: enter aura (sptm_virt_park_enter), do some work there, leave, run "guest code"
 * for a while. A park's owner does what the GPU hook or a periodic hook does. The fake proxy
 * session checks, at its start and its end, that no other core is inside aura and that none has
 * run aura code since the session began; after each park every core must run again. Every atomic
 * access of the code under test may stall first (park_host.h), to open the races it must close.
 */
#define PARK_HOST_HARNESS
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "park_host.h"

#define NCPU 7

__thread u64 host_reg_ELR_EL2, host_reg_SPSR_EL2, host_reg_ESR_EL2, host_reg_FAR_EL2;
__thread u64 host_reg_SYS_IMP_APL_ELR_GL1, host_reg_SYS_IMP_APL_SPSR_GL1,
    host_reg_SYS_IMP_APL_ESR_GL1, host_reg_SYS_IMP_APL_FAR_GL1;
__thread int park_host_cpu = MAX_CPUS;
__thread bool park_host_gl;
volatile bool sptm_virt_guest_live = true;
iodev_id_t uartproxy_iodev = 1;

static atomic_int nfail;
#define FAIL(...)                                                                                  \
    do {                                                                                           \
        fprintf(stderr, "FAIL: " __VA_ARGS__);                                                     \
        fputc('\n', stderr);                                                                       \
        atomic_fetch_add(&nfail, 1);                                                               \
    } while (0)

u64 park_host_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000UL + ts.tv_nsec;
}

static void spin_ns(u64 ns)
{
    u64 end = park_host_now() + ns;
    while (park_host_now() < end)
        ;
}

static __thread unsigned seed = 1;
void park_host_race(void)
{
    unsigned r = rand_r(&seed);
    if ((r & 7) == 0) {
        if (r & 0x100)
            sched_yield();
        else
            usleep((r >> 9) % 40);
    }
}

void udelay(u32 us)
{
    usleep(us);
}

/* ---- the event register ------------------------------------------------------------------- */
static pthread_mutex_t ev_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ev_cond = PTHREAD_COND_INITIALIZER;
static bool ev[MAX_CPUS + 1];

void park_host_sysop(const char *op)
{
    if (!strcmp(op, "sev")) {
        pthread_mutex_lock(&ev_lock);
        for (int i = 0; i <= MAX_CPUS; i++)
            ev[i] = true;
        pthread_cond_broadcast(&ev_cond);
        pthread_mutex_unlock(&ev_lock);
    } else if (!strcmp(op, "wfe")) {
        int me = park_host_cpu;
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_sec += 2;
        pthread_mutex_lock(&ev_lock);
        while (!ev[me])
            if (pthread_cond_timedwait(&ev_cond, &ev_lock, &dl) == ETIMEDOUT) {
                FAIL("cpu%d: no event ended its wfe for 2 s", me);
                break;
            }
        ev[me] = false;
        pthread_mutex_unlock(&ev_lock);
    } else {
        atomic_thread_fence(memory_order_seq_cst);
    }
}

/* ---- the console ------------------------------------------------------------------------ */
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static char logbuf[1 << 16];
static size_t loglen;

int park_host_printf(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&log_lock);
    size_t len = strlen(line);
    if (loglen + len < sizeof(logbuf)) {
        memcpy(logbuf + loglen, line, len + 1);
        loglen += len;
    }
    pthread_mutex_unlock(&log_lock);
    fputs(line, stdout);
    return n;
}

static size_t log_mark(void)
{
    pthread_mutex_lock(&log_lock);
    size_t m = loglen;
    pthread_mutex_unlock(&log_lock);
    return m;
}

static bool log_has(size_t from, const char *what)
{
    pthread_mutex_lock(&log_lock);
    bool r = strstr(logbuf + from, what) != NULL;
    pthread_mutex_unlock(&log_lock);
    return r;
}

/* ---- the proxy pipe ------------------------------------------------------------------------ */
static pthread_mutex_t pipe_lock = PTHREAD_MUTEX_INITIALIZER;
static u8 pipe_buf[4096];
static size_t pipe_r, pipe_w;
static atomic_int polls;
static atomic_ulong link_down_until;

static void pipe_push(const void *p, size_t n)
{
    pthread_mutex_lock(&pipe_lock);
    memcpy(pipe_buf + pipe_w, p, n);
    pipe_w += n;
    pthread_mutex_unlock(&pipe_lock);
}

int iodev_get_usage(iodev_id_t id)
{
    return id == 1 ? USAGE_UARTPROXY : 0;
}

void iodev_handle_events(iodev_id_t id)
{
    (void)id;
    atomic_fetch_add(&polls, 1);
}

ssize_t iodev_can_read(iodev_id_t id)
{
    (void)id;
    pthread_mutex_lock(&pipe_lock);
    ssize_t n = pipe_w - pipe_r;
    pthread_mutex_unlock(&pipe_lock);
    return n;
}

ssize_t iodev_read(iodev_id_t id, void *buf, size_t n)
{
    (void)id;
    pthread_mutex_lock(&pipe_lock);
    if (pipe_w - pipe_r < n) {
        pthread_mutex_unlock(&pipe_lock);
        FAIL("a pipe read of %zu bytes with fewer there (it would block)", n);
        return -1;
    }
    memcpy(buf, pipe_buf + pipe_r, n);
    pipe_r += n;
    pthread_mutex_unlock(&pipe_lock);
    return n;
}

bool iodev_can_write(iodev_id_t id)
{
    (void)id;
    return park_host_now() >= atomic_load(&link_down_until);
}

/* ---- the cores ----------------------------------------------------------------------------- */
typedef void (*action_fn)(int me);
static _Atomic(action_fn) action[NCPU];
/* 0 plain, 1 EL2 poller, 2 GL poller, 3 EL2 poller every 40 ms, 4 EL2 pollers that claim together
 */
static atomic_int mode[NCPU];
static atomic_int in_aura[NCPU];
static atomic_ulong progress[NCPU];
static atomic_bool stop_all;
static atomic_int proxy_owner = -1;
static atomic_ulong stragglers_ok;
static atomic_int parks_done;
/* A core in its own election: inside aura, but stopped while it loses (the proxy checks that). */
static atomic_int electing[NCPU];

static atomic_int script_i; /* which uartproxy_run of the park this is */

/* The trap's return state: set before a park, required back after it. */
static void regs_set(int me, bool gl)
{
    park_host_gl = gl;
    host_reg_ELR_EL2 = 0x1100 + me;
    host_reg_SPSR_EL2 = 0x2200 + me;
    host_reg_ESR_EL2 = 0x3300 + me;
    host_reg_FAR_EL2 = 0x4400 + me;
    host_reg_SYS_IMP_APL_ELR_GL1 = 0x5500 + me;
    host_reg_SYS_IMP_APL_SPSR_GL1 = 0x6600 + me;
    host_reg_SYS_IMP_APL_ESR_GL1 = 0x7700 + me;
    host_reg_SYS_IMP_APL_FAR_GL1 = 0x8800 + me;
}

static void regs_check(int me, bool gl)
{
    if (host_reg_ELR_EL2 != 0x1100UL + me || host_reg_SPSR_EL2 != 0x2200UL + me ||
        host_reg_ESR_EL2 != 0x3300UL + me || host_reg_FAR_EL2 != 0x4400UL + me)
        FAIL("cpu%d: the trap's EL2 return state wasn't restored after the park", me);
    if (gl && (host_reg_SYS_IMP_APL_ELR_GL1 != 0x5500UL + me ||
               host_reg_SYS_IMP_APL_SPSR_GL1 != 0x6600UL + me ||
               host_reg_SYS_IMP_APL_ESR_GL1 != 0x7700UL + me ||
               host_reg_SYS_IMP_APL_FAR_GL1 != 0x8800UL + me))
        FAIL("cpu%d: the trap's GL return state wasn't restored after the park", me);
    park_host_gl = false;
}

static void act_park(int me)
{
    struct exc_info ctx = {.elr = 0xfffffe0000004000UL + me};
    bool gl = rand_r(&seed) & 1;
    regs_set(me, gl);
    atomic_store(&electing[me], 1);
    sptm_virt_park_others();
    atomic_store(&electing[me], 0);
    atomic_store(&script_i, 0); /* this core owns the park now: the session script restarts */
    sptm_virt_park_proxy(&ctx, 0x47505521);
    regs_check(me, gl);
    atomic_fetch_add(&parks_done, 1);
}

/* Two cores elect at the same moment: both reach the election together. */
static atomic_int rv2;
static void act_park_together(int me)
{
    atomic_fetch_add(&rv2, 1);
    u64 end = park_host_now() + 100000000UL;
    while (atomic_load(&rv2) < 2 && park_host_now() < end)
        ;
    act_park(me);
}

static void act_stall(int me)
{
    (void)me;
    usleep(300000);
}

static atomic_int nested_owner = -1;
static void act_nested(int me)
{
    /* A fault inside one of aura's handlers while another core is electing itself. */
    u64 end = park_host_now() + 2000000000UL;
    while (__atomic_load_n(&sptm_virt_parking_cpu, __ATOMIC_SEQ_CST) != atomic_load(&nested_owner))
        if (park_host_now() > end) {
            FAIL("cpu%d: the owner never started its election", me);
            return;
        }
    usleep(2000);
    int t = sptm_virt_park_enter();
    usleep(1000);
    sptm_virt_park_exit(t);
}

static void host_poll(int me, bool gl)
{
    struct exc_info ctx = {.elr = 0xfffffe0000008000UL + me};
    park_host_gl = gl;
    if (sptm_virt_park_requested(gl)) {
        regs_set(me, gl);
        sptm_virt_park_host(&ctx);
        regs_check(me, gl);
    }
    park_host_gl = false;
}

/* Every core that sees the request claims it at the same moment. */
static atomic_int rv4;
static void host_poll_together(int me)
{
    struct exc_info ctx = {.elr = 0xfffffe000000c000UL + me};
    if (!sptm_virt_park_requested(false))
        return;
    atomic_fetch_add(&rv4, 1);
    u64 end = park_host_now() + 50000000UL;
    while (atomic_load(&rv4) < NCPU && park_host_now() < end)
        ;
    regs_set(me, false);
    sptm_virt_park_host(&ctx);
    regs_check(me, false);
}

static void *cpu_main(void *arg)
{
    int me = (int)(intptr_t)arg;
    u64 last_el2 = 0;

    park_host_cpu = me;
    seed = 7919 * (me + 1) + (unsigned)park_host_now();
    while (!atomic_load(&stop_all)) {
        int t = sptm_virt_park_enter();
        atomic_store(&in_aura[me], 1);
        int o = atomic_load(&proxy_owner);
        if (o >= 0 && o != me && !(atomic_load(&stragglers_ok) & BIT(me)))
            FAIL("cpu%d entered aura during cpu%d's proxy session", me, o);
        spin_ns(rand_r(&seed) % 60000);
        action_fn a = atomic_exchange(&action[me], NULL);
        if (a)
            a(me);
        int m = atomic_load(&mode[me]);
        if (m == 1 || m == 2)
            host_poll(me, m == 2);
        if (m == 4)
            host_poll_together(me);
        if (m == 3 && park_host_now() - last_el2 > 40000000UL) {
            last_el2 = park_host_now();
            host_poll(me, false);
        }
        atomic_store(&in_aura[me], 0);
        atomic_fetch_add(&progress[me], 1);
        sptm_virt_park_exit(t);
        spin_ns(rand_r(&seed) % 30000);
        if (!(rand_r(&seed) & 15))
            usleep(rand_r(&seed) % 200);
    }
    return NULL;
}

/* ---- the proxy session --------------------------------------------------------------------- */
static atomic_int in_proxy, proxy_calls;
static int script[4], script_n;
static u64 link_down_ns;
static u64 park_snap[NCPU];
static atomic_ulong session_t;
static atomic_int session_owner = -1;
static u32 last_code;
static void *last_reserved;
static u64 session_ns = 20000000UL;

static void check_quiet(int me, const char *when)
{
    u64 ok = atomic_load(&stragglers_ok);
    for (int c = 0; c < NCPU; c++)
        if (c != me && !(ok & BIT(c)) && atomic_load(&in_aura[c]) && !atomic_load(&electing[c]))
            FAIL("cpu%d was inside aura at the %s of cpu%d's proxy session", c, when, me);
}

static void check_still(int me, const char *when)
{
    u64 ok = atomic_load(&stragglers_ok);
    for (int c = 0; c < NCPU; c++)
        if (c != me && !(ok & BIT(c)) && atomic_load(&progress[c]) != park_snap[c])
            FAIL("cpu%d ran aura code during cpu%d's park (%s)", c, me, when);
}

int uartproxy_run(struct uartproxy_msg_start *st)
{
    int me = park_host_cpu;
    int i = atomic_fetch_add(&script_i, 1);

    if (atomic_fetch_add(&in_proxy, 1))
        FAIL("two cores in the proxy at once (cpu%d and cpu%d)", me, atomic_load(&proxy_owner));
    atomic_store(&proxy_owner, me);
    if (st->reason != START_HV)
        FAIL("START reason %u", st->reason);
    last_code = st->code;
    last_reserved = st->reserved;
    check_quiet(me, "start");
    if (!i) {
        for (int c = 0; c < NCPU; c++)
            park_snap[c] = atomic_load(&progress[c]);
        atomic_store(&session_t, park_host_now());
        atomic_store(&session_owner, me);
    } else {
        check_still(me, "while the host had closed the port");
    }
    /* A fault in the session rewrites the trap's return state. */
    host_reg_ELR_EL2 = host_reg_SPSR_EL2 = host_reg_ESR_EL2 = host_reg_FAR_EL2 = 0xbad;
    host_reg_SYS_IMP_APL_ELR_GL1 = host_reg_SYS_IMP_APL_SPSR_GL1 = 0xbad;
    host_reg_SYS_IMP_APL_ESR_GL1 = host_reg_SYS_IMP_APL_FAR_GL1 = 0xbad;
    usleep(session_ns / 1000);
    check_quiet(me, "end");
    check_still(me, "the session");
    int ret = i < script_n ? script[i] : 1;
    if (ret < 0) /* the host closed the port: its DTR comes back after a while */
        atomic_store(&link_down_until, park_host_now() + link_down_ns);
    atomic_store(&proxy_owner, -1);
    atomic_fetch_sub(&in_proxy, 1);
    atomic_fetch_add(&proxy_calls, 1);
    return ret;
}

/* ---- scenarios --------------------------------------------------------------------------- */
static void wait_parks(int target, u64 ms, const char *what)
{
    u64 end = park_host_now() + ms * 1000000UL;
    while (atomic_load(&parks_done) < target)
        if (park_host_now() > end) {
            FAIL("%s: %d of %d parks after %lu ms", what, atomic_load(&parks_done), target, ms);
            return;
        }
}

static void wait_sessions(int target, u64 ms, const char *what)
{
    u64 end = park_host_now() + ms * 1000000UL;
    while (atomic_load(&proxy_calls) < target)
        if (park_host_now() > end) {
            FAIL("%s: %d of %d proxy sessions after %lu ms", what, atomic_load(&proxy_calls),
                 target, ms);
            return;
        }
}

static void check_resumed(const char *what)
{
    u64 before[NCPU];
    for (int c = 0; c < NCPU; c++)
        before[c] = atomic_load(&progress[c]);
    u64 end = park_host_now() + 2000000000UL;
    for (int c = 0; c < NCPU; c++)
        while (atomic_load(&progress[c]) == before[c])
            if (park_host_now() > end) {
                FAIL("%s: cpu%d never ran again after the park", what, c);
                break;
            }
}

static void new_park(void)
{
    atomic_store(&script_i, 0);
    script_n = 0;
}

static void ms_sleep(u64 ms)
{
    usleep(ms * 1000);
}

/* A core out of reset (cpu7, not one of the looping ones) while a park is held. */
static atomic_ulong arrived_t;
static void *reset_core(void *arg)
{
    (void)arg;
    park_host_cpu = NCPU;
    sptm_virt_park_arrive();
    atomic_store(&arrived_t, park_host_now());
    return NULL;
}

static void on_alarm(int sig)
{
    (void)sig;
    static const char m[] = "FAIL: the harness hung (a core never came back)\n";
    write(2, m, sizeof(m) - 1);
    _exit(3);
}

static const u8 magic[8] = {0x1b, 'A', 'U', 'R', 'A', 'P', 'R', 'K'};

int main(void)
{
    pthread_t th[NCPU];

    signal(SIGALRM, on_alarm);
    alarm(20);
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (int c = 0; c < NCPU; c++)
        pthread_create(&th[c], NULL, cpu_main, (void *)(intptr_t)c);
    ms_sleep(20);

    /* A: parks one after the other, by any core. */
    size_t mark = log_mark();
    for (int i = 0; i < 12 && !nfail; i++) {
        new_park();
        int owner = rand() % NCPU, done = atomic_load(&parks_done);
        atomic_store(&action[owner], act_park);
        wait_parks(done + 1, 3000, "A");
        check_resumed("A");
    }
    if (log_has(mark, "still inside aura"))
        FAIL("A: a park named a straggler where every core leaves aura in microseconds");
    if (last_reserved != (void *)1 || last_code != 0x47505521)
        FAIL("A: START said code 0x%x, reserved %p (want 'GPU!', resumes)", last_code,
             last_reserved);
    printf("A: 12 parks by random cores\n");

    /* B: two cores park at the same time: one after the other, neither counted as inside. */
    sptm_virt_park_wait_ms = 300;
    for (int i = 0; i < 4 && !nfail; i++) {
        new_park();
        mark = log_mark();
        int a = rand() % NCPU, b = (a + 1 + rand() % (NCPU - 1)) % NCPU;
        int done = atomic_load(&parks_done);
        atomic_store(&rv2, 0);
        atomic_store(&action[a], act_park_together);
        atomic_store(&action[b], act_park_together);
        wait_parks(done + 2, 5000, "B");
        if (log_has(mark, "still inside aura"))
            FAIL("B: the winner waited for the core that lost the election");
        check_resumed("B");
    }
    printf("B: two at once\n");

    /* C: a core that stays inside aura is waited for a bounded time, and named. */
    if (!nfail) {
        new_park();
        atomic_store(&stragglers_ok, BIT(3));
        atomic_store(&action[3], act_stall);
        while (!atomic_load(&in_aura[3]) || atomic_load(&action[3]))
            ;
        sptm_virt_park_wait_ms = 50;
        mark = log_mark();
        u64 t0 = park_host_now();
        int done = atomic_load(&parks_done);
        atomic_store(&action[0], act_park);
        wait_parks(done + 1, 3000, "C");
        u64 waited = (atomic_load(&session_t) - t0) / 1000000;
        if (waited > 200)
            FAIL("C: the owner waited %lu ms for a core that stays inside aura (bound 50)", waited);
        if (!log_has(mark, "cores 0x8 still inside aura"))
            FAIL("C: the core left inside aura wasn't named");
        ms_sleep(400);
        atomic_store(&stragglers_ok, 0);
        sptm_virt_park_wait_ms = 500;
        check_resumed("C");
    }
    printf("C: a straggler\n");

    /* D: a nested entry (a fault inside a handler) never waits for a park. */
    if (!nfail) {
        new_park();
        sptm_virt_park_wait_ms = 2000;
        atomic_store(&nested_owner, 0);
        mark = log_mark();
        atomic_store(&action[2], act_nested);
        while (!atomic_load(&in_aura[2]) || atomic_load(&action[2]))
            ;
        u64 t0 = park_host_now();
        int done = atomic_load(&parks_done);
        atomic_store(&action[0], act_park);
        wait_parks(done + 1, 4000, "D");
        if ((atomic_load(&session_t) - t0) / 1000000 > 1000 || log_has(mark, "still inside aura"))
            FAIL("D: a nested entry waited for the park, and the owner for it");
        sptm_virt_park_wait_ms = 500;
        check_resumed("D");
    }
    printf("D: a nested entry\n");

    /* E: the host closes the port during a park: the park holds until it opens it again. */
    if (!nfail) {
        new_park();
        script[0] = -1;
        script[1] = 1;
        script_n = 2;
        link_down_ns = 100000000UL;
        int calls = atomic_load(&proxy_calls), done = atomic_load(&parks_done);
        u64 t0 = park_host_now();
        atomic_store(&action[1], act_park);
        wait_parks(done + 1, 3000, "E");
        if (atomic_load(&proxy_calls) != calls + 2)
            FAIL("E: %d proxy sessions for a park whose host closed the port once (want 2)",
                 atomic_load(&proxy_calls) - calls);
        if ((park_host_now() - t0) / 1000000 < 100)
            FAIL("E: the park ended before the host opened the port again");
        check_resumed("E");
    }
    printf("E: the host closes the port\n");

    /* F: the host's request on the pipe. */
    if (!nfail) {
        sptm_virt_park_poll_ms = 10;
        for (int c = 0; c < NCPU; c++)
            atomic_store(&mode[c], 1);
        ms_sleep(30);
        int p0 = atomic_load(&polls);
        u64 tp = park_host_now();
        ms_sleep(300);
        int np = atomic_load(&polls) - p0;
        u64 ms = (park_host_now() - tp) / 1000000;
        if (np > (int)(ms / 10) + 2)
            FAIL("F: the pipe was polled %d times in %lu ms (at most every 10 ms)", np, ms);
        if (np < (int)(ms / 10 / 2))
            FAIL("F: the pipe was polled only %d times in %lu ms", np, ms);

        int s0 = atomic_load(&proxy_calls);
        static const u8 junk[] = "ATZ\r\nAURAPRK\x1b"
                                 "AURAPR";
        pipe_push(junk, sizeof(junk) - 1);
        ms_sleep(100);
        if (atomic_load(&proxy_calls) != s0)
            FAIL("F: bytes that aren't the request parked the guest");

        new_park();
        pipe_push(magic, 3);
        ms_sleep(30);
        pipe_push(magic + 3, 3);
        ms_sleep(30);
        u64 t_last = park_host_now();
        pipe_push(magic + 6, 2);
        wait_sessions(s0 + 1, 1000, "F (split request)");
        if (atomic_load(&session_t) < t_last)
            FAIL("F: the park started before the request was complete");
        ms_sleep(200);
        if (atomic_load(&proxy_calls) != s0 + 1)
            FAIL("F: one request parked %d times", atomic_load(&proxy_calls) - s0);
        check_resumed("F");

        /* Every core that sees a request tries to claim it at once: one park. */
        for (int c = 0; c < NCPU; c++)
            atomic_store(&mode[c], 4);
        ms_sleep(30);
        new_park();
        atomic_store(&rv4, 0);
        s0 = atomic_load(&proxy_calls);
        pipe_push(magic, 8);
        wait_sessions(s0 + 1, 1000, "F (claimed together)");
        ms_sleep(300);
        if (atomic_load(&proxy_calls) != s0 + 1)
            FAIL("F: one request claimed together parked %d times", atomic_load(&proxy_calls) - s0);
        check_resumed("F");

        /* An EL2 trap is preferred to GL ones while the grace lasts. */
        sptm_virt_park_gl_grace_ms = 300;
        for (int c = 0; c < NCPU; c++)
            atomic_store(&mode[c], c == NCPU - 1 ? 3 : 2);
        ms_sleep(50);
        new_park();
        s0 = atomic_load(&proxy_calls);
        u64 t0 = park_host_now();
        pipe_push(magic, 8);
        wait_sessions(s0 + 1, 1000, "F (EL2 preferred)");
        u64 took = (atomic_load(&session_t) - t0) / 1000000;
        if (atomic_load(&session_owner) != NCPU - 1 || took >= 250)
            FAIL("F: with an EL2 trap every 40 ms, cpu%d parked after %lu ms (want cpu%d, EL2)",
                 atomic_load(&session_owner), took, NCPU - 1);
        check_resumed("F");

        /* GL traps only: the request parks once the grace is over. */
        sptm_virt_park_gl_grace_ms = 150;
        for (int c = 0; c < NCPU; c++)
            atomic_store(&mode[c], 2);
        ms_sleep(50);
        new_park();
        s0 = atomic_load(&proxy_calls);
        t0 = park_host_now();
        pipe_push(magic, 8);
        wait_sessions(s0 + 1, 1500, "F (GL only)");
        took = (atomic_load(&session_t) - t0) / 1000000;
        if (took < 150)
            FAIL("F: GL traps only: parked after %lu ms, inside the 150 ms grace", took);
        for (int c = 0; c < NCPU; c++)
            atomic_store(&mode[c], 0);
        check_resumed("F");
    }
    printf("F: the host's request\n");

    /* H: a core out of reset during a park waits until it ends. */
    if (!nfail) {
        new_park();
        session_ns = 150000000UL;
        int done = atomic_load(&parks_done);
        atomic_store(&action[5], act_park);
        while (atomic_load(&proxy_owner) != 5)
            ;
        pthread_t rt;
        pthread_create(&rt, NULL, reset_core, NULL);
        wait_parks(done + 1, 3000, "H");
        u64 end = park_host_now();
        pthread_join(rt, NULL);
        if (atomic_load(&arrived_t) + 1000000UL < atomic_load(&session_t) + session_ns)
            FAIL("H: a core out of reset went on into the guest during the park");
        (void)end;
        session_ns = 20000000UL;
        check_resumed("H");
    }
    printf("H: a core out of reset\n");

    /* G: afterwards the counts are back where they were: a park is quick and names no one. */
    if (!nfail) {
        new_park();
        mark = log_mark();
        u64 t0 = park_host_now();
        int done = atomic_load(&parks_done);
        atomic_store(&action[4], act_park);
        wait_parks(done + 1, 3000, "G");
        u64 took = (atomic_load(&session_t) - t0) / 1000000;
        if (took > 100 || log_has(mark, "still inside aura"))
            FAIL("G: a park after the others took %lu ms or named a straggler", took);
        check_resumed("G");
    }
    printf("G: counts balanced\n");

    atomic_store(&stop_all, true);
    int f = atomic_load(&nfail);
    printf("%s: %d failures\n", f ? "FAILED" : "ok", f);
    fflush(stdout);
    _exit(f ? 1 : 0);
}
