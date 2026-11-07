/* SPDX-License-Identifier: MIT */

/*
 * macOS suspend to RAM under the virtualized SPTM, and its trace.
 *
 * aura's debug link (DockChannel, and the ATC0 USB port the CDC proxy uses) is hidden from XNU,
 * but XNU's PMGR driver still lowers the link's power domains: a hook on those PS words keeps them
 * on while macOS runs. With the sleep path enabled (SPTM_VIRT_S2R), pass-through stage-2 hooks on
 * the power-management blocks record every access into a ring in aura's heap (the carve-out, which
 * survives the sleep in self-refresh), and two of them mark the sleep: each core disabling itself
 * in pmgr-soc-misc, and the boot core starting the S2R timer in pmgr-aon-pst, the commit.
 *
 * Stop mode stops in the proxy before the commit, with the whole entry sequence in the ring. Go
 * mode lets the sleep happen as natively: from the first core's self-disable aura prints nothing
 * and its console moves to the always-on DockChannel; at the commit it hands the link's domains
 * the states XNU asked for and can tear its USB gadget down; the first reset after the commit is
 * the wake, and the link can be taken again on a running macOS.
 *
 * MMIO addresses are T8140's, as is everything in sptm_virt*.c.
 */

#include "sptm_virt.h"
#include "adt.h"
#include "aic.h"
#include "arm_cpu_regs.h"
#include "exception.h"
#include "hv.h"
#include "iodev.h"
#include "malloc.h"
#include "memory.h"
#include "nv2.h"
#include "pmgr.h"
#include "smp.h"
#include "sptm_virt_priv.h"
#include "string.h"
#include "types.h"
#include "uartproxy.h"
#include "usb.h"
#include "usb_cdc_atc.h"
#include "utils.h"

#define S2R_RING_BYTES (8UL << 20)

/* The sleep rules start a while after setup: XNU's PMGR driver writes the same registers while
 * it boots, and a sleep comes minutes after the desktop. After a wake with the link re-taken they
 * come back quickly. */
#define S2R_ARM_AFTER_S   45
#define S2R_REARM_AFTER_S 3

/* The S2R timer start in pmgr-aon-pst (ADT pmgr reg 31): the last step of XNU's quiesceHW. */
#define S2R_TIMER_OFF 0x4000

/* The always-on NUB's AP wake-source status words. */
#define S2R_NUB_WAKE_STATUS 0x308004028UL
#define S2R_NUB_WAKE_WORDS  8

/*
 * The AMCC error-enable writes macOS's error handler makes at quiesce, for when that handler is
 * hidden from XNU (as it is from Linux): an enabled, pending memory-controller error at the commit
 * wakes the SoC within 20 ms. Word 0 bit 25 is DSID_AGE_OUT, word 1 bit 8 UNEXP_RT_HIT_DIR.
 */
#define S2R_AMCC_ERR_EN(plane, word) (0x220025000UL + ((u64)(plane) << 18) + (word) * 8)
#define S2R_AMCC_PLANES              4
#define S2R_QUIESCE_DSID_AGE_OUT     BIT(3)
#define S2R_QUIESCE_RT_HIT_DIR       BIT(4)

struct s2r_rec {
    u64 t;    /* CNTPCT_EL0 */
    u64 meta; /* pa [47:0] | width [49:48] | write [50] | cpu [54:51] | marker [55] */
    u64 val;  /* value written or read, or the marker's argument */
    u64 pc;   /* guest PC, or the marker's tag (8 chars) */
};

#define S2R_META(pa, width, write, cpu, marker)                                                    \
    (((pa) & GENMASK(47, 0)) | ((u64)((width) & 3) << 48) | ((u64) !!(write) << 50) |              \
     ((u64)((cpu) & 15) << 51) | ((u64) !!(marker) << 55))
#define S2R_MARKER BIT(55)
#define S2R_WRITE  BIT(50)

/* For the host: read over the proxy after a stop, or after a reset that kept DRAM. */
struct s2r_rec *sptm_virt_s2r_ring;
u64 sptm_virt_s2r_nrec;          /* capacity in records */
volatile u64 sptm_virt_s2r_head; /* records ever appended */
u32 sptm_virt_s2r_pages;         /* pages hooked */

/* Set by the host before entry (run_guest_sptm.py). */
u32 sptm_virt_s2r_quiesce;      /* S2R_QUIESCE_* */
u32 sptm_virt_s2r_usb_retake;   /* 1: re-take the link after a wake; n > 1: also drop it n s in */
u32 sptm_virt_s2r_usb_delay_ms; /* delay from XNU's return to the re-take, 0 = default */

static struct {
    u64 page;
    const char *what;
    bool seen;
} s2r_pages[160];

static bool s2r_go, s2r_fulltrace, s2r_coproc, s2r_drop_usb;
static u64 s2r_arm_at;
/* Cores with their last WFI trapped (stop mode), parked there, and the core that committed. */
static volatile u32 s2r_twi_mask, s2r_parked_mask;
static volatile int s2r_timer_cpu = -1;
/* Go mode, from the first core's self-disable until the wake: aura prints nothing. Printing in
 * that window delays the boot core's last steps past macOS's 5 s reconfig watchdog. */
volatile bool sptm_virt_s2r_quiet;

/*
 * The debug link's power domains: the PS words of the ATC0 USB controller, its DART and PHY, and
 * all their ancestors. A write that lowers one is not performed, and reads then report what XNU
 * asked for, so its polls complete. At the commit the states XNU asked for are applied.
 */
#define S2R_KEEP_MAX 32
static u64 s2r_keep[S2R_KEEP_MAX];
static u32 s2r_keep_shadow[S2R_KEEP_MAX];
static bool s2r_keep_low[S2R_KEEP_MAX], s2r_keep_told[S2R_KEEP_MAX];
static int s2r_nkeep;
/* With the proxy on CDC the rule holds from setup: XNU lowers three of these domains about a
 * second into its boot. */
static bool s2r_keep_always;
static volatile bool s2r_keep_released;
static const char *const s2r_link_paths[] = {"/arm-io/usb-drd", "/arm-io/dart-usb",
                                             "/arm-io/atc-phy0"};

/*
 * The wake. The commit time is cleaned to DRAM, since the first reset after it reads it with the
 * MMU off; that reset only records the wake, and the banner and the trail are printed later from
 * MMU-on contexts (the first guest trap, the first GEXIT into XNU, the hooks).
 */
static volatile u64 s2r_commit_t;
static volatile u64 s2r_wake_t, s2r_wake_commit_t;
static volatile int s2r_wake_cpu = -1;
static volatile u32 s2r_wake_count;
static volatile u32 s2r_wake_traps; /* guest traps to log after a wake */
static volatile u32 s2r_wake_mmio;  /* hooked accesses to log after a wake */
static volatile bool s2r_wake_banner_done, s2r_wake_xnu_done;
static u64 s2r_commit_head, s2r_wake_head;

static void s2r_usb_tick(bool byte);
static void s2r_usb_quiesce(void);
static void s2r_usb_wake(void);
static void s2r_usb_setup(void);

