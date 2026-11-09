/* SPDX-License-Identifier: MIT */

/*
 * Virtualized SPTM: the carve-out, the host's loader entry points, the guest entry and the reset
 * path every other core takes into the guest.
 */

#include "sptm_virt.h"
#include "adt.h"
#include "cpu_regs.h"
#include "dockchannel_uart.h"
#include "exception.h"
#include "fb.h"
#include "gxf.h"
#include "heapblock.h"
#include "iodev.h"
#include "memory.h"
#include "nv2.h"
#include "smp.h"
#include "soc.h"
#include "sptm_virt_priv.h"
#include "string.h"
#include "uartproxy.h"
#include "usb.h"
#include "utils.h"
#include "xnuboot.h"

/*
 * aura's top-of-RAM carve-out: its heap (per-CPU NV2 and GL pages, stage-2 tables, the trace ring)
 * and the host's proxy staging heap, which starts 128 MiB above the heap base and runs for about
 * as much again.
 */
#define SPTM_VIRT_AURA_HEAP (512UL << 20)
#define SPTM_VIRT_ALIGN     (2UL << 20) /* an L2 block */

volatile bool sptm_virt_guest_live;
volatile bool sptm_virt_xnu_live;
volatile u64 sptm_virt_bss_canary;
u64 sptm_virt_flags;
u32 sptm_virt_cpu_entries[MAX_CPUS];

static u64 sptm_entry_pa, sptm_handoff_pa;
static bool sptm_images_loaded;
static u64 sptm_reset_pa;
static u32 sptm_cpu_resets[MAX_CPUS];
/* False while a core runs SPTM's own (re)entry path, true from its first GEXIT into XNU. */
static volatile bool sptm_cpu_in_xnu[MAX_CPUS];

bool sptm_virt_requested(void)
{
    /* What the hypervisor needs from the CPU; the host refuses an SPTM it can't patch. */
    return FIELD_GET(ID_AA64MMFR2_NV, mrs(SYS_ID_AA64MMFR2_EL1)) >= 2 && supports_gxf();
}

/* FEAT_MTE2: the SoC has allocation-tag storage. */
bool sptm_virt_mte(void)
{
    return FIELD_GET(ID_AA64PFR1_MTE, mrs(SYS_ID_AA64PFR1_EL1)) >= 2;
}

/*
 * With MTE, iBoot puts the tag storage at the top of the managed RAM it would hand SPTM, and SPTM
 * checks it there, so SPTM's window keeps iBoot's end and aura's carve-out comes off its bottom.
 */
static bool sptm_virt_carveout_low(void)
{
    return sptm_virt_mte();
}

int sptm_virt_compute_layout(u64 phys_base, u64 mem_size, u64 ram_base, u64 ram_top, u64 fw_high,
                             u64 aura_size, struct sptm_virt_layout *out)
{
    if (!out)
        return -1;
    memset(out, 0, sizeof(*out));

    if (!mem_size || !aura_size)
        return -1;
    if (ram_top <= ram_base || phys_base < ram_base || phys_base >= ram_top)
        return -1;
    if (mem_size > ram_top - phys_base)
        return -1;

    /* Managed RAM must end before the earliest fixed high-firmware region, or SPTM rejects it. */
    u64 managed_limit = phys_base + mem_size;
    if (fw_high) {
        if (fw_high <= phys_base)
            return -1;
        if (fw_high < managed_limit)
            managed_limit = fw_high;
    }
    managed_limit = ALIGN_DOWN(managed_limit, SPTM_VIRT_ALIGN);
    if (managed_limit <= phys_base)
        return -1;

    /* aura comes off the top, and SPTM is told its window ends below it. */
    aura_size = ALIGN_UP(aura_size, SPTM_VIRT_ALIGN);
    if (aura_size >= managed_limit - phys_base)
        return -1; /* must leave managed RAM for SPTM/XNU below it */
    u64 aura_base = managed_limit - aura_size;
    if (aura_base <= phys_base)
        return -1;

    out->ram_base = ram_base;
    out->ram_top = ram_top;
    out->managed_base = phys_base;
    out->managed_end = aura_base;
    out->aura_base = aura_base;
    out->aura_top = managed_limit;
    out->fw_high = fw_high;
    out->aura_size = aura_size;
    return 0;
}

