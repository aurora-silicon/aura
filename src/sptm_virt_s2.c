/* SPDX-License-Identifier: MIT */

/*
 * The stage 2 aura gives the OS at virtual EL2 under the virtualized SPTM: an identity map of the
 * low 2 TiB (all MMIO and DRAM on T8140) with a few pages trapped. uart0 becomes aura's vuart
 * (XNU's console is moved there in the ADT, so its serial stack never binds the DockChannel that
 * carries the proxy), the SoC watchdogs are guarded so XNU cannot reset the Mac, and aura's own
 * memory is unmapped so a stray guest access stops in aura. SPTM's own bring-up runs without it:
 * each core arms it at its GEXIT into XNU.
 */

#include "sptm_virt.h"
#include "adt.h"
#include "cpu_regs.h"
#include "exception.h"
#include "hv.h"
#include "iodev.h"
#include "memory.h"
#include "nv2.h"
#include "smp.h"
#include "sptm_virt_priv.h"
#include "string.h"
#include "uartproxy.h"
#include "utils.h"
#include "xnuboot.h"

static u64 sptm_s2_sum;
static u32 sptm_s2_tables;
static bool sptm_s2_ready;

/* A guest MMIO access, performed for real at its width. */
void sptm_virt_mmio_rw(u64 addr, u64 *val, bool write, int width)
{
    if (write) {
        switch (width) {
            case 3:
                write64(addr, *val);
                break;
            case 2:
                write32(addr, *val);
                break;
            case 1:
                write16(addr, *val);
                break;
            default:
                write8(addr, *val);
                break;
        }
    } else {
        switch (width) {
            case 3:
                *val = read64(addr);
                break;
            case 2:
                *val = read32(addr);
                break;
            case 1:
                *val = read16(addr);
                break;
            default:
                *val = read8(addr);
                break;
        }
    }
}

/*
 * CTRR-class range registers refuse a block translation whose output overlaps their range: the
 * walk takes an address-size fault although the block descriptor is valid. On T8140 the firmware's
 * secondary-RO range and the CTRR A/B ranges do this, so every block over one is mapped with L3
 * pages.
 */
static void sptm_s2_split_range(const char *what, u64 lo, u64 hi)
{
    const u64 blk = 1UL << 25; /* 32 MiB L2 block at 16K granule */

    if (hi <= lo)
        return;
    for (u64 b = lo & ~(blk - 1); b < hi; b += blk)
        hv_pt_split_block(b);
    printf("SPTM-virt[s2]: %s 0x%lx..0x%lx: blocks 0x%lx..0x%lx mapped with L3 pages\n", what, lo,
           hi, lo & ~(blk - 1), (hi + blk - 1) & ~(blk - 1));
}

static void sptm_virt_s2_split_ctrr_blocks(void)
{
    u64 lower = SAFE_MRS("s3_0_c11_c0_0"), upper = SAFE_MRS("s3_0_c11_c0_1");
    if (lower != SAFE_MRS_FAILED && upper != SAFE_MRS_FAILED && upper >= lower && lower)
        sptm_s2_split_range("M4 secondary-RO", lower, upper + 0x1000);
    /* CTRR A/B, EL2 and EL1 copies; the upper bound includes its last page */
    const struct {
        const char *name;
        u64 lo, hi;
    } r[] = {
        {"CTRR A (EL2)", SAFE_MRS("s3_4_c15_c11_0"), SAFE_MRS("s3_4_c15_c11_1")},
        {"CTRR B (EL2)", SAFE_MRS("s3_4_c15_c11_2"), SAFE_MRS("s3_4_c15_c11_3")},
        {"CTRR A (EL1)", SAFE_MRS("s3_4_c15_c9_0"), SAFE_MRS("s3_4_c15_c9_1")},
        {"CTRR B (EL1)", SAFE_MRS("s3_4_c15_c9_2"), SAFE_MRS("s3_4_c15_c9_3")},
    };
    for (unsigned i = 0; i < ARRAY_SIZE(r); i++)
        if (r[i].lo && r[i].lo != SAFE_MRS_FAILED && r[i].hi != SAFE_MRS_FAILED &&
            r[i].hi >= r[i].lo)
            sptm_s2_split_range(r[i].name, r[i].lo, r[i].hi + 0x4000);
}

/* ---- reset guard: XNU can never reset the Mac --------------------------------------------
 * Every SoC watchdog page is trapped. Reads pass through. A write that would let a watchdog reset
 * the SoC is neutered (reset enable stripped, the secondary watchdog kept at 0, the node's other
 * registers never written) and logged. A restart (reset enabled with an alarm under ~1 s) parks
 * the core in aura's proxy instead, so a reset becomes a hypervisor event.
 */