static void s2r_clean(const volatile void *p, size_t n)
{
    dc_civac_range((void *)p, n);
    sysop("dsb sy");
}

/* Count down a bounded log budget shared by all cores; false once it is spent. */
static bool s2r_take(volatile u32 *budget)
{
    u32 left = *budget;
    while (left)
        if (__atomic_compare_exchange_n(budget, &left, left - 1, false, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED))
            return true;
    return false;
}

/* ---- the ring ------------------------------------------------------------------------------- */

static struct s2r_rec *s2r_slot(void)
{
    if (!sptm_virt_s2r_ring)
        return NULL;
    u64 i = __atomic_fetch_add(&sptm_virt_s2r_head, 1, __ATOMIC_RELAXED);
    return &sptm_virt_s2r_ring[i % sptm_virt_s2r_nrec];
}

void sptm_virt_s2r_note(struct exc_info *ctx, u64 addr, u64 val, bool write, int width)
{
    struct s2r_rec *r = s2r_slot();
    if (!r)
        return;
    r->t = mrs(CNTPCT_EL0);
    r->meta = S2R_META(addr, width, write, smp_id(), 0);
    r->val = val;
    r->pc = ctx ? ctx->elr : 0;
}

static void s2r_marker(const char *tag, u64 arg)
{
    struct s2r_rec *r = s2r_slot();
    u64 t = 0;
    for (int i = 0; i < 8 && tag[i]; i++)
        t |= (u64)(u8)tag[i] << (8 * i);
    if (r) {
        r->t = mrs(CNTPCT_EL0);
        r->meta = S2R_META(0, 0, 0, smp_id(), 1);
        r->val = arg;
        r->pc = t;
    }
    if (!sptm_virt_s2r_quiet)
        printf("SPTM-virt[s2r]: marker %.8s 0x%lx (cpu%d, rec %lu)\n", tag, arg, smp_id(),
               sptm_virt_s2r_head);
}

static void s2r_print_rec(u64 i)
{
    struct s2r_rec *r = &sptm_virt_s2r_ring[i % sptm_virt_s2r_nrec];
    unsigned cpu = (r->meta >> 51) & 15;

    if (r->meta & S2R_MARKER)
        printf("S2R %lu t=%lx c%u MARK %.8s 0x%lx\n", i, r->t, cpu, (const char *)&r->pc, r->val);
    else
        printf("S2R %lu t=%lx c%u %s%u 0x%lx = 0x%lx pc=0x%lx\n", i, r->t, cpu,
               (r->meta & S2R_WRITE) ? "W" : "R", 8u << ((r->meta >> 48) & 3),
               r->meta & GENMASK(47, 0), r->val, r->pc);
}

/* The last n records, oldest first: at a proxy stop, and for the wake record. */
void sptm_virt_s2r_dump_tail(u64 n)
{
    if (!sptm_virt_s2r_ring)
        return;
    u64 head = sptm_virt_s2r_head;
    u64 have = min(head, sptm_virt_s2r_nrec);
    n = min(n, have);
    printf("SPTM-virt[s2r]: last %lu of %lu records\n", n, head);
    for (u64 i = head - n; i < head; i++)
        s2r_print_rec(i);
}

/* ---- hooked pages --------------------------------------------------------------------------- */

static const char *s2r_page_name(u64 page, bool *first)
{
    for (u32 i = 0; i < sptm_virt_s2r_pages; i++)
        if (s2r_pages[i].page == page) {
            *first = !s2r_pages[i].seen;
            s2r_pages[i].seen = true;
            return s2r_pages[i].what;
        }
    *first = false;
    return "?";
}

static bool s2r_is_mbox(const char *what)
{
    size_t n = strlen(what);
    return n > 5 && !strcmp(what + n - 5, "-mbox");
}

/* A 16-byte access (a trapped LDP/STP: width 4 and two values) as one LDP/STP, as the guest issued
 * it. The ASC mailboxes take their messages as such a pair. */
static void s2r_rw128(u64 addr, u64 *val, bool write)
{
    if (write)
        __asm__ volatile("stp %0, %1, [%2]" ::"r"(val[0]), "r"(val[1]), "r"(addr) : "memory");
    else
        __asm__ volatile("ldp %0, %1, [%2]" : "=r"(val[0]), "=r"(val[1]) : "r"(addr) : "memory");
}

static int s2r_keep_idx(u64 addr)
{
    for (int i = 0; i < s2r_nkeep; i++)
        if (s2r_keep[i] == (addr & ~3UL))
            return i;
    return -1;
}

static bool s2r_armed(void)
{
    return s2r_arm_at && mrs(CNTPCT_EL0) >= s2r_arm_at;
}

/* ---- the sleep ------------------------------------------------------------------------------ */

/* From the commit everything aura writes (console, proxy, XNU's vuart TX) goes to DockChannel:
 * uart0 and the CDC link are SoC-domain devices that are off at the wake. */
static void s2r_console_to_aon(void)
{
    s2r_usb_quiesce();
    for (iodev_id_t id = 0; id < IODEV_MAX; id++)
        if (id != IODEV_DOCKCHANNEL_UART && id != IODEV_FB)
            iodev_set_usage(id, iodev_get_usage(id) & ~(USAGE_CONSOLE | USAGE_UARTPROXY));
    iodev_set_usage(IODEV_DOCKCHANNEL_UART,
                    iodev_get_usage(IODEV_DOCKCHANNEL_UART) | USAGE_CONSOLE | USAGE_UARTPROXY);
    uartproxy_iodev = IODEV_DOCKCHANNEL_UART;
    hv_vuart_sleep_commit(IODEV_DOCKCHANNEL_UART);
}

static void s2r_console_off(void)
{
    for (iodev_id_t id = 0; id < IODEV_MAX; id++)
        iodev_set_usage(id, iodev_get_usage(id) & ~USAGE_CONSOLE);
    sptm_virt_s2r_quiet = true;
    s2r_clean(&sptm_virt_s2r_quiet, sizeof(sptm_virt_s2r_quiet));
}

/*
 * At the commit, power the debug link the way a real sleep leaves it. XNU never manages ATC0, so it
 * would stay on, an armed USB wake source (ATC0_PMGR_USB_WAKEUP) whose half-done wakes trip macOS's
 * reconfig watchdog. The gadget goes first (with SPTM_VIRT_S2R_DROP_USB), while its domains are
 * still on: its shutdown touches DWC3 and its DART, and the host then sees a real disconnect.
 */
static void s2r_release_kept(void)
{
    int n = 0;

    if (s2r_drop_usb) {
        s2r_marker("USBDROP", 0);
        usb_iodev_shutdown();
    }
    for (int k = 0; k < s2r_nkeep; k++) {
        if (!s2r_keep_low[k])
            continue;
        write32(s2r_keep[k], s2r_keep_shadow[k]);
        s2r_keep_low[k] = false;
        n++;
    }
    s2r_keep_released = true;
    int failed = 0;
    for (unsigned i = 0; i < ARRAY_SIZE(s2r_link_paths); i++)
        failed += pmgr_adt_power_disable(s2r_link_paths[i]) < 0;
    if (!sptm_virt_s2r_quiet)
        printf("SPTM-virt[s2r]: released %d debug-link domains; powered the ATC0 link down%s\n", n,
               failed ? " (some domains refused)" : "");
}

