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
 * The SoC's blocks (PMGR, PMS, the AON wake status, the AMCC, the debug link) come from the ADT
 * (sptm_virt_s2r_soc.c); a SoC whose ADT lacks one gets no sleep path. On a PMGR whose sleep is the
 * S2R timer's start alone (no per-core self-disable block), the trace records the PMGR's PS groups
 * and the cores' windows, the commit is that start, and none of the per-core or domain rules run.
 */

#include "sptm_virt.h"
#include "adt.h"
#include "aic.h"
#include "arm_cpu_regs.h"
#include "dockchannel_uart.h"
#include "exception.h"
#include "hv.h"
#include "iodev.h"
#include "malloc.h"
#include "memory.h"
#include "nv2.h"
#include "pmgr.h"
#include "s2r_dcp_pkt.h"
#include "smp.h"
#include "spmi.h"
#include "sptm_virt_priv.h"
#include "sptm_virt_s2r_soc.h"
#include "string.h"
#include "types.h"
#include "uartproxy.h"
#include "tps6598x.h"
#include "usb.h"
#include "wdt.h"
#include "usb_cdc_atc.h"
#include "utils.h"
#include "xnuboot.h"

#define S2R_RING_BYTES     (8UL << 20)
#define S2R_RING_BYTES_BIG (64UL << 20) /* a whole sleep entry; 8 MiB if the heap is short */

/* The sleep rules start a while after setup: XNU's PMGR driver writes the same registers while
 * it boots, and a sleep comes minutes after the desktop. After a wake with the link re-taken they
 * come back quickly. */
#define S2R_ARM_AFTER_S   (sptm_virt_s2r_arm_s ? sptm_virt_s2r_arm_s : 45)
#define S2R_REARM_AFTER_S 3

/* The S2R timer start in pmgr-aon-pst: the last step of XNU's quiesceHW. */
#define S2R_TIMER_OFF 0x4000

/* The always-on NUB's AP wake-source status words. */
#define S2R_NUB_WAKE_WORDS 8

static struct s2r_soc s2r_soc_adt;
static const struct s2r_soc *s2r_soc;
/* Phandles of the USB PHYs whose ports the host hid from the guest for aura's link. */
u32 sptm_virt_link_phys[2];

/* Link domains XNU requires off at quiesce (ADT device flag 0x2). They stay on because the link's
 * hidden devices are never powered down, so once the sleep has begun reads report them off. */
#define S2R_PMGR_OFF_AT_SLEEP 0x2
static u64 s2r_report_off_regs[4];
static int s2r_n_report_off;

/*
 * The AMCC error-enable writes macOS's error handler makes at quiesce, for when that handler is
 * hidden from XNU (as it is from Linux): an enabled, pending memory-controller error at the commit
 * wakes the SoC within 20 ms. Word 0 bit 25 is DSID_AGE_OUT, word 1 bit 8 UNEXP_RT_HIT_DIR.
 */
#define S2R_AMCC_ERR_EN(plane, word) (s2r_soc->amcc + ((u64)(plane) << 18) + (word) * 8)
#define S2R_QUIESCE_DSID_AGE_OUT     BIT(3)
#define S2R_QUIESCE_RT_HIT_DIR       BIT(4)

struct s2r_rec {
    u64 t;    /* CNTPCT_EL0 */
    u64 meta; /* pa [47:0] | width [49:48] | write [50] | cpu [54:51] | marker [55] */
    u64 val;  /* value written or read, or the marker's argument */
    u64 pc;   /* guest PC, or the marker's tag (8 chars) */
};

/* cpu bits 5:4 go in [57:56], for up to 64 cores. */
#define S2R_META(pa, width, write, cpu, marker)                                                    \
    (((pa) & GENMASK(47, 0)) | ((u64)((width) & 3) << 48) | ((u64) !!(write) << 50) |              \
     ((u64)((cpu) & 15) << 51) | ((u64) !!(marker) << 55) | ((u64)(((cpu) >> 4) & 3) << 56))
#define S2R_MARKER BIT(55)
#define S2R_WRITE  BIT(50)
#define S2R_J613_SNAPSHOT BIT(13) /* AURA_S2R_CRUMBS */
#define S2R_J613_TRAFFIC  BIT(14)

/* For the host: read over the proxy after a stop, or after a reset that kept DRAM. */
struct s2r_rec *sptm_virt_s2r_ring;
u64 sptm_virt_s2r_nrec;          /* capacity in records */
volatile u64 sptm_virt_s2r_head; /* records ever appended */
u32 sptm_virt_s2r_pages;         /* pages hooked */

/* Set by the host before entry (run_guest_sptm.py). */
u32 sptm_virt_s2r_quiesce;    /* S2R_QUIESCE_* */
u32 sptm_virt_s2r_wake_s;     /* GO: PMU RTC alarm this many s after the commit */
u32 sptm_virt_s2r_go_count;   /* GO, timer-only sleep: stop at the commit after this many wakes */
u32 sptm_virt_s2r_crumbs;    /* go-mode experiments on J813, a bit mask: see s2r_crumb() */
u32 sptm_virt_s2r_late_stop; /* J613 stop: shadow the commit, stop at the final deep WFI */
u32 sptm_virt_s2r_arm_s;     /* host: seconds from setup to the sleep rules (0: 45) */
u32 sptm_virt_s2r_win[2];
/* The wake core + 1 when it is to arm the stage 2 at its SPTM entry (option bit 256), else 0. */
u32 sptm_virt_s2r_early_s2;
/* host: extra registers read at the wake trap with option bit 512 (AURA_S2R_PROBE) */
u64 sptm_virt_s2r_probe[4];
u64 sptm_virt_s2r_prewrite[2];
u64 sptm_virt_s2r_probe_at;    /* host: records before / after the first commit in the after-wake dump */
u32 sptm_virt_s2r_usb_retake; /* 1: re-take the link after a wake; n > 1: also drop it n s in */
u32 sptm_virt_s2r_usb_delay_ms; /* delay from XNU's return to the re-take, 0 = default */
u32 sptm_virt_s2r_cp_skip_ans;  /* T6050 COPROC: leave ANS's own pages unhooked */
u32 sptm_virt_s2r_cp_ansx;      /* T6050 COPROC: S2R_CP_ANSX_* pages around ANS too */
/* S2R_CP_ANSX_QUIET: XNU's NVMe registers (sart-ans reg[2]); per-I/O writes are only counted */
static u64 s2r_nvme_quiet_page, s2r_nvme_quiet_n;

static struct {
    u64 page;
    const char *what;
    bool seen;
#ifdef AURACTL
    bool ctl; /* armed through auractl; page 0 marks a slot auractl released */
#endif
} s2r_pages[S2R_TRACE_MAX_PAGES];

static bool s2r_go, s2r_fulltrace, s2r_coproc, s2r_drop_usb;
static u64 s2r_arm_at;
/* Cores with their last WFI trapped (stop mode), parked there, and the core that committed. */
static volatile u32 s2r_twi_mask, s2r_parked_mask;
/* J613 stop-mode insurance while its clean-room sleep-block map is being qualified. */
static bool s2r_j613_soc_misc_marked, s2r_j613_aon_pst_marked;
/* Go mode: the ring position where the first core opened the sleep window (0: none yet). */
static volatile u64 s2r_sleep_head;
static volatile u64 s2r_sleep_t;
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
u32 sptm_virt_s2r_live;
/* Stop mode, more blocks in the full entry trace (AURA_S2R_EXTRA): 1 the MCC's blocks (the AMCC
 * planes without their first pages, which aura hooks itself), 2 aon-ptd, 4 the other
 * coprocessors' mailboxes (not the SEP's). */
u32 sptm_virt_s2r_extra;
static volatile bool s2r_keep_released;

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
/* The ring position of this boot's first commit, for the after-wake window (AURA_S2R_WIN). */
static u64 s2r_first_commit_head;

static void s2r_usb_tick(bool byte);
static iodev_id_t s2r_link_iodev = IODEV_USB0; /* the port the proxy was on before the sleep */
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

#ifdef AURACTL
volatile bool sptm_virt_s2r_ctl_paused; /* auractl stop: hooks stay, nothing is appended */
#endif

static struct s2r_rec *s2r_slot(void)
{
    if (!sptm_virt_s2r_ring)
        return NULL;
#ifdef AURACTL
    if (sptm_virt_s2r_ctl_paused)
        return NULL;
#endif
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

static struct s2r_rec *s2r_record_marker(const char *tag, u64 arg)
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
    return r;
}

static void s2r_marker(const char *tag, u64 arg)
{
    s2r_record_marker(tag, arg);
    if (!sptm_virt_s2r_quiet)
        printf("SPTM-virt[s2r]: marker %.8s 0x%lx (cpu%d, rec %lu)\n", tag, arg, smp_id(),
               sptm_virt_s2r_head);
}

/* These measurements must survive a powered-off core, including its dirty cache lines. */
static void s2r_retained_marker(const char *tag, u64 arg)
{
    struct s2r_rec *r = s2r_record_marker(tag, arg);
    if (r)
        s2r_clean(r, sizeof(*r));
}