#define RG_MAX         8
#define WDT_ALARM      0x14
#define WDT_CTL        0x1c
#define WDT_CTL_RESET  BIT(2)
#define WDT_RESTART_TK 24000000 /* an alarm under ~1 s at 24 MHz is a restart */

static struct {
    u64 base;
    const char *what;
} rg_pages[RG_MAX];
static int rg_n;
static u64 rg_wdt, rg_wdt2, rg_xwdt; /* primary, secondary register, exclave watchdog */
static u64 rg_extra[4];              /* the wdt node's other single registers */
static u32 rg_alarm_shadow[2];
static u32 rg_events;

static void sptm_virt_rg_report(struct exc_info *ctx, const char *what, u64 addr, u64 val,
                                bool park)
{
    printf("SPTM-virt[rst c%d]: %s: XNU write 0x%lx -> [0x%lx] at pc=0x%lx lr=0x%lx%s\n", smp_id(),
           what, val, addr, ctx->elr, ctx->regs[30],
           park ? " -- RESET REQUEST BLOCKED, parking this CPU" : " (blocked, continuing)");
    if (!park)
        return;
    printf("SPTM-virt[rst]: x0=0x%lx x1=0x%lx x2=0x%lx x3=0x%lx x19=0x%lx x20=0x%lx x21=0x%lx "
           "x29=0x%lx spsr=0x%lx\n",
           ctx->regs[0], ctx->regs[1], ctx->regs[2], ctx->regs[3], ctx->regs[19], ctx->regs[20],
           ctx->regs[21], ctx->regs[29], ctx->spsr);
    printf("SPTM-virt[rst]: RESET TRAPPED (%s) on cpu%d at XNU pc 0x%lx: the Mac stays up; XNU's "
           "panic text is in the console above. aura is entering the proxy.\n",
           what, smp_id(), ctx->elr);
    iodev_console_flush();
    struct uartproxy_msg_start start = {
        .reason = START_HV,
        .code = 0x52535421, /* 'RST!' */
        .info = ctx,
    };
    uartproxy_run(&start);
    while (1)
        sysop("wfe");
}

static bool sptm_virt_rg_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    u64 a = addr & ~3UL;

    if (!write) {
        sptm_virt_mmio_rw(addr, val, false, width);
        if (width == 2 && (a == rg_wdt + WDT_ALARM || a == rg_xwdt + WDT_ALARM))
            *val = rg_alarm_shadow[a == rg_xwdt + WDT_ALARM];
        return true;
    }
    u64 v = *val;
    bool log = rg_events++ < 64;
    for (int w = 0; w < 2; w++) {
        u64 base = w ? rg_xwdt : rg_wdt;
        if (!base || a < base || a >= base + 0x4000)
            continue;
        if (a == base + WDT_ALARM && width == 2) { /* keep XNU's value, never arm short */
            rg_alarm_shadow[w] = v;
            if (log)
                printf("SPTM-virt[rst]: %s ALARM <- 0x%lx (pc 0x%lx)\n", w ? "xwdt" : "wdt", v,
                       ctx->elr);
            return true;
        }
        if (a == base + WDT_CTL) {
            if (v & WDT_CTL_RESET)
                sptm_virt_rg_report(ctx, w ? "exclave watchdog CTL" : "SoC watchdog CTL", addr, v,
                                    rg_alarm_shadow[w] < WDT_RESTART_TK);
            write32(a, v & ~WDT_CTL_RESET);
            return true;
        }
        if (log)
            printf("SPTM-virt[rst]: %s +0x%lx <- 0x%lx (pc 0x%lx, passed)\n", w ? "xwdt" : "wdt",
                   a - base, v, ctx->elr);
        if (width == 3)
            write64(addr, v);
        else
            write32(a, v);
        return true;
    }
    if (rg_wdt2 && a == rg_wdt2) {
        if (v)
            sptm_virt_rg_report(ctx, "secondary watchdog", addr, v, true);
        write32(a, 0);
        return true;
    }
    for (int i = 0; i < 4; i++) {
        if (rg_extra[i] && a == rg_extra[i]) {
            /* XNU writes these only on its way down: stop at the first attempt. */
            sptm_virt_rg_report(ctx, "watchdog-node register", addr, v, v != 0);
            return true;
        }
    }
    if (log)
        printf("SPTM-virt[rst]: other write 0x%lx -> [0x%lx] (pc 0x%lx, passed)\n", v, addr,
               ctx->elr);
    sptm_virt_mmio_rw(addr, val, true, width);
    return true;
}

