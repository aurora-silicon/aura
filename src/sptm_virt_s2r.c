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
#include "usb.h"
#include "usb_cdc_atc.h"
#include "utils.h"
#include "xnuboot.h"

#define S2R_RING_BYTES     (8UL << 20)
#define S2R_RING_BYTES_BIG (64UL << 20) /* a whole sleep entry; 8 MiB if the heap is short */

/* The sleep rules start a while after setup: XNU's PMGR driver writes the same registers while
 * it boots, and a sleep comes minutes after the desktop. After a wake with the link re-taken they
 * come back quickly. */
#define S2R_ARM_AFTER_S   45
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

/* For the host: read over the proxy after a stop, or after a reset that kept DRAM. */
struct s2r_rec *sptm_virt_s2r_ring;
u64 sptm_virt_s2r_nrec;          /* capacity in records */
volatile u64 sptm_virt_s2r_head; /* records ever appended */
u32 sptm_virt_s2r_pages;         /* pages hooked */

/* Set by the host before entry (run_guest_sptm.py). */
u32 sptm_virt_s2r_quiesce;    /* S2R_QUIESCE_* */
u32 sptm_virt_s2r_wake_s;     /* GO, timer-only sleep: PMU RTC alarm this many s after the commit */
u32 sptm_virt_s2r_go_count;   /* GO, timer-only sleep: stop at the commit after this many wakes */
u32 sptm_virt_s2r_usb_retake; /* 1: re-take the link after a wake; n > 1: also drop it n s in */
u32 sptm_virt_s2r_usb_delay_ms; /* delay from XNU's return to the re-take, 0 = default */
u32 sptm_virt_s2r_cp_skip_ans;  /* T6050 COPROC: leave ANS's own pages unhooked */
u32 sptm_virt_s2r_cp_ansx;      /* T6050 COPROC: S2R_CP_ANSX_* pages around ANS too */
/* S2R_CP_ANSX_QUIET: XNU's NVMe registers (sart-ans reg[2]); per-I/O writes are only counted */
static u64 s2r_nvme_quiet_page, s2r_nvme_quiet_n;

/* From the host (AURA_S2R_CP_ONLY): a comma list of /arm-io node names for the T6050 COPROC trace
 * to hook; empty hooks every coprocessor. A name also matches that node's "dart-" form. */
char sptm_virt_s2r_cp_only[256];
/* From the host (AURA_S2R_CP_NODES): a comma list of /arm-io nodes whose every reg window (up to
 * 1 MiB) is hooked, such as the ATC PHYs and ACIOs; "name:N" hooks only reg N, whatever its size. */
char sptm_virt_s2r_cp_nodes[512];
/* From the host (AURA_S2R_CP_RANGE): up to 4 { base, size } of extra pages to hook, such as the
 * AOP's SRAM. */
u64 sptm_virt_s2r_cp_range[8];

static bool s2r_cp_wanted(const char *name)
{
    const char *l = sptm_virt_s2r_cp_only;

    if (!*l)
        return true;
    if (!strncmp(name, "dart-", 5))
        name += 5;
    size_t n = strlen(name);
    while (*l) {
        const char *e = l;
        while (*e && *e != ',')
            e++;
        if ((size_t)(e - l) == n && !strncmp(l, name, n))
            return true;
        l = *e ? e + 1 : e;
    }
    return false;
}

static struct {
    u64 page;
    const char *what;
    bool seen;
} s2r_pages[S2R_TRACE_MAX_PAGES];

static bool s2r_go, s2r_fulltrace, s2r_coproc, s2r_drop_usb;
static u64 s2r_arm_at;
/* Cores with their last WFI trapped (stop mode), parked there, and the core that committed. */
static volatile u32 s2r_twi_mask, s2r_parked_mask;
/* Go mode: the ring position where the first core opened the sleep window (0: none yet). */
static volatile u64 s2r_sleep_head;
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
        s2r_sleep_head = sptm_virt_s2r_head;
        s2r_clean(&s2r_sleep_head, sizeof(s2r_sleep_head));
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
static void s2r_rtc_arm(void)
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
    return;