static void s2r_quiesce(void)
{
    if (sptm_virt_s2r_quiesce & S2R_QUIESCE_DSID_AGE_OUT)
        for (int p = 0; p < S2R_AMCC_PLANES; p++)
            clear32(S2R_AMCC_ERR_EN(p, 0), BIT(25));
    if (sptm_virt_s2r_quiesce & S2R_QUIESCE_RT_HIT_DIR)
        for (int p = 0; p < S2R_AMCC_PLANES; p++)
            clear32(S2R_AMCC_ERR_EN(p, 1), BIT(8));
    sysop("dsb sy");
}

/*
 * A core taking part in the sleep: it disabled itself in PMGR, or the boot core armed the
 * clusters off or committed. Go mode gets out of its way for good: its last WFI powers it down
 * natively, aura's EL2 timers are stopped on it, and from the first such core aura is silent.
 * Stop mode traps its last WFI instead (sptm_virt_s2r_wfi).
 */
static void s2r_core_sleeps(void)
{
    int cpu = smp_id();
    if (s2r_twi_mask & BIT(cpu))
        return;
    bool first = !__atomic_fetch_or(&s2r_twi_mask, BIT(cpu), __ATOMIC_SEQ_CST);
    if (!s2r_go) {
        nv2_hcr_update(HCR_TWI, 0);
        if (s2r_timer_cpu < 0)
            s2r_timer_cpu = cpu;
        s2r_marker("TWI-ARM", cpu);
        return;
    }
    if (first) {
        printf("SPTM-virt[s2r]: cpu%d opens the sleep window: aura silent until the wake\n", cpu);
        s2r_console_to_aon();
        iodev_console_flush();
        s2r_console_off();
    }
    msr(CNTP_CTL_EL0, 0); /* aura's EL2 physical timer (E2H: CNTHP) */
    msr(CNTV_CTL_EL0, 0); /* aura's EL2 virtual timer (CNTHV) */
    sysop("isb");
    s2r_marker("SLEEPS", cpu);
}

/* The commit in go mode, before the S2R timer write goes through. */
static void s2r_commit(void)
{
    s2r_commit_t = mrs(CNTPCT_EL0);
    s2r_clean(&s2r_commit_t, sizeof(s2r_commit_t));
    s2r_clean(&sptm_virt_s2r_usb_retake, sizeof(sptm_virt_s2r_usb_retake));
    s2r_console_to_aon();
    s2r_release_kept();
    s2r_console_off();
    s2r_quiesce();
}

/*
 * Stop mode: the S2R timer start is where the hardware takes over (XNU's "go hib" to the PMU
 * follows, and the PMU does not wait for the last core), so stop before performing it. The ring
 * holds the whole entry sequence and the machine stays up for the host to read it.
 */
static void __attribute__((noreturn)) s2r_stop_before_timer(struct exc_info *ctx, u64 addr, u64 val)
{
    s2r_marker("S2RSTOP", addr);
    printf("SPTM-virt[s2r]: cpu%d is starting the S2R timer ([0x%lx] = 0x%lx, pc 0x%lx): NOT "
           "performed (stop mode). TWI armed 0x%x, parked 0x%x, %lu records. Stopping in the "
           "proxy.\n",
           smp_id(), addr, val, ctx->elr, s2r_twi_mask, s2r_parked_mask, sptm_virt_s2r_head);
    sptm_virt_s2r_dump_tail(4096);
    struct uartproxy_msg_start start = {
        .reason = START_HV,
        .code = 0x53325221, /* 'S2R!' */
        .info = ctx,
    };
    uartproxy_run(&start);
    while (1)
        sysop("wfe");
}

/*
 * A trapped WFI on a core with TWI armed (stop mode). An idle WFI is stepped over. A deep-sleep WFI
 * (bit 0 of S3_5_C15_C6_2 set) is the core's last instruction before power-off: the other cores
 * park there, and when the committing core arrives the host gets the proxy.
 */
bool sptm_virt_s2r_wfi(u64 *regs, u64 esr)
{
    UNUSED(regs);
    int cpu = smp_id();

    if (!(s2r_twi_mask & BIT(cpu)) || (esr & 1)) /* not armed here, or a WFE */
        return false;
    u64 elr = mrs(ELR_EL2);
    u64 ds = SAFE_MRS("s3_5_c15_c6_2");
    if (ds == SAFE_MRS_FAILED || !(ds & BIT(0))) {
        msr(ELR_EL2, elr + 4); /* return as if an interrupt woke it */
        return true;
    }
    s2r_marker("DEEPWFI", ((u64)cpu << 56) | (elr & GENMASK(55, 0)));
    printf("SPTM-virt[s2r]: cpu%d deep-sleep WFI at 0x%lx (S3_5_C15_C6_2 0x%lx)\n", cpu, elr, ds);
    if (cpu != s2r_timer_cpu) {
        __atomic_or_fetch(&s2r_parked_mask, BIT(cpu), __ATOMIC_SEQ_CST);
        printf("SPTM-virt[s2r]: cpu%d parked at its last WFI (stop mode)\n", cpu);
        while (1)
            sysop("wfe");
    }
    printf("SPTM-virt[s2r]: macOS reached its final WFI on the S2R-timer core cpu%d; parked 0x%x; "
           "%lu records. Stopping in the proxy (stop mode).\n",
           cpu, s2r_parked_mask, sptm_virt_s2r_head);
    sptm_virt_s2r_dump_tail(4096);
    struct uartproxy_msg_start start = {
        .reason = START_HV, .code = 0x53325221, /* 'S2R!' */
    };
    uartproxy_run(&start);
    while (1)
        sysop("wfe");
}