bool sptm_virt_rg_owns(u64 page)
{
    for (int i = 0; i < rg_n; i++)
        if (rg_pages[i].base == page)
            return true;
    return false;
}

static void sptm_virt_rg_page(u64 pa, const char *what)
{
    u64 page = pa & ~0x3fffUL;

    if (sptm_virt_rg_owns(page) || rg_n >= RG_MAX)
        return;
    rg_pages[rg_n].base = page;
    rg_pages[rg_n].what = what;
    rg_n++;
    hv_map_hook(page, sptm_virt_rg_hook, 0x4000);
}

static void sptm_virt_reset_guard_setup(void)
{
    int path[8];
    u64 pa, sz;
    int node = adt_path_offset_trace(adt, "/arm-io/wdt", path);

    if (node >= 0) {
        for (int i = 0; i < 5; i++) {
            if (adt_get_reg(adt, path, "reg", i, &pa, &sz) < 0)
                break;
            if (i == 0)
                rg_wdt = pa;
            else if (i == 1)
                rg_extra[0] = pa;
            else if (i == 2)
                rg_wdt2 = pa;
            else
                rg_extra[i - 2] = pa;
            sptm_virt_rg_page(pa, "wdt");
        }
        rg_alarm_shadow[0] = rg_wdt ? read32(rg_wdt + WDT_ALARM) : 0;
    }
    node = adt_path_offset_trace(adt, "/arm-io/hib-exclave-watchdog", path);
    if (node >= 0 && adt_get_reg(adt, path, "reg", 0, &pa, &sz) >= 0) {
        rg_xwdt = pa;
        rg_alarm_shadow[1] = read32(rg_xwdt + WDT_ALARM);
        sptm_virt_rg_page(pa, "hib-exclave-watchdog");
    }
    printf("SPTM-virt[rst]: reset guard on %d pages (wdt 0x%lx, 2nd 0x%lx, "
           "extra 0x%lx/0x%lx/0x%lx, exclave wdt 0x%lx); XNU cannot reset the SoC through them\n",
           rg_n, rg_wdt, rg_wdt2, rg_extra[0], rg_extra[1], rg_extra[2], rg_xwdt);
}

/* ---- aura's own memory is never the guest's ----------------------------------------------
 * The guest's stage 2 identity-maps all RAM, including aura's image (at stage 1's base, inside
 * SPTM's window as an SPTM-reserved region) and its carve-out. Unmap both, so a guest access stops
 * here and names who made it. A writer this does not catch is a DMA master, not a CPU.
 */
static u64 sptm_guard_lo[2], sptm_guard_hi[2];

static void sptm_virt_aura_guard_setup(void)
{
    struct sptm_virt_layout l;

    sptm_guard_lo[0] = ALIGN_DOWN((u64)_base, SZ_16K);
    sptm_guard_hi[0] = ALIGN_UP((u64)_end, SZ_16K);
    if (sptm_virt_layout(&l) == 0) {
        sptm_guard_lo[1] = l.aura_base;
        sptm_guard_hi[1] = l.aura_top;
    }
    for (int i = 0; i < 2; i++) {
        if (sptm_guard_hi[i] <= sptm_guard_lo[i])
            continue;
        int rc = hv_unmap(sptm_guard_lo[i], sptm_guard_hi[i] - sptm_guard_lo[i]);
        printf("SPTM-virt[guard]: aura %s [0x%lx, 0x%lx) unmapped from the guest%s\n",
               i ? "carve-out" : "image", sptm_guard_lo[i], sptm_guard_hi[i],
               rc ? " -- FAILED" : "");
    }
}

static void sptm_virt_fill_ctx(struct exc_info *ctx, u64 *regs, bool gl)
{
    memset(ctx, 0, sizeof(*ctx));
    memcpy(ctx->regs, regs, 31 * sizeof(u64));
    ctx->spsr = gl ? mrs(SYS_IMP_APL_SPSR_GL1) : mrs(SPSR_EL2);
    ctx->elr = gl ? mrs(SYS_IMP_APL_ELR_GL1) : mrs(ELR_EL2);
    ctx->esr = gl ? mrs(SYS_IMP_APL_ESR_GL1) : mrs(ESR_EL2);
    ctx->far = gl ? mrs(SYS_IMP_APL_FAR_GL1) : mrs(FAR_EL2);
    if (!gl)
        ctx->afsr1 = mrs(AFSR1_EL2);
    ctx->cpu_id = smp_id();
    ctx->mpidr = mrs(MPIDR_EL1);
}