fail:
    printf("SPTM-virt[s2r]: PMU RTC alarm not armed (SPMI error); no timed wake\n");
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
    if (s2r_nvme_quiet_page)
        printf("SPTM-virt[s2r]: %lu NVMe per-I/O writes not recorded\n", s2r_nvme_quiet_n);
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

static void s2r_wakerec_fallback(void);
static void s2r_wakerec_pump(void);
static bool s2r_wakerec_printed;

static bool s2r_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    /* Timer-only sleep: the S2R timer's start is the commit; everything else is recorded. */
    if (s2r_soc->timer_only) {
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
    s2r_wakerec_fallback();
    if (s2r_wakerec_printed)
        s2r_wakerec_pump();

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
    s2r_hook_blk(&s2r_soc->clkgen, "clkgen-dcs", 0x5c000, 0x4000);
    s2r_hook_blk(&s2r_soc->clkgen, "clkgen-mux", 0x40000, 0x4000);
    for (int i = 0; i < 3; i++)
        s2r_hook_adt("/arm-io/nub-spmi0", i, "spmi0", 0, 0x4000);
    for (int i = 0; i < 3; i++)
        s2r_hook_adt("/arm-io/nub-spmi-a0", i, "spmi-a0", 0, 0x4000);
    s2r_hook_adt("/arm-io/smc", 0, "smc-mbox", 0x8000, 0x4000);
    s2r_hook_adt("/arm-io/pmp", 0, "pmp-mbox", 0x8000, 0x4000);
    s2r_hook_adt("/arm-io/dcp", 0, "dcp-mbox", 0x8000, 0x4000);
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
#define S2R_CP_NAMES     384
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

static bool s2r_cp_access(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
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

/* The host's boot deadline (AURA_GPU_STOP_BOOT_SECS) is offered after the access is recorded, so
 * a trace without the GPU hooks still parks and leaves the ring for the host to read. */
static bool s2r_cp_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    bool ok = s2r_cp_access(ctx, addr, val, write, width);
    if (ok && sptm_virt_gpu_deadline_due())
        sptm_virt_gpu_deadline(ctx);
    return ok;
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

/* AURA_S2R_CP_NODES: every reg window of each listed /arm-io node, or only reg N for "name:N". */
static void s2r_cp_nodes(void)
{
    const char *l = sptm_virt_s2r_cp_nodes;

    while (*l) {
        char name[40], path[64], what[8];
        const char *e = l;
        int p[8], only = -1;

        while (*e && *e != ',')
            e++;
        size_t n = min((size_t)(e - l), sizeof(name) - 1);
        memcpy(name, l, n);
        name[n] = 0;
        l = *e ? e + 1 : e;
        char *c = strchr(name, ':');
        if (c) {
            *c = 0;
            for (only = 0, c++; *c >= '0' && *c <= '9'; c++)
                only = only * 10 + *c - '0';
        }
        snprintf(path, sizeof(path), "/arm-io/%s", name);
        if (adt_path_offset_trace(adt, path, p) < 0) {
            printf("SPTM-virt[s2r]: cp-nodes: no %s\n", path);
            continue;
        }
        for (int r = 0; r < 32; r++) {
            u64 pa, sz;
            if (adt_get_reg(adt, p, "reg", r, &pa, &sz) < 0)
                break;
            if (only >= 0 ? r != only : sz > 0x100000) {
                if (only < 0)
                    printf("SPTM-virt[s2r]: cp-nodes: %s reg%d 0x%lx + 0x%lx too big\n", name, r, pa, sz);
                continue;
            }
            snprintf(what, sizeof(what), "r%d", r);
            u64 base = ALIGN_DOWN(pa, 0x4000);
            s2r_cp_range(s2r_cp_name(name, what), base, ALIGN_UP(pa + sz, 0x4000) - base);
        }
    }
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
        if ((sptm_virt_s2r_cp_skip_ans && !strcmp(name, "ans")) || !s2r_cp_wanted(name))
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
        for (int r = 0; r < 3 && s2r_cp_wanted(spmi[i]); r++)
            s2r_cp_reg(spmi[i], r, "regs", 0, 0x4000);
    /* ANS's storage side: its SART, the NVMe BAR's registers, the AUS5 PHY windows. */
    bool ans = s2r_cp_wanted("ans");
    if (ans && (sptm_virt_s2r_cp_ansx & S2R_CP_ANSX_SART))
        for (int r = 0; r < 3; r++) {
            u64 pa = s2r_cp_reg("sart-ans", r, "regs", 0, 0x4000);
            if (r == 2 && (sptm_virt_s2r_cp_ansx & S2R_CP_ANSX_QUIET))
                s2r_nvme_quiet_page = pa;
        }
    if (ans && (sptm_virt_s2r_cp_ansx & S2R_CP_ANSX_NVME))
        s2r_cp_reg("ans", 9, "nvme", 0, 0x4000);
    if (ans && (sptm_virt_s2r_cp_ansx & S2R_CP_ANSX_AUS5))
        for (int r = 10; r < 14; r++)
            s2r_cp_reg_page("apcie0", r, "aus5");
    if (mem) {
        /* The MCC's own pages, then each AMCC and its DCSs (amcc-reg-idx on). */
        for (int r = 0; r < 64 && s2r_cp_wanted("mcc"); r++)
            if (!s2r_cp_reg("mcc", r, r < 14 ? "mcc" : r < 22 ? "amcc" : "dcs", 0, 0))
                break;
        for (unsigned i = 0; i < ARRAY_SIZE(darts); i++)
            if (s2r_cp_wanted(darts[i]))
                s2r_cp_reg(darts[i], 0, "regs", 0, 0);
    }
    s2r_cp_nodes();
    for (unsigned i = 0; i < ARRAY_SIZE(sptm_virt_s2r_cp_range); i += 2) {
        u64 base = ALIGN_DOWN(sptm_virt_s2r_cp_range[i], 0x4000);
        u64 end = ALIGN_UP(sptm_virt_s2r_cp_range[i] + sptm_virt_s2r_cp_range[i + 1], 0x4000);
        if (sptm_virt_s2r_cp_range[i + 1])
            s2r_cp_range("extra", base, end - base);
    }
    printf("SPTM-virt[s2r]: coprocessor trace: %u pages (SEP mailbox 0x%lx, ESM 0x%lx cut to "
           "headers)\n",
           s2r_n_cp_pages - before, s2r_sep_mbox, s2r_esm_mbox);
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
        sptm_virt_flags &= ~(SPTM_VIRT_S2R | SPTM_VIRT_S2R_GO | SPTM_VIRT_S2R_FULL |
                             SPTM_VIRT_S2R_COPROC | SPTM_VIRT_S2R_DROP_USB);
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
    u64 ring_bytes = s2r_soc->timer_only ? S2R_RING_BYTES_BIG : S2R_RING_BYTES;
    sptm_virt_s2r_ring = memalign(0x4000, ring_bytes);
    if (!sptm_virt_s2r_ring && ring_bytes != S2R_RING_BYTES) {
        ring_bytes = S2R_RING_BYTES;
        sptm_virt_s2r_ring = memalign(0x4000, ring_bytes);
    }
    if (!sptm_virt_s2r_ring) {
        printf("SPTM-virt[s2r]: no memory for the ring; sleep path off\n");
        sptm_virt_flags &= ~SPTM_VIRT_S2R;
        if (cdc && !s2r_soc->timer_only)
            s2r_keep_setup();
        return;
    }
    memset(sptm_virt_s2r_ring, 0, ring_bytes);
    sptm_virt_s2r_nrec = ring_bytes / sizeof(struct s2r_rec);
    sptm_virt_s2r_head = 0;
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
    s2r_usb_setup();

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
        if (s2r_soc->nub_wake_status) {
            printf("SPTM-virt[s2r]: AON wake-source status 0x%lx..+0x1c:",
                   s2r_soc->nub_wake_status);
            for (int i = 0; i < S2R_NUB_WAKE_WORDS; i++)
                printf(" %08x", read32(s2r_soc->nub_wake_status + 4 * i));
            printf("\n");
        }
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
        if (!(sptm_virt_s2r_ring[s2r_wakerec_rp % sptm_virt_s2r_nrec].meta & S2R_MARKER))
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