static bool s2r_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    int k = width == 2 && !s2r_keep_released && (s2r_keep_always || s2r_armed())
                ? s2r_keep_idx(addr)
                : -1;

    /* The link's domains: XNU's lowering writes are shadowed, and reads then see them. */
    if (k >= 0 && write && (*val & 0xf) < (read32(addr) & 0xf)) {
        s2r_keep_shadow[k] = *val;
        s2r_keep_low[k] = true;
        sptm_virt_s2r_note(ctx, addr, *val, true, width);
        if (!s2r_keep_told[k]) {
            s2r_keep_told[k] = true;
            s2r_marker("KEEPDBG", addr);
            printf("SPTM-virt[s2r]: kept debug-link domain on: "
                   "XNU wrote 0x%lx -> 0x%lx (pc 0x%lx)\n",
                   *val, addr, ctx->elr);
        }
        return true;
    }
    if (k >= 0 && !write && s2r_keep_low[k]) {
        u32 tgt = s2r_keep_shadow[k] & 0xf;
        *val = (read32(addr) & ~0xffUL) | tgt | (tgt << 4);
        sptm_virt_s2r_note(ctx, addr, *val, false, width);
        return true;
    }
    if (k >= 0 && write) {
        /* Not lowering it. A low write only gets here with the domain already low, after a wake
         * and before the re-take powers it: XNU still wants it low. */
        s2r_keep_low[k] = !(*val & 0xf);
        if (s2r_keep_low[k])
            s2r_keep_shadow[k] = *val;
    }

    bool first;
    const char *what = s2r_page_name(addr & ~0x3fffUL, &first);
    bool timer = write && width <= 3 && *val == 1 && !strcmp(what, "pmgr-aon-pst") &&
                 (addr & 0x7fff) == S2R_TIMER_OFF && s2r_armed();
    if (timer && !s2r_go) {
        sptm_virt_rg_stop_wd2();
        sptm_virt_s2r_note(ctx, addr, *val, true, width);
        s2r_stop_before_timer(ctx, addr, *val);
    }
    if (timer)
        s2r_commit();

    if (s2r_wake_mmio && !s2r_is_mbox(what) && s2r_take(&s2r_wake_mmio))
        printf("S2R-WAKE c%d %s %s%u 0x%lx%s0x%lx pc 0x%lx\n", smp_id(), what, write ? "W" : "R",
               8u << width, addr, write ? " = " : " (before read) ", write ? *val : 0UL, ctx->elr);

    if (width == 4) {
        s2r_rw128(addr, val, write);
        sptm_virt_s2r_note(ctx, addr, val[0], write, 3);
        sptm_virt_s2r_note(ctx, addr + 8, val[1], write, 3);
        return true;
    }
    if (width > 4) {
        printf("SPTM-virt[s2r]: %u-byte access at 0x%lx (pc 0x%lx) not supported\n", 1u << width,
               addr, ctx->elr);
        return false;
    }
    sptm_virt_mmio_rw(addr, val, write, width);
    sptm_virt_s2r_note(ctx, addr, *val, write, width);

    if (first && !sptm_virt_s2r_quiet)
        printf("SPTM-virt[s2r]: first access to %s page 0x%lx: %s 0x%lx <- 0x%lx pc 0x%lx\n", what,
               addr & ~0x3fffUL, write ? "wr" : "rd", addr, *val, ctx->elr);
    if (!s2r_armed())
        return true;
    if (timer) {
        s2r_timer_cpu = smp_id();
        s2r_marker("S2RTIMER", addr);
        s2r_commit_head = sptm_virt_s2r_head;
        s2r_core_sleeps();
    }
    /* Each core disables itself in the PMGR CPU block on its way to its last WFI; a cluster armed
     * off is the same moment for the boot core. */
    if (write && (!strcmp(what, "pmgr-soc-misc") ||
                  (!strcmp(what, "pmgr-cluster-ctl") && (*val & 0xf) == 0)))
        s2r_core_sleeps();
    return true;
}

static void s2r_hook_range(const char *what, u64 base, u64 size)
{
    for (u64 page = base & ~0x3fffUL; page < base + size; page += 0x4000) {
        if (sptm_virt_rg_owns(page))
            continue; /* the reset guard records into the ring itself */
        bool dup = false;
        for (u32 i = 0; i < sptm_virt_s2r_pages; i++)
            dup |= s2r_pages[i].page == page;
        if (dup)
            continue; /* already hooked under a more specific name */
        if (sptm_virt_s2r_pages >= ARRAY_SIZE(s2r_pages)) {
            printf("SPTM-virt[s2r]: page table full at %s 0x%lx\n", what, page);
            return;
        }
        s2r_pages[sptm_virt_s2r_pages].page = page;
        s2r_pages[sptm_virt_s2r_pages].what = what;
        sptm_virt_s2r_pages++;
        hv_map_hook(page, s2r_hook, 0x4000);
    }
}

static void s2r_hook_adt(const char *path, int idx, const char *what, u64 off, u64 size)
{
    int p[8];
    u64 pa, sz;

    if (adt_path_offset_trace(adt, path, p) < 0 || adt_get_reg(adt, p, "reg", idx, &pa, &sz) < 0) {
        printf("SPTM-virt[s2r]: no %s reg[%d]; %s not traced\n", path, idx, what);
        return;
    }
    s2r_hook_range(what, pa + off, size ? size : sz);
}

static void s2r_keep_setup(void)
{
    for (unsigned i = 0; i < ARRAY_SIZE(s2r_link_paths); i++) {
        u64 regs[S2R_KEEP_MAX];
        int n = pmgr_adt_collect_psregs(s2r_link_paths[i], regs, S2R_KEEP_MAX);
        if (n < 0)
            printf("SPTM-virt[s2r]: cannot list the power domains of %s\n", s2r_link_paths[i]);
        for (int j = 0; j < n; j++) {
            if (s2r_keep_idx(regs[j]) >= 0)
                continue;
            if (s2r_nkeep >= S2R_KEEP_MAX) {
                printf("SPTM-virt[s2r]: too many debug-link domains; 0x%lx not kept\n", regs[j]);
                continue;
            }
            s2r_keep[s2r_nkeep++] = regs[j];
            s2r_hook_range("pmgr-debugpath", regs[j], 4);
        }
    }
    printf("SPTM-virt[s2r]: keeping %d debug-link PMGR domains on %s:", s2r_nkeep,
           s2r_keep_always ? "from boot (proxy on CDC)" : "across sleep");
    for (int i = 0; i < s2r_nkeep; i++)
        printf(" %d=0x%lx", i, s2r_keep[i]);
    printf("\n");
}

/* The rest of the PMGR, SPMI and mailbox blocks: the full entry trace. */
static void s2r_hook_full(void)
{
    s2r_hook_range("pms-pmc", 0x302800000UL, 0x4000);
    s2r_hook_range("pms-pmc", 0x3028a8000UL, 0x4000);
    s2r_hook_adt("/arm-io/pmgr", 21, "pmgr-soc-ps", 0, 0);
    s2r_hook_adt("/arm-io/pmgr", 28, "pmgr-cluster-ctl", 0, 0);
    s2r_hook_adt("/arm-io/pmgr", 22, "pmgr-pwrgate", 0, 0);
    s2r_hook_adt("/arm-io/pmgr", 27, "pmgr-cllt", 0, 0);
    s2r_hook_adt("/arm-io/pmgr", 4, "nub-ap-ctrl", 0, 0x4000);
    s2r_hook_adt("/arm-io/pmgr", 1, "pmgr-aon", 0, 0);
    s2r_hook_adt("/arm-io/pmgr", 2, "clkgen-dcs", 0x5c000, 0x4000);
    s2r_hook_adt("/arm-io/pmgr", 2, "clkgen-mux", 0x40000, 0x4000);
    for (int i = 0; i < 3; i++)
        s2r_hook_adt("/arm-io/nub-spmi0", i, "spmi0", 0, 0x4000);
    for (int i = 0; i < 3; i++)
        s2r_hook_adt("/arm-io/nub-spmi-a0", i, "spmi-a0", 0, 0x4000);
    s2r_hook_adt("/arm-io/smc", 0, "smc-mbox", 0x8000, 0x4000);
    s2r_hook_adt("/arm-io/pmp", 0, "pmp-mbox", 0x8000, 0x4000);
    s2r_hook_adt("/arm-io/dcp", 0, "dcp-mbox", 0x8000, 0x4000);
}