static void s2r_print_rec(u64 i)
{
    struct s2r_rec *r = &sptm_virt_s2r_ring[i % sptm_virt_s2r_nrec];
    unsigned cpu = (r->meta >> 51) & 15;

    if ((r->meta & S2R_MARKER) && (r->meta & GENMASK(47, 0)))
        printf("S2R %lu t=%lx c%u MARK %.8s 0x%lx 0x%lx\n", i, r->t, cpu,
               (const char *)&r->pc, r->meta & GENMASK(47, 0), r->val);
    else if (r->meta & S2R_MARKER)
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

/*
 * The ring's tail for a console that may die before it has printed it all: the most wanted first,
 * newest first within each class, one short line each. Classes: A writes and markers outside the
 * mailboxes and SPMI buses, with the pc; B SPMI writes; C mailbox writes; D the reads outside the
 * mailboxes. First all four classes of the last @near records (the sleep entry itself), then A,
 * B and C of the @far records before them. Line: class, record, counter, cpu or width, address,
 * value (and pc).
 */
static u64 s2r_dump_class(u64 from, u64 to, int pass)
{
    u64 n = 0;
    for (u64 i = to; i-- > from;) {
        struct s2r_rec *r = &sptm_virt_s2r_ring[i % sptm_virt_s2r_nrec];
        unsigned cpu = (r->meta >> 51) & 15;
        u64 pa = r->meta & GENMASK(47, 0);
        if (r->meta & S2R_MARKER) {
            if (pass == 0) {
                printf("S2A %lu %lx c%u MARK %.8s %lx\n", i, r->t, cpu, (const char *)&r->pc,
                       r->val);
                n++;
            }
            continue;
        }
        bool first, wr = r->meta & S2R_WRITE;
        const char *what = s2r_page_name(pa & ~0x3fffUL, &first);
        bool mbox = s2r_is_mbox(what), spmi = !strncmp(what, "spmi", 4);
        int cls = wr ? (mbox ? 2 : spmi ? 1 : 0) : (mbox ? -1 : 3);
        if (cls != pass)
            continue;
        unsigned w = 8u << ((r->meta >> 48) & 3);
        if (pass == 0)
            printf("S2A %lu %lx c%u W%u %lx %lx %lx\n", i, r->t, cpu, w, pa, r->val, r->pc);
        else
            printf("S2%c %lu %lx %c%u %lx %lx\n", "ABCD"[pass], i, r->t, wr ? 'W' : 'R', w, pa,
                   r->val);
        n++;
    }
    return n;
}

void sptm_virt_s2r_dump_prio(u64 near, u64 far)
{
    if (!sptm_virt_s2r_ring)
        return;
    u64 head = sptm_virt_s2r_head;
    u64 have = min(head, sptm_virt_s2r_nrec);
    near = min(near, have);
    far = min(far, have - near);
    printf("SPTM-virt[s2r]: prio dump: head %lu, near %lu, far %lu, CNTFRQ %lu\n", head, near, far,
           mrs(CNTFRQ_EL0));
    for (int pass = 0; pass < 4; pass++)
        printf("SPTM-virt[s2r]: prio dump: near class %c complete, %lu lines\n", "ABCD"[pass],
               s2r_dump_class(head - near, head, pass));
    for (int pass = 0; pass < 3; pass++)
        printf("SPTM-virt[s2r]: prio dump: far class %c complete, %lu lines\n", "ABCD"[pass],
               s2r_dump_class(head - near - far, head - near, pass));
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

static bool s2r_report_off(u64 addr)
{
    for (int i = 0; i < s2r_n_report_off; i++)
        if (s2r_report_off_regs[i] == (addr & ~3UL))
            return true;
    return false;
}

static bool s2r_armed(void)
{
    return s2r_arm_at && mrs(CNTPCT_EL0) >= s2r_arm_at;
}

/* Bit 11 of AURA_S2R_CRUMBS is a one-run stall witness. Keep the already-selected AON console
 * usable until the commit, then let trapped WFE/WFI instructions identify every CPU which is
 * still waiting. The ordinary GO path keeps the console silent and unchanged. */
void sptm_virt_s2r_diag_wfx(u64 *regs, u64 esr)
{
    static volatile u32 reported;
    static volatile bool ring_dumped;

    if (!(sptm_virt_s2r_crumbs & 2048) || !s2r_sleep_t)
        return;
    u64 now = mrs(CNTPCT_EL0), f = mrs(CNTFRQ_EL0);
    if (now - s2r_sleep_t < 2 * f)
        return;
    int cpu = smp_id();
    if (!(__atomic_fetch_or(&reported, BIT(cpu), __ATOMIC_SEQ_CST) & BIT(cpu))) {
        u64 pc = mrs(ELR_EL2), fp = regs[29];
        printf("SPTM-virt[s2r-stall c%d]: %s pc=0x%lx lr=0x%lx sp=0x%lx fp=0x%lx x0=0x%lx "
               "esr=0x%lx\n",
               cpu, (esr & 1) ? "WFE" : "WFI", pc, regs[30], mrs(SP_EL1), fp, regs[0], esr);
        for (int i = 0; i < 16 && fp; i++) {
            u64 nfp, lr;
            if (!sptm_virt_el1_va_read64(fp, &nfp) ||
                !sptm_virt_el1_va_read64(fp + 8, &lr))
                break;
            printf("SPTM-virt[s2r-stall c%d]:   frame %d lr 0x%lx\n", cpu, i, lr);
            if (nfp <= fp)
                break;
            fp = nfp;
        }
    }
    if (!__atomic_exchange_n(&ring_dumped, true, __ATOMIC_SEQ_CST)) {
        printf("SPTM-virt[s2r-stall]: last trapped PMGR/ASC accesses\n");
        sptm_virt_s2r_dump_prio(2048, 8192);
    }
    iodev_console_flush();
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

    if (s2r_keep_released)
        return; /* done early (sptm_virt_s2r_early_release) */

    if (s2r_drop_usb) {
        s2r_marker("USBDROP", 0);
        /*
         * Option bit 5 (J813): only the link's own port. usb_iodev_shutdown() stops every gadget
         * stage 1 registered, and on J813 that includes USB0, the port macOS itself drives: its
         * controller and DART were being stopped under XNU at the commit.
         */
        struct iodev *d;
        if ((sptm_virt_s2r_crumbs & 32) && s2r_link_iodev >= IODEV_USB0 &&
            (d = iodev_unregister_device(s2r_link_iodev)))
            usb_dwc3_shutdown(d->opaque);
        else
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
    for (int i = 0; i < s2r_soc->n_link; i++)
        failed += pmgr_adt_power_disable(s2r_soc->link_paths[i]) < 0;
    if (!sptm_virt_s2r_quiet)
        printf("SPTM-virt[s2r]: released %d debug-link domains; powered the ATC0 link down%s\n", n,
               failed ? " (some domains refused)" : "");
}

static void s2r_quiesce(void)
{
    if (!s2r_soc->amcc)
        return;
    if (sptm_virt_s2r_quiesce & S2R_QUIESCE_DSID_AGE_OUT)
        for (u32 p = 0; p < s2r_soc->amcc_planes; p++)
            clear32(S2R_AMCC_ERR_EN(p, 0), BIT(25));
    if (sptm_virt_s2r_quiesce & S2R_QUIESCE_RT_HIT_DIR)
        for (u32 p = 0; p < s2r_soc->amcc_planes; p++)
            clear32(S2R_AMCC_ERR_EN(p, 1), BIT(8));
    sysop("dsb sy");
}

/*
 * Go mode, where the commit ends in a reset (J813: macOS's 5 s WD2 fires, the SoC never powers
 * down): steps noted in the main PMU's scratch, which a reset keeps, for the host to read through
 * the proxy afterwards. Bytes 0x830d..0x830f of SID 0xe on nub-spmi0, after the five Linux's sleep
 * driver uses at 0x8308: the step, then the mask of cores that reached their deep-sleep WFI.
 * Host: AURA_S2R_CRUMBS=1 notes up to the commit; 2 also after it (SPMI after the commit is a
 * risk of its own) and at the committing core's last WFI. With crumbs on, the cores' last WFIs
 * trap (TWI) and aura executes them, so that it can count them.
 */
static spmi_dev_t *s2r_crumb_dev;
static volatile u32 s2r_crumb_wfi;

static void s2r_crumb(u8 step)
{
    if (!s2r_crumb_dev)
        return;
    u32 m = s2r_crumb_wfi;
    u8 b[3] = {step, m, m >> 8};
    spmi_ext_write_long(s2r_crumb_dev, 0xe, 0x830d, b, sizeof(b));
}

/* More notes: 0x8310 the wake/re-take step, 0x8311 the number of commits this boot. */
static u8 s2r_crumb_commits;

static void s2r_crumb_at(u8 off, u8 v)
{
    if (s2r_crumb_dev)
        spmi_ext_write_long(s2r_crumb_dev, 0xe, 0x8308 + off, &v, 1);
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
        if (s2r_timer_cpu < 0) {
            s2r_timer_cpu = cpu;
            /* read MMU-off by sptm_virt_s2r_restarted_in_sleep */
            s2r_clean(&s2r_timer_cpu, sizeof(s2r_timer_cpu));
        }
        s2r_marker("TWI-ARM", cpu);
        return;
    }
    if (first) {
        s2r_sleep_head = sptm_virt_s2r_head;
        s2r_sleep_t = mrs(CNTPCT_EL0);
        s2r_clean(&s2r_sleep_head, sizeof(s2r_sleep_head));
        s2r_clean(&s2r_sleep_t, sizeof(s2r_sleep_t));
        printf("SPTM-virt[s2r]: cpu%d opens the sleep window: aura silent until the wake\n", cpu);
        s2r_console_to_aon();
        iodev_console_flush();
        if (sptm_virt_s2r_crumbs & 2048) {
            sptm_virt_s2r_quiet = true;
            s2r_clean(&sptm_virt_s2r_quiet, sizeof(sptm_virt_s2r_quiet));
        } else {
            s2r_console_off();
        }
    }
    if (sptm_virt_s2r_crumbs & 1) /* bit 0: aura takes the cores' deep-sleep WFI (TWI) */
        nv2_hcr_update(HCR_TWI, 0);
    msr(CNTP_CTL_EL0, 0); /* aura's EL2 physical timer (E2H: CNTHP) */
    msr(CNTV_CTL_EL0, 0); /* aura's EL2 virtual timer (CNTHV) */
    sysop("isb");
    s2r_marker("SLEEPS", cpu);
}

/* ---- Timed wake: the PMU's RTC alarm ---------------------------------------------------------
 *
 * The PMU and its RTC registers come from the ADT: the first child of an SPMI bus with info-rtc*
 * properties (control with its enable, event and monitor bits, the counter in 1/65536 s, the
 * compare in counter seconds, the IRQ mask). At the GO commit aura arms the alarm wake_s seconds
 * out. XNU's own later writes to it go through the bus's command register, where the hook follows
 * each command and its data words and keeps them on aura's time.
 */
#define S2R_SPMI_EXT_WRITEL 0x30

static struct {
    spmi_dev_t *dev;
    u64 cmd; /* PA of the controller's command register */
    u32 slave;
    u32 ctrl, counter, compare, mask, arm_bit, irq_bit, event_bit, monitor_bit;
    u32 wake_at;    /* PMU counter seconds; 0 until armed */
    u32 data_reg;   /* register the next data word of XNU's write covers */
    int data_words; /* data words still to come for that write */
    bool data_pmu;  /* the write is to the PMU */
} s2r_rtc;
/* J613 RTC-only GO must stop before the commit if discovery or arming fails. */
static bool s2r_rtc_required;

static u32 s2r_rtc_patch(u32 reg, u32 v)
{
    for (u32 b = 0; b < 4; b++) {
        u32 r = reg + b, sh = 8 * b;
        if (r >= s2r_rtc.compare && r < s2r_rtc.compare + 4)
            v = (v & ~(0xffU << sh)) |
                (((s2r_rtc.wake_at >> (8 * (r - s2r_rtc.compare))) & 0xff) << sh);
        else if (r == s2r_rtc.mask)
            v &= ~(s2r_rtc.irq_bit << sh);
        else if (r == s2r_rtc.ctrl)
            v |= (s2r_rtc.arm_bit | s2r_rtc.monitor_bit) << sh;
    }
    return v;
}

static bool s2r_spmi_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    if (write && width == 2 && addr == s2r_rtc.cmd) {
        u32 v = *val;
        if (s2r_rtc.data_words > 0) {
            s2r_rtc.data_words--;
            if (s2r_rtc.data_pmu && s2r_rtc.wake_at)
                v = s2r_rtc_patch(s2r_rtc.data_reg, v);
            s2r_rtc.data_reg += 4;
        } else {
            u32 opc = v & 0xff, len = 0, reg = v >> 16;
            if ((opc & 0xf8) == S2R_SPMI_EXT_WRITEL) {
                len = (opc & 7) + 1;
            } else if (opc < 0x10) { /* extended write, 8-bit register */
                len = (opc & 0xf) + 1;
                reg &= 0xff;
            }
            s2r_rtc.data_words = (len + 3) / 4;
            s2r_rtc.data_reg = reg;
            s2r_rtc.data_pmu = ((v >> 8) & 0x7f) == s2r_rtc.slave;
        }
        *val = v;
    }
    sptm_virt_mmio_rw(addr, val, write, width);
    sptm_virt_s2r_note(ctx, addr, *val, write, width);
    return true;
}

static u32 s2r_adt_u32(int node, const char *name)
{
    u32 v = 0;
    return ADT_GETPROP(adt, node, name, &v) == sizeof(v) ? v : 0;
}

static bool s2r_rtc_from(const char *bus, int pmu)
{
    u32 len;
    const u32 *reg = adt_getprop(adt, pmu, "reg", &len);
    if (!reg || len < 4 || !(s2r_rtc.ctrl = s2r_adt_u32(pmu, "info-rtc_alarm_ctrl")) ||
        !(s2r_rtc.counter = s2r_adt_u32(pmu, "info-rtc")) ||
        !(s2r_rtc.compare = s2r_adt_u32(pmu, "info-rtc_alarm_offset")) ||
        !(s2r_rtc.mask = s2r_adt_u32(pmu, "info-rtc_irq_mask_offset")) ||
        !(s2r_rtc.arm_bit = s2r_adt_u32(pmu, "info-rtc_alarm_ctrl_en_mask")) ||
        !(s2r_rtc.irq_bit = s2r_adt_u32(pmu, "info-rtc_alarm_mask")))
        return false;
    char path[48];
    snprintf(path, sizeof(path), "/arm-io/%s", bus);
    s2r_rtc.slave = reg[0];
    s2r_rtc.event_bit = s2r_adt_u32(pmu, "info-rtc_alarm_event_mask");
    s2r_rtc.monitor_bit = s2r_adt_u32(pmu, "info-rtc_alarm_monitor_mask");
    s2r_rtc.dev = spmi_init(path);
    if (!s2r_rtc.dev)
        return false;
    s2r_rtc.cmd = spmi_cmd_reg(s2r_rtc.dev);
    if (hv_map_hook(s2r_rtc.cmd & ~0x3fffUL, s2r_spmi_hook, 0x4000) < 0)
        return false;
    printf("SPTM-virt[s2r]: timed wake %u s after the commit: %s/%s (SPMI 0x%x, cmd 0x%lx), RTC "
           "ctrl 0x%x compare 0x%x mask 0x%x\n",
           sptm_virt_s2r_wake_s, bus, adt_get_name(adt, pmu), s2r_rtc.slave, s2r_rtc.cmd,
           s2r_rtc.ctrl, s2r_rtc.compare, s2r_rtc.mask);
    return true;
}

static bool s2r_cp_compat(int node, const char *prefix);
static void s2r_regs_snapshot(const char *tag);
static void s2r_probe_one(u64 e);

/* A coprocessor whose power domain is already off aborts an ASC register read.  That is an
 * expected commit-time state, not a hypervisor failure, so make the read observational. */
static int s2r_snapshot_read32(u64 addr, u32 *value)
{
    enum exc_guard_t saved = exc_guard;
    int before = exc_count;
    u64 daif = mrs(DAIF);

    exc_guard = GUARD_MARK | GUARD_SILENT;
    *value = read32(addr);
    /* Let a pending asynchronous SError arrive while the guard is still armed. */
    sysop("dsb sy");
    sysop("msr daifclr, 4");
    sysop("isb");
    msr(DAIF, daif);
    sysop("isb");
    exc_guard = saved;
    return exc_count == before ? 0 : -1;
}

static bool s2r_j613_snapshot_enabled(void)
{
    return chip_id == T8122 && (sptm_virt_s2r_crumbs & S2R_J613_SNAPSHOT);
}

static bool s2r_j613_traffic_enabled(void)
{
    return chip_id == T8122 && (sptm_virt_s2r_crumbs & S2R_J613_TRAFFIC);
}

static void s2r_j613_snapshot_word(const char *tag, u64 addr)
{
    u32 val;
    bool fault = s2r_snapshot_read32(addr, &val) < 0;
    struct s2r_rec *r = s2r_record_marker(tag, fault ? ~0UL : val);
    if (r) {
        r->meta |= addr & GENMASK(47, 0);
        s2r_clean(r, sizeof(*r));
    }
}

/* Read physical state after releasing the debug domains and again at reset, before SPTM. */
static void s2r_j613_snapshot(const char *tag, bool wake)
{
    static const struct {
        u64 first, last;
        unsigned stride;
    } pmgr[] = {
        {0x2d0700100, 0x2d07004c0, 8},
        {0x2d0704000, 0x2d0704060, 8},
        {0x2d0708000, 0x2d0708020, 8},
        {0x2d0710000, 0x2d0710000, 8},
    }, common[] = {
        {0x2e4280000, 0x2e4280100, 8}, /* mini PMGR power words */
        {0x2e42b0000, 0x2e42b001c, 4}, /* watchdog */
        {0x2e4004000, 0x2e4004044, 4}, /* wake-source block */
    };
    static const u64 regs[] = {
        0x2d0700c00, 0x2d0724080, 0x2d0724088, 0x2d0724cac, 0x2d0724cd0, 0x2d0724cf0,
        0x2d0780000, 0x2d0780004, 0x2d0768000, 0x2d0768024, 0x2d0734000, 0x2e42d4000,
        0x2e42b8008, 0x2e4714040, 0x2e4908040,
    };
    static const char *const asc[] = {
        "/arm-io/pmp", "/arm-io/ans", "/arm-io/sio", "/arm-io/dcp", "/arm-io/gfx-asc", "/arm-io/smc",
    };

    if (wake)
        for (unsigned i = 0; i < ARRAY_SIZE(pmgr); i++)
            for (u64 a = pmgr[i].first; a <= pmgr[i].last; a += pmgr[i].stride)
                s2r_j613_snapshot_word(tag, a);
    for (unsigned i = 0; i < ARRAY_SIZE(common); i++)
        for (u64 a = common[i].first; a <= common[i].last; a += common[i].stride)
            s2r_j613_snapshot_word(tag, a);
    for (unsigned i = 0; i < ARRAY_SIZE(regs); i++)
        s2r_j613_snapshot_word(tag, regs[i]);
    if (!wake)
        for (unsigned i = 0; i < ARRAY_SIZE(asc); i++) {
            int path[8];
            u64 base;
            if (adt_path_offset_trace(adt, asc[i], path) < 0 ||
                adt_get_reg(adt, path, "reg", 0, &base, NULL) < 0)
                continue;
            s2r_j613_snapshot_word(tag, base + 0x44);
            s2r_j613_snapshot_word(tag, base + 0x48);
        }
    s2r_clean(&sptm_virt_s2r_head, sizeof(sptm_virt_s2r_head));
}

/* Bit 10 of AURA_S2R_CRUMBS: a compact, read-only J613 commit-time snapshot. The access trace
 * cannot recover domains which were already steady before tracing began, and a successful sleep
 * may make the ring unavailable, so put the values on the always-on console and flush them before
 * forwarding or withholding the commit. */
static void s2r_j613_commit_snapshot(void)
{
    static u64 addr[384];
    static u32 value[384];
    /* Candidate 18 reads the same named Linux power-domain block successfully. Other ADT PMGR
     * banks are not part of that comparison and may already be inaccessible this late. */
    printf("J613-COMMIT-SNAPSHOT begin\n");
    iodev_console_flush();
    int n = pmgr_snapshot_range(addr, value, ARRAY_SIZE(addr), 0x2d0700000, 0x2d0714000);

    printf("J613-COMMIT-SNAPSHOT PMGR %d\n", n);
    for (int i = 0; i < n; i++)
        printf("J613-COMMIT-PMGR 0x%lx 0x%08x\n", addr[i], value[i]);

    static const struct {
        const char *name, *path;
    } cp[] = {
        {"SEP", "/arm-io/sep"},       {"PMP", "/arm-io/pmp"},
        {"GPU", "/arm-io/gfx-asc"}, {"ANS", "/arm-io/ans"},
        {"DCP", "/arm-io/dcp"},
    };
    for (unsigned i = 0; i < ARRAY_SIZE(cp); i++) {
        int path[8];
        u64 base = 0;
        int node = adt_path_offset_trace(adt, cp[i].path, path);
        if (node < 0 || adt_get_reg(adt, path, "reg", 0, &base, NULL) < 0) {
            printf("J613-COMMIT-ASC %s unavailable\n", cp[i].name);
            continue;
        }
        u32 ctl;
        if (s2r_snapshot_read32(base + 0x44, &ctl) < 0) {
            printf("J613-COMMIT-ASC %s 0x%lx powered-off-or-unavailable\n", cp[i].name,
                   base + 0x44);
            continue;
        }
        printf("J613-COMMIT-ASC %s 0x%lx 0x%08x RUN=%u\n", cp[i].name, base + 0x44, ctl,
               !!(ctl & 0x10));
    }
    iodev_console_flush();
}

static void s2r_rtc_setup(void)
{
    int bus = adt_path_offset(adt, "/arm-io");
    if (bus >= 0)
        ADT_FOREACH_CHILD(adt, bus)
        {
            const char *name = adt_get_name(adt, bus);
            int pmu = bus;
            if (strncmp(name, "nub-spmi", 8))
                continue;
            ADT_FOREACH_CHILD(adt, pmu)
            {
                if (s2r_rtc_from(name, pmu))
                    return;
            }
        }
    printf("SPTM-virt[s2r]: no PMU RTC on an SPMI bus; no timed wake\n");
    sptm_virt_s2r_wake_s = 0;
}

/* At the GO commit, on the committing core: XNU's PMU accesses are quiesced behind it. */
static bool s2r_rtc_arm(void)
{
    u8 c[6], b;
    if (!s2r_rtc.dev || spmi_ext_read_long(s2r_rtc.dev, s2r_rtc.slave, s2r_rtc.counter, c, 6) < 0)
        goto fail;
    u64 cnt = 0;
    for (int i = 0; i < 6; i++)
        cnt |= (u64)c[i] << (8 * i);
    u32 at = (cnt >> 16) + sptm_virt_s2r_wake_s;
    u8 cmp[4] = {at, at >> 8, at >> 16, at >> 24};
    if (spmi_ext_read_long(s2r_rtc.dev, s2r_rtc.slave, s2r_rtc.ctrl, &b, 1) < 0)
        goto fail;
    u8 ack = b | s2r_rtc.event_bit;
    if (spmi_ext_write_long(s2r_rtc.dev, s2r_rtc.slave, s2r_rtc.ctrl, &ack, 1) < 0 ||
        spmi_ext_write_long(s2r_rtc.dev, s2r_rtc.slave, s2r_rtc.compare, cmp, 4) < 0 ||
        spmi_ext_read_long(s2r_rtc.dev, s2r_rtc.slave, s2r_rtc.mask, &b, 1) < 0)
        goto fail;
    b &= ~s2r_rtc.irq_bit;
    if (spmi_ext_write_long(s2r_rtc.dev, s2r_rtc.slave, s2r_rtc.mask, &b, 1) < 0 ||
        spmi_ext_read_long(s2r_rtc.dev, s2r_rtc.slave, s2r_rtc.ctrl, &b, 1) < 0)
        goto fail;
    b = (b & ~s2r_rtc.event_bit) | s2r_rtc.arm_bit | s2r_rtc.monitor_bit;
    if (spmi_ext_write_long(s2r_rtc.dev, s2r_rtc.slave, s2r_rtc.ctrl, &b, 1) < 0)
        goto fail;
    s2r_rtc.wake_at = at;
    s2r_marker("RTCWAKE", at);
    printf("SPTM-virt[s2r]: PMU RTC alarm armed for counter second %u (now %lu, +%u s)\n", at,
           (unsigned long)(cnt >> 16), sptm_virt_s2r_wake_s);
    return true;
fail:
    printf("SPTM-virt[s2r]: PMU RTC alarm not armed (SPMI error); no timed wake\n");
    return false;
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
static int usb_iodev_reinit(int i);
static bool s2r_link_back; /* this stop brought the link up itself: nobody is at the proxy */

/*
 * A stop after a wake (AURA_S2R_GO_COUNT): the first sleep took the debug link's port down with
 * the SoC and nothing brought it back (no re-take on T8142). macOS is halted here on its last
 * core, so bring that one port up cold, as stage 1 does at boot, wait for the host to open it,
 * and only then print. Steps noted in PMU scratch (0x8310) for when it resets instead.
 */
static void s2r_stop_link_back(void)
{
    if (chip_id != T8142 || !s2r_keep_released || s2r_link_iodev < IODEV_USB0)
        return;
    s2r_crumb_at(8, 0x70);
    int ret = usb_iodev_reinit(s2r_link_iodev - IODEV_USB0);
    s2r_crumb_at(8, ret ? 0x7e : 0x71);
    if (ret)
        return;
    uartproxy_iodev = s2r_link_iodev;
    sptm_virt_s2r_quiet = false;
    u64 end = mrs(CNTPCT_EL0) + 40 * mrs(CNTFRQ_EL0);
    while (mrs(CNTPCT_EL0) < end) {
        iodev_handle_events(s2r_link_iodev);
        if (iodev_can_write(s2r_link_iodev))
            break;
        udelay(1000);
    }
    s2r_crumb_at(8, iodev_can_write(s2r_link_iodev) ? 0x72 : 0x7d);
    s2r_link_back = true;
    /* the host has enumerated the port; give its reader a moment to attach */
    end = mrs(CNTPCT_EL0) + 4 * mrs(CNTFRQ_EL0);
    while (mrs(CNTPCT_EL0) < end) {
        iodev_handle_events(s2r_link_iodev);
        udelay(1000);
    }
}

static void __attribute__((noreturn)) s2r_stop_before_timer(struct exc_info *ctx, u64 addr, u64 val)
{
    sptm_virt_j613_pmu_mark(0xaf); /* Linux-readable proof that the commit was withheld. */
    s2r_marker("S2RSTOP", addr);
    s2r_stop_link_back();
    printf("SPTM-virt[s2r]: cpu%d is starting the S2R timer ([0x%lx] = 0x%lx, pc 0x%lx): NOT "
           "performed (stop mode). TWI armed 0x%x, parked 0x%x, %lu records. Stopping in the "
           "proxy.\n",
           smp_id(), addr, val, ctx->elr, s2r_twi_mask, s2r_parked_mask, sptm_virt_s2r_head);
    if (s2r_nvme_quiet_page)
        printf("SPTM-virt[s2r]: %lu NVMe per-I/O writes not recorded\n", s2r_nvme_quiet_n);
    if (s2r_link_back && s2r_first_commit_head) {
        /*
         * After a wake what matters is around the FIRST commit, which a newest-first dump may not
         * reach once more is hooked: the end of that entry and everything for a while after the
         * wake, every record. The ring must not have wrapped past it.
         */
        u64 head = sptm_virt_s2r_head;
        u64 before = sptm_virt_s2r_win[0] ? sptm_virt_s2r_win[0] : 4000;
        u64 after = sptm_virt_s2r_win[1] ? sptm_virt_s2r_win[1] : 30000;
        u64 from = s2r_first_commit_head > before ? s2r_first_commit_head - before : 0;
        u64 to = min(head, s2r_first_commit_head + after);
        if (head - from > sptm_virt_s2r_nrec)
            from = head - sptm_virt_s2r_nrec;
        printf("SPTM-virt[s2r]: wake window: records %lu..%lu of %lu (first commit at %lu)\n", from,
               to, head, s2r_first_commit_head);
        for (u64 i = from; i < to; i++) {
            s2r_print_rec(i);
            if (!(i & 255))
                iodev_handle_events(s2r_link_iodev);
        }
    } else {
        sptm_virt_s2r_dump_prio(6000, 60000);
    }
    if (s2r_link_back) {
        /* Hands-free: the host has the dump; let the last of it drain, then reset into stage 1. */
        printf("SPTM-virt[s2r]: dump complete after a wake; resetting in 3 s\n");
        iodev_console_flush();
        u64 end = mrs(CNTPCT_EL0) + 3 * mrs(CNTFRQ_EL0);
        while (mrs(CNTPCT_EL0) < end) {
            iodev_handle_events(s2r_link_iodev);
            udelay(1000);
        }
        s2r_crumb_at(8, 0x73);
        wdt_reboot();
        while (1)
            sysop("wfe");
    }
    struct uartproxy_msg_start start = {
        .reason = START_HV,
        .code = 0x53325221, /* 'S2R!' */
        .info = ctx,
    };
    sptm_virt_park_others();
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
    if (s2r_go) {
        __atomic_or_fetch(&s2r_crumb_wfi, BIT(cpu), __ATOMIC_SEQ_CST);
        if (cpu == s2r_timer_cpu)
            s2r_crumb(0x40);
        /* J613 entry trace, opt-in via crumb bit 12. CPU and counter are in each record. */
        bool trace = chip_id == T8122 && (sptm_virt_s2r_crumbs & BIT(12));
        if (trace) {
            s2r_retained_marker("WFIDS", ds);
            s2r_retained_marker("WFIPT", mrs(CNTP_CTL_EL02));
            s2r_retained_marker("WFIVT", mrs(CNTV_CTL_EL02));
            s2r_retained_marker("WFIVFIQ", SAFE_MRS("s3_5_c15_c1_3"));
            s2r_retained_marker("WFIPMU", SAFE_MRS("s3_1_c15_c0_0"));
            s2r_retained_marker("WFIIPI", SAFE_MRS("s3_5_c15_c1_1"));
            s2r_retained_marker("WFIISR", mrs(ISR_EL1));
            s2r_retained_marker("WFISPSR", mrs(SPSR_EL2));
            s2r_retained_marker("WFIHCR", mrs(HCR_EL2));
            s2r_retained_marker("WFIHPT", mrs(CNTHP_CTL_EL2));
            s2r_retained_marker("WFIHVT", mrs(CNTHV_CTL_EL2));
            s2r_retained_marker("WFIENTER", elr);
            s2r_clean(&sptm_virt_s2r_head, sizeof(sptm_virt_s2r_head));
        }
        /* The core's power-down; a wake resets it. Bit 3 of the option: never hand a WFI that
         * came back (something pending) to the guest, wait again instead. */
        do {
            sysop("wfi");
            if (trace) {
                s2r_retained_marker("WFIRET", elr);
                s2r_clean(&sptm_virt_s2r_head, sizeof(sptm_virt_s2r_head));
                trace = false;
            }
        } while (sptm_virt_s2r_crumbs & 8);
        msr(ELR_EL2, elr + 4);
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
    sptm_virt_s2r_dump_prio(6000, 60000); /* whole classes, newest first: the proxy read-out fails with ten cores */
    struct uartproxy_msg_start start = {
        .reason = START_HV, .code = 0x53325221, /* 'S2R!' */
    };
    sptm_virt_park_others();
    uartproxy_run(&start);
    while (1)
        sysop("wfe");
}

static void s2r_wakerec_fallback(void);
static void s2r_wakerec_pump(void);
static void s2r_j613_export_tick(void);
static bool s2r_wakerec_printed;

/*
 * XNU wrote one of the PMGR CPU-start registers (+0x4: BIT(cpu); +0x8 + 4 * cluster: BIT(core),
 * which is what lets the core run). Watch the started core's crumbs for a while. If it has not
 * reached its SPTM entry with the MMU on by then, say where it stopped and give the host the
 * proxy: the boot core would only wait for it forever.
 */
static void s2r_cpustart_watch(struct exc_info *ctx, u64 addr, u64 val)
{
    u8 *c = (u8 *)sptm_smp_crumbs;
    u64 off = addr & 0x3fff, f = mrs(CNTFRQ_EL0), t0 = mrs(CNTPCT_EL0);
    int slot = off >= 8 ? (int)((off - 8) / 4) * 8 + __builtin_ctzl(val) : -1;
    u8 st = 0;

    if (slot < 0 || slot >= 16) {
        printf("SPTM-virt[smp]: CPUSTART [0x%lx] <- 0x%lx (pc 0x%lx)\n", addr, val, ctx->elr);
        return;
    }
    do {
        dc_civac_range(c + slot * 64, 64);
        st = c[slot * 64];
        if (st >= 7 || (st & 0x80))
            break;
    } while (mrs(CNTPCT_EL0) - t0 < f / 2);
    u64 *w = (u64 *)(c + slot * 64 + 8);
    printf("SPTM-virt[smp]: CPUSTART [0x%lx] <- 0x%lx (pc 0x%lx): slot %d stage 0x%x count %u "
           "after %lu us\n",
           addr, val, ctx->elr, slot, st, c[slot * 64 + 1], (mrs(CNTPCT_EL0) - t0) * 1000000 / f);
    if (st >= 7 && !(st & 0x80))
        return;
    if (st & 0x80)
        printf("SPTM-virt[smp]: slot %d faulted with its MMU off: ESR 0x%lx ELR 0x%lx FAR 0x%lx\n",
               slot, w[0], w[1], w[2]);
    printf("SPTM-virt[smp]: SMP-PARK: the started core did not reach SPTM; stopping in the "
           "proxy\n");
    iodev_console_flush();
    struct uartproxy_msg_start start = {
        .reason = START_HV,
        .code = 0x534d5021, /* 'SMP!' */
        .info = ctx,
    };
    sptm_virt_park_others();
    uartproxy_run(&start);
    while (1)
        sysop("wfe");
}

static bool s2r_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    /* With no debug-link USB port (a KIS/DockChannel proxy) the ring is armed for live in-guest
     * tracing with no s2r_soc and no sleep path, so take the plain record path below. */
    if (s2r_soc && s2r_soc->timer_only) {
        if (write && width == 2 && addr == s2r_soc->stop_timer && *val == 1 && s2r_armed()) {
            sptm_virt_s2r_note(ctx, addr, *val, true, width);
            if (!s2r_go || (sptm_virt_s2r_go_count && s2r_wake_count >= sptm_virt_s2r_go_count)) {
                sptm_virt_rg_disarm();
                s2r_stop_before_timer(ctx, addr, *val);
            }
            s2r_marker("S2RGO", addr);
            printf("SPTM-virt[s2r]: cpu%d starts the S2R timer (pc 0x%lx), %lu records; aura "
                   "silent until the wake\n",
                   smp_id(), ctx->elr, sptm_virt_s2r_head);
            if (sptm_virt_s2r_wake_s)
                s2r_rtc_arm();
            iodev_console_flush();
            s2r_commit();
            msr(CNTP_CTL_EL0, 0); /* aura's EL2 timers (E2H: CNTHP, CNTHV) on this core */
            msr(CNTV_CTL_EL0, 0);
            sysop("isb");
            write32(addr, *val);
            return true;
        }
        if (width == 4) {
            s2r_rw128(addr, val, write);
            sptm_virt_s2r_note(ctx, addr, val[0], write, 3);
            sptm_virt_s2r_note(ctx, addr + 8, val[1], write, 3);
        } else if (width >= 0 && width <= 3) {
            sptm_virt_mmio_rw(addr, val, write, width);
            /* XNU polls the cores' counters there all the time; their writes still go in. */
            bool first;
            if (write || strcmp(s2r_page_name(addr & ~0x3fffUL, &first), "cpu-impl"))
                sptm_virt_s2r_note(ctx, addr, *val, write, width);
        } else {
            return false;
        }
        return true;
    }
    int k = width == 2 && !s2r_keep_released && (s2r_keep_always || s2r_armed())
                ? s2r_keep_idx(addr)
                : -1;

    /* From the first core's self-disable, a domain XNU must find off at quiesce reads off. */
    if (!write && width == 2 && s2r_twi_mask && s2r_report_off(addr)) {
        *val = read32(addr) & ~0xffUL;
        sptm_virt_s2r_note(ctx, addr, *val, false, width);
        return true;
    }

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
    if (write && chip_id == T8122) {
        if (!strcmp(what, "pmgr-soc-misc") && !s2r_j613_soc_misc_marked) {
            s2r_j613_soc_misc_marked = true;
            sptm_virt_j613_pmu_mark(0xa1);
        } else if (!strcmp(what, "pmgr-aon-pst") && !s2r_j613_aon_pst_marked) {
            s2r_j613_aon_pst_marked = true;
            sptm_virt_j613_pmu_mark(0xa2);
        }
    }
    bool timer = write && width <= 3 && *val == 1 && !strcmp(what, "pmgr-aon-pst") &&
                 (addr & 0x7fff) == S2R_TIMER_OFF && s2r_armed();
    bool shadow_timer = timer && !s2r_go && chip_id == T8122 && sptm_virt_s2r_late_stop;
    if (timer && chip_id == T8122 && (sptm_virt_s2r_crumbs & 1024))
        s2r_j613_commit_snapshot();
    /* AURA_S2R_GO_COUNT here too (J813): after n go-mode wakes, the next commit stops like stop
     * mode, so the ring holds the sleep, the wake and the next entry for the proxy to read. */
    if (timer && (!s2r_go || (sptm_virt_s2r_go_count && s2r_wake_count >= sptm_virt_s2r_go_count))) {
        sptm_virt_rg_stop_wd2();
        sptm_virt_s2r_note(ctx, addr, *val, true, width);
        if (!shadow_timer)
            s2r_stop_before_timer(ctx, addr, *val);
        sptm_virt_j613_pmu_mark(0xaf);
        s2r_marker("S2RSHDW", addr);
        printf("SPTM-virt[s2r]: J613 sleep commit [0x%lx] <- 0x%lx shadowed; "
               "waiting for every core's final deep WFI\n",
               addr, *val);
    }
    if (timer && !shadow_timer) {
        s2r_crumb_at(9, ++s2r_crumb_commits);
        s2r_crumb(0x10);
        bool rtc_ok = !s2r_rtc_required;
        if (sptm_virt_s2r_wake_s)
            rtc_ok = s2r_rtc_arm();
        if (!rtc_ok) {
            sptm_virt_rg_disarm();
            s2r_stop_before_timer(ctx, addr, *val);
        }
        s2r_commit();
        if (s2r_j613_snapshot_enabled())
            s2r_j613_snapshot("J613PRE", false);
        s2r_crumb(0x20);
    }

    if (!s2r_j613_traffic_enabled() && s2r_wake_mmio && !s2r_is_mbox(what) &&
        s2r_take(&s2r_wake_mmio))
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
    if (!shadow_timer)
        sptm_virt_mmio_rw(addr, val, write, width);
    if (timer && !shadow_timer)
        s2r_crumb(0x30);
    if (!shadow_timer)
        sptm_virt_s2r_note(ctx, addr, *val, write, width);
    /* host: AURA_S2R_PROBE_AT, a guest write after a wake that, once performed, triggers the
     * AURA_S2R_PROBE reads (once per wake): where in macOS's own sequence a block becomes
     * readable. */
    if (write && sptm_virt_s2r_probe_at && addr == sptm_virt_s2r_probe_at && s2r_wake_t) {
        static u32 done_wake;
        if (done_wake != s2r_wake_count) {
            done_wake = s2r_wake_count;
            s2r_marker("PROBEAT", addr);
            for (unsigned i = 0; i < ARRAY_SIZE(sptm_virt_s2r_probe) && sptm_virt_s2r_probe[i]; i++)
                s2r_probe_one(sptm_virt_s2r_probe[i]);
        }
    }
    bool cpustart = !strcmp(what, "pmgr-soc-misc") && (addr & 0xfffff) >= 0x34004 &&
                    (addr & 0xfffff) < 0x34018;
    if (write && cpustart && *val && sptm_smp_crumbs)
        s2r_cpustart_watch(ctx, addr, *val);
    /* AURA_S2R_LIVE: the guest's writes to the hooked power blocks, as they happen, for a stall
     * that leaves nothing to read back. */
    if (write && sptm_virt_s2r_live) {
        static u32 n;
        if (n++ < 600)
            printf("SPTM-virt[pmgr wr] %s 0x%lx <- 0x%lx pc 0x%lx\n", what, addr, *val, ctx->elr);
        /* And every 10 s, how many writes each hooked block took (a display that swaps shows
         * up as steady DCP mailbox traffic). */
        static struct {
            const char *what;
            u32 n;
        } cnt[16];
        static u64 next;
        for (unsigned i = 0; i < ARRAY_SIZE(cnt); i++) {
            if (cnt[i].what == what || !cnt[i].what) {
                cnt[i].what = what;
                cnt[i].n++;
                break;
            }
        }
        u64 now = mrs(CNTPCT_EL0);
        if (now >= next) {
            next = now + 10 * mrs(CNTFRQ_EL0);
            char line[400];
            int len = 0;
            for (unsigned i = 0; i < ARRAY_SIZE(cnt) && cnt[i].what; i++) {
                len += snprintf(line + len, sizeof(line) - len, " %s=%u", cnt[i].what, cnt[i].n);
                cnt[i].n = 0;
                if (len >= (int)sizeof(line) - 1)
                    break;
            }
            printf("SPTM-virt[hook writes/10s]%s\n", line);
        }
    }
    if (s2r_j613_traffic_enabled()) {
        s2r_j613_export_tick();
    } else {
        s2r_wakerec_fallback();
        if (s2r_wakerec_printed)
            s2r_wakerec_pump();
    }

    /* Every access to the cluster power gates and cluster control: few, and they decide whether
     * the boot core can power off at the commit. */
    static u32 pg_logs;
    if ((!strcmp(what, "pmgr-pwrgate") || !strcmp(what, "pmgr-cluster-ctl")) && pg_logs++ < 96)
        printf("SPTM-virt[pg c%d]: %s 0x%lx %s 0x%lx pc 0x%lx\n", smp_id(), write ? "W" : "R", addr,
               write ? "<-" : "=", *val, ctx->elr);
    if (first && !sptm_virt_s2r_quiet)
        printf("SPTM-virt[s2r]: first access to %s page 0x%lx: %s 0x%lx <- 0x%lx pc 0x%lx\n", what,
               addr & ~0x3fffUL, write ? "wr" : "rd", addr, *val, ctx->elr);
    if (!s2r_armed())
        return true;
    if (timer) {
        s2r_timer_cpu = smp_id();
        s2r_clean(&s2r_timer_cpu, sizeof(s2r_timer_cpu));
        s2r_marker("S2RTIMER", addr);
        if (sptm_virt_s2r_crumbs & 512)
            s2r_regs_snapshot("CREG");
        s2r_commit_head = sptm_virt_s2r_head;
        if (!s2r_first_commit_head)
            s2r_first_commit_head = s2r_commit_head;
        s2r_core_sleeps();
    }
    /* Each core disables itself in the PMGR CPU block on its way to its last WFI; a cluster armed
     * off is the same moment for the boot core. */
    /* Not a write that starts another core: the start registers are in the same block. */
    if (write && ((!strcmp(what, "pmgr-soc-misc") && !cpustart) ||
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

/* Validated, aligned discovery apertures only. Unlike the legacy block mapper, this preserves
 * a partial last page using hv_map_hook's 4-byte L4 granularity. No access outside the ADT range
 * is routed to our hook, and mapping a range does not read its registers. */
static bool s2r_hook_capture(const struct s2r_trace_blk *b)
{
    for (u64 page = b->base; page < b->base + b->size; page += 0x4000) {
        if (sptm_virt_rg_owns(page))
            continue;
        u64 size = min(0x4000UL, b->base + b->size - page);
        if (sptm_virt_s2r_pages >= ARRAY_SIZE(s2r_pages) || hv_map_hook(page, s2r_hook, size) < 0) {
            printf("SPTM-virt[s2r]: cannot hook capture %s 0x%lx + 0x%lx\n", b->what, page, size);
            return false;
        }
        s2r_pages[sptm_virt_s2r_pages].page = page;
        s2r_pages[sptm_virt_s2r_pages].what = b->what;
        sptm_virt_s2r_pages++;
    }
    return true;
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

static void s2r_hook_blk(const struct s2r_blk *blk, const char *what, u64 off, u64 size)
{
    if (blk->base)
        s2r_hook_range(what, blk->base + off, size ? size : blk->size);
}

#ifdef AURACTL
#include "auractl_abi.h"
/*
 * auractl (sptm_virt_ctl.c), with the guest running: hook or release one 16 KiB page. Its block
 * was split at setup, so this rewrites one L3 leaf; the caller invalidates the TLBs and takes the
 * new tree as the stage-2 reference. Returns 1 if the page changed, 0 if it already was in that
 * state, or -errno: a page another aura hook owns, or one the trace setup hooked, is refused.
 */
AURA_XTEXT int sptm_virt_s2r_ctl_page(const char *what, u64 page, bool arm)
{
    int slot = -1, spare = -1;

    for (u32 i = 0; i < sptm_virt_s2r_pages; i++) {
        if (s2r_pages[i].page == page)
            slot = i;
        else if (!s2r_pages[i].page && spare < 0)
            spare = i;
    }
    if (!arm) {
        if (slot < 0)
            return 0;
        if (!s2r_pages[slot].ctl)
            return -AURACTL_EBUSY;
        if (hv_map_hw(page, page, 0x4000) < 0)
            return -AURACTL_EINVAL;
        s2r_pages[slot].ctl = false;
        s2r_pages[slot].page = 0;
        return 1;
    }
    if (slot >= 0)
        return 0; /* the trace setup or an earlier call hooks it already */
    if (!sptm_virt_s2r_ring)
        return -AURACTL_ENODEV;
    /* Any other aura hook (vuart, reset guard, AMCC watch, GPU, coprocessor trace) or the aura
     * guard leaves the page's first leaf invalid, a sub-page hook included. */
    if (sptm_virt_rg_owns(page) || !(hv_pt_walk(page) & PTE_VALID))
        return -AURACTL_EACCES;
    if (spare < 0) {
        if (sptm_virt_s2r_pages >= ARRAY_SIZE(s2r_pages))
            return -AURACTL_ENOSPC;
        spare = sptm_virt_s2r_pages;
    }
    s2r_pages[spare].what = what;
    s2r_pages[spare].seen = false;
    s2r_pages[spare].ctl = true;
    __atomic_store_n(&s2r_pages[spare].page, page, __ATOMIC_RELEASE);
    if ((u32)spare == sptm_virt_s2r_pages)
        sptm_virt_s2r_pages++;
    if (hv_map_hook(page, s2r_hook, 0x4000) < 0) {
        s2r_pages[spare].page = 0;
        s2r_pages[spare].ctl = false;
        return -AURACTL_EINVAL;
    }
    return 1;
}

/* Release every page auractl armed; returns how many. */
AURA_XTEXT u32 sptm_virt_s2r_ctl_release_all(void)
{
    u32 n = 0;
    for (u32 i = 0; i < sptm_virt_s2r_pages; i++)
        if (s2r_pages[i].page && s2r_pages[i].ctl)
            n += sptm_virt_s2r_ctl_page(NULL, s2r_pages[i].page, false) > 0;
    return n;
}

/* The ring slot of record seq (the caller checks seq against head and capacity). */
AURA_XTEXT const void *sptm_virt_s2r_ctl_rec(u64 seq)
{
    _Static_assert(sizeof(struct s2r_rec) == sizeof(struct auractl_rec), "record layout");
    return &sptm_virt_s2r_ring[seq % sptm_virt_s2r_nrec];
}

/* Pages of [base, base + size) hooked by anyone, and how many of those auractl armed. */
AURA_XTEXT u32 sptm_virt_s2r_ctl_count(u64 base, u64 size, u32 *ctl_pages)
{
    u32 n = 0, c = 0;
    for (u32 i = 0; i < sptm_virt_s2r_pages; i++) {
        u64 p = s2r_pages[i].page;
        if (p && p + 0x4000 > base && p < base + size) {
            n++;
            c += s2r_pages[i].ctl;
        }
    }
    if (ctl_pages)
        *ctl_pages = c;
    return n;
}

AURA_XTEXT void sptm_virt_s2r_marker(const char *tag, u64 arg)
{
    s2r_marker(tag, arg);
}

/* The SoC blocks the sleep path found in the ADT, or NULL: auractl's catalogue. */
AURA_XTEXT const struct s2r_soc *sptm_virt_s2r_ctl_soc(void)
{
    return s2r_soc;
}
#endif

static void s2r_keep_setup(void)
{
    for (int i = 0; i < s2r_soc->n_link; i++) {
        u64 regs[S2R_KEEP_MAX];
        u8 flags[S2R_KEEP_MAX];
        int n = pmgr_adt_collect_psregs(s2r_soc->link_paths[i], regs, flags, S2R_KEEP_MAX);
        if (n < 0)
            printf("SPTM-virt[s2r]: cannot list the power domains of %s\n", s2r_soc->link_paths[i]);
        for (int j = 0; j < n; j++) {
            if (s2r_keep_idx(regs[j]) >= 0 || s2r_report_off(regs[j]))
                continue;
            /* From boot (the proxy on the link for the whole run) there is no sleep to report
             * them off for: keep them on like the rest, or the guest's power manager switches
             * them off under the link. */
            if ((flags[j] & S2R_PMGR_OFF_AT_SLEEP) && !s2r_keep_always &&
                s2r_n_report_off < (int)ARRAY_SIZE(s2r_report_off_regs)) {
                s2r_report_off_regs[s2r_n_report_off++] = regs[j];
                s2r_hook_range("pmgr-debugpath", regs[j], 4);
                printf("SPTM-virt[s2r]: not keeping 0x%lx; reads report it off once the sleep "
                       "begins\n",
                       regs[j]);
                continue;
            }
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
    for (int i = 0; i < 2 && s2r_soc->pms_pmc[0]; i++)
        s2r_hook_range("pms-pmc", s2r_soc->pms_pmc[i], 0x4000);
    s2r_hook_blk(&s2r_soc->soc_ps, "pmgr-soc-ps", 0, 0);
    s2r_hook_blk(&s2r_soc->cluster_ctl, "pmgr-cluster-ctl", 0, 0);
    s2r_hook_blk(&s2r_soc->pwrgate, "pmgr-pwrgate", 0, 0);
    s2r_hook_blk(&s2r_soc->cllt, "pmgr-cllt", 0, 0);
    s2r_hook_blk(&s2r_soc->nub_ap, "nub-ap-ctrl", 0, 0x4000);
    s2r_hook_blk(&s2r_soc->aon, "pmgr-aon", 0, 0);
    if (s2r_soc->clkgen.size >= 0x60000) {
        s2r_hook_blk(&s2r_soc->clkgen, "clkgen-dcs", 0x5c000, 0x4000);
        s2r_hook_blk(&s2r_soc->clkgen, "clkgen-mux", 0x40000, 0x4000);
    } else {
        /* A short clock block (T8142): the whole of it, as its pages are not known yet. */
        s2r_hook_blk(&s2r_soc->clkgen, "clkgen", 0, 0);
    }
    for (int i = 0; i < 3; i++)
        s2r_hook_adt("/arm-io/nub-spmi0", i, "spmi0", 0, 0x4000);
    for (int i = 0; i < 3; i++)
        s2r_hook_adt("/arm-io/nub-spmi-a0", i, "spmi-a0", 0, 0x4000);
    s2r_hook_adt("/arm-io/smc", 0, "smc-mbox", 0x8000, 0x4000);
    s2r_hook_adt("/arm-io/pmp", 0, "pmp-mbox", 0x8000, 0x4000);
    s2r_hook_adt("/arm-io/dcp", 0, "dcp-mbox", 0x8000, 0x4000);
    if (sptm_virt_s2r_extra & 1) {
        int mp[8];
        u64 pa, sz;
        /* reg[0] is the cluster block, hooked above. reg[1] holds the AMCC planes, 0x40000 apart:
         * the first page of each is aura's own hook (amcc_hook) and stays that. */
        if (adt_path_offset_trace(adt, "/arm-io/mcc", mp) >= 0)
            for (int r = 1; r < 32 && adt_get_reg(adt, mp, "reg", r, &pa, &sz) >= 0; r++) {
                if (r == 1) {
                    for (u64 page = pa; page < pa + sz; page += 0x4000)
                        if ((page - pa) & 0x3ffff)
                            s2r_hook_range("amcc", page, 0x4000);
                } else {
                    s2r_hook_range(r < 8 ? "mcc-b" : r < 10 ? "mcc-ptd" : "mcc-ch", pa, sz);
                }
            }
    }
    if (sptm_virt_s2r_extra & 2)
        for (int r = 0; r < 4; r++)
            s2r_hook_adt("/arm-io/aon-ptd", r, "aon-ptd", 0, 0);
    /* Further blocks by node name: bit 4 the AIC, bit 5 every DART, bit 6 the PCIe controllers,
     * bit 7 USB-C port 0's controller (the port macOS drives; the debug link is port 1). Its PHY
     * (atc-phy0) hooked too reset the Mac during macOS's boot. */
    if (sptm_virt_s2r_extra & 0x1f0) {
        int node = adt_path_offset(adt, "/arm-io");
        if (node >= 0)
            ADT_FOREACH_CHILD(adt, node)
            {
                const char *name = adt_get_name(adt, node);
                const char *what = (sptm_virt_s2r_extra & 0x10) && !strcmp(name, "aic")      ? "aic"
                                   : (sptm_virt_s2r_extra & 0x20) && !strncmp(name, "dart-", 5) ? "dart"
                                   : (sptm_virt_s2r_extra & 0x40) && !strncmp(name, "apcie", 5) ? "apcie"
                                   : (sptm_virt_s2r_extra & 0x80) && !strcmp(name, "usb-drd0")
                                       ? "usb0"
                                       : NULL;
                char path[48];
                int p[8];
                u64 pa, sz;
                /* bit 8: each RTKit coprocessor's ASC page (CPU control at +0x44) */
                if (!what && (sptm_virt_s2r_extra & 0x100) &&
                    (s2r_cp_compat(node, "iop,ascwrap") || s2r_cp_compat(node, "iop-sep,ascwrap"))) {
                    snprintf(path, sizeof(path), "/arm-io/%s", name);
                    if (adt_path_offset_trace(adt, path, p) >= 0 &&
                        adt_get_reg(adt, p, "reg", 0, &pa, &sz) >= 0)
                        s2r_hook_range("asc", pa, 0x4000);
                    continue;
                }
                if (!what)
                    continue;
                snprintf(path, sizeof(path), "/arm-io/%s", name);
                if (adt_path_offset_trace(adt, path, p) < 0)
                    continue;
                for (int r = 0; r < 32 && adt_get_reg(adt, p, "reg", r, &pa, &sz) >= 0; r++)
                    if (sz && sz <= 0x200000)
                        s2r_hook_range(what, pa, sz);
                    /* PCIe's big windows (config space, reg[5]): their first 0x24000 only, where
                     * the axi2af tunables (offsets up to 0x20058) would land */
                    else if (sz && !strcmp(what, "apcie") && pa < 0x1000000000UL)
                        s2r_hook_range(what, pa, 0x24000);
            }
    }
    /* The SMC's SRAM: its shared buffer is in there, and a key write's value only travels there. */
    if (sptm_virt_s2r_extra & 8)
        /* Not in the ADT's /arm-io/smc reg list; from the Linux DT's "sram" (t8142-j813-j815.dtsi). */
        s2r_hook_range("smc-sram", 0x38de00000UL, 0x120000);
    if (sptm_virt_s2r_extra & 4) {
        static const struct {
            const char *path, *what;
        } mb[] = {
            {"/arm-io/aop", "aop-mbox"},       {"/arm-io/sio", "sio-mbox"},
            {"/arm-io/ans", "ans-mbox"},       {"/arm-io/mtp", "mtp-mbox"},
            {"/arm-io/dcpext0", "dcpext0-mbox"}, {"/arm-io/dcpext1", "dcpext1-mbox"},
            {"/arm-io/gfx-asc", "gfx-mbox"},
        };
        for (unsigned i = 0; i < ARRAY_SIZE(mb); i++)
            s2r_hook_adt(mb[i].path, 0, mb[i].what, 0x8000, 0x4000);
    }
}

/* ---- T6050: the coprocessors and the memory side ---------------------------------------------
 *
 * COPROC hooks each ASC's CPU-control page, mailbox page and wrapper window, the mxwrap
 * coprocessors' mailbox and control pages, the exclave mailboxes, the SMC's small window and the
 * other SPMI buses; MEM adds the MCC, AMCC and DCS apertures and the coprocessors' DARTs. Each
 * guest access is performed as issued and recorded; aura itself reads none of these (on the M5 a
 * read of the SMC ASC's status takes an external abort).
 *
 * The enclave's message words are cut to their headers before they reach the ring: a SEP
 * message keeps its endpoint, tag, opcode and param, and its data only on the control and boot
 * endpoints; an ESM word at or above 0x800 keeps bits 63:48 and 7:0.
 */
#define S2R_CP_PAGES_MAX 2048
#define S2R_CP_NAMES     192
#define S2R_SEP_EP_CTRL  0x00
#define S2R_SEP_EP_BOOT  0xff

static u64 *s2r_cp_pages;
static u32 s2r_n_cp_pages;
static char (*s2r_cp_names)[28];
static int s2r_n_cp_names;
static u64 s2r_sep_mbox, s2r_esm_mbox;

static u64 s2r_cp_cut(u64 addr, u64 v, int width)
{
    u64 page = addr & ~0x3fffUL, off = addr & 0x3fff;

    if (off < 0x800 || (page != s2r_sep_mbox && page != s2r_esm_mbox))
        return v;
    if (page == s2r_esm_mbox)
        return v & (width == 3 ? GENMASK(63, 48) | GENMASK(7, 0) : GENMASK(7, 0));
    /* SEP message word 0: A2I at 0x800, I2A at 0x830 */
    if ((off & ~7UL) != 0x800 && (off & ~7UL) != 0x830)
        return v;
    if (off & 4)
        return 0;
    u8 ep = v & 0xff;
    if (width == 3 && ep != S2R_SEP_EP_CTRL && ep != S2R_SEP_EP_BOOT)
        v &= GENMASK(31, 0);
    return v;
}

/* T6050 COPROC: a raw record for s2r_dcp_pkt.c, whose packet bytes go in the ring as they pass. */
void sptm_virt_s2r_rec_raw(u64 pa, u64 val, u64 tag, bool write)
{
    struct s2r_rec *r = s2r_slot();
    if (!r)
        return;
    r->t = mrs(CNTPCT_EL0);
    r->meta = S2R_META(pa, 3, write, smp_id(), 0);
    r->val = val;
    r->pc = tag;
}

/* INTMS, INTMC and the doorbells: written for every I/O, they would fill the ring */
static bool s2r_nvme_quiet(u64 addr, bool write)
{
    u64 off = addr - s2r_nvme_quiet_page;

    return write && s2r_nvme_quiet_page && off < 0x2000 &&
           (off == 0xc || off == 0x10 || off >= 0x1000);
}

static bool s2r_cp_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    if (width == 4 || width == 5) {
        for (int h = 0; h < width - 3; h++) {
            u64 a = addr + 16 * h;
            s2r_rw128(a, val + 2 * h, write);
            sptm_virt_s2r_note(ctx, a, s2r_cp_cut(a, val[2 * h], 3), write, 3);
            sptm_virt_s2r_note(ctx, a + 8, s2r_cp_cut(a + 8, val[2 * h + 1], 3), write, 3);
        }
        s2r_dcp_watch(addr, val, 4);
        return true;
    }
    if (width < 0 || width > 3)
        return false;
    sptm_virt_mmio_rw(addr, val, write, width);
    if (s2r_nvme_quiet(addr, write)) {
        __atomic_fetch_add(&s2r_nvme_quiet_n, 1, __ATOMIC_RELAXED);
        return true;
    }
    sptm_virt_s2r_note(ctx, addr, s2r_cp_cut(addr, *val, width), write, width);
    s2r_dcp_watch(addr, val, width);
    return true;
}

static bool s2r_cp_hooked(u64 page)
{
    for (u32 i = 0; i < sptm_virt_s2r_pages; i++)
        if (s2r_pages[i].page == page)
            return true;
    for (u32 i = 0; i < s2r_n_cp_pages; i++)
        if (s2r_cp_pages[i] == page)
            return true;
    return false;
}

/* [base, base + size) exactly, a page at a time; pages hooked already keep their hook. */
static void s2r_cp_range(const char *what, u64 base, u64 size)
{
    u32 n = 0;

    if (!size || (base & 0x3fff))
        return;
    for (u64 page = base; page < base + size; page += 0x4000) {
        if (sptm_virt_rg_owns(page) || s2r_cp_hooked(page))
            continue;
        u64 len = ALIGN_UP(min(0x4000UL, base + size - page), 4);
        if (s2r_n_cp_pages >= S2R_CP_PAGES_MAX || hv_map_hook(page, s2r_cp_hook, len) < 0) {
            printf("SPTM-virt[s2r]: cannot hook %s 0x%lx\n", what, page);
            break;
        }
        s2r_cp_pages[s2r_n_cp_pages++] = page;
        n++;
    }
    if (n)
        printf("SPTM-virt[s2r]: capture %s 0x%lx + 0x%lx (%u pages)\n", what, base, size, n);
}

static const char *s2r_cp_name(const char *node, const char *what)
{
    if (s2r_n_cp_names >= S2R_CP_NAMES)
        return what;
    char *n = s2r_cp_names[s2r_n_cp_names++];
    snprintf(n, sizeof(s2r_cp_names[0]), "%s-%s", node, what);
    return n;
}

/* reg[idx] of /arm-io/<name>, hooked from off for size bytes (0: to its end). */
static u64 s2r_cp_reg(const char *name, int idx, const char *what, u64 off, u64 size)
{
    char path[64];
    int p[8];
    u64 pa, sz;

    snprintf(path, sizeof(path), "/arm-io/%s", name);
    if (adt_path_offset_trace(adt, path, p) < 0 || adt_get_reg(adt, p, "reg", idx, &pa, &sz) < 0 ||
        off >= sz)
        return 0;
    s2r_cp_range(s2r_cp_name(name, what), pa + off, size ? min(size, sz - off) : sz - off);
    return pa + off;
}

/* The page holding reg[idx] of /arm-io/<name>, for windows that don't start on one. */
static void s2r_cp_reg_page(const char *name, int idx, const char *what)
{
    char path[64];
    int p[8];
    u64 pa, sz;

    snprintf(path, sizeof(path), "/arm-io/%s", name);
    if (adt_path_offset_trace(adt, path, p) >= 0 && adt_get_reg(adt, p, "reg", idx, &pa, &sz) >= 0)
        s2r_cp_range(s2r_cp_name(name, what), ALIGN_DOWN(pa, 0x4000), 0x4000);
}

static bool s2r_cp_compat(int node, const char *prefix)
{
    u32 len;
    const char *c = adt_getprop(adt, node, "compatible", &len);
    return c && len && !strncmp(c, prefix, strlen(prefix));
}

static void s2r_t6050_coproc(bool mem)
{
    static const char *const spmi[] = {"nub-spmi2", "nub-spmi3", "nub-spmi-a0", "nub-spmi-a1",
                                       "aop-spmi0"};
    static const char *const darts[] = {
        "dart-mtp",     "dart-aop",      "dart-sio",      "dart-pmp0",     "sart-ans",
        "dart-dcp",     "dart-disp0",    "dart-dcpext0",  "dart-dcpext1",  "dart-dcpext2",
        "dart-dcpext3", "dart-dispext0", "dart-dispext1", "dart-dispext2", "dart-dispext3",
    };
    int io = adt_path_offset(adt, "/arm-io"), node = io;
    u32 before = s2r_n_cp_pages;

    s2r_cp_pages = malloc(S2R_CP_PAGES_MAX * sizeof(*s2r_cp_pages));
    s2r_cp_names = malloc(S2R_CP_NAMES * sizeof(*s2r_cp_names));
    if (io < 0 || !s2r_cp_pages || !s2r_cp_names) {
        printf("SPTM-virt[s2r]: no coprocessor trace (no memory or no /arm-io)\n");
        return;
    }
    ADT_FOREACH_CHILD(adt, node)
    {
        const char *name = adt_get_name(adt, node);
        if (sptm_virt_s2r_cp_skip_ans && !strcmp(name, "ans"))
            continue;
        if (s2r_cp_compat(node, "iop,ascwrap") || s2r_cp_compat(node, "iop-sep,ascwrap")) {
            s2r_cp_reg(name, 0, "asc", 0, 0x4000);
            u64 mb = s2r_cp_reg(name, 0, "mbox", 0x8000, 0x4000);
            s2r_cp_reg(name, 1, "wrap", 0, 0x4000);
            if (!strncmp(name, "dcp", 3) && mb)
                s2r_dcp_add(name, mb);
            if (!strcmp(name, "sep"))
                s2r_sep_mbox = mb;
            if (!strcmp(name, "smc"))
                s2r_cp_reg(name, 2, "win", 0, 0x4000);
        } else if (s2r_cp_compat(node, "iop,mxwrap")) {
            s2r_cp_reg(name, 0, "mbox", 0, 0x4000);
            s2r_cp_reg(name, 1, "asc", 0, 0x4000);
        } else if (s2r_cp_compat(node, "iop,exclave-sep-manager")) {
            s2r_esm_mbox = s2r_cp_reg(name, 0, "mbox", 0, 0x4000);
        } else if (s2r_cp_compat(node, "iop,secure-rtbuddy-proxy")) {
            s2r_cp_reg(name, 0, "mbox", 0, 0x4000);
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(spmi); i++)
        for (int r = 0; r < 3; r++)
            s2r_cp_reg(spmi[i], r, "regs", 0, 0x4000);
    /* ANS's storage side: its SART, the NVMe BAR's registers, the AUS5 PHY windows. */
    if (sptm_virt_s2r_cp_ansx & S2R_CP_ANSX_SART)
        for (int r = 0; r < 3; r++) {
            u64 pa = s2r_cp_reg("sart-ans", r, "regs", 0, 0x4000);
            if (r == 2 && (sptm_virt_s2r_cp_ansx & S2R_CP_ANSX_QUIET))
                s2r_nvme_quiet_page = pa;
        }
    if (sptm_virt_s2r_cp_ansx & S2R_CP_ANSX_NVME)
        s2r_cp_reg("ans", 9, "nvme", 0, 0x4000);
    if (sptm_virt_s2r_cp_ansx & S2R_CP_ANSX_AUS5)
        for (int r = 10; r < 14; r++)
            s2r_cp_reg_page("apcie0", r, "aus5");
    if (mem) {
        /* The MCC's own pages, then each AMCC and its DCSs (amcc-reg-idx on). */
        for (int r = 0; r < 64; r++)
            if (!s2r_cp_reg("mcc", r, r < 14 ? "mcc" : r < 22 ? "amcc" : "dcs", 0, 0))
                break;
        for (unsigned i = 0; i < ARRAY_SIZE(darts); i++)
            s2r_cp_reg(darts[i], 0, "regs", 0, 0);
    }
    printf("SPTM-virt[s2r]: coprocessor trace: %u pages (SEP mailbox 0x%lx, ESM 0x%lx cut to "
           "headers)\n",
           s2r_n_cp_pages - before, s2r_sep_mbox, s2r_esm_mbox);
}

/* Allocate and clear the record ring, setting its capacity and head. A big ring falls back to the
 * small size if the heap is short. Returns false only if even the small ring will not fit. */
static bool s2r_ring_alloc(u64 ring_bytes)
{
    sptm_virt_s2r_ring = memalign(0x4000, ring_bytes);
    if (!sptm_virt_s2r_ring && ring_bytes != S2R_RING_BYTES) {
        ring_bytes = S2R_RING_BYTES;
        sptm_virt_s2r_ring = memalign(0x4000, ring_bytes);
    }
    if (!sptm_virt_s2r_ring)
        return false;
    memset(sptm_virt_s2r_ring, 0, ring_bytes);
    sptm_virt_s2r_nrec = ring_bytes / sizeof(struct s2r_rec);
    sptm_virt_s2r_head = 0;
    return true;
}

/* cdc: the proxy runs over the DWC3 CDC link, whose domains are kept on even without the trace. */
void sptm_virt_s2r_setup(bool cdc)
{
    u64 flags = sptm_virt_flags;

    const char *missing;
    if (s2r_soc_from_adt(adt, sptm_virt_link_phys, ARRAY_SIZE(sptm_virt_link_phys), &s2r_soc_adt,
                         &missing) < 0) {
        printf("SPTM-virt[s2r]: no sleep path, the ADT gives no %s; debug-link domains not kept\n",
               missing);
        /* The sleep path -- keeping the debug-link domains alive across the sleep, the capture
         * hooks and the sleep-commit rules -- needs that USB link, so drop every sleep flag. The
         * record ring and the live in-guest catalogue (auractl arm/read on named MMIO over hvc) do
         * not: on a KIS/DockChannel proxy there is no debug-link USB port, yet the guest still
         * drives auractl. If a trace was asked for, allocate the ring alone so recording works; the
         * sleep machinery stays gated (SPTM_VIRT_S2R stays clear), so nothing on the wake or sleep
         * path runs without an s2r_soc. */
        sptm_virt_flags &= ~(SPTM_VIRT_S2R | SPTM_VIRT_S2R_GO | SPTM_VIRT_S2R_FULL |
                             SPTM_VIRT_S2R_COPROC | SPTM_VIRT_S2R_DROP_USB);
        if ((flags & SPTM_VIRT_S2R) && s2r_ring_alloc(S2R_RING_BYTES))
            printf("SPTM-virt[s2r]: no debug-link USB port; %lu-record ring at 0x%lx for live "
                   "in-guest tracing, no sleep path\n",
                   sptm_virt_s2r_nrec, (u64)sptm_virt_s2r_ring);
        return;
    }
    s2r_soc = &s2r_soc_adt;
    u64 sleep_flags = SPTM_VIRT_S2R | SPTM_VIRT_S2R_GO | SPTM_VIRT_S2R_FULL | SPTM_VIRT_S2R_COPROC |
                      SPTM_VIRT_S2R_DROP_USB | SPTM_VIRT_S2R_MEM;
    u64 passive_ok =
        SPTM_VIRT_S2R_FULL | SPTM_VIRT_S2R_GO | SPTM_VIRT_S2R_COPROC | SPTM_VIRT_S2R_MEM;
    if (s2r_soc->timer_only && ((flags & sleep_flags & ~passive_ok) != SPTM_VIRT_S2R ||
                                sptm_virt_s2r_quiesce || sptm_virt_s2r_usb_retake)) {
        printf("SPTM-virt[s2r]: a timer-only sleep takes TRACE (0x1) with FULL (0x4), GO (0x2), "
               "COPROC "
               "(0x8) and MEM (0x100) only, quiesce=0, usb_retake=0; debug-link domains not "
               "kept\n");
        sptm_virt_flags &= ~sleep_flags;
        return;
    }
    for (int i = 0; i < s2r_soc->n_skipped; i++)
        printf("SPTM-virt[s2r]: the ADT gives no %s; not traced\n", s2r_soc->skipped[i]);
    s2r_keep_always = cdc;
    if (!(flags & SPTM_VIRT_S2R)) {
        if (cdc)
            s2r_keep_setup();
        return;
    }
    /* A timer-only sleep has no stop before the commit to read a ring from: keep it all. */
    /* With AURA_S2R_GO_COUNT the ring must hold a sleep, its wake and the next entry. */
    u64 ring_bytes = (s2r_soc->timer_only || chip_id == T8122) ? S2R_RING_BYTES_BIG
                     : sptm_virt_s2r_go_count ? (48UL << 20)
                                              : S2R_RING_BYTES;
    if (!s2r_ring_alloc(ring_bytes)) {
        printf("SPTM-virt[s2r]: no memory for the ring; sleep path off\n");
        sptm_virt_flags &= ~SPTM_VIRT_S2R;
        if (cdc && !s2r_soc->timer_only)
            s2r_keep_setup();
        return;
    }
    if (s2r_soc->timer_only) {
        /* No link keeping, USB, per-core or WFI rules: the timer's start is the sleep. */
        s2r_go = flags & SPTM_VIRT_S2R_GO;
        sptm_virt_rg_passthrough = s2r_go;
        if (s2r_go && sptm_virt_s2r_wake_s)
            s2r_rtc_setup();
        s2r_arm_at = mrs(CNTPCT_EL0) + S2R_ARM_AFTER_S * mrs(CNTFRQ_EL0);
        for (int i = 0; i < s2r_soc->n_trace; i++) {
            const struct s2r_trace_blk *b = &s2r_soc->trace[i];
            if (!(flags & SPTM_VIRT_S2R_FULL) && strcmp(b->what, "cpu-impl"))
                continue;
            /* With the cluster ACC/CPM windows trapped, XNU's ACC rail-gating callback timed out
             * after 30 s (J714s, GO). Their accesses aren't needed to let the sleep happen. */
            if (s2r_go && (!strcmp(b->what, "acc-impl") || !strcmp(b->what, "cpm-impl")))
                continue;
            printf("SPTM-virt[s2r]: capture %s 0x%lx + 0x%lx (ADT size 0x%lx)\n", b->what, b->base,
                   b->size, b->declared_size);
            if (!s2r_hook_capture(b)) {
                s2r_arm_at = 0; /* partially installed hooks must never claim a complete stop */
                sptm_virt_flags &= ~sleep_flags;
                return;
            }
        }
        s2r_hook_range("s2r-timer", s2r_soc->stop_timer & ~0x3fffUL, 0x4000);
        if (flags & (SPTM_VIRT_S2R_COPROC | SPTM_VIRT_S2R_MEM))
            s2r_t6050_coproc(flags & SPTM_VIRT_S2R_MEM);
        s2r_marker("TIMERSLP", sptm_virt_s2r_pages + s2r_n_cp_pages);
        printf("SPTM-virt[s2r]: timer-only sleep, %s: %u pages, %lu-record ring at 0x%lx; "
               "32-bit [0x%lx]=1 %s after %u s\n",
               s2r_go ? "GO" : "PRECOMMIT STOP", sptm_virt_s2r_pages, sptm_virt_s2r_nrec,
               (u64)sptm_virt_s2r_ring, s2r_soc->stop_timer,
               s2r_go ? "commits the sleep" : "withheld", S2R_ARM_AFTER_S);
        return;
    }
    s2r_go = flags & SPTM_VIRT_S2R_GO;
    s2r_fulltrace = flags & SPTM_VIRT_S2R_FULL;
    s2r_coproc = flags & SPTM_VIRT_S2R_COPROC;
    s2r_drop_usb = flags & SPTM_VIRT_S2R_DROP_USB;
    /* In go mode the reset guard leaves macOS's sleep-time watchdog alone. */
    sptm_virt_rg_passthrough = s2r_go;
    s2r_link_iodev = uartproxy_iodev;
    s2r_usb_setup();
    s2r_rtc_required = chip_id == T8122 && s2r_go && sptm_virt_s2r_wake_s;
    if (s2r_go && sptm_virt_s2r_wake_s)
        s2r_rtc_setup();
    if (s2r_go && (sptm_virt_s2r_crumbs & 2)) { /* bit 1: SPMI notes */
        s2r_crumb_dev = spmi_init("/arm-io/nub-spmi0");
        s2r_crumb(0x01);
        s2r_crumb_at(8, 0);
        s2r_crumb_at(9, 0);
        printf("SPTM-virt[s2r]: crumbs in PMU scratch 0x830d: %s\n", s2r_crumb_dev ? "on" : "no SPMI");
    }

    /* Each core's self-disable and the commit: a handful of accesses. */
    s2r_hook_blk(&s2r_soc->soc_misc, "pmgr-soc-misc", 0, 0);
    s2r_hook_blk(&s2r_soc->aon_pst, "pmgr-aon-pst", 0, 0);
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
    if (!s2r_go || s2r_fulltrace) {
        s2r_hook_full();
    } else if ((sptm_virt_s2r_crumbs & 128) || s2r_j613_traffic_enabled()) {
        /* The cluster words, the power gates and CLLT: few writes, and most of a wake. */
        s2r_hook_blk(&s2r_soc->cluster_ctl, "pmgr-cluster-ctl", 0, 0);
        s2r_hook_blk(&s2r_soc->pwrgate, "pmgr-pwrgate", 0, 0);
        s2r_hook_blk(&s2r_soc->cllt, "pmgr-cllt", 0, 0);
    }

    if (s2r_j613_traffic_enabled()) {
        s2r_hook_blk(&s2r_soc->soc_ps, "pmgr-soc-ps", 0, 0);
        s2r_hook_range("pmgr-mini", 0x2e4280000, 0x4000);
        s2r_hook_range("spmi0", 0x2e4714000, 0x4000);
        s2r_hook_range("spmi-a0", 0x2e4908000, 0x4000);
        s2r_hook_range("watchdog", 0x2e42b0000, 0x4000); /* skipped if reset guard owns it */
    }

    s2r_keep_setup();

    s2r_arm_at = mrs(CNTPCT_EL0) + S2R_ARM_AFTER_S * mrs(CNTFRQ_EL0);
    s2r_marker("S2RSETUP", sptm_virt_s2r_pages);
    printf("SPTM-virt[s2r]: %u pages hooked, %lu-record ring at 0x%lx; %s\n", sptm_virt_s2r_pages,
           sptm_virt_s2r_nrec, (u64)sptm_virt_s2r_ring,
           !s2r_go         ? "stop mode: stopping before the S2R timer start"
           : s2r_fulltrace ? "GO mode + full trace (heavy; expect a reset)"
                           : "GO mode: minimal hooks, the sleep proceeds at native speed");
}

/*
 * Option bit 6 (J813): release the debug link when macOS zeroes the watchdog's count on its way
 * into the sleep, about 14 ms before the commit and before any core has disabled itself, instead
 * of at the commit. At the commit the teardown (dwc3 stop, three PMGR power-downs with their
 * polls) sits between the cluster words and the cores' last WFI, where the sleep engine allows
 * 33 ms, and between WD2's arming and the commit write. Here it only delays the whole entry.
 */
void sptm_virt_s2r_early_release(void)
{
    if (!s2r_go || !(sptm_virt_s2r_crumbs & 64) || s2r_keep_released || !s2r_armed())
        return;
    s2r_crumb_at(8, 0x60); /* early release entered */
    s2r_console_to_aon();
    s2r_crumb_at(8, 0x61);
    s2r_release_kept();
    s2r_crumb_at(8, 0x62); /* early release done */
}

/* ---- the wake ------------------------------------------------------------------------------- */

/*
 * sptm_virt_cpu_reset, MMU off. After the commit the SoC's sleep sequencer may restart a core at
 * its reset vector (linux: "arm64: spin table: the holding pen's deep-sleep WFI after an Apple
 * sleep's commit", from T6050). It has to take the deep-sleep WFI again or its cluster cannot
 * power down, the sequencer waits with no timeout, and macOS's 5 s WD2 resets the SoC. Sent on
 * into the guest it would execute that WFI at EL1, which does not power a T8142 core down. So a
 * core other than the committing one that resets while a commit is pending is not the wake: it
 * takes the WFI here. The committing core's reset is the wake and clears the pending commit
 * before XNU starts any other core. Host: AURA_S2R_CRUMBS bit 4.
 */
bool sptm_virt_s2r_restarted_in_sleep(int cpu)
{
    return s2r_go && (sptm_virt_s2r_crumbs & 16) && s2r_commit_t && cpu != s2r_timer_cpu;
}

/* sptm_virt_cpu_reset, MMU off: the first reset after a commit is the wake. */
void sptm_virt_s2r_wake(int cpu)
{
    u64 commit = s2r_commit_t;
    if (!commit)
        return;
    u64 now = mrs(CNTPCT_EL0), frq = mrs(CNTFRQ_EL0);
    if (s2r_j613_snapshot_enabled()) {
        s2r_wake_head = sptm_virt_s2r_head;
        s2r_retained_marker("J613RST", now);
        s2r_j613_snapshot("J613WAKE", true);
    }
    s2r_commit_t = 0;
    s2r_wake_t = now;
    s2r_wake_commit_t = commit;
    s2r_wake_cpu = cpu;
    s2r_wake_count++;
    /* Option bit 256: this core arms aura's stage 2 at its entry to SPTM's reset vector, not at
     * its first GEXIT into XNU, so the hooks also see SPTM's own resume path. */
    sptm_virt_s2r_early_s2 = (sptm_virt_s2r_crumbs & 256) ? cpu + 1 : 0;
    s2r_clean(&sptm_virt_s2r_early_s2, sizeof(sptm_virt_s2r_early_s2));
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

/* One AURA_S2R_PROBE read into the ring: bit 63 of the entry asks for a 64-bit read (the AMCC
 * takes only those from macOS), else 32-bit. Marker: low 32 address bits << 32 | low 32 value bits. */
static void s2r_probe_one(u64 e)
{
    u64 a = e & ~BIT(63);
    u64 v = (e & BIT(63)) ? read64(a) : read32(a);
    s2r_marker("PROBE", (a << 32) | (v & 0xffffffff));
}

/* Option bit 512: a few registers of blocks macOS rewrites at the wake, read into the ring (low 32
 * bits of the address << 32 | value) at the commit and at the first trap after the wake. Fixed J813
 * addresses. With the memory controller's blocks in the list as well (0x220028000, 0x220068000,
 * 0x24f010000, 0x221000580/590, 0x3803d01b8/d8) the wake failed (m2: no wake step, WD2 reset). */
static void s2r_regs_snapshot(const char *tag)
{
    static const u64 regs[] = {
        0x380038000, 0x380038190, 0x380780000, 0x380780004, 0x380724000, 0x380724060,
        0x380700c00, 0x380700118, 0x380717ff0, 0x381000014, 0x381001000, 0x381001010,
        0x3810010f0, 0x381010000, 0x381010004, 0x381010100, 0x381010400, 0x381010800,
    };
    for (unsigned i = 0; i < ARRAY_SIZE(regs); i++)
        s2r_marker(tag, (regs[i] << 32) | read32(regs[i]));
    /* the AP wake-source page's first 24 words (status at +0x2c..; the enables are near) */
    for (unsigned i = 0; i < 24; i++)
        s2r_marker(tag[0] == 'C' ? "CNUB" : "WNUB",
                   ((u64)(4 * i) << 32) | read32(0x388004000UL + 4 * i));
    /* host: AURA_S2R_PREWRITE (address, value), written before the probes, at the wake only */
    if (tag[0] == 'W' && sptm_virt_s2r_prewrite[0]) {
        write32(sptm_virt_s2r_prewrite[0], (u32)sptm_virt_s2r_prewrite[1]);
        s2r_marker("PREWR", (sptm_virt_s2r_prewrite[0] << 32) | read32(sptm_virt_s2r_prewrite[0]));
    }
    /* host: AURA_S2R_PROBE, up to four more, read at the wake only (unless AURA_S2R_PROBE_AT
     * names a later point for them) */
    if (tag[0] == 'W' && !sptm_virt_s2r_probe_at)
        for (unsigned i = 0; i < ARRAY_SIZE(sptm_virt_s2r_probe) && sptm_virt_s2r_probe[i]; i++)
            s2r_probe_one(sptm_virt_s2r_probe[i]);
}

/* exc_sync: the first guest trap after a wake prints the banner; a bounded trail follows. */
void sptm_virt_s2r_wake_trap(u64 esr, u64 elr, bool in_gl)
{
    if (!s2r_wake_t)
        return;
    if (!s2r_wake_banner_done &&
        !__atomic_exchange_n(&s2r_wake_banner_done, true, __ATOMIC_SEQ_CST)) {
        if (!s2r_j613_snapshot_enabled())
            s2r_wake_head = sptm_virt_s2r_head;
        u64 frq = mrs(CNTFRQ_EL0), now = mrs(CNTPCT_EL0);
        printf("SPTM-virt[s2r]: WAKE #%u banner: first guest trap on cpu%d, %lu us after the "
               "reset on cpu%d\n",
               s2r_wake_count, smp_id(), frq ? (now - s2r_wake_t) * 1000000 / frq : 0,
               s2r_wake_cpu);
        if (s2r_soc->nub_wake_status) {
            printf("SPTM-virt[s2r]: AON wake-source status 0x%lx..+0x1c:",
                   s2r_soc->nub_wake_status);
            for (int i = 0; i < S2R_NUB_WAKE_WORDS; i++)
                printf(" %08x", read32(s2r_soc->nub_wake_status + 4 * i));
            printf("\n");
        }
        s2r_aic_dump("wake");
        s2r_marker("WAKE", s2r_wake_cpu);
        /* The WAKE record is this first trap; the reset was earlier, by this many counter ticks. */
        s2r_marker("RSTAGO", now - s2r_wake_t);
        /* The same state into the ring, which the after-wake dump carries (the console lines
         * above are lost to the wake's overrun): AIC CONFIG and mask word 0 as SPTM finds them,
         * and each set AON wake-source word (index << 32 | value). */
        if (aic) {
            s2r_marker("AICCFG", read32(aic->base + 0x14));
            s2r_marker("AICMASK0", read32(aic->base + aic->regs.mask_set));
        }
        /* Option bit 512: what survived the sleep in blocks macOS rewrites at the wake, read
         * before it does; the same registers are read at the commit (CREG). */
        if (sptm_virt_s2r_crumbs & 512)
            s2r_regs_snapshot("WREG");
        if (s2r_soc->nub_wake_status)
            for (int i = 0; i < S2R_NUB_WAKE_WORDS; i++) {
                u32 v = read32(s2r_soc->nub_wake_status + 4 * i);
                if (v)
                    s2r_marker("WAKESRC", ((u64)i << 32) | v);
            }
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
    s2r_crumb_at(8, 0x48); /* XNU running again after a wake */
    s2r_usb_wake();
}

/*
 * The wake's MMIO record (with the full trace): XNU stopping the AON watchdog ends its platform
 * wake, so everything the ring holds since the wake's first guest trap is the record. With the
 * link re-taken it is printed from the re-take's poll loop, a few records at a time.
 */
static u64 s2r_wakerec_rp, s2r_wakerec_end;
static u64 s2r_wakerec_wdstop;
/* After the wake record, the sleep record: from before the first self-disable to the wake. */
#define S2R_SLEEPREC_BEFORE 512UL
#define S2R_SLEEPREC_MAX    8192UL
static u64 s2r_sleeprec_from, s2r_sleeprec_to;

void sptm_virt_s2r_wd_stopped(void)
{
    if (s2r_wakerec_end || !s2r_fulltrace || !s2r_wake_t || !s2r_wake_banner_done)
        return;
    s2r_wakerec_rp = s2r_wake_head;
    s2r_wakerec_end = sptm_virt_s2r_head;
    s2r_wakerec_wdstop = s2r_wakerec_end;
    /* Without the re-take, s2r_wakerec_fallback schedules it once the link has settled. */
}

/*
 * Without the re-take, the record starts 2 s after the wake and is paced by s2r_wakerec_pump from
 * the hooks. Printed at the watchdog stop (about 22 ms in on T8132), or in one burst, it overruns
 * the DockChannel. It runs from the wake's first guest trap, up to S2R_WAKEREC_MAX records, and
 * names the watchdog stop's record.
 */
#define S2R_WAKEREC_MAX 4096UL

static void s2r_wakerec_fallback(void)
{
    if (!s2r_fulltrace || s2r_wakerec_printed || sptm_virt_s2r_usb_retake || !s2r_wake_t ||
        !s2r_wake_banner_done)
        return;
    if (mrs(CNTPCT_EL0) - s2r_wake_t < 2 * mrs(CNTFRQ_EL0))
        return;
    s2r_wakerec_printed = true;
    s2r_wakerec_rp = s2r_wake_head;
    s2r_wakerec_end = min(sptm_virt_s2r_head, s2r_wake_head + S2R_WAKEREC_MAX);
    if (s2r_sleep_head && s2r_sleep_head < s2r_wake_head) {
        s2r_sleeprec_to = s2r_wake_head;
        s2r_sleeprec_from =
            s2r_sleep_head > S2R_SLEEPREC_BEFORE ? s2r_sleep_head - S2R_SLEEPREC_BEFORE : 0;
        if (s2r_sleeprec_to - s2r_sleeprec_from > S2R_SLEEPREC_MAX)
            s2r_sleeprec_to = s2r_sleeprec_from + S2R_SLEEPREC_MAX;
    }
    printf("SPTM-virt[s2r]: wake record: records %lu..%lu, watchdog stop at %lu; sleep record "
           "%lu..%lu, sleep window opened at %lu\n",
           s2r_wakerec_rp, s2r_wakerec_end, s2r_wakerec_wdstop, s2r_sleeprec_from, s2r_sleeprec_to,
           (u64)s2r_sleep_head);
}

static void s2r_wakerec_pump(void)
{
    static int started;

    if (!s2r_wakerec_end || s2r_wakerec_rp >= s2r_wakerec_end)
        return;
    /* Over CDC when re-taken; otherwise the DockChannel, a record at a time while it has room. */
    bool dock = !sptm_virt_s2r_usb_retake;
    if (dock ? dockchannel_uart_tx_free() < 512 : !iodev_can_write(IODEV_USB0))
        return;
    if (dock && sptm_virt_s2r_head - s2r_wakerec_rp > sptm_virt_s2r_nrec) {
        printf("SPTM-virt[s2r]: the wake record was overwritten in the ring\n");
        s2r_wakerec_rp = s2r_wakerec_end;
        return;
    }
    if (!started) {
        started = 1;
        printf("S2R-WAKEREC start: %lu records\n", s2r_wakerec_end - s2r_wakerec_rp);
    }
    for (int k = 0; k < 8 && s2r_wakerec_rp < s2r_wakerec_end; k++, s2r_wakerec_rp++) {
        if (dock && dockchannel_uart_tx_free() < 256)
            break;
        u64 meta = sptm_virt_s2r_ring[s2r_wakerec_rp % sptm_virt_s2r_nrec].meta;
        if (!(meta & S2R_MARKER) || s2r_j613_snapshot_enabled())
            s2r_print_rec(s2r_wakerec_rp);
    }
    if (s2r_wakerec_rp < s2r_wakerec_end)
        return;
    if (!s2r_sleeprec_to) {
        printf(started == 2 ? "S2R-SLEEPREC end\n" : "S2R-WAKEREC end\n");
        return;
    }
    /* The wake record is out: the sleep record follows through the same pump. */
    printf("S2R-WAKEREC end\n");
    printf("S2R-SLEEPREC start: %lu records\n", s2r_sleeprec_to - s2r_sleeprec_from);
    started = 2;
    s2r_wakerec_rp = s2r_sleeprec_from;
    s2r_wakerec_end = s2r_sleeprec_to;
    s2r_sleeprec_to = 0;
}

/* J613's KIS DockChannel works after XNU resumes even if the CDC retake fails. Scan the
 * retained ring once per reset and pace the ordered entry/wake export independently of CDC. */
static void s2r_j613_export_tick(void)
{
    static unsigned busy;
    static u32 cycle;
    static u64 from, to;
    if (!s2r_wake_xnu_done || sptm_virt_s2r_quiet || !sptm_virt_s2r_ring)
        return;
    if (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE))
        return;

    if (cycle != s2r_wake_count) {
        u64 freq = mrs(CNTFRQ_EL0);
        if (mrs(CNTPCT_EL0) - s2r_wake_t < 10 * freq || dockchannel_uart_tx_free() < 512)
            goto done;
        cycle = s2r_wake_count;
        to = sptm_virt_s2r_head;
        u64 oldest = to > sptm_virt_s2r_nrec ? to - sptm_virt_s2r_nrec : 0;
        u64 start = s2r_wake_commit_t - 2 * freq;
        from = min(max(oldest, s2r_commit_head), to);
        while (from > oldest && sptm_virt_s2r_ring[(from - 1) % sptm_virt_s2r_nrec].t >= start)
            from--;
        printf("J613-TRACE start wake=%u records=%lu..%lu commit=%lx reset=%lx freq=%lu\n",
               cycle, from, to, s2r_wake_commit_t, s2r_wake_t, freq);
    }
    if (from >= to)
        goto done;
    if (sptm_virt_s2r_head - from > sptm_virt_s2r_nrec) {
        printf("J613-TRACE overwritten wake=%u record=%lu\n", cycle, from);
        from = to;
        goto done;
    }
    for (unsigned scanned = 0, sent = 0; from < to && scanned < 256 && sent < 4;
         scanned++, from++) {
        if (dockchannel_uart_tx_free() < 256)
            break;
        const struct s2r_rec *r = &sptm_virt_s2r_ring[from % sptm_virt_s2r_nrec];
        u64 page = r->meta & GENMASK(47, 14);
        bool first;
        const char *what = page ? s2r_page_name(page, &first) : "";
        if ((r->meta & (S2R_MARKER | S2R_WRITE)) || s2r_is_mbox(what) ||
            page == 0x2e4714000 || page == 0x2e4908000) {
            s2r_print_rec(from);
            sent++;
        }
    }
    if (from == to)
        printf("J613-TRACE end wake=%u\n", cycle);
done:
    __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
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


/* One port again after a sleep took its controller's power: the old device is dropped unstopped. */
static bool s2r_reinit_hpm_match(char *hpm_path, void *data)
{
    size_t len = strlen(hpm_path);
    return len >= 4 && !memcmp(hpm_path + len - 4, "hpm", 3) && hpm_path[len - 1] - '0' == *(int *)data;
}

static int s2r_reinit_hpm_one(char *hpm_path, tps6598x_dev_t *tps, void *data)
{
    UNUSED(hpm_path);
    *(int *)data = tps6598x_powerup(tps) < 0 ? -2 : -1; /* -1: powered up, -2: it refused */
    return HPM_ACTION_STOP;
}

/*
 * The whole of what boot does for one port (usb_init + usb_iodev_init): its PD controller powered
 * up, then the PHY with its three power domains, then the DART and the controller. An earlier
 * version skipped the first two and went straight to the DART of an unpowered block, which never
 * came back.
 */
static int usb_iodev_reinit(int i)
{
    int hpm = i;
    tps6598x_foreach_hpm(s2r_reinit_hpm_match, s2r_reinit_hpm_one, &hpm);
    s2r_crumb_at(8, hpm == -1 ? 0x74 : hpm == -2 ? 0x7c : 0x7b); /* PD up / refused / not found */
    if (usb_phy_bringup(i) < 0) {
        s2r_crumb_at(8, 0x7a);
        return -1;
    }
    s2r_crumb_at(8, 0x75); /* PHY and power domains up */
    struct iodev *d = iodev_unregister_device(IODEV_USB0 + i);
    dwc3_dev_t *opaque = usb_iodev_bringup(i);
    if (!opaque || !(d || (d = memalign(SPINLOCK_ALIGN, sizeof(*d)))))
        return -1;
    d->ops = &iodev_usb_ops;
    d->opaque = opaque;
    d->usage = USAGE_CONSOLE | USAGE_UARTPROXY;
    spin_init(&d->lock);
    iodev_register_device(IODEV_USB0 + i, d);
    return 0;
}

static void __attribute__((noreturn)) s2r_usb_main(void)
{
    u64 frq = mrs(CNTFRQ_EL0);

    s2r_crumb_at(8, 0x50); /* re-take started */
    s2r_usb_keep_rearm();
    pmgr_keep_configured = true;
    s2r_usb_yieldable = true;
    /* T8142: usb_cdc_link_start has no re-take; bring the link's own port up again (J813: USB1). */
    int ret = chip_id == T8142 ? usb_iodev_reinit(s2r_link_iodev - IODEV_USB0)
                               : usb_cdc_link_start(s2r_usb_step, usb_cdc_atc_lane());
    s2r_usb_yieldable = false;
    pmgr_keep_configured = false;
    s2r_crumb_at(8, ret ? 0x5e : 0x51); /* port bring-up failed / returned */
    if (ret) {
        /* The domains stay on and the keep rule stands, so the next commit powers them down. */
        printf("USB-RETAKE: failed at step %u\n", s2r_usb_step_now);
        s2r_usb_state = S2R_USB_OFF;
        while (1)
            s2r_usb_yield(0);
    }
    hv_vuart_usb_back(chip_id == T8142 ? s2r_link_iodev : IODEV_USB0);
    uartproxy_iodev = chip_id == T8142 ? s2r_link_iodev : IODEV_USB0;
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
        iodev_handle_events(uartproxy_iodev);
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
    s2r_link_iodev = uartproxy_iodev;
    if (!s2r_go || !s2r_keep_always || (!s2r_drop_usb && chip_id != T8142)) {
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
    if (s2r_j613_traffic_enabled())
        s2r_j613_export_tick();
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