/* A stage-2 abort on aura's own memory: report it with the guest's frame chain and stop in the
 * proxy. False for any other abort. */
static bool sptm_virt_aura_guard_fault(u64 *regs, bool gl)
{
    u64 far = gl ? mrs(SYS_IMP_APL_FAR_GL1) : mrs(FAR_EL2);
    u64 ipa = ((mrs(HPFAR_EL2) & GENMASK(43, 4)) << 8) | (far & 0x3fff);
    int hit = -1;

    for (int i = 0; i < 2; i++)
        if (ipa >= sptm_guard_lo[i] && ipa < sptm_guard_hi[i])
            hit = i;
    if (hit < 0)
        return false;
    struct exc_info ctx;
    sptm_virt_fill_ctx(&ctx, regs, gl);
    printf("SPTM-virt[guard c%d]: GUEST %s aura's %s at ipa 0x%lx (+0x%lx): pc=0x%lx far=0x%lx "
           "esr=0x%lx lr=0x%lx spsr=0x%lx%s\n",
           smp_id(), (ctx.esr & BIT(6)) ? "WROTE" : "READ", hit ? "carve-out" : "image", ipa,
           ipa - sptm_guard_lo[hit], ctx.elr, far, ctx.esr, regs[30], ctx.spsr,
           gl ? " (from SPTM, GL1)" : "");
    printf("SPTM-virt[guard]: x0=0x%lx x1=0x%lx x2=0x%lx x3=0x%lx x8=0x%lx x19=0x%lx x20=0x%lx "
           "x21=0x%lx x29=0x%lx\n",
           regs[0], regs[1], regs[2], regs[3], regs[8], regs[19], regs[20], regs[21], regs[29]);
    u64 fp = regs[29];
    for (int i = 0; i < 16 && fp; i++) {
        u64 nfp, lr;
        if (!sptm_virt_el1_va_read64(fp, &nfp) || !sptm_virt_el1_va_read64(fp + 8, &lr))
            break;
        printf("SPTM-virt[guard]:   frame %d lr 0x%lx\n", i, lr | 0xffff000000000000UL);
        if (nfp <= fp)
            break;
        fp = nfp;
    }
    iodev_console_flush();
    struct uartproxy_msg_start start = {
        .reason = START_HV,
        .code = 0x41555221, /* 'AUR!' */
        .info = &ctx,
    };
    uartproxy_run(&start);
    while (1)
        sysop("wfe");
}

void sptm_virt_s2_setup(void)
{
    hv_pt_init(); /* programs this CPU's VTCR/VTTBR */
    if (hv_map_hw(0, 0, 1UL << 41)) {
        printf("SPTM-virt[s2]: identity map failed; stage 2 off\n");
        return;
    }
    int path[8];
    int node = adt_path_offset_trace(adt, "/arm-io/uart0", path);
    u64 base = 0;
    u32 len = 0;
    const u32 *irqs = node >= 0 ? adt_getprop(adt, node, "interrupts", &len) : NULL;
    if (node < 0 || adt_get_reg(adt, path, "reg", 0, &base, NULL) < 0 || !irqs || len < 4) {
        printf("SPTM-virt[s2]: no /arm-io/uart0; stage 2 off\n");
        return;
    }
    hv_map_vuart(base, irqs[0], uartproxy_iodev);
    sptm_virt_aura_guard_setup();
    sptm_virt_s2_split_ctrr_blocks();
    sptm_virt_reset_guard_setup();
    /* After the guard, whose pages the sleep path leaves alone. */
    sptm_virt_s2r_setup(uartproxy_iodev >= IODEV_USB0 && uartproxy_iodev < IODEV_MAX);

    /* SPTM's own bring-up runs without it; each core arms it at its GEXIT into XNU, after
     * checking that nothing changed the tree in between. */
    u64 vtcr = mrs(VTCR_EL2), vttbr = mrs(VTTBR_EL2);
    sptm_s2_sum = hv_pt_checksum(&sptm_s2_tables);
    sptm_s2_ready = true;
    nv2_set_l1_stage2_deferred(vtcr, vttbr);
    msr(VTTBR_EL2, 0);
    printf("SPTM-virt[s2]: stage 2 deferred to each CPU's XNU entry: identity [0, 2 TiB), vuart at "
           "0x%lx irq %u -> iodev %d, %u tables at 0x%lx sum 0x%lx\n",
           base, irqs[0], uartproxy_iodev, sptm_s2_tables, vttbr, sptm_s2_sum);
}

