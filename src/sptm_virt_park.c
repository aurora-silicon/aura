/* SPDX-License-Identifier: MIT */

/*
 * Stopping the world for the host while the virtualized SPTM's guest runs, and starting it again.
 *
 * One core at a time owns the proxy. Before it hands the host the proxy, every other core stops
 * at its next entry into aura (sptm_virt_park_enter, first in every exception handler), and the
 * owner waits, for a bounded time, until no other core is still inside one of aura's handlers:
 * the proxy and the console share one USB controller, and with the other cores still printing
 * the link died (J813, ten cores, 2026-11-15). A core that runs guest code without trapping, or
 * is powered down in idle, holds nothing of aura's; it stops when it next enters.
 *
 * A park that resumes ends with the host's P_EXIT: the owner releases the others and returns into
 * the guest, the trap it parked in completed as if the park had not happened. The host closing
 * the port does not end it (the CDC pipe closes with the host's DTR, which ends uartproxy_run's
 * read): the park waits for the port to open again and offers the proxy anew.
 *
 * The host can ask for a park while the guest runs, by writing PARK_MAGIC on the proxy pipe,
 * which nothing else reads then. The periodic hooks (the GL traps, the trapped WFx, every
 * stage-2 abort) look at the pipe at most every sptm_virt_park_poll_ms, and the next trap after
 * the request parks; a GL trap only once no EL2 trap has come for sptm_virt_park_gl_grace_ms,
 * since a fault in a proxy read in GL2 does not return the way it does at EL2.
 */

#ifdef PARK_HOST
#include "park_host.h"
#else
#include "cpu_regs.h"
#include "exception.h"
#include "gxf.h"
#include "iodev.h"
#include "memory.h"
#include "smp.h"
#include "sptm_virt.h"
#include "sptm_virt_priv.h"
#include "uartproxy.h"
#include "utils.h"
#include "xnuboot.h"
#endif

/* "\x1bAURAPRK" on the proxy pipe, oldest byte lowest: the host's request for a park. */
#define PARK_MAGIC     0x4b5250415255411bUL
#define PARK_CODE_HOST 0x50524b21 /* 'PRK!' */
/* START's reserved word: this park resumes on P_EXIT (tools/sptm_park.py --continue). */
#define PARK_START_RESUMES ((void *)1)

volatile int sptm_virt_parking_cpu = -1;
u32 sptm_virt_park_wait_ms = 500;      /* how long an owner waits for the others to leave aura */
u32 sptm_virt_park_poll_ms = 10;       /* how often the hooks look at the proxy pipe; 0: never */
u32 sptm_virt_park_gl_grace_ms = 1000; /* how long a request waits for an EL2 trap */

/*
 * For the host, by symbol (tools/sptm_park.py): the park held now, or the last one. state: 0 none,
 * 1 a park that resumes, 2 one that doesn't. The masks are by aura core index.
 */
struct sptm_virt_park_info {
    u32 state;
    s32 cpu;
    u32 code;
    u32 relinks;    /* the host closed the port and opened it again */
    u64 count;      /* parks that resumed */
    u64 waiting;    /* cores stopped at their entry when the proxy started */
    u64 stragglers; /* cores still inside aura when the owner gave up waiting */
    u64 wait_ticks; /* the owner's wait for the others */
    u64 t_start, t_end;
    u64 requests;  /* the host's requests seen on the pipe */
    u32 poke_back; /* what sptm_virt_park_poke32 read back */
    u32 pad;
} sptm_virt_park_info;

/* Per core: how many of aura's handlers it is inside (a fault in one nests). */
static volatile u32 park_busy[MAX_CPUS];
static volatile u64 park_waiting;

static volatile u64 park_req_t; /* when the host's request was seen; 0: none pending */
static volatile u64 park_poll_next;
static volatile u32 park_poll_busy;
static u64 park_sr; /* the pipe's last 8 bytes */

static u64 park_ms(u32 ms)
{
    return (u64)ms * (mrs(CNTFRQ_EL0) / 1000);
}

/* With park_busy[me] == n: wait out of aura's count while another core owns a park. */
static void park_wait(int me, u32 n)
{
    while (1) {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        int p = __atomic_load_n(&sptm_virt_parking_cpu, __ATOMIC_SEQ_CST);
        if (p < 0 || p == me)
            return;
        __atomic_fetch_or(&park_waiting, BIT(me), __ATOMIC_SEQ_CST);
        __atomic_store_n(&park_busy[me], 0, __ATOMIC_SEQ_CST);
        while ((p = __atomic_load_n(&sptm_virt_parking_cpu, __ATOMIC_ACQUIRE)) >= 0 && p != me)
            sysop("wfe");
        __atomic_store_n(&park_busy[me], n, __ATOMIC_SEQ_CST);
        __atomic_fetch_and(&park_waiting, ~BIT(me), __ATOMIC_SEQ_CST);
    }
}