/* cdc: the proxy runs over the DWC3 CDC link, whose domains are kept on even without the trace. */
void sptm_virt_s2r_setup(bool cdc)
{
    u64 flags = sptm_virt_flags;

    s2r_keep_always = cdc;
    if (!(flags & SPTM_VIRT_S2R)) {
        if (cdc)
            s2r_keep_setup();
        return;
    }
    sptm_virt_s2r_ring = memalign(0x4000, S2R_RING_BYTES);
    if (!sptm_virt_s2r_ring) {
        printf("SPTM-virt[s2r]: no memory for the ring; sleep path off\n");
        sptm_virt_flags &= ~SPTM_VIRT_S2R;
        if (cdc)
            s2r_keep_setup();
        return;
    }
    memset(sptm_virt_s2r_ring, 0, S2R_RING_BYTES);
    sptm_virt_s2r_nrec = S2R_RING_BYTES / sizeof(struct s2r_rec);
    sptm_virt_s2r_head = 0;
    s2r_go = flags & SPTM_VIRT_S2R_GO;
    s2r_fulltrace = flags & SPTM_VIRT_S2R_FULL;
    s2r_coproc = flags & SPTM_VIRT_S2R_COPROC;
    s2r_drop_usb = flags & SPTM_VIRT_S2R_DROP_USB;
    /* In go mode the reset guard leaves macOS's sleep-time watchdog alone. */
    sptm_virt_rg_passthrough = s2r_go;
    s2r_usb_setup();

    /* Each core's self-disable and the commit: a handful of accesses. */
    s2r_hook_adt("/arm-io/pmgr", 24, "pmgr-soc-misc", 0, 0);
    s2r_hook_adt("/arm-io/pmgr", 31, "pmgr-aon-pst", 0, 0);
    if (s2r_coproc) {
        static const struct {
            const char *path, *what;
        } mb[] = {
            {"/arm-io/aop", "aop-mbox"},     {"/arm-io/aop2", "aop2-mbox"},
            {"/arm-io/pmp", "pmp-mbox"},     {"/arm-io/smc", "smc-mbox"},
            {"/arm-io/sep", "sep-mbox"},     {"/arm-io/sio", "sio-mbox"},
            {"/arm-io/ans", "ans-mbox"},     {"/arm-io/mtp", "mtp-mbox"},
            {"/arm-io/dcp", "dcp-mbox"},     {"/arm-io/dcpext", "dcpext-mbox"},
            {"/arm-io/gfx-asc", "gfx-mbox"}, {"/arm-io/gfx1-asc", "gfx1-mbox"},
        };
        for (unsigned i = 0; i < ARRAY_SIZE(mb); i++)
            s2r_hook_adt(mb[i].path, 0, mb[i].what, 0x8000, 0x4000);
        /* Without stage 2 in the secure kernel's world these see XNU's side only. */
        s2r_hook_adt("/arm-io/aop-exclave-mailbox", 0, "aopx-mbox", 0, 0);
        s2r_hook_adt("/arm-io/aop2-exclave-mailbox", 0, "aop2x-mbox", 0, 0);
        s2r_hook_adt("/arm-io/dcp-exclave-mailbox", 0, "dcpx-mbox", 0, 0);
        s2r_hook_adt("/arm-io/esm", 0, "esm-mbox", 0, 0);
    }
    /*
     * The full entry trace. Not in a plain go-mode sleep: XNU hammers these blocks in the last
     * second of its quiesce, and trapping each access slows the power-down past the 5 s
     * reconfig watchdog.
     */
    if (!s2r_go || s2r_fulltrace)
        s2r_hook_full();

    s2r_keep_setup();

    s2r_arm_at = mrs(CNTPCT_EL0) + S2R_ARM_AFTER_S * mrs(CNTFRQ_EL0);
    s2r_marker("S2RSETUP", sptm_virt_s2r_pages);
    printf("SPTM-virt[s2r]: %u pages hooked, %lu-record ring at 0x%lx; %s\n", sptm_virt_s2r_pages,
           sptm_virt_s2r_nrec, (u64)sptm_virt_s2r_ring,
           !s2r_go         ? "stop mode: stopping before the S2R timer start"
           : s2r_fulltrace ? "GO mode + full trace (heavy; expect a reset)"
                           : "GO mode: minimal hooks, the sleep proceeds at native speed");
}

/* ---- the wake ------------------------------------------------------------------------------- */

/* sptm_virt_cpu_reset, MMU off: the first reset after a commit is the wake. */
void sptm_virt_s2r_wake(int cpu)
{
    u64 commit = s2r_commit_t;
    if (!commit)
        return;
    u64 now = mrs(CNTPCT_EL0), frq = mrs(CNTFRQ_EL0);
    s2r_commit_t = 0;
    s2r_wake_t = now;
    s2r_wake_commit_t = commit;
    s2r_wake_cpu = cpu;
    s2r_wake_count++;
    sptm_virt_s2r_quiet = false;
    iodev_set_usage(IODEV_DOCKCHANNEL_UART,
                    iodev_get_usage(IODEV_DOCKCHANNEL_UART) | USAGE_CONSOLE | USAGE_UARTPROXY);
    s2r_wake_banner_done = false;
    s2r_wake_xnu_done = false;
    s2r_wake_traps = 48;
    s2r_wake_mmio = 160;
    s2r_twi_mask = 0;
    s2r_timer_cpu = -1;
    /* the sleep rules come back for the next sleep */
    s2r_arm_at = now + (sptm_virt_s2r_usb_retake ? S2R_REARM_AFTER_S : S2R_ARM_AFTER_S) * frq;
    s2r_clean(&s2r_commit_t, sizeof(s2r_commit_t));
    s2r_clean(&sptm_virt_s2r_quiet, sizeof(sptm_virt_s2r_quiet));
    s2r_clean(&s2r_wake_t, sizeof(s2r_wake_t));
    s2r_clean(&s2r_wake_commit_t, sizeof(s2r_wake_commit_t));
    s2r_clean(&s2r_wake_cpu, sizeof(s2r_wake_cpu));
    s2r_clean(&s2r_wake_count, sizeof(s2r_wake_count));
    s2r_clean(&s2r_wake_banner_done, sizeof(s2r_wake_banner_done));
    s2r_clean(&s2r_wake_xnu_done, sizeof(s2r_wake_xnu_done));
    s2r_clean(&s2r_wake_traps, sizeof(s2r_wake_traps));
    s2r_clean(&s2r_wake_mmio, sizeof(s2r_wake_mmio));
    s2r_clean(&s2r_twi_mask, sizeof(s2r_twi_mask));
    s2r_clean(&s2r_timer_cpu, sizeof(s2r_timer_cpu));
    s2r_clean(&s2r_arm_at, sizeof(s2r_arm_at));
    printf("SPTM-virt[s2r]: WAKE #%u: cpu%d reset after the sleep commit; counter 0x%lx, commit "
           "0x%lx (%ld s)\n",
           s2r_wake_count, cpu, now, commit, frq ? (long)((s64)(now - commit) / (s64)frq) : -1L);
}