static u64 sptm_virt_adt_u64(int node, const char *prop)
{
    if (node < 0)
        return 0;
    u32 len = 0;
    const void *p = adt_getprop(adt, node, prop, &len);
    if (!p || len < sizeof(u64))
        return 0;
    u64 v;
    memcpy(&v, p, sizeof(v));
    return v;
}

/* The lowest fixed high-firmware PA SPTM requires outside managed RAM, 0 if none. */
static u64 sptm_virt_fw_high(void)
{
    int sgx = adt_path_offset(adt, "/arm-io/sgx");
    int sapt = adt_path_offset(adt, "/arm-io/sapt");
    const struct {
        int node;
        const char *prop;
    } probes[] = {
        {sapt, "table-address"},         {sgx, "gfx-handoff-base"},
        {sgx, "gfx-shared-region-base"}, {sgx, "gfx-shared-l2-region-base"},
        {sgx, "gpu-region-base"},
    };
    u64 lo = 0;
    for (size_t i = 0; i < ARRAY_SIZE(probes); i++) {
        u64 v = sptm_virt_adt_u64(probes[i].node, probes[i].prop);
        if (v && (!lo || v < lo))
            lo = v;
    }
    return lo;
}

/*
 * Lowest PA SPTM's window may start at: above aura's image, its boot-time heap, iBoot's boot data
 * and the firmware's read-only range, all of which sit at the bottom of what iBoot calls managed
 * RAM. Candidates at or above limit (the carve-out) are ignored.
 */
static u64 sptm_virt_low_floor(u64 limit)
{
    u64 ro_start = 0, ro_end = 0;
    u64 floor = cur_boot_args.phys_base;
    /* A low carve-out holds the heap itself, so only what lies below it counts. */
    bool heap = !sptm_virt_carveout_low();
    const u64 cand[] = {
        (u64)_end,
        cur_boot_args.top_of_kernel_data,
        heap ? (u64)heapblock_high_water() : 0,
        heap ? (u64)heapblock_limit() : 0,
        memory_fw_ro_range(&ro_start, &ro_end) > 0 ? ro_end : 0,
    };

    for (size_t i = 0; i < ARRAY_SIZE(cand); i++)
        if (cand[i] < limit && cand[i] > floor)
            floor = cand[i];
    return ALIGN_UP(floor, SZ_32M); /* SPTM maps its window in 32 MiB blocks */
}

/*
 * The layout with MTE. Two constraints decide it:
 *  - SPTM's window must end where iBoot's does, because the MTE tag storage is its top 512 MiB and
 *    SPTM derives the tag base from the window's end.
 *  - its physBase must lie in the first 512 MiB of DRAM; SPTM checks that (t8110dart's DAPF
 *    bootstrap: physBase rounded down to 512 MiB must be the DRAM base).
 * So aura's carve-out goes between what aura and iBoot left low and a cap inside that first
 * 512 MiB, and SPTM manages everything above it.
 */
#define SPTM_VIRT_LOW_CAP  0x1e000000UL /* 480 MiB: physBase stays under the 512 MiB line */
#define SPTM_VIRT_LOW_HEAP (192UL << 20)

static int sptm_virt_layout_low(u64 ram_base, struct sptm_virt_layout *out)
{
    u64 end = cur_boot_args.phys_base + cur_boot_args.mem_size;
    u64 fw_high = sptm_virt_fw_high();

    memset(out, 0, sizeof(*out));
    if (fw_high && fw_high < end)
        return -1; /* the tag storage would no longer be at the window's top */
    out->ram_base = ram_base;
    out->ram_top = ram_base + mem_size_actual;
    out->fw_high = fw_high;
    out->aura_base = ALIGN_UP(sptm_virt_low_floor(end), SPTM_VIRT_ALIGN);
    out->aura_top = ram_base + SPTM_VIRT_LOW_CAP;
    out->aura_size = out->aura_top - out->aura_base;
    out->managed_base = out->aura_top;
    out->managed_end = end;
    if (out->aura_top <= out->aura_base || out->aura_size < SPTM_VIRT_LOW_HEAP)
        return -1; /* aura and iBoot left too little room below the cap */
    if (out->managed_base + (2UL << 30) >= end)
        return -1;
    return 0;
}