/*
 * First in every exception handler. Counts this core as inside aura, then, if another core owns a
 * park, waits until it ends. A nested entry (a fault in one of aura's own handlers) never waits:
 * the outer handler finishes first, and the owner waits for that. Returns what
 * sptm_virt_park_exit takes; -1 with the MMU off (a core out of reset), where no atomic works.
 */
int sptm_virt_park_enter(void)
{
    if (!mmu_active())
        return -1;
    int me = smp_id();
    if (__atomic_fetch_add(&park_busy[me], 1, __ATOMIC_SEQ_CST))
        return me;
    park_wait(me, 1);
    return me;
}

/*
 * sptm_virt_cpu_arm, before a core enters the guest from reset (XNU started it, or it woke from
 * idle): it has no handler of aura's below it, whatever its count says (a core that powered down
 * in a WFI inside a handler left it raised). While another core owns a park it waits here, before
 * it runs guest code.
 */
void sptm_virt_park_arrive(void)
{
    if (!mmu_active())
        return;
    int me = smp_id();
    __atomic_store_n(&park_busy[me], 1, __ATOMIC_SEQ_CST);
    park_wait(me, 1);
    __atomic_store_n(&park_busy[me], 0, __ATOMIC_RELEASE);
}

/* Last in every exception handler: the core leaves aura. */
void sptm_virt_park_exit(int me)
{
    if (me >= 0)
        __atomic_fetch_sub(&park_busy[me], 1, __ATOMIC_RELEASE);
}

static u64 park_inside(int me)
{
    u64 m = 0;
    for (int c = 0; c < MAX_CPUS; c++)
        if (c != me && __atomic_load_n(&park_busy[c], __ATOMIC_ACQUIRE))
            m |= BIT(c);
    return m;
}

/*
 * Make this core the only one past its entry: win the election (a core that loses waits like any
 * other, then takes its turn), then wait up to sptm_virt_park_wait_ms for every other core to leave
 * aura. One that doesn't is named and left inside: it is in a handler, and finishes on its own.
 */