static void s2r_aic_dump(const char *tag)
{
    if (!aic)
        return;
    for (u32 w = 0; w <= aic->nr_irq / 32; w++) {
        /* The raw line state, the array right after MASK_CLR: asserting lines, masked or not. */
        u32 hw = read32(aic->base + aic->regs.mask_clr + (aic->max_irq >> 5) * 4 + 4 * w);
        u32 sw = read32(aic->base + aic->regs.sw_set + 4 * w);
        if (hw | sw)
            printf("AIC[%s]: irq %u..: hw 0x%08x sw 0x%08x mask 0x%08x\n", tag, w * 32, hw, sw,
                   read32(aic->base + aic->regs.mask_set + 4 * w));
    }
    /* All ones here at a wake: XNU's quiesce masks survived, so the AIC kept its state. */
    printf("AIC[%s]: mask word 0 0x%08x, last 0x%08x\n", tag,
           read32(aic->base + aic->regs.mask_set),
           read32(aic->base + aic->regs.mask_set + 4 * (aic->nr_irq / 32)));
}

/* exc_sync: the first guest trap after a wake prints the banner; a bounded trail follows. */
void sptm_virt_s2r_wake_trap(u64 esr, u64 elr, bool in_gl)
{
    if (!s2r_wake_t)
        return;
    if (!s2r_wake_banner_done &&
        !__atomic_exchange_n(&s2r_wake_banner_done, true, __ATOMIC_SEQ_CST)) {
        s2r_wake_head = sptm_virt_s2r_head;
        u64 frq = mrs(CNTFRQ_EL0), now = mrs(CNTPCT_EL0);
        printf("SPTM-virt[s2r]: WAKE #%u banner: first guest trap on cpu%d, %lu us after the "
               "reset on cpu%d\n",
               s2r_wake_count, smp_id(), frq ? (now - s2r_wake_t) * 1000000 / frq : 0,
               s2r_wake_cpu);
        printf("SPTM-virt[s2r]: AON wake-source status 0x308004028..44:");
        for (int i = 0; i < S2R_NUB_WAKE_WORDS; i++)
            printf(" %08x", read32(S2R_NUB_WAKE_STATUS + 4 * i));
        printf("\n");
        s2r_aic_dump("wake");
        s2r_marker("WAKE", s2r_wake_cpu);
    }
    if (s2r_wake_traps && s2r_take(&s2r_wake_traps))
        printf("S2R-WAKE-TRAP c%d %s EC 0x%02lx iss 0x%06lx pc 0x%lx\n", smp_id(),
               in_gl ? "GL" : "EL", (esr >> 26) & 0x3f, esr & 0x1ffffff, elr);
}

/* A GEXIT into XNU's normal world: the first one after a wake means XNU runs again. */
void sptm_virt_s2r_xnu_entry(int cpu, u64 target)
{
    s2r_usb_tick(false);
    if (!s2r_wake_t || s2r_wake_xnu_done)
        return;
    s2r_wake_xnu_done = true;
    u64 frq = mrs(CNTFRQ_EL0), now = mrs(CNTPCT_EL0);
    printf("SPTM-virt[s2r]: WAKE #%u: XNU running again on cpu%d at 0x%lx, "
           "%lu ms after the reset\n",
           s2r_wake_count, cpu, target, frq ? (now - s2r_wake_t) * 1000 / frq : 0);
    s2r_marker("XNUBACK", cpu);
    s2r_usb_wake();
}

/*
 * The wake's MMIO record (with the full trace): XNU stopping the AON watchdog ends its platform
 * wake, so everything the ring holds since the wake's first guest trap is the record. With the
 * link re-taken it is printed from the re-take's poll loop, a few records at a time.
 */
static u64 s2r_wakerec_rp, s2r_wakerec_end;

void sptm_virt_s2r_wd_stopped(void)
{
    if (s2r_wakerec_end || !s2r_fulltrace || !s2r_wake_t || !s2r_wake_banner_done)
        return;
    s2r_wakerec_rp = s2r_wake_head;
    s2r_wakerec_end = sptm_virt_s2r_head;
    if (!sptm_virt_s2r_usb_retake) {
        printf("S2R-WAKEREC start: %lu records\n", s2r_wakerec_end - s2r_wakerec_rp);
        sptm_virt_s2r_dump_tail(min(sptm_virt_s2r_head - s2r_wake_head, 16384UL));
        printf("S2R-WAKEREC end\n");
        iodev_console_flush();
    }
}

static void s2r_wakerec_pump(void)
{
    static bool started;

    if (!s2r_wakerec_end || s2r_wakerec_rp >= s2r_wakerec_end || !iodev_can_write(IODEV_USB0))
        return;
    if (!started) {
        started = true;
        printf("S2R-WAKEREC start: %lu records\n", s2r_wakerec_end - s2r_wakerec_rp);
    }
    for (int k = 0; k < 8 && s2r_wakerec_rp < s2r_wakerec_end; k++, s2r_wakerec_rp++)
        if (!(sptm_virt_s2r_ring[s2r_wakerec_rp % sptm_virt_s2r_nrec].meta & S2R_MARKER))
            s2r_print_rec(s2r_wakerec_rp);
    if (s2r_wakerec_rp >= s2r_wakerec_end)
        printf("S2R-WAKEREC end\n");
}

/* With the coprocessor mailboxes hooked, after the wake record: every mailbox access from before
 * the commit to now, a few per poll. */
static void s2r_coproc_pump(void)
{
    static u64 rp, end;
    static int st;

    if (!s2r_coproc || st == 2 || !s2r_wakerec_end || s2r_wakerec_rp < s2r_wakerec_end ||
        !iodev_can_write(IODEV_USB0))
        return;
    u64 frq = mrs(CNTFRQ_EL0);
    if (!st) {
        u64 head = sptm_virt_s2r_head, from = s2r_wake_commit_t - (s2r_fulltrace ? 10 : 60) * frq;
        u64 oldest = head > sptm_virt_s2r_nrec ? head - sptm_virt_s2r_nrec + 64 : 0;
        rp = s2r_commit_head;
        while (rp > oldest && sptm_virt_s2r_ring[(rp - 1) % sptm_virt_s2r_nrec].t >= from)
            rp--;
        end = head;
        printf("S2R-COPROC start: %lu ring records, commit t=%lx wake t=%lx frq %lu\n", end - rp,
               s2r_wake_commit_t, s2r_wake_t, frq);
        st = 1;
    }
    int printed = 0;
    for (int k = 0; k < 2000 && printed < 8 && rp < end; k++, rp++) {
        struct s2r_rec *r = &sptm_virt_s2r_ring[rp % sptm_virt_s2r_nrec];
        unsigned cpu = (r->meta >> 51) & 15;
        if (r->meta & S2R_MARKER) {
            printf("S2C t=%lx c%u MARK %.8s 0x%lx\n", r->t, cpu, (const char *)&r->pc, r->val);
            printed++;
            continue;
        }
        bool first;
        u64 pa = r->meta & GENMASK(47, 0);
        const char *what = s2r_page_name(pa & ~0x3fffUL, &first);
        bool near = r->t >= s2r_wake_commit_t - 2 * frq && r->t <= s2r_wake_t;
        if (!s2r_is_mbox(what) && strcmp(what, "pms-pmc") && !near)
            continue;
        printf("S2C t=%lx c%u %s %s%u 0x%lx = 0x%lx\n", r->t, cpu, what,
               (r->meta & S2R_WRITE) ? "W" : "R", 8u << ((r->meta >> 48) & 3), pa, r->val);
        printed++;
    }
    if (rp >= end) {
        printf("S2R-COPROC end\n");
        st = 2;
    }
}