int sptm_virt_layout(struct sptm_virt_layout *out)
{
    u64 ram_base = ALIGN_DOWN(cur_boot_args.phys_base, BIT(32));
    if (sptm_virt_carveout_low())
        return sptm_virt_layout_low(ram_base, out);
    int rc = sptm_virt_compute_layout(cur_boot_args.phys_base, cur_boot_args.mem_size, ram_base,
                                      ram_base + mem_size_actual, sptm_virt_fw_high(),
                                      SPTM_VIRT_AURA_HEAP, out);
    if (rc < 0)
        return rc;
    u64 floor = sptm_virt_low_floor(out->managed_end);
    if (floor > out->managed_base) {
        if (floor + (64UL << 20) >= out->managed_end)
            return -1; /* nothing sensible left to manage */
        out->managed_base = floor;
    }
    return 0;
}

/* heapblock_init: aura's heap lives in the carve-out, never in SPTM's window. */
bool sptm_virt_heap_window(u64 *base, u64 *limit)
{
    struct sptm_virt_layout l;

    if (!sptm_virt_requested() || sptm_virt_layout(&l) < 0 || l.aura_top <= l.aura_base)
        return false;
    *base = l.aura_base;
    *limit = l.aura_top;
    return true;
}

__attribute__((used, retain)) int sptm_virt_place(struct sptm_virt_placement *out)
{
    struct sptm_virt_layout l;

    if (!out)
        return -1;
    memset(out, 0, sizeof(*out));
    if (sptm_virt_layout(&l) < 0 || sptm_virt_gl_alloc() < 0)
        return -1;
    out->managed_base = l.managed_base;
    out->managed_end = l.managed_end;
    return 0;
}

/*
 * The handoff SPTM's __start reads from x0 is an XNU boot_args (revision 2, version 2): virtBase
 * +8, physBase +0x10, memSize +0x18, topOfKernelData +0x20. The host builds it
 * (proxyclient/aura/sptm_virt_plan.py); this only refuses one that is not a boot_args or that would
 * let SPTM manage aura's memory.
 */
struct sptm_virt_boot_args {
    u16 revision, version;
    u32 pad;
    u64 virt_base, phys_base, mem_size, top_of_kernel_data;
};

static int sptm_virt_check_handoff(const struct sptm_virt_layout *l, u64 handoff)
{
    const volatile struct sptm_virt_boot_args *ba = (const void *)handoff;

    if (handoff & (SZ_16K - 1)) {
        printf("SPTM-virt: load refused, handoff 0x%lx is not 16 KiB aligned\n", handoff);
        return -1;
    }
    dc_civac_range((void *)handoff, sizeof(*ba));
    u16 rev = ba->revision, ver = ba->version;
    u64 pbase = ba->phys_base, msize = ba->mem_size, top = ba->top_of_kernel_data;
    printf("SPTM-virt: handoff boot_args rev %u ver %u virtBase 0x%lx physBase 0x%lx memSize 0x%lx "
           "top 0x%lx\n",
           rev, ver, (u64)ba->virt_base, pbase, msize, top);
    if ((rev != 2 && rev != 3) || ver != 2) {
        printf("SPTM-virt: load refused, handoff is not a revision-2/3 version-2 boot_args\n");
        return -1;
    }
    if (pbase < l->managed_base || pbase >= l->managed_end || msize > l->managed_end - pbase ||
        top > l->managed_end) {
        printf("SPTM-virt: load refused, boot_args managed window [0x%lx, 0x%lx) is not inside "
               "SPTM's window [0x%lx, 0x%lx)\n",
               pbase, pbase + msize, l->managed_base, l->managed_end);
        return -1;
    }
    u64 wend = pbase + msize;
    u64 ranges[][2] = {{(u64)_base, (u64)_end},
                       {(u64)heapblock_high_water(), (u64)heapblock_limit()}};
    for (size_t i = 0; i < ARRAY_SIZE(ranges); i++) {
        if (ranges[i][1] > ranges[i][0] && ranges[i][0] < wend && ranges[i][1] > pbase) {
            printf("SPTM-virt: load refused, SPTM's window [0x%lx, 0x%lx) overlaps aura [0x%lx, "
                   "0x%lx)\n",
                   pbase, wend, ranges[i][0], ranges[i][1]);
            return -1;
        }
    }
    return 0;
}

/* The guest starts with its MMU off, so whatever the host wrote must reach the point of coherency.
 */
__attribute__((used, retain)) void sptm_virt_flush(u64 pa, u64 size)
{
    dc_civac_range((void *)pa, size);
    sysop("dsb sy");
}