void sptm_virt_park_others(void)
{
    int me = smp_id();

    while (1) {
        int none = -1;
        if (__atomic_compare_exchange_n(&sptm_virt_parking_cpu, &none, me, false, __ATOMIC_SEQ_CST,
                                        __ATOMIC_SEQ_CST) ||
            none == me)
            break;
        park_wait(me, __atomic_load_n(&park_busy[me], __ATOMIC_RELAXED));
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    u64 t0 = mrs(CNTPCT_EL0), lim = park_ms(sptm_virt_park_wait_ms), inside;
    while ((inside = park_inside(me)) && mrs(CNTPCT_EL0) - t0 < lim)
        ;
    sptm_virt_park_info.cpu = me;
    sptm_virt_park_info.wait_ticks = mrs(CNTPCT_EL0) - t0;
    sptm_virt_park_info.waiting = __atomic_load_n(&park_waiting, __ATOMIC_SEQ_CST);
    sptm_virt_park_info.stragglers = inside;
    if (inside)
        printf("SPTM-virt[park]: cpu%d: cores 0x%lx still inside aura after %u ms; going on "
               "without them\n",
               me, inside, sptm_virt_park_wait_ms);
}

#ifndef PARK_HOST
/*
 * For the host, from the owner's proxy: the cores that have entered the guest and are not stopped
 * at an entry now (running guest code, powered down in idle, or inside aura). 0 means every guest
 * core is stopped where it can't run guest code until the park ends.
 */
__attribute__((used, retain)) u64 sptm_virt_park_unstopped(void)
{
    int me = smp_id();
    u64 m = 0, waiting = __atomic_load_n(&park_waiting, __ATOMIC_SEQ_CST);

    for (int c = 0; c < MAX_CPUS; c++)
        if (c != me && sptm_virt_cpu_entries[c] && !(waiting & BIT(c)))
            m |= BIT(c);
    return m;
}

/*
 * For the host, from the owner's proxy: replace one 32-bit word of guest code at pa, which must
 * hold expect, with val, and make every core fetch the new word. Only in a park that resumes, with
 * every guest core stopped. The word is read back from DRAM (cleaned and invalidated to the point
 * of coherency first): a range lock in the memory controller drops a write-back without a fault,
 * and a read that hit the cache would not show it. 0, or -1 not this core's resumable park, -2 a
 * guest core not stopped, -3 the word isn't expect, -4 the word didn't take (it reads back
 * as returned in sptm_virt_park_info.poke_back), -5 pa isn't 4-byte aligned guest RAM.
 */
__attribute__((used, retain)) int sptm_virt_park_poke32(u64 pa, u32 expect, u32 val)
{
    u64 ram = ALIGN_DOWN(cur_boot_args.phys_base, BIT(32));

    if (sptm_virt_parking_cpu != smp_id() || sptm_virt_park_info.state != 1)
        return -1;
    if (sptm_virt_park_unstopped())
        return -2;
    if ((pa & 3) || pa < cur_boot_args.phys_base || pa + 4 > ram + mem_size_actual)
        return -5;
    dc_civac_range((void *)pa, 4);
    sysop("dsb sy");
    if (read32(pa) != expect)
        return -3;
    write32(pa, val);
    dc_civac_range((void *)pa, 4);
    sysop("dsb ish");
    sysop("ic ialluis");
    sysop("dsb ish");
    sysop("isb");
    u32 back = read32(pa);
    sptm_virt_park_info.poke_back = back;
    return back == val ? 0 : -4;
}
#endif

/* The owner lets the others go. */
void sptm_virt_park_release(void)
{
    __atomic_store_n(&sptm_virt_parking_cpu, -1, __ATOMIC_SEQ_CST);
    sysop("dsb sy");
    sysop("sev");
}

/* A core that stops for good (a one-way park's): out of aura's count, and waiting. */
void sptm_virt_park_forever(void)
{
    int me = smp_id();
    __atomic_fetch_or(&park_waiting, BIT(me), __ATOMIC_SEQ_CST);
    __atomic_store_n(&park_busy[me], 0, __ATOMIC_SEQ_CST);
    while (1)
        sysop("wfe");
}

/*
 * The proxy until the host's P_EXIT. A failed read means the host closed the port (its DTR
 * dropped): wait until one opens it again, and start over with a new START.
 */
int sptm_virt_proxy_hold(struct uartproxy_msg_start *start)
{
    iodev_id_t id = uartproxy_iodev;

    while (1) {
        int ret = uartproxy_run(start);
        if (ret >= 0)
            return ret;
        sptm_virt_park_info.relinks++;
        do {
            iodev_handle_events(id);
            udelay(1000);
        } while (!iodev_can_write(id));
    }
}

struct park_regs {
    u64 elr, spsr, esr, far, gelr, gspsr, gesr, gfar;
    bool gl;
};

/* The trap's return state: a fault inside the proxy session rewrites it. */
static void park_save(struct park_regs *r)
{
    r->elr = mrs(ELR_EL2);
    r->spsr = mrs(SPSR_EL2);
    r->esr = mrs(ESR_EL2);
    r->far = mrs(FAR_EL2);
    r->gl = in_gl12();
    if (!r->gl)
        return;
    r->gelr = mrs(SYS_IMP_APL_ELR_GL1);
    r->gspsr = mrs(SYS_IMP_APL_SPSR_GL1);
    r->gesr = mrs(SYS_IMP_APL_ESR_GL1);
    r->gfar = mrs(SYS_IMP_APL_FAR_GL1);
}

static void park_restore(const struct park_regs *r)
{
    msr(ELR_EL2, r->elr);
    msr(SPSR_EL2, r->spsr);
    msr(ESR_EL2, r->esr);
    msr(FAR_EL2, r->far);
    if (r->gl) {
        msr(SYS_IMP_APL_ELR_GL1, r->gelr);
        msr(SYS_IMP_APL_SPSR_GL1, r->gspsr);
        msr(SYS_IMP_APL_ESR_GL1, r->gesr);
        msr(SYS_IMP_APL_FAR_GL1, r->gfar);
    }
    sysop("isb");
}

/*
 * After sptm_virt_park_others: give the host the proxy (START first, nothing flushed before it),
 * and on its P_EXIT release the others and return into the trap.
 */
int sptm_virt_park_proxy(struct exc_info *ctx, u32 code)
{
    struct park_regs r;
    int me = smp_id();

    if (sptm_virt_parking_cpu != me)
        sptm_virt_park_others();
    park_save(&r);
    sptm_virt_park_info.state = 1;
    sptm_virt_park_info.code = code;
    sptm_virt_park_info.t_start = mrs(CNTPCT_EL0);
    struct uartproxy_msg_start start = {
        .reason = START_HV,
        .code = code,
        .info = ctx,
        .reserved = PARK_START_RESUMES,
    };
    int ret = sptm_virt_proxy_hold(&start);
    park_restore(&r);
    sptm_virt_park_info.t_end = mrs(CNTPCT_EL0);
    sptm_virt_park_info.state = 0;
    sptm_virt_park_info.count++;
    sptm_virt_park_release();
    printf("SPTM-virt[park]: cpu%d resumes the guest after %lu ms (P_EXIT %d); 0x%lx had stopped "
           "in aura, 0x%lx were still inside; the host reconnected %u times\n",
           me, (sptm_virt_park_info.t_end - sptm_virt_park_info.t_start) / (mrs(CNTFRQ_EL0) / 1000),
           ret, sptm_virt_park_info.waiting, sptm_virt_park_info.stragglers,
           sptm_virt_park_info.relinks);
    return ret;
}

/* Read what the host wrote on the proxy pipe while the guest runs, looking for PARK_MAGIC. */
static void park_poll(u64 now)
{
    iodev_id_t id = uartproxy_iodev;

    if (!(iodev_get_usage(id) & USAGE_UARTPROXY))
        return;
    iodev_handle_events(id);
    for (int left = 64; left > 0;) {
        u8 b[16];
        ssize_t n = iodev_can_read(id);
        if (n <= 0)
            break;
        n = min(n, (ssize_t)sizeof(b));
        n = min(n, (ssize_t)left);
        if (iodev_read(id, b, n) != n)
            break;
        left -= n;
        for (ssize_t i = 0; i < n; i++) {
            park_sr = (park_sr >> 8) | ((u64)b[i] << 56);
            if (park_sr == PARK_MAGIC) {
                park_sr = 0;
                sptm_virt_park_info.requests++;
                __atomic_store_n(&park_req_t, now, __ATOMIC_SEQ_CST);
            }
        }
    }
}

/*
 * From the periodic hooks, before anything else there: is a park the host asked for due on this
 * trap? Cheap unless the pipe is due a look. gl: the trap is in GL2.
 */
bool sptm_virt_park_requested(bool gl)
{
    if (!sptm_virt_park_poll_ms || !sptm_virt_guest_live)
        return false;
    u64 now = mrs(CNTPCT_EL0);
    u64 req = __atomic_load_n(&park_req_t, __ATOMIC_ACQUIRE);
    if (!req) {
        if (now < park_poll_next || __atomic_exchange_n(&park_poll_busy, 1, __ATOMIC_ACQUIRE))
            return false;
        /* Another core may have looked since this one read the counter. */
        now = mrs(CNTPCT_EL0);
        if (now < park_poll_next) {
            __atomic_store_n(&park_poll_busy, 0, __ATOMIC_RELEASE);
            return false;
        }
        park_poll_next = now + park_ms(sptm_virt_park_poll_ms);
        park_poll(now);
        __atomic_store_n(&park_poll_busy, 0, __ATOMIC_RELEASE);
        req = __atomic_load_n(&park_req_t, __ATOMIC_ACQUIRE);
        if (!req)
            return false;
    }
    /* now may predate a request another core has just seen. */
    return !gl || (now > req && now - req >= park_ms(sptm_virt_park_gl_grace_ms));
}

/* A park that never resumes: the proxy until P_EXIT, then this core stops too. */
void sptm_virt_park_oneway(struct uartproxy_msg_start *start)
{
    if (sptm_virt_parking_cpu != smp_id())
        sptm_virt_park_others();
    sptm_virt_park_info.state = 2;
    sptm_virt_park_info.code = start->code;
    sptm_virt_park_info.t_start = mrs(CNTPCT_EL0);
    sptm_virt_proxy_hold(start);
    sptm_virt_park_forever();
}

/* The park the host asked for, on the one core that claims the request. */
void sptm_virt_park_host(struct exc_info *ctx)
{
    if (!__atomic_exchange_n(&park_req_t, 0, __ATOMIC_SEQ_CST))
        return;
    sptm_virt_park_others();
    printf("SPTM-virt[park]: the host asked: parking cpu%d in the proxy at XNU pc 0x%lx%s; P_EXIT "
           "resumes the guest\n",
           smp_id(), ctx->elr, in_gl12() ? " (in GL2: no EL2 trap came)" : "");
    sptm_virt_park_proxy(ctx, PARK_CODE_HOST);
}