/* ---- the debug link after a wake -------------------------------------------------------------
 *
 * s2r_release_kept took the link down at the commit and usb_cdc_link_start brings it up, the same
 * code stage 1 runs, except it cannot run all at once: it waits 300 ms in fixed delays, and XNU
 * must not lose a core for that long. So it runs as a coroutine on its own stack. Each GEXIT into
 * XNU and each trapped console access may resume it for one slice, and its waits
 * (usb_cdc_wait_us, usb_cdc_poll32) switch back to the trap. Once the link is up the coroutine
 * polls the controller, which nothing else does while XNU is quiet.
 *
 * The waits only switch during the bring-up, where no lock is held; the poll runs under the
 * console lock, so there they just delay. The port controller is not touched: CC stays up across
 * a sleep, and hpm0 shares its SPMI controller with XNU's hpm1.
 */
#define S2R_USB_STACK_BYTES 0x8000UL
#define S2R_USB_RING_BYTES  0x10000UL /* XNU's console since the sleep, until the host opens it */
#define S2R_USB_START_MS    300       /* after XNU's return; all cores are back by about 110 ms */
#define S2R_USB_GAP_US      200       /* XNU runs at least this long between slices */
#define S2R_USB_POLL_US     1000

enum {
    S2R_USB_OFF, /* not asked for, failed, or quiesced for a sleep */
    S2R_USB_SELFTEST,
    S2R_USB_PENDING,
    S2R_USB_BRINGUP,
    S2R_USB_UP,
};
static volatile u32 s2r_usb_state, s2r_usb_busy;
static volatile u64 s2r_usb_next; /* CNTPCT of the next resume */
static u8 *s2r_usb_stack;
static u64 s2r_usb_co_sp, s2r_usb_main_sp;
static bool s2r_usb_yieldable;
static u64 s2r_usb_t0, s2r_usb_slice_max;
static u32 s2r_usb_slices, s2r_usb_step_now;
static u8 *s2r_usb_ring;
static volatile u64 s2r_usb_ring_wp;
static u64 s2r_usb_ring_rp;
static volatile bool s2r_usb_capture;

/*
 * Saves the callee-saved registers on this stack, stores sp in *save_sp, then loads load_sp and
 * returns into whatever saved its frame there. Top-level asm: gcc ignores naked here and drops
 * the arguments. The frame is 12 words, x30 at word 11.
 */
void s2r_usb_switch(u64 *save_sp, u64 load_sp);
__asm__(".pushsection .text.s2r_usb_switch, \"ax\", %progbits\n"
        ".p2align 2\n"
        ".type s2r_usb_switch, %function\n"
        "s2r_usb_switch:\n"
        "stp x19, x20, [sp, #-96]!\n"
        "stp x21, x22, [sp, #16]\n"
        "stp x23, x24, [sp, #32]\n"
        "stp x25, x26, [sp, #48]\n"
        "stp x27, x28, [sp, #64]\n"
        "stp x29, x30, [sp, #80]\n"
        "mov x9, sp\n"
        "str x9, [x0]\n"
        "mov sp, x1\n"
        "ldp x21, x22, [sp, #16]\n"
        "ldp x23, x24, [sp, #32]\n"
        "ldp x25, x26, [sp, #48]\n"
        "ldp x27, x28, [sp, #64]\n"
        "ldp x29, x30, [sp, #80]\n"
        "ldp x19, x20, [sp], #96\n"
        "ret\n"
        ".size s2r_usb_switch, . - s2r_usb_switch\n"
        ".popsection\n");

static void s2r_usb_yield(u64 ticks)
{
    s2r_usb_next = mrs(CNTPCT_EL0) + ticks;
    s2r_usb_switch(&s2r_usb_co_sp, s2r_usb_main_sp);
}

/* usb.c: true if the caller runs on the re-take's coroutine, where waits switch back to XNU. */
bool sptm_virt_s2r_usb_yieldable(void)
{
    u64 sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    return s2r_usb_yieldable && s2r_usb_stack && sp - (u64)s2r_usb_stack < S2R_USB_STACK_BYTES;
}

bool sptm_virt_s2r_usb_yield(u32 us)
{
    if (!sptm_virt_s2r_usb_yieldable())
        return false;
    s2r_usb_yield((u64)max(us, (u32)S2R_USB_GAP_US) * mrs(CNTFRQ_EL0) / 1000000);
    return true;
}

static void s2r_usb_step(unsigned step)
{
    s2r_usb_step_now = step;
}

/* The keep rule again, before anything is powered, so XNU cannot lower the domains under the link.
 * What XNU last asked for is what the hardware holds now: its writes went through since the commit.
 */
static void s2r_usb_keep_rearm(void)
{
    for (int k = 0; k < s2r_nkeep; k++) {
        u32 v = read32(s2r_keep[k]);
        s2r_keep_shadow[k] = v;
        s2r_keep_low[k] = !(v & 0xf);
        s2r_keep_told[k] = false;
    }
    sysop("dmb ish");
    s2r_keep_released = false;
}

/* XNU's bytes since the sleep, then its live output, once the host has the port open. */
static void s2r_usb_ring_drain(void)
{
    u64 wp = s2r_usb_ring_wp, rp = s2r_usb_ring_rp;

    if (wp == rp || !iodev_can_write(IODEV_USB_VUART))
        return;
    if (wp - rp > S2R_USB_RING_BYTES) {
        printf("USB-RETAKE: lost %lu bytes of XNU's console\n", wp - rp - S2R_USB_RING_BYTES);
        rp = wp - S2R_USB_RING_BYTES;
    }
    u64 off = rp % S2R_USB_RING_BYTES;
    u64 n = min(min(wp - rp, S2R_USB_RING_BYTES - off), 4096UL);
    ssize_t w = iodev_write(IODEV_USB_VUART, s2r_usb_ring + off, n);
    s2r_usb_ring_rp = rp + (w > 0 ? w : 0);
}