__attribute__((used, retain)) int sptm_virt_load(u64 sptm_entry, u64 handoff)
{
    struct sptm_virt_layout l;

    if (sptm_virt_layout(&l) < 0) {
        printf("SPTM-virt: load refused, no valid carve-out layout\n");
        return -1;
    }
    if (sptm_virt_gl_alloc() < 0)
        return -1;
    if (sptm_entry < l.managed_base || sptm_entry >= l.managed_end) {
        printf("SPTM-virt: load refused, entry 0x%lx outside managed RAM [0x%lx, 0x%lx)\n",
               sptm_entry, l.managed_base, l.managed_end);
        return -1;
    }
    if (handoff < l.managed_base || handoff >= l.managed_end) {
        printf("SPTM-virt: load refused, handoff 0x%lx outside managed RAM\n", handoff);
        return -1;
    }
    if (sptm_virt_check_handoff(&l, handoff) < 0)
        return -1;
    sptm_entry_pa = sptm_entry;
    sptm_handoff_pa = handoff;
    sptm_images_loaded = true;
    printf("SPTM-virt: images recorded, entry 0x%lx handoff 0x%lx; ready to enter\n", sptm_entry,
           handoff);
    return 0;
}

/*
 * The phandle of the USB PHY whose port aura's proxy runs on, 0 on any other transport (KIS):
 * the host hides that port from the guest.
 */
__attribute__((used, retain)) u32 sptm_virt_proxy_phy(void)
{
    u32 phy = 0;

    if (uartproxy_iodev < IODEV_USB0 || uartproxy_iodev >= IODEV_MAX)
        return 0;
    u32 idx = uartproxy_iodev - IODEV_USB0;
    char path[32];
    snprintf(path, sizeof(path), "/arm-io/usb-drd%u", idx);
    int node = adt_path_offset(adt, path);
    if (node < 0 && idx == 0)
        node = adt_path_offset(adt, "/arm-io/usb-drd");
    if (node < 0 || ADT_GETPROP(adt, node, "atc-phy-parent", &phy) < 0)
        return 0;
    return phy;
}

__attribute__((used, retain)) void sptm_virt_config(u64 flags)
{
    sptm_virt_flags = flags;
    printf("SPTM-virt: flags 0x%lx\n", flags);
}

/* ---- the pre-XNU sampling tick --------------------------------------------------------------
 * Until a core's first GEXIT into XNU there is nothing to see if the guest spins without trapping:
 * SPTM runs with its own FIQs masked and aura gets no tick. With SPTM_VIRT_SAMPLE, physical FIQ is
 * routed to aura (HCR_FMO) while a core is pre-XNU and aura's own timer samples the guest's PC;
 * EL1's mask does not apply to an interrupt routed to EL2, and SPTM has no FIQ of its own here.
 * The routing is dropped at the core's XNU entry, so XNU keeps its interrupts.
 */
u32 sptm_virt_sample_hz = 4;
u32 sptm_virt_sample_max = 64;
static u32 sptm_sample_prints;

static void sptm_virt_sample_arm_here(void)
{
    if (!(sptm_virt_flags & SPTM_VIRT_SAMPLE) || !sptm_virt_sample_hz)
        return;
    nv2_hcr_update(HCR_FMO, 0);
    msr(CNTP_TVAL_EL0, mrs(CNTFRQ_EL0) / sptm_virt_sample_hz);
    msr(CNTP_CTL_EL0, CNTx_CTL_ENABLE);
    sysop("isb");
}

static void sptm_virt_sample_disarm_here(void)
{
    if (!(sptm_virt_flags & SPTM_VIRT_SAMPLE))
        return;
    msr(CNTP_CTL_EL0, 0);
    sysop("isb");
    nv2_hcr_update(0, HCR_FMO);
}