void sptm_virt_s2_enter_xnu(int cpu)
{
    static u32 checks;

    if (!sptm_s2_ready)
        return;
    bool verbose = checks++ < 8;
    u32 nt = 0;
    u64 sum = hv_pt_checksum(&nt);
    bool ok = sum == sptm_s2_sum && nt == sptm_s2_tables;
    if (verbose || !ok)
        printf("SPTM-virt[s2]: cpu%d XNU entry: stage-2 tree %s (%u tables, 0x%lx vs 0x%lx)%s\n",
               cpu, ok ? "intact" : "CHANGED", nt, sum, sptm_s2_sum,
               ok ? "; arming it" : "; NOT arming");
    if (ok)
        nv2_l1_stage2_enable_here();
}

/* A stage-2 abort from SPTM at GL1 (its runtime calls run with the stage 2 armed): the GL1 banks
 * hold the faulting state; resume through the mirrored ELR_EL1. */
bool sptm_virt_s2_dabort_gl(u64 *regs)
{
    static u32 n;
    struct exc_info ctx;

    if (sptm_virt_aura_guard_fault(regs, true))
        return true;
    sptm_virt_fill_ctx(&ctx, regs, true);
    if (n++ < 8)
        printf("SPTM-virt[s2]: stage-2 abort from SPTM (GL1) pc=0x%lx far=0x%lx esr=0x%lx\n",
               ctx.elr, ctx.far, ctx.esr);
    if (!hv_handle_dabort(&ctx))
        return false;
    memcpy(regs, ctx.regs, 31 * sizeof(u64));
    msr(ELR_EL1, ctx.elr + 4);
    return true;
}

/* A stage-2 abort from the OS at virtual EL2: a trapped page, emulated by the HV's MMIO code. */
bool sptm_virt_s2_dabort(u64 *regs)
{
    struct exc_info ctx;

    if (sptm_virt_aura_guard_fault(regs, false))
        return true;
    sptm_virt_fill_ctx(&ctx, regs, false);
    if (hv_handle_dabort(&ctx)) {
        memcpy(regs, ctx.regs, 31 * sizeof(u64));
        msr(ELR_EL2, ctx.elr + 4);
        return true;
    }

    /*
     * XNU touches iBoot's reserved low RAM, [DRAM base, physBase): a boot handoff, SEP and USB
     * regions. The identity map's entry there faults; map the page identity-RW and retry.
     */
    u64 dram_base = ALIGN_DOWN(cur_boot_args.phys_base, BIT(32));
    u64 par = 0;
    u64 ipa = hv_translate(ctx.far, true, false, &par);
    if (ipa >= dram_base && ipa < cur_boot_args.phys_base) {
        static u32 mapped;
        if (mapped++ < 16)
            printf("SPTM-virt[s2]: mapping iBoot low page identity-RW: ipa=0x%lx far=0x%lx "
                   "esr=0x%lx\n",
                   ipa, ctx.far, ctx.esr);
        hv_map_hw(ipa & ~0x3fffUL, ipa & ~0x3fffUL, 0x4000);
        __asm__ volatile("tlbi vmalls12e1" ::: "memory");
        sysop("dsb ish");
        sysop("isb");
        return true;
    }

    /* A stale stage-2 TLB entry: invalidate and retry once per page; a repeat is reported. */
    static u64 retried[32];
    static u32 nretried;
    u64 pg = ipa & ~0x3fffUL;
    bool seen = false;
    for (u32 i = 0; i < nretried; i++)
        seen |= retried[i] == pg;
    if (!seen && nretried < ARRAY_SIZE(retried)) {
        retried[nretried++] = pg;
        __asm__ volatile("tlbi vmalls12e1is" ::: "memory");
        sysop("dsb ish");
        sysop("isb");
        return true;
    }

    static u32 fails;
    if (fails++ < 8) {
        u64 hpfar_ipa = (mrs(HPFAR_EL2) & GENMASK(43, 4)) << 8 | (ctx.far & 0x3fff);
        u32 nt = 0;
        u64 sum = hv_pt_checksum(&nt);
        printf("SPTM-virt[s2]: unhandled stage-2 abort pc=0x%lx far=0x%lx esr=0x%lx ipa(s1)=0x%lx "
               "ipa(hpfar)=0x%lx s2desc=0x%lx tree %u tables sum 0x%lx (%s)\n",
               ctx.elr, ctx.far, ctx.esr, ipa, hpfar_ipa, hv_pt_walk(hpfar_ipa), nt, sum,
               sum == sptm_s2_sum && nt == sptm_s2_tables ? "intact" : "CHANGED");
    }
    return false;
}