static void __attribute__((noreturn)) s2r_usb_main(void)
{
    u64 frq = mrs(CNTFRQ_EL0);

    s2r_usb_keep_rearm();
    pmgr_keep_configured = true;
    s2r_usb_yieldable = true;
    int ret = usb_cdc_link_start(s2r_usb_step, usb_cdc_atc_lane());
    s2r_usb_yieldable = false;
    pmgr_keep_configured = false;
    if (ret) {
        /* The domains stay on and the keep rule stands, so the next commit powers them down. */
        printf("USB-RETAKE: failed at step %u\n", s2r_usb_step_now);
        s2r_usb_state = S2R_USB_OFF;
        while (1)
            s2r_usb_yield(0);
    }
    hv_vuart_usb_back(IODEV_USB0);
    uartproxy_iodev = IODEV_USB0;
    printf("USB-RETAKE: controller up %lu ms after the start: %u slices, longest %lu us\n",
           frq ? (mrs(CNTPCT_EL0) - s2r_usb_t0) * 1000 / frq : 0, s2r_usb_slices,
           frq ? s2r_usb_slice_max * 1000000 / frq : 0);
    /* The wake's own lines went out while the link was down, and the console buffer has usually
     * dropped them by now: say again which wake this is. */
    if (s2r_wake_t)
        printf("SPTM-virt[s2r]: WAKE #%u: cpu%d reset after the sleep commit, link back %lu ms "
               "after the reset\n",
               s2r_wake_count, s2r_wake_cpu, frq ? (mrs(CNTPCT_EL0) - s2r_wake_t) * 1000 / frq : 0);
    s2r_usb_state = S2R_USB_UP;
    while (1) {
        iodev_handle_events(IODEV_USB0);
        s2r_usb_ring_drain();
        s2r_wakerec_pump();
        s2r_coproc_pump();
        s2r_usb_yield(S2R_USB_POLL_US * frq / 1000000);
    }
}

static void s2r_usb_wake(void)
{
    /* Nothing to do if the link never went down. */
    if (!sptm_virt_s2r_usb_retake || !s2r_usb_stack || !s2r_keep_released)
        return;
    u32 ms = sptm_virt_s2r_usb_delay_ms ? sptm_virt_s2r_usb_delay_ms : S2R_USB_START_MS;
    s2r_usb_next = mrs(CNTPCT_EL0) + (u64)ms * mrs(CNTFRQ_EL0) / 1000;
    sysop("dmb ish");
    s2r_usb_state = S2R_USB_PENDING;
}

/* Re-take value n > 1: also drop the link n seconds in with no sleep and take it again, which tests
 * all of this under a running macOS without a sleep cycle. */
static void s2r_usb_selftest(void)
{
    u32 want = S2R_USB_SELFTEST;

    if (!__atomic_compare_exchange_n(&s2r_usb_state, &want, S2R_USB_OFF, false, __ATOMIC_SEQ_CST,
                                     __ATOMIC_SEQ_CST))
        return;
    printf("USB-RETAKE: self-test: dropping the link on cpu%d\n", smp_id());
    s2r_console_to_aon();
    udelay(2000); /* other cores' in-flight writes to it */
    s2r_release_kept();
    s2r_usb_wake();
}

static void s2r_usb_tick(bool byte)
{
    u32 state = s2r_usb_state;

    if (state == S2R_USB_OFF || (byte && state != S2R_USB_UP))
        return;
    u64 now = mrs(CNTPCT_EL0);
    if (!byte && now < s2r_usb_next)
        return;
    if (state == S2R_USB_SELFTEST) {
        s2r_usb_selftest();
        return;
    }
    if (__atomic_exchange_n(&s2r_usb_busy, 1, __ATOMIC_ACQUIRE))
        return;
    state = s2r_usb_state;
    if (state == S2R_USB_PENDING) {
        /* A new coroutine; an earlier one is abandoned if a sleep overtook it. */
        u64 *frame = (u64 *)(s2r_usb_stack + S2R_USB_STACK_BYTES - 96);
        memset(frame, 0, 96);
        frame[11] = (u64)s2r_usb_main; /* x30 in s2r_usb_switch's frame */
        s2r_usb_co_sp = (u64)frame;
        s2r_usb_t0 = now;
        s2r_usb_slices = 0;
        s2r_usb_slice_max = 0;
        printf("USB-RETAKE: starting on cpu%d, lane %d\n", smp_id(), usb_cdc_atc_lane());
        s2r_usb_state = state = S2R_USB_BRINGUP;
    }
    if (state != S2R_USB_OFF) {
        s2r_usb_switch(&s2r_usb_main_sp, s2r_usb_co_sp);
        if (state == S2R_USB_BRINGUP) {
            u64 dt = mrs(CNTPCT_EL0) - now;
            s2r_usb_slices++;
            s2r_usb_slice_max = max(s2r_usb_slice_max, dt);
        }
    }
    __atomic_store_n(&s2r_usb_busy, 0, __ATOMIC_RELEASE);
}

/* The re-take in reverse, first thing on the way into the next sleep. */
static void s2r_usb_quiesce(void)
{
    if (!sptm_virt_s2r_usb_retake)
        return;
    s2r_usb_state = S2R_USB_OFF;
    sysop("dsb ish");
    while (__atomic_load_n(&s2r_usb_busy, __ATOMIC_ACQUIRE))
        ;
    s2r_usb_yieldable = false;
    pmgr_keep_configured = false;
    s2r_usb_capture = s2r_usb_ring;
}

static void s2r_usb_setup(void)
{
    if (!sptm_virt_s2r_usb_retake)
        return;
    if (!s2r_go || !s2r_keep_always || !s2r_drop_usb) {
        printf("USB-RETAKE: needs go mode, the proxy on CDC and the USB drop; off\n");
        sptm_virt_s2r_usb_retake = 0;
        return;
    }
    s2r_usb_stack = memalign(0x4000, S2R_USB_STACK_BYTES);
    s2r_usb_ring = memalign(0x4000, S2R_USB_RING_BYTES);
    if (!s2r_usb_stack || !s2r_usb_ring) {
        printf("USB-RETAKE: no memory; off\n");
        sptm_virt_s2r_usb_retake = 0;
        return;
    }
    if (sptm_virt_s2r_usb_retake > 1) {
        s2r_usb_next = mrs(CNTPCT_EL0) + (u64)sptm_virt_s2r_usb_retake * mrs(CNTFRQ_EL0);
        s2r_usb_state = S2R_USB_SELFTEST;
    }
    printf("USB-RETAKE: armed%s\n", sptm_virt_s2r_usb_retake > 1 ? ", with the self-test" : "");
}

/* hv_vuart: every trapped console access may resume the re-take. */
void sptm_virt_s2r_uart_tick(void)
{
    s2r_usb_tick(false);
}

/* A trapped byte of XNU's console. True if the ring holds it, for the CDC pipe. */
bool sptm_virt_s2r_uart_byte(u8 b)
{
    if (!s2r_usb_capture)
        return false;
    u64 i = __atomic_fetch_add(&s2r_usb_ring_wp, 1, __ATOMIC_RELAXED);
    s2r_usb_ring[i % S2R_USB_RING_BYTES] = b;
    s2r_usb_tick(true);
    return true;
}