/* An FIQ at EL2 while this core is still in SPTM: aura's sampling timer (exc_fiq). */
bool sptm_virt_sample_fiq(u64 *regs)
{
    if (!(sptm_virt_flags & SPTM_VIRT_SAMPLE) || !sptm_virt_guest_live)
        return false;
    if ((mrs(CNTP_CTL_EL0) & (CNTx_CTL_ISTATUS | CNTx_CTL_ENABLE)) !=
        (CNTx_CTL_ISTATUS | CNTx_CTL_ENABLE))
        return false;

    bool gl = in_gl12();
    u64 pc = gl ? mrs(SYS_IMP_APL_ELR_GL1) : mrs(ELR_EL2);
    if (sptm_sample_prints++ < sptm_virt_sample_max) {
        printf("SPTM-virt[sample c%d]: guest pc=0x%lx%s sp=0x%lx x0=0x%lx x1=0x%lx x8=0x%lx "
               "x19=0x%lx x29=0x%lx lr=0x%lx\n",
               smp_id(), pc, gl ? " (GL1)" : "", mrs(SP_EL1), regs[0], regs[1], regs[8], regs[19],
               regs[29], regs[30]);
        u64 fp = regs[29];
        for (int i = 0; i < 8 && fp; i++) {
            u64 nfp, lr;
            if (!sptm_virt_el1_va_read64(fp, &nfp) || !sptm_virt_el1_va_read64(fp + 8, &lr))
                break;
            printf("SPTM-virt[sample]:   frame %d lr 0x%lx\n", i, lr);
            if (nfp <= fp)
                break;
            fp = nfp;
        }
    }
    msr(CNTP_TVAL_EL0, mrs(CNTFRQ_EL0) / sptm_virt_sample_hz);
    msr(CNTP_CTL_EL0, CNTx_CTL_ENABLE);
    sysop("isb");
    return true;
}

/* The last step of every guest entry on a core, from nv2_enter_os. */
void sptm_virt_cpu_arm(void)
{
    int cpu = smp_id();
    u32 ent = sptm_virt_cpu_entries[cpu]++;

    sptm_cpu_in_xnu[cpu] = false;
    if (sptm_virt_guest_live && (ent < 4 || (ent & 0xff) == 0))
        printf("SPTM-virt[smp]: cpu%d (MPIDR 0x%06lx) enters SPTM, entry #%u, TWE armed\n", cpu,
               mrs(MPIDR_EL1) & 0xffffff, ent);

    /* GXF and SPRR run natively below aura: nothing traps GENTER/GEXIT or their configuration. */
    msr(HACR_EL2, 0);
    reg_clr(HCR_EL2, HCR_TIDCP);
    /* Until this core's first GEXIT into XNU, a WFE is SPTM's panic park: trap it (see
     * sptm_virt_el1_wfx), since under aura no event would ever end it. */
    reg_set(HCR_EL2, HCR_TWE);
    sysop("isb");

    /*
     * SPTM enables its own GXF from virtual EL2, which needs aura's GL2 to exist. SPRR first
     * (GXF_CONFIG UNDEFs without it); the permission maps were loaded at boot (mmu_init_sprr),
     * so aura's own pages stay accessible. Nothing above wrote EL2 state with GXF on.
     */
    msr(SYS_IMP_APL_SPRR_CONFIG_EL1, SPRR_CONFIG_EN);
    sysop("isb");
    msr(SYS_IMP_APL_GXF_CONFIG_EL1, GXF_CONFIG_EN);
    sysop("isb");

    sptm_virt_world_arm(cpu);
    sptm_virt_cl4_seed(cpu);
    sptm_virt_amx_host();
    sptm_virt_sample_arm_here();
}

bool sptm_virt_cpu_in_xnu(void)
{
    return sptm_cpu_in_xnu[smp_id()];
}

/* A GEXIT into XNU's normal world: this core runs XNU from now on. */
void sptm_virt_cpu_enters_xnu(int cpu)
{
    if (sptm_cpu_in_xnu[cpu])
        return;
    sptm_cpu_in_xnu[cpu] = true;
    sptm_virt_sample_disarm_here();
    nv2_hcr_update(0, HCR_TWE);
    sptm_virt_ctrr_publish_rorgn();
    sptm_virt_s2_enter_xnu(cpu);
    if (sptm_virt_wfi_watch)
        nv2_hcr_update(HCR_TWI | (sptm_virt_wfi_watch > 1 ? HCR_TWE : 0), 0);
}

