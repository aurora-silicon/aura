/* SPDX-License-Identifier: MIT */

/*
 * The trace of macOS's suspend to RAM under the virtualized SPTM.
 *
 * aura's debug link (DockChannel, and the ATC0 USB port the CDC proxy uses) is hidden from XNU,
 * but XNU's PMGR driver still lowers the link's power domains: a hook on those PS words keeps them
 * on while macOS runs. With the sleep path enabled (SPTM_VIRT_S2R), pass-through stage-2 hooks on
 * the power-management blocks record every access into a ring in aura's heap, and two of them
 * mark the sleep: each core disabling itself in pmgr-soc-misc, and the boot core starting the S2R
 * timer in pmgr-aon-pst, the commit. aura stops in the proxy before the commit, with the whole
 * entry sequence in the ring.
 *
 * MMIO addresses are T8140's, as is everything in sptm_virt*.c.
 */

#include "sptm_virt.h"
#include "adt.h"
#include "arm_cpu_regs.h"
#include "exception.h"
#include "hv.h"
#include "malloc.h"
#include "nv2.h"
#include "pmgr.h"
#include "smp.h"
#include "sptm_virt_priv.h"
#include "string.h"
#include "types.h"
#include "uartproxy.h"
#include "utils.h"

#define S2R_RING_BYTES (8UL << 20)

/* The sleep rules start a while after setup: XNU's PMGR driver writes the same registers while
 * it boots, and a sleep comes minutes after the desktop. */
#define S2R_ARM_AFTER_S 45

/* The S2R timer start in pmgr-aon-pst (ADT pmgr reg 31): the last step of XNU's quiesceHW. */
#define S2R_TIMER_OFF 0x4000

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

static struct {
    u64 page;
    const char *what;
    bool seen;
} s2r_pages[160];

static bool s2r_coproc;
static u64 s2r_arm_at;
/* Cores with their last WFI trapped (stop mode), parked there, and the core that committed. */
static volatile u32 s2r_twi_mask, s2r_parked_mask;
static volatile int s2r_timer_cpu = -1;

/*
 * The debug link's power domains: the PS words of the ATC0 USB controller, its DART and PHY, and
 * all their ancestors. A write that lowers one is not performed, and reads then report what XNU
 * asked for, so its polls complete.
 */
#define S2R_KEEP_MAX 32
static u64 s2r_keep[S2R_KEEP_MAX];
static u32 s2r_keep_shadow[S2R_KEEP_MAX];
static bool s2r_keep_low[S2R_KEEP_MAX], s2r_keep_told[S2R_KEEP_MAX];
static int s2r_nkeep;
/* With the proxy on CDC the rule holds from setup: XNU lowers three of these domains about a
 * second into its boot. */
static bool s2r_keep_always;
static const char *const s2r_link_paths[] = {"/arm-io/usb-drd", "/arm-io/dart-usb",
                                             "/arm-io/atc-phy0"};

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

/*
 * A core taking part in the sleep: it disabled itself in PMGR, or the boot core armed the
 * clusters off. Its last WFI is trapped (sptm_virt_s2r_wfi).
 */
static void s2r_core_sleeps(void)
{
    int cpu = smp_id();
    if (s2r_twi_mask & BIT(cpu))
        return;
    __atomic_or_fetch(&s2r_twi_mask, BIT(cpu), __ATOMIC_SEQ_CST);
    nv2_hcr_update(HCR_TWI, 0);
    if (s2r_timer_cpu < 0)
        s2r_timer_cpu = cpu;
    s2r_marker("TWI-ARM", cpu);
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
    int k = width == 2 && (s2r_keep_always || s2r_armed()) ? s2r_keep_idx(addr) : -1;

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
    if (write && width <= 3 && *val == 1 && !strcmp(what, "pmgr-aon-pst") &&
        (addr & 0x7fff) == S2R_TIMER_OFF && s2r_armed()) {
        sptm_virt_rg_stop_wd2();
        sptm_virt_s2r_note(ctx, addr, *val, true, width);
        s2r_stop_before_timer(ctx, addr, *val);
    }

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

    if (first)
        printf("SPTM-virt[s2r]: first access to %s page 0x%lx: %s 0x%lx <- 0x%lx pc 0x%lx\n", what,
               addr & ~0x3fffUL, write ? "wr" : "rd", addr, *val, ctx->elr);
    if (!s2r_armed())
        return true;
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
    s2r_coproc = flags & SPTM_VIRT_S2R_COPROC;

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
    s2r_hook_full();

    s2r_keep_setup();

    s2r_arm_at = mrs(CNTPCT_EL0) + S2R_ARM_AFTER_S * mrs(CNTFRQ_EL0);
    s2r_marker("S2RSETUP", sptm_virt_s2r_pages);
    printf("SPTM-virt[s2r]: %u pages hooked, %lu-record ring at 0x%lx; stop mode: stopping "
           "before the S2R timer start\n",
           sptm_virt_s2r_pages, sptm_virt_s2r_nrec, (u64)sptm_virt_s2r_ring);
}