__attribute__((used, retain)) void sptm_virt_enter(void)
{
    if (!sptm_images_loaded)
        panic("sptm_virt: no SPTM image recorded (call sptm_virt_load first)\n");

    printf("\nSPTM-virt: entering SPTM at 0x%lx, boot_args 0x%lx\n", sptm_entry_pa,
           sptm_handoff_pa);
    nv2_active = true;
    nv2_guarded = true;

    /* iBoot's entry contract: EL1&0 MMU and caches off, x0 = boot_args. */
    msr(SYS_SCTLR_EL12, mrs(SYS_SCTLR_EL12) & ~(SCTLR_M | SCTLR_C | SCTLR_I));
    sysop("isb");

    /*
     * aura's own USB gadgets (DWC3 + DART, set up at boot as proxy transports) would keep running
     * under the guest with their interrupts and DMA into aura's heap. With the proxy on the
     * DockChannel nothing uses them, and the guest, which has their nodes hidden, does not know
     * them: shut them down.
     */
    if (uartproxy_iodev == IODEV_DOCKCHANNEL_UART)
        usb_iodev_shutdown();

    /* The proxy flow never passes through kboot's nv2_prepare. */
    nv2_prepare();
    sptm_virt_s2_setup();

    /*
     * Every other core reaches the guest through a hardware reset into aura
     * (sptm_virt_cpu_reset), which enters SPTM's reset vector: the first instruction of the 16K
     * page holding its cold entry. Give each one its GL stacks now and publish its reset stack.
     */
    sptm_reset_pa = sptm_entry_pa & ~0x3fffUL;
    for (int i = 0; i < MAX_CPUS; i++)
        if (smp_cpu_valid(i))
            gxf_prealloc(i);
    int ready = smp_prepare_guest_resets();
    sptm_virt_guest_live = true;
    /* From here aura prints from the guest's traps on its own cores. */
    dockchannel_uart_set_tx_timeout(20);
    dc_cvac_range((void *)&sptm_virt_guest_live, sizeof(sptm_virt_guest_live));
    dc_cvac_range((void *)&sptm_reset_pa, sizeof(sptm_reset_pa));
    printf("SPTM-virt: SMP: %d cores reach aura on reset; SPTM reset vector 0x%lx (insn 0x%x)\n",
           ready, sptm_reset_pa, read32(sptm_reset_pa));

    /* MMU-off fetches must not see stale I-cache lines of whatever ran here. */
    ic_iallu();
    sysop("dsb sy");
    sysop("isb");

    /* The framebuffer is no-access to aura once SPRR is live: stop drawing the console into it. */
#ifdef USE_FB
    fb_set_active(false);
#endif

    nv2_enter_os(sptm_entry_pa, sptm_handoff_pa, 0, 0);
}

/*
 * A core out of a hardware reset while the guest is live: XNU started it (PMGR CPUSTART), or it
 * power-gated in idle or sleep and woke. Its iBoot-locked RVBAR lands in aura, MMU off, on the
 * stack smp_prepare_guest_resets published. Rebuild its GL2 and EL2 state and enter SPTM's reset
 * vector at virtual EL2; SPTM picks cold start or resume itself.
 */
void sptm_virt_cpu_reset(void)
{
    msr(DAIF, 0xfUL << 6);
    u64 mpidr = mrs(MPIDR_EL1) & 0xffffff;
    int cpu = smp_index_for_mpidr(mpidr);
    if (cpu < 0) {
        printf("SPTM-virt[smp]: reset of unknown core MPIDR 0x%lx; parking\n", mpidr);
        while (1)
            sysop("wfe");
    }
    msr(TPIDR_EL2, cpu);
    if (sptm_virt_flags & SPTM_VIRT_S2R)
        sptm_virt_s2r_wake(cpu);

    /* Everything so far went straight to DRAM: drop stale lines an earlier life of this core
     * left in the cluster cache before the caches come back on. */
    u64 sbase, ssize;
    smp_reset_stack_range(cpu, &sbase, &ssize);
    if (ssize)
        dc_ivac_range((void *)sbase, ssize);
    nv2_cpu_reset_invalidate(cpu);
    sysop("dsb sy");

    u32 n = sptm_cpu_resets[cpu]++;
    if (n < 4 || (n & 0xff) == 0)
        printf("SPTM-virt[smp]: cpu%d (MPIDR 0x%06lx) reset #%u -> SPTM reset vector 0x%lx\n", cpu,
               mpidr, n, sptm_reset_pa);

    gxf_init();

    msr(SYS_SCTLR_EL12, mrs(SYS_SCTLR_EL12) & ~(SCTLR_M | SCTLR_C | SCTLR_I));
    ic_iallu();
    sysop("dsb sy");
    sysop("isb");

    nv2_enter_os(sptm_reset_pa, 0, 0, 0);
}
