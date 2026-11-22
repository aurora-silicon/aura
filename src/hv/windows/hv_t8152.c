/* SPDX-License-Identifier: GPL-2.0-only */

/*
 * T8152 (J804) safety policy for the Windows carrier.  See hv_t8152.h.
 *
 * Everything here reads the live ADT and live registers at run time; no
 * per-machine value is compiled in except the J804 QSPI controller pages,
 * which are a second, fixed copy of what the ADT already names, so that the
 * NVRAM NOR stays out of the guest even if the ADT walk misses it.
 */

#include "hv.h"
#include "adt.h"
#include "aic.h"
#include "ctxr_exec.h"
#include "heapblock.h"
#include "hv_t8152.h"
#include "memory.h"
#include "smp.h"
#include "soc.h"
#include "string.h"
#include "utils.h"
#include "xnuboot.h"

#define J804_BOARD_ID       0x22
#define J804_BOOT_MPIDR     0x10100
#define J804_QSPI_BASE      0x331118000UL
#define J804_QSPI_SIZE      0x8000UL /* the two 16 KiB controller pages */
#define T8152_PAGE          0x4000UL
#define T8152_GUEST_ALIGN   0x10000UL /* Arm UEFI runtime regions: 64 KiB */
#define T8152_MIN_GUEST     (256UL << 20)
#define T8152_MAX_DENY      640
#define T8152_MAX_NODE_REGS 1024 /* per node; J804 pmgr-child has 64, error-handler 120 */
/*
 * Carrier exceptions: the BGRA shadow, the ADT, and G0's GPU entries (eight admitted firmware
 * regions, the handoff page), with room for what G1/G2 add.
 */
#define T8152_MAX_ALLOW     24
/*
 * AIC, usb-drd1 xHCI, usb-drd3 xHCI (D4), and room for G1's measured sgx sub-ranges and ASC
 * wrapper pages (16 KiB granular, so each nested hole splits a window; WINDOWS-GPU-PLAN.md
 * critic correction 1).
 */
#define T8152_MAX_MMIO      32
/*
 * D4: the xHCI register file at the start of a usb-drd node's reg 0 (dwc3 core.h
 * DWC3_XHCI_REGS_START/END 0x0..0x7fff; the dwc3 globals start at +0xc100).  Two 16 KiB pages.
 */
#define T8152_XHCI_SIZE     0x8000UL
#define T8152_INPUT_AREA_ALIGN 0x200000UL /* I1 reserved MTP area */
#define T8152_USB_PORTS     4 /* usb-drd0..3; port 0 (the proxy's) is never split */
#define T8152_REPORT_LIMIT  16
#define T8152_CPUS          12 /* E cpu0-5, P cpu6-7, M cpu8-11 (smp_init_t8152 checks the ADT) */
/*
 * One level-2 translation entry with the 16 KiB granule (Arm ARM, VMSAv8-64 translation
 * granules): the unit of the code-placement limit in code_placement_check().
 */
#define T8152_L2_BLOCK (32UL << 20)
/*
 * The OS exec window inside CTXR_D starts at ALIGN_UP(_base + slot, align): the same values
 * as CTXR_EXEC_AURA_SLOT and CTXR_EXEC_ALIGN in ctxr_exec.c (the host policy test checks).
 */
#define T8152_EXEC_SLOT  (16UL << 20)
#define T8152_EXEC_ALIGN (2UL << 20)

#define S2_MEMATTR       GENMASK(5, 2)
#define S2_MEMATTR_NGNRE 0x0UL /* Device-nGnRnE */
#define S2_MEMATTR_NORM  0xfUL /* stage 1 decides (normal RAM) */
#define S2_AP            GENMASK(7, 6)
#define S2_AP_RO         0x1UL
#define S2_VALID         BIT(0)
#define S2_SPTE_TYPE     GENMASK(52, 50)
#define S2_TARGET        GENMASK(49, 14)
#define S2_TARGET_L4     GENMASK(49, 2)

enum { SPTE_MAP = 0, SPTE_HOOK = 1 };

struct t8152_range {
    u64 start, end;
    const char *name;
};

struct t8152_allow {
    u64 ipa, start, end;
    bool read_only;
    const char *name;
};

static bool armed;
static bool ram_set;
static bool store_set;
static u64 store_start, store_end; /* D1 AHCI store, when set */
static bool input_set;
static u64 input_start, input_end; /* I1 reserved MTP area, when set */
static u64 ram_start, ram_end;   /* guest RAM window */
static u64 ram_host_top;         /* the launcher's heap top, from hv_t8152_set_guest_ram() */
static u64 dram_start, dram_end; /* boot_args normal RAM */

static struct t8152_range deny[T8152_MAX_DENY];
static unsigned int deny_count;
static struct t8152_range mmio_allow[T8152_MAX_MMIO];
static unsigned int mmio_allow_count;
static struct t8152_allow allow[T8152_MAX_ALLOW];
static unsigned int allow_count;
/*
 * D4: per usb-drd port 1..3, the denylist entry that holds that port's xHCI window (the first
 * 0x8000 bytes of its ADT reg 0), or -1.  hv_t8152_usb_allow() empties the entry while the
 * carrier passes the window through; hv_t8152_usb_revoke() puts it back.
 */
static int usb_xhci_deny[T8152_USB_PORTS];
static struct t8152_range usb_xhci_saved[T8152_USB_PORTS];
static int usb_xhci_mmio[T8152_USB_PORTS];

static bool overlaps(u64 a0, u64 a1, u64 b0, u64 b1)
{
    return a0 < b1 && b0 < a1;
}

static bool inside(u64 a0, u64 a1, u64 b0, u64 b1)
{
    return b0 <= a0 && a1 <= b1;
}

bool hv_t8152_policy_armed(void)
{
    return armed;
}

/*
 * All 12 CPUs must be in aura's dispatch loop (spin table, smp_is_alive) before a guest
 * starts.  The J804 Mu profile's MADT is static (12 GICCs, all enabled) and there is no
 * live mask Mu could patch it from, while the vGIC builds one redistributor per live CPU
 * (hv_vgic_live_cpu_count) and PSCI CPU_ON can only release a core that is already in
 * the spin table.  A partial machine would give Windows GICC entries with no
 * redistributor and CPU_ON targets with no core behind them, so refuse instead.
 * smp_init_t8152() publishes the boot CPU's spin-table flag itself; it is counted
 * explicitly anyway, the way the vGIC counts it.  Read-only: nothing is started here.
 */
static int live_cpus_check(void)
{
    char missing[T8152_CPUS * 6 + 1];
    unsigned int live = 0, extra = 0, pos = 0;

    missing[0] = 0;
    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        bool alive = cpu == boot_cpu_idx || smp_is_alive(cpu);
        if (cpu >= T8152_CPUS) {
            extra += alive;
            continue;
        }
        if (alive) {
            live++;
        } else {
            int n = snprintf(missing + pos, sizeof(missing) - pos, " cpu%d", cpu);
            if (n > 0 && (unsigned int)n < sizeof(missing) - pos)
                pos += n;
        }
    }

    if (live == T8152_CPUS && !extra) {
        printf("HV: T8152: all %d CPUs live in aura's dispatch loop\n", T8152_CPUS);
        return 0;
    }
    /*
     * smp_start_t8152() returns at once once any start has failed (smp_start_fail_mask),
     * so after a failed start only a fresh carrier session brings the missing cores up.
     */
    if (live != T8152_CPUS)
        printf("HV: T8152: only %u of %d CPUs are live in aura's dispatch loop (not live:%s); "
               "refusing. All %d must be live: start the secondaries once in a fresh carrier "
               "session; after a failed start (\"SMP: T8152 started ... failed\" nonzero) a "
               "second start does nothing, reload the carrier\n",
               live, T8152_CPUS, missing, T8152_CPUS);
    if (extra)
        printf("HV: T8152: %u spin-table entries beyond cpu%d are live; refusing\n", extra,
               T8152_CPUS - 1);
    return -1;
}

bool hv_t8152_guest_ram(u64 *base, u64 *size)
{
    if (!ram_set)
        return false;
    *base = ram_start;
    *size = ram_end - ram_start;
    return true;
}

static int deny_add(u64 start, u64 size, const char *name)
{
    if (!size || start > UINT64_MAX - size)
        return 0;
    if (deny_count >= T8152_MAX_DENY) {
        printf("HV: T8152: denylist is full at %s\n", name);
        return -1;
    }
    deny[deny_count].start = ALIGN_DOWN(start, T8152_PAGE);
    deny[deny_count].end = ALIGN_UP(start + size, T8152_PAGE);
    deny[deny_count].name = name;
    deny_count++;
    return 0;
}

static const struct t8152_range *deny_hit(u64 start, u64 end)
{
    for (unsigned int i = 0; i < deny_count; i++)
        if (overlaps(start, end, deny[i].start, deny[i].end))
            return &deny[i];
    return NULL;
}

/*
 * Top-level /arm-io nodes the guest must never reach, by name.  Matching is by
 * prefix, plus "audio" anywhere in the name.  Children are not walked: their
 * reg properties are bus-local (NOR partitions, i2c addresses).
 */
static const char *const deny_prefixes[] = {
    "qspi", "sep",     "aop", "admac",   "i2c",  "iop-audio", "dp-audio", "gpio",
    "nub-", "dp2hdmi", "wdt", "usb-drd", "pmgr", "spmi",      "dart",     "sart",
    "ans",  "smc",     "mtp", "spi",     "dcp",  "sio",       "atc-phy",
};

/*
 * D4: "usb-drd1".."usb-drd3" -> 1..3; anything else (usb-drd0, the proxy's port, included)
 * -> 0, which keeps the whole of every reg denied.
 */
static int usb_port_of(const char *name)
{
    if (strncmp(name, "usb-drd", 7) || name[7] < '1' || name[7] > '3' || name[8])
        return 0;
    return name[7] - '0';
}

static bool deny_name(const char *name)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(deny_prefixes); i++)
        if (!strncmp(name, deny_prefixes[i], strlen(deny_prefixes[i])))
            return true;
    for (const char *p = name; *p; p++)
        if (!strncmp(p, "audio", 5))
            return true;
    return false;
}

/*
 * Every reg entry of the node, however many: a silently dropped tail is a hole.  usb-drd1..3
 * reg 0 goes in as two entries, the xHCI window and the rest, both denied; only D4's attach
 * ever lifts the first (hv_t8152_usb_allow).
 */
static int deny_node_regs(int *path, const char *name, bool *found)
{
    int port = usb_port_of(name);
    for (int idx = 0; idx < T8152_MAX_NODE_REGS; idx++) {
        u64 base, size;
        if (adt_get_reg(adt, path, "reg", idx, &base, &size) < 0)
            return 0;
        if (!size)
            continue;
        *found = true;
        if (idx == 0 && port && size > T8152_XHCI_SIZE && !(base & (T8152_PAGE - 1))) {
            usb_xhci_deny[port] = deny_count;
            if (deny_add(base, T8152_XHCI_SIZE, name))
                return -1;
            base += T8152_XHCI_SIZE;
            size -= T8152_XHCI_SIZE;
        }
        if (deny_add(base, size, name))
            return -1;
    }
    printf("HV: T8152: /arm-io/%s has more than %d reg entries; refusing an incomplete denylist\n",
           name, T8152_MAX_NODE_REGS);
    return -1;
}

static int carveout(const char *id, u64 *start, u64 *size)
{
    int node = adt_path_offset(adt, "/chosen/carveout-memory-map");
    u32 len;
    if (node < 0)
        return -1;
    const u64 *range = adt_getprop(adt, node, id, &len);
    if (!range || len != 2 * sizeof(u64))
        return -1;
    memcpy(start, range, sizeof(*start));
    memcpy(size, range + 1, sizeof(*size));
    return *size ? 0 : -1;
}

static int build_denylist(void)
{
    int armio = adt_path_offset(adt, "/arm-io");
    bool qspi = false, sep = false, wdt = false;
    deny_count = 0;
    for (int port = 0; port < T8152_USB_PORTS; port++) {
        usb_xhci_deny[port] = -1;
        usb_xhci_mmio[port] = -1;
    }

    if (armio < 0) {
        printf("HV: T8152: /arm-io is missing from the ADT\n");
        return -1;
    }

    int child = armio;
    ADT_FOREACH_CHILD(adt, child)
    {
        const char *name = adt_get_name(adt, child);
        if (!name || !deny_name(name))
            continue;
        int path[3] = {armio, child, 0};
        bool found = false;
        if (deny_node_regs(path, name, &found))
            return -1;
        if (found && !strcmp(name, "qspi"))
            qspi = true;
        if (found && !strcmp(name, "sep"))
            sep = true;
        if (found && !strcmp(name, "wdt"))
            wdt = true;
    }

    if (!qspi || !sep || !wdt) {
        printf("HV: T8152: the ADT lacks %s%s%s; refusing a guest without a complete denylist\n",
               qspi ? "" : "/arm-io/qspi ", sep ? "" : "/arm-io/sep ", wdt ? "" : "/arm-io/wdt");
        return -1;
    }

    /* Second, fixed copy of the NVRAM controller: never trust only the walk. */
    if (deny_add(J804_QSPI_BASE, J804_QSPI_SIZE, "qspi NOR/NVRAM (J804 fixed)"))
        return -1;

    u64 start, size;
    /* MTE tag storage and the faulting carve-out above RAM: never guest memory. */
    if (carveout("region-id-218", &start, &size) == 0 &&
        deny_add(start, size, "region-id-218 (MTE tag storage)"))
        return -1;
    if (carveout("region-id-4", &start, &size) == 0 &&
        deny_add(start, size, "region-id-4 (above RAM)"))
        return -1;

    printf("HV: T8152: %u denylist ranges from the live ADT\n", deny_count);
    return 0;
}

static int build_mmio_allow(void)
{
    mmio_allow_count = 0;
#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH
    int path[8];
    u64 base, size;
    if (adt_path_offset_trace(adt, "/arm-io/aic", path) < 0 ||
        adt_get_reg(adt, path, "reg", 0, &base, &size) < 0 || !size || !aic || aic->base != base) {
        printf("HV: T8152: cannot read the AIC aperture from the ADT\n");
        return -1;
    }
    mmio_allow[0].start = ALIGN_DOWN(base, T8152_PAGE);
    mmio_allow[0].end = ALIGN_UP(base + size, T8152_PAGE);
    mmio_allow[0].name = "aic (native AIC passthrough)";
    mmio_allow_count = 1;
    if (deny_hit(mmio_allow[0].start, mmio_allow[0].end)) {
        printf("HV: T8152: AIC aperture overlaps the denylist\n");
        return -1;
    }
#endif
    return 0;
}

/*
 * Where aura's code [_base, _rodata_end) may sit, from what Part A (J804, 2026-11-16,
 * ctxr_el1) showed EL2 on the secondaries executing.  That range holds the EL2 vectors,
 * smp_secondary_entry, the hypervisor vectors and guest entry/exit; the guest itself never
 * runs in CTXR_D (ram_conflicts refuses the overlap).
 *
 * Executed at EL2 on cpu0, cpu7 and cpu8: aura's own image (the dispatch loop) and the
 * first pages of the CTXR exec window (window base + 2 MiB).  Faulted: the last 16 KiB page
 * of that 128 MiB exec window, window base + 128 MiB - 16 KiB (0x100117fc000 there), which
 * is NOT the last page of CTXR_D (CTXR_D ended at 0x10011898000).  ESR_EL2 was 0x86000002:
 * EC 0x21 (instruction abort, same EL), IFSC 0b000010, an address size fault at level 2
 * (Arm ARM, ESR_ELx ISS encoding for instruction aborts), not a permission fault.  The
 * cause is unknown and only that one page was probed; with the 16 KiB granule a level-2
 * entry covers 32 MiB, so the bad region may be larger than the page.
 *
 * So, conservatively: the code must lie inside CTXR_D (UPR + 4 KiB, aligned down to 16 KiB
 * as the ctxr_el1 harness does) and end at or below both the exec window base (aura's image
 * always does: ctxr_exec_find_window() requires _end below it) and the 32 MiB-aligned
 * block that holds CTXR_D's last 16 KiB page, which was never probed.  With Part A's
 * numbers the limit is the window base 0x10009800000 and the code ends near 0x1000878c000.
 *
 * Not covered: the RVBAR veneer smp_prepare_rvbar_t8152() may write at the locked RVBAR,
 * below _base.  A secondary runs it only when it resets; every secondary ran it to reach
 * the dispatch loop (live_cpus_check() requires all 12), and the carrier never resets or
 * powers off a core afterwards (PSCI CPU_OFF parks in the spin table).
 */
static int code_placement_check(u64 start, u64 end)
{
    u64 code_end = ALIGN_UP((u64)_rodata_end, T8152_PAGE);
    u64 top = ALIGN_DOWN(end, T8152_PAGE);
    u64 window = ALIGN_UP((u64)_base + T8152_EXEC_SLOT, T8152_EXEC_ALIGN);
    u64 last_block =
        top > start + T8152_PAGE ? ALIGN_DOWN(top - T8152_PAGE, T8152_L2_BLOCK) : start;
    u64 limit = window < last_block ? window : last_block;

    if ((u64)_base < start || code_end > limit) {
        printf("HV: T8152: aura's code [0x%lx, 0x%lx) must lie inside CTXR_D [0x%lx, 0x%lx) and "
               "end at or below 0x%lx: the CTXR exec window base 0x%lx (EL2 on the secondaries "
               "faulted on the window's last page in Part A) and the 32 MiB block 0x%lx that "
               "holds CTXR_D's last page (never probed); refusing\n",
               (u64)_base, code_end, start, top, limit, window, last_block);
        return -1;
    }
    printf("HV: T8152: aura 0x%lx inside CTXR_D [0x%lx, 0x%lx), code ends 0x%lx at or below "
           "0x%lx (below the exec window and CTXR_D's last 32 MiB block)\n",
           (u64)_base, start, top, code_end, limit);
    return 0;
}

int hv_t8152_preflight(void)
{
    int bad = 0;
    u64 start, end;

    armed = false;
    ram_set = false;
    store_set = false;
    input_set = false;
    allow_count = 0;

    if (chip_id != T8152) {
        printf("HV: T8152 policy invoked on chip 0x%x\n", chip_id);
        return -1;
    }
    if (board_id != J804_BOARD_ID) {
        printf("HV: T8152: board 0x%x is not J804 (0x%x); no qualified guest profile\n", board_id,
               J804_BOARD_ID);
        bad++;
    }
    u64 mpidr = mrs(MPIDR_EL1) & 0xffffff;
    if (mpidr != J804_BOOT_MPIDR) {
        printf("HV: T8152: boot CPU MPIDR 0x%lx is not cpu6 0x%x\n", mpidr, J804_BOOT_MPIDR);
        bad++;
    }
    if (!ctxr_range(CTXR_RANGE_D, &start, &end)) {
        printf("HV: T8152: CTXR_D is not armed or not readable; refusing\n");
        bad++;
    } else if ((u64)_base < start || (u64)_base >= end) {
        printf("HV: T8152: aura at 0x%lx is outside CTXR_D [0x%lx, 0x%lx): chainload it in place, "
               "never with --relocate\n",
               (u64)_base, start, end);
        bad++;
    } else if (code_placement_check(start, end)) {
        bad++;
    }
    if (live_cpus_check())
        bad++;
    if (!aic || aic->version != 3) {
        printf("HV: T8152: AIC3 is not initialized\n");
        bad++;
    } else {
        printf("HV: T8152: AIC3 at 0x%lx, %u IRQs\n", aic->base, aic->nr_irq);
    }
    if (mrs(HCR_EL2) & HCR_FWB) {
        printf("HV: T8152: HCR_EL2.FWB is set; stage-2 device attributes would not hold\n");
        bad++;
    }
    if (!mrs(CNTFRQ_EL0)) {
        printf("HV: T8152: CNTFRQ_EL0 is zero\n");
        bad++;
    }

    dram_start = cur_boot_args.phys_base;
    dram_end = cur_boot_args.phys_base + cur_boot_args.mem_size;
    if (!cur_boot_args.mem_size || dram_end <= dram_start) {
        printf("HV: T8152: boot_args RAM is empty\n");
        bad++;
    }

    if (build_denylist() || build_mmio_allow())
        bad++;

    if (bad) {
        printf("HV: T8152: %d precondition(s) failed; the guest will not start\n", bad);
        return -1;
    }

    armed = true;
    printf("HV: T8152: stage-2 allowlist policy armed (RAM window not yet set)\n");
    return 0;
}

/* CTXR_A-D and CTRR_C-D: the ranges the boot chain locked; nothing of the guest's may touch them. */
static int ctxr_conflicts(u64 base, u64 top, const char *what)
{
    static const struct {
        enum ctxr_range id;
        const char *name;
    } ctxr[] = {
        {CTXR_RANGE_A, "CTXR_A"}, {CTXR_RANGE_B, "CTXR_B"}, {CTXR_RANGE_C, "CTXR_C"},
        {CTXR_RANGE_D, "CTXR_D"}, {CTRR_RANGE_C, "CTRR_C"}, {CTRR_RANGE_D, "CTRR_D"},
    };
    int bad = 0;
    for (unsigned int i = 0; i < ARRAY_SIZE(ctxr); i++) {
        u64 s, e;
        if (ctxr_range(ctxr[i].id, &s, &e) && overlaps(base, top, s, e)) {
            printf("HV: T8152: %s overlaps %s [0x%lx, 0x%lx)\n", what, ctxr[i].name, s, e);
            bad++;
        }
    }
    return bad;
}

static int ram_conflicts(u64 base, u64 top, u64 host_top)
{
    int bad = 0;
    int node = adt_path_offset(adt, "/chosen/carveout-memory-map");
    if (node < 0) {
        printf("HV: T8152: /chosen/carveout-memory-map is missing\n");
        return 1;
    }
    ADT_FOREACH_PROPERTY(adt, node, prop)
    {
        u64 r[2];
        if (prop->size != sizeof(r))
            continue;
        memcpy(r, prop->value, sizeof(r));
        if (!r[1])
            continue;
        /* The carve-out that is the usable RAM itself (region-id-24 on J804). */
        if (r[0] == dram_start && r[1] == dram_end - dram_start)
            continue;
        if (overlaps(base, top, r[0], r[0] + r[1])) {
            printf("HV: T8152: guest RAM overlaps carve-out %s [0x%lx, 0x%lx)\n", prop->name, r[0],
                   r[0] + r[1]);
            bad++;
        }
    }

    node = adt_path_offset(adt, "/chosen/memory-map");
    if (node >= 0) {
        ADT_FOREACH_PROPERTY(adt, node, prop)
        {
            u64 r[2];
            if (prop->size != sizeof(r))
                continue;
            memcpy(r, prop->value, sizeof(r));
            if (r[1] && overlaps(base, top, r[0], r[0] + r[1])) {
                printf("HV: T8152: guest RAM overlaps memory-map %s [0x%lx, 0x%lx)\n", prop->name,
                       r[0], r[0] + r[1]);
                bad++;
            }
        }
    }

    bad += ctxr_conflicts(base, top, "guest RAM");

    /*
     * aura's heap may still grow up to its limit, and the launcher's own heap
     * sits above that limit (ProxyUtils): the whole span is occupied.
     */
    u64 heap_limit = heapblock_limit();
    if (!heap_limit) {
        printf("HV: T8152: aura's heap has no limit, so its top is unbounded; refusing\n");
        bad++;
    }
    u64 aura_top = heapblock_high_water();
    if (heap_limit > aura_top)
        aura_top = heap_limit;
    if (host_top > aura_top)
        aura_top = host_top;
    if (overlaps(base, top, (u64)_base, aura_top)) {
        printf("HV: T8152: guest RAM overlaps aura and its heaps [0x%lx, 0x%lx)\n", (u64)_base,
               aura_top);
        bad++;
    }

    u64 adt_pa = (u64)cur_boot_args.devtree - cur_boot_args.virt_base + cur_boot_args.phys_base;
    if (overlaps(base, top, adt_pa, adt_pa + cur_boot_args.devtree_size)) {
        printf("HV: T8152: guest RAM overlaps the live ADT\n");
        bad++;
    }
    u64 video_size = (u64)cur_boot_args.video.stride * cur_boot_args.video.height;
    if (cur_boot_args.video.base &&
        overlaps(base, top, cur_boot_args.video.base, cur_boot_args.video.base + video_size)) {
        printf("HV: T8152: guest RAM overlaps the scanout framebuffer\n");
        bad++;
    }
    const struct t8152_range *hit = deny_hit(base, top);
    if (hit) {
        printf("HV: T8152: guest RAM overlaps %s [0x%lx, 0x%lx)\n", hit->name, hit->start,
               hit->end);
        bad++;
    }
    return bad;
}

int hv_t8152_set_guest_ram(u64 base, u64 size, u64 host_top)
{
    if (!armed) {
        printf("HV: T8152: guest RAM refused: the policy is not armed (hv_init first)\n");
        return -1;
    }
    if (ram_set) {
        printf("HV: T8152: guest RAM is already set for this session\n");
        return -1;
    }
    if (!size || base > UINT64_MAX - size || (base & (T8152_GUEST_ALIGN - 1)) ||
        (size & (T8152_PAGE - 1)) || size < T8152_MIN_GUEST) {
        printf("HV: T8152: guest RAM 0x%lx+0x%lx must be 64 KiB aligned, 16 KiB sized and at "
               "least 256 MiB\n",
               base, size);
        return -1;
    }
    u64 top = base + size;
    if (!inside(base, top, dram_start, dram_end)) {
        printf("HV: T8152: guest RAM [0x%lx, 0x%lx) is not inside boot_args RAM [0x%lx, 0x%lx)\n",
               base, top, dram_start, dram_end);
        return -1;
    }
    if (host_top <= (u64)_base) {
        printf("HV: T8152: the launcher did not report the top of its heap (0x%lx)\n", host_top);
        return -1;
    }
    int bad = ram_conflicts(base, top, host_top);
    if (bad) {
        printf("HV: T8152: guest RAM [0x%lx, 0x%lx) refused (%d conflict(s))\n", base, top, bad);
        return -1;
    }
    ram_start = base;
    ram_end = top;
    ram_host_top = host_top;
    ram_set = true;
    printf("HV: T8152: guest RAM [0x%lx, 0x%lx) accepted\n", base, top);
    return 0;
}

int hv_t8152_reserve_store(u64 start, u64 end)
{
    if (!armed || !ram_set) {
        printf("HV: T8152: AHCI store refused: the policy is not armed or guest RAM is not set\n");
        return -1;
    }
    if (store_set) {
        printf("HV: T8152: AHCI store refused: a store is already reserved this session\n");
        return -1;
    }
    if (end <= start || (start | end) & (T8152_PAGE - 1) || start < ram_end) {
        printf("HV: T8152: AHCI store [0x%lx, 0x%lx) refused: it must be a 16 KiB aligned range "
               "above guest RAM [0x%lx, 0x%lx)\n",
               start, end, ram_start, ram_end);
        return -1;
    }
    if (!inside(start, end, dram_start, dram_end)) {
        printf("HV: T8152: AHCI store [0x%lx, 0x%lx) is not inside boot_args RAM [0x%lx, 0x%lx)\n",
               start, end, dram_start, dram_end);
        return -1;
    }
    int bad = ram_conflicts(start, end, ram_host_top);
    if (bad) {
        printf("HV: T8152: AHCI store [0x%lx, 0x%lx) refused (%d conflict(s); the lines above say "
               "\"guest RAM\" for the store)\n",
               start, end, bad);
        return -1;
    }
    if (input_set && overlaps(start, end, input_start, input_end)) {
        printf("HV: T8152: AHCI store [0x%lx, 0x%lx) overlaps the input area [0x%lx, 0x%lx)\n",
               start, end, input_start, input_end);
        return -1;
    }
    if (deny_add(start, end - start, "AHCI disk store (D1)"))
        return -1;
    store_set = true;
    store_start = start;
    store_end = end;
    printf("HV: T8152: AHCI store [0x%lx, 0x%lx) reserved and denied to stage 2\n", start, end);
    return 0;
}

int hv_t8152_reserve_input_area(u64 start, u64 end)
{
    if (!armed || !ram_set) {
        printf("HV: T8152: input area refused: the policy is not armed or guest RAM is not set\n");
        return -1;
    }
    if (input_set) {
        printf("HV: T8152: input area refused: an area is already reserved this session\n");
        return -1;
    }
    if (end <= start || (start | end) & (T8152_INPUT_AREA_ALIGN - 1) || start < ram_end) {
        printf("HV: T8152: input area [0x%lx, 0x%lx) refused: it must be a 2 MiB aligned range "
               "above guest RAM [0x%lx, 0x%lx)\n",
               start, end, ram_start, ram_end);
        return -1;
    }
    if (!inside(start, end, dram_start, dram_end)) {
        printf("HV: T8152: input area [0x%lx, 0x%lx) is not inside boot_args RAM [0x%lx, 0x%lx)\n",
               start, end, dram_start, dram_end);
        return -1;
    }
    if (store_set && overlaps(start, end, store_start, store_end)) {
        printf("HV: T8152: input area [0x%lx, 0x%lx) overlaps the AHCI store [0x%lx, 0x%lx)\n",
               start, end, store_start, store_end);
        return -1;
    }
    const struct t8152_range *hit = deny_hit(start, end);
    if (hit) {
        printf("HV: T8152: input area [0x%lx, 0x%lx) overlaps denied %s\n", start, end,
               hit->name);
        return -1;
    }
    int bad = ram_conflicts(start, end, ram_host_top);
    if (bad) {
        printf("HV: T8152: input area [0x%lx, 0x%lx) refused (%d conflict(s); the lines above say "
               "\"guest RAM\" for the area)\n",
               start, end, bad);
        return -1;
    }
    if (deny_add(start, end - start, "input MTP area (I1)"))
        return -1;
    input_set = true;
    input_start = start;
    input_end = end;
    printf("HV: T8152: input area [0x%lx, 0x%lx) reserved and denied to stage 2\n", start, end);
    return 0;
}

bool hv_t8152_carrier_ram(u64 start, u64 end, u64 map_lo, u64 map_hi)
{
    u64 c_start, c_size;
    if (end <= start || !inside(start, end, map_lo, map_hi))
        return false;
    if (carveout("region-id-4", &c_start, &c_size) == 0 &&
        overlaps(start, end, c_start, c_start + c_size))
        return false;
    if (ram_set && overlaps(start, end, ram_start, ram_end))
        return false;
    if (store_set && overlaps(start, end, store_start, store_end))
        return false;
    if (input_set && overlaps(start, end, input_start, input_end))
        return false;
    return true;
}

int hv_t8152_allow(u64 ipa, u64 pa, u64 size, bool read_only, const char *name)
{
    if (!armed)
        return 0; /* not T8152, or refused already */
    u64 end = pa + size;
    if (!size || pa > UINT64_MAX - size || (pa | size | ipa) & (T8152_PAGE - 1) ||
        !inside(pa, end, dram_start, dram_end) || deny_hit(pa, end) ||
        allow_count >= T8152_MAX_ALLOW) {
        printf("HV: T8152: exception %s [0x%lx, 0x%lx) refused\n", name, pa, end);
        return -1;
    }
    allow[allow_count++] = (struct t8152_allow){ipa, pa, end, read_only, name};
    printf("HV: T8152: exception %s IPA 0x%lx -> [0x%lx, 0x%lx)%s\n", name, ipa, pa, end,
           read_only ? " read-only" : "");
    return 0;
}

/*
 * G0: one region of the admitted GPU firmware carve-out (gpu_t8152_admit.c), the only kind of
 * exception that lies outside boot_args RAM: iBoot placed the UAT regions above it and the ASC
 * __DATA segments below it.  Identity only (IPA = PA), registered, never mapped here.  The
 * region must be 16 KiB aligned, inside DRAM [dram_lo, dram_hi) as /chosen gives it, entirely
 * outside boot_args RAM, and clear of CTXR_A-D, CTRR_C-D, the denylist and guest RAM.
 */
int hv_t8152_gpu_fw_allow(u64 pa, u64 size, bool read_only, u64 dram_lo, u64 dram_hi,
                          const char *name)
{
    if (!armed || !ram_set) {
        printf("HV: T8152: GPU firmware %s refused: the policy is not armed or guest RAM is not "
               "set\n",
               name);
        return -1;
    }
    u64 end = pa + size;
    const struct t8152_range *hit = NULL;
    if (!size || pa > UINT64_MAX - size || (pa | size) & (T8152_PAGE - 1) ||
        !inside(pa, end, dram_lo, dram_hi) || overlaps(pa, end, dram_start, dram_end) ||
        overlaps(pa, end, ram_start, ram_end) || (hit = deny_hit(pa, end)) ||
        ctxr_conflicts(pa, end, name) || allow_count >= T8152_MAX_ALLOW) {
        printf("HV: T8152: GPU firmware %s [0x%lx, 0x%lx) refused: it must be 16 KiB aligned "
               "DRAM [0x%lx, 0x%lx) outside boot_args RAM [0x%lx, 0x%lx), clear of CTXR/CTRR "
               "and the denylist%s%s (%u of %d exceptions used)\n",
               name, pa, end, dram_lo, dram_hi, dram_start, dram_end, hit ? "; hits " : "",
               hit ? hit->name : "", allow_count, T8152_MAX_ALLOW);
        return -1;
    }
    allow[allow_count++] = (struct t8152_allow){pa, pa, end, read_only, name};
    printf("HV: T8152: GPU firmware %s [0x%lx, 0x%lx) admitted as an identity exception%s (not "
           "mapped)\n",
           name, pa, end, read_only ? ", read-only" : "");
    return 0;
}

/*
 * G0: a failed GPU admission takes back every exception it added (hv_gpu.c admit()).  The mark is
 * allow_count before the first one; the proxy runs one op at a time, so nothing else adds in
 * between.  Exceptions are registered, never mapped, so dropping them is enough.
 */
unsigned int hv_t8152_allow_mark(void)
{
    return allow_count;
}

void hv_t8152_allow_rollback(unsigned int mark, const char *why)
{
    if (mark >= allow_count)
        return;
    printf("HV: T8152: %s: dropping %u exception(s) from %s on\n", why, allow_count - mark,
           allow[mark].name);
    allow_count = mark;
}

/*
 * G0: memory that must never reach stage 2 for the rest of the session (the GPU firmware
 * __TEXT segments).  Outside guest RAM; it joins the denylist (map filter and verify).
 */
int hv_t8152_gpu_deny(u64 start, u64 end, const char *name)
{
    if (!armed || !ram_set) {
        printf("HV: T8152: %s refused: the policy is not armed or guest RAM is not set\n", name);
        return -1;
    }
    if (end <= start || (start | end) & (T8152_PAGE - 1) || overlaps(start, end, ram_start, ram_end)) {
        printf("HV: T8152: %s [0x%lx, 0x%lx) refused: it must be 16 KiB aligned and outside guest "
               "RAM [0x%lx, 0x%lx)\n",
               name, start, end, ram_start, ram_end);
        return -1;
    }
    for (unsigned int i = 0; i < allow_count; i++)
        if (overlaps(start, end, allow[i].start, allow[i].end)) {
            printf("HV: T8152: %s [0x%lx, 0x%lx) refused: it overlaps exception %s\n", name, start,
                   end, allow[i].name);
            return -1;
        }
    if (deny_add(start, end - start, name))
        return -1;
    printf("HV: T8152: %s [0x%lx, 0x%lx) denied to stage 2\n", name, start, end);
    return 0;
}

/*
 * D4: pass usb-drd<port>'s xHCI window [xhci, xhci + 0x8000) through to the guest.  It must
 * be exactly the window build_denylist() split off that port's ADT reg 0; nothing else on the
 * denylist may overlap it once that entry is lifted.  0, or -1 with the reason (and the
 * denylist unchanged).
 */
int hv_t8152_usb_allow(int port, u64 xhci)
{
    if (!armed || !ram_set) {
        printf("HV: T8152: USB xHCI refused: the policy is not armed or guest RAM is not set\n");
        return -1;
    }
    if (port < 1 || port >= T8152_USB_PORTS || usb_xhci_deny[port] < 0 ||
        usb_xhci_mmio[port] >= 0) {
        printf("HV: T8152: USB xHCI of port %d refused: no split usb-drd%d reg 0 in the "
               "denylist, or already passed through\n",
               port, port);
        return -1;
    }
    struct t8152_range *entry = &deny[usb_xhci_deny[port]];
    if (entry->start != xhci || entry->end != xhci + T8152_XHCI_SIZE) {
        printf("HV: T8152: USB xHCI 0x%lx is not usb-drd%d's ADT window [0x%lx, 0x%lx)\n", xhci,
               port, entry->start, entry->end);
        return -1;
    }
    if (mmio_allow_count >= T8152_MAX_MMIO) {
        printf("HV: T8152: USB xHCI of port %d refused: the MMIO allowlist is full\n", port);
        return -1;
    }
    usb_xhci_saved[port] = *entry;
    entry->start = entry->end = 0; /* an empty range overlaps nothing */
    const struct t8152_range *hit = deny_hit(xhci, xhci + T8152_XHCI_SIZE);
    if (hit || overlaps(xhci, xhci + T8152_XHCI_SIZE, dram_start, dram_end)) {
        printf("HV: T8152: USB xHCI [0x%lx, 0x%lx) also overlaps %s; refusing\n", xhci,
               xhci + T8152_XHCI_SIZE, hit ? hit->name : "RAM");
        *entry = usb_xhci_saved[port];
        return -1;
    }
    usb_xhci_mmio[port] = mmio_allow_count;
    mmio_allow[mmio_allow_count].start = xhci;
    mmio_allow[mmio_allow_count].end = xhci + T8152_XHCI_SIZE;
    mmio_allow[mmio_allow_count].name = port == 1 ? "usb-drd1 xHCI (D4)" : port == 2 ?
                                        "usb-drd2 xHCI (D4)" : "usb-drd3 xHCI (D4)";
    mmio_allow_count++;
    printf("HV: T8152: usb-drd%d xHCI [0x%lx, 0x%lx) passed through (Device-nGnRnE); the rest "
           "of the controller, its PHY and DARTs stay denied\n",
           port, xhci, xhci + T8152_XHCI_SIZE);
    return 0;
}

/* D4 teardown: deny usb-drd<port>'s xHCI window again.  The caller unmapped it first. */
void hv_t8152_usb_revoke(int port)
{
    if (port < 1 || port >= T8152_USB_PORTS || usb_xhci_mmio[port] < 0)
        return;
    unsigned int slot = usb_xhci_mmio[port];
    for (unsigned int i = slot; i + 1 < mmio_allow_count; i++)
        mmio_allow[i] = mmio_allow[i + 1];
    mmio_allow_count--;
    for (int p = 1; p < T8152_USB_PORTS; p++)
        if (usb_xhci_mmio[p] > (int)slot)
            usb_xhci_mmio[p]--;
    usb_xhci_mmio[port] = -1;
    deny[usb_xhci_deny[port]] = usb_xhci_saved[port];
    printf("HV: T8152: usb-drd%d xHCI denied again\n", port);
}

/*
 * D4: carrier memory the guest must never reach (the USB DART tables).  It must lie in aura's
 * own heap span [_base, max(heap high water, heap limit)), clear of guest RAM and the store, and
 * it joins the denylist for the rest of the session (map filter and hv_t8152_verify), even
 * after the carrier frees it.  0, or -1 with the reason.
 */
int hv_t8152_reserve_carrier(u64 start, u64 end, const char *name)
{
    if (!armed || !ram_set) {
        printf("HV: T8152: %s refused: the policy is not armed or guest RAM is not set\n", name);
        return -1;
    }
    u64 aura_top = heapblock_high_water();
    if (heapblock_limit() > aura_top)
        aura_top = heapblock_limit();
    if (end <= start || (start | end) & (T8152_PAGE - 1) ||
        !inside(start, end, (u64)_base, aura_top) || !inside(start, end, dram_start, dram_end) ||
        overlaps(start, end, ram_start, ram_end) || deny_hit(start, end)) {
        printf("HV: T8152: %s [0x%lx, 0x%lx) refused: it must be 16 KiB aligned carrier heap "
               "[0x%lx, 0x%lx), clear of guest RAM [0x%lx, 0x%lx) and the denylist\n",
               name, start, end, (u64)_base, aura_top, ram_start, ram_end);
        return -1;
    }
    if (deny_add(start, end - start, name))
        return -1;
    printf("HV: T8152: %s [0x%lx, 0x%lx) denied to stage 2\n", name, start, end);
    return 0;
}

static const struct t8152_range *mmio_hit(u64 start, u64 end)
{
    for (unsigned int i = 0; i < mmio_allow_count; i++)
        if (inside(start, end, mmio_allow[i].start, mmio_allow[i].end))
            return &mmio_allow[i];
    return NULL;
}

int hv_t8152_describe_ipa(u64 ipa, const char **name)
{
    *name = NULL;
    if (!armed)
        return HV_T8152_IPA_UNLISTED;
    if (ram_set && ipa >= ram_start && ipa < ram_end) {
        *name = "guest RAM";
        return HV_T8152_IPA_RAM;
    }
    for (unsigned int i = 0; i < allow_count; i++) {
        if (ipa >= allow[i].ipa && ipa - allow[i].ipa < allow[i].end - allow[i].start) {
            *name = allow[i].name;
            return HV_T8152_IPA_EXCEPTION;
        }
    }
    const struct t8152_range *hit = mmio_hit(ipa, ipa + 1);
    if (hit) {
        *name = hit->name;
        return HV_T8152_IPA_MMIO;
    }
    hit = deny_hit(ipa, ipa + 1);
    if (hit) {
        *name = hit->name;
        return HV_T8152_IPA_DENIED;
    }
    if (ipa >= dram_start && ipa < dram_end) {
        *name = "RAM outside the guest window";
        return HV_T8152_IPA_HOST_RAM;
    }
    return HV_T8152_IPA_UNLISTED;
}

/*
 * The one rule for a mapping that reaches a physical address: either through
 * a hardware leaf (the guest touches it directly) or a software map leaf (the
 * HV performs the guest's access on it).  Returns NULL if allowed, else why.
 */
static const char *pa_rule(u64 ipa, u64 pa, u64 size, bool hw, u64 memattr, bool read_only)
{
    u64 end = pa + size;
    const struct t8152_range *hit = deny_hit(pa, end);
    if (hit)
        return hit->name;
    if (ram_set && inside(pa, end, ram_start, ram_end)) {
        if (ipa != pa)
            return "guest RAM must be identity mapped";
        if (hw && memattr != S2_MEMATTR_NORM)
            return "guest RAM must keep the normal-memory attribute";
        return NULL;
    }
    for (unsigned int i = 0; i < allow_count; i++) {
        if (!inside(pa, end, allow[i].start, allow[i].end))
            continue;
        if (ipa - allow[i].ipa != pa - allow[i].start)
            return "exception mapped at the wrong IPA";
        if (allow[i].read_only && !read_only)
            return "read-only exception mapped writable";
        return NULL;
    }
    if (mmio_hit(pa, end)) {
        if (ipa != pa)
            return "MMIO passthrough must be identity mapped";
        if (hw && memattr != S2_MEMATTR_NGNRE)
            return "MMIO passthrough must be Device-nGnRnE";
        return NULL;
    }
    if (overlaps(pa, end, dram_start, dram_end))
        return "RAM outside the guest window (aura, carve-outs, firmware)";
    return "MMIO not on the allowlist";
}

bool hv_t8152_map_filter(u64 from, u64 *to, u64 size)
{
    if (!armed || !*to)
        return true; /* unmapping is always allowed */

    u64 v = *to;
    bool hw = v & S2_VALID;
    u64 type = FIELD_GET(S2_SPTE_TYPE, v);

    if (!hw && type == SPTE_HOOK)
        return true; /* C hooks: emulated by the carrier */

    if (!hw && type != SPTE_MAP) {
        /*
         * Host (proxy) hooks: the Python handler performs the access at PA = IPA, and
         * stage 2 here covers exactly the PA space, so every such hook would be an
         * MMIO or RAM passthrough the allowlist never saw.  None is allowed.
         */
        const struct t8152_range *hit = deny_hit(from, from + size);
        printf("HV: T8152: refusing a host hook at IPA 0x%lx +0x%lx%s%s\n", from, size,
               hit ? ": " : "", hit ? hit->name : "");
        return false;
    }

    u64 pa = hw ? (v & S2_TARGET) : (v & S2_TARGET_L4);
    u64 end = pa + size;
    if (hw && !(ram_set && inside(pa, end, ram_start, ram_end)) && mmio_hit(pa, end)) {
        /* Device memory is Device-nGnRnE in stage 2, whatever the guest's stage 1 says. */
        v = (v & ~S2_MEMATTR) | FIELD_PREP(S2_MEMATTR, S2_MEMATTR_NGNRE);
    }
    const char *why = pa_rule(from, pa, size, hw, FIELD_GET(S2_MEMATTR, v),
                              hw && FIELD_GET(S2_AP, v) == S2_AP_RO);
    if (why) {
        printf("HV: T8152: refusing stage-2 map IPA 0x%lx -> PA 0x%lx +0x%lx: %s\n", from, pa, size,
               why);
        return false;
    }
    *to = v;
    return true;
}

/*
 * HCR_EL2.PTW (D3 run 1): the AIC passthrough is the one stage-2 Device leaf.  With PTW clear
 * a stage-1 table walk -- the guest's own, or an AT the carrier runs on a guest VA -- that
 * reaches it is performed as a Normal Non-cacheable read of AIC registers (Arm ARM, HCR_EL2.PTW);
 * with PTW set it is a stage-2 Permission fault instead, reported in PAR_EL1 for an AT.  No
 * guest keeps translation tables in the AIC, so this changes nothing for a working guest.
 */
u64 hv_t8152_hcr_filter(u64 hcr)
{
    if (!armed)
        return hcr;
    return (hcr | HCR_TWI | HCR_TWE | HCR_PTW) & ~HCR_FWB;
}

struct verify_ctx {
    unsigned int leaves, hw, sw, hooks, bad;
};

static bool verify_leaf(void *opaque, u64 ipa, u64 size, u64 pte, int level)
{
    struct verify_ctx *ctx = opaque;
    const char *why = NULL;
    u64 pa = 0;
    bool hw = pte & S2_VALID;
    u64 type = FIELD_GET(S2_SPTE_TYPE, pte);

    ctx->leaves++;
    if (level < 2) {
        why = "block entry at L1";
    } else if (hw) {
        ctx->hw++;
        pa = pte & S2_TARGET;
        why = pa_rule(ipa, pa, size, true, FIELD_GET(S2_MEMATTR, pte),
                      FIELD_GET(S2_AP, pte) == S2_AP_RO);
    } else if (type == SPTE_MAP) {
        ctx->sw++;
        pa = level == 4 ? (pte & S2_TARGET_L4) : (pte & S2_TARGET);
        why = pa_rule(ipa, pa, size, false, 0, false);
    } else if (type == SPTE_HOOK) {
        ctx->hooks++;
    } else {
        ctx->hooks++;
        why = "host (proxy) hook: the host would access PA = IPA";
    }

    if (why) {
        if (ctx->bad < T8152_REPORT_LIMIT)
            printf("HV: T8152: stage-2 L%d leaf IPA 0x%lx +0x%lx (PA 0x%lx, pte 0x%lx): %s\n",
                   level, ipa, size, pa, pte, why);
        ctx->bad++;
    }
    return true;
}

int hv_t8152_verify(u64 entry, u64 bootargs, u64 secondary_hcr, bool fb_shadow)
{
    int bad = 0;

    if (!armed) {
        printf("HV: T8152: policy not armed; refusing guest entry\n");
        return -1;
    }
    if (!ram_set) {
        printf("HV: T8152: no guest RAM window was set; refusing guest entry\n");
        bad++;
    } else {
        if (!inside(entry, entry + 4, ram_start, ram_end)) {
            printf("HV: T8152: entry 0x%lx is outside guest RAM\n", entry);
            bad++;
        }
        if (!inside(bootargs, bootargs + 1, ram_start, ram_end)) {
            printf("HV: T8152: boot_args 0x%lx are outside guest RAM\n", bootargs);
            bad++;
        }
    }
    if (!fb_shadow) {
        printf("HV: T8152: framebuffer=bgra-shadow is required (the scanout is never mapped)\n");
        bad++;
    }
    if (live_cpus_check()) /* again: nothing may have left the spin table since hv_init() */
        bad++;
    u64 hcr = mrs(HCR_EL2);
    if (!(hcr & HCR_TWI) || !(secondary_hcr & HCR_TWI) || !(hcr & HCR_TWE) ||
        !(secondary_hcr & HCR_TWE)) {
        printf("HV: T8152: HCR_EL2.TWI/TWE are not both set (boot 0x%lx, secondaries 0x%lx)\n", hcr,
               secondary_hcr);
        bad++;
    }
    if ((hcr | secondary_hcr) & HCR_FWB) {
        printf("HV: T8152: HCR_EL2.FWB is set\n");
        bad++;
    }
    if (!(hcr & HCR_PTW) || !(secondary_hcr & HCR_PTW)) {
        printf("HV: T8152: HCR_EL2.PTW is not set (boot 0x%lx, secondaries 0x%lx)\n", hcr,
               secondary_hcr);
        bad++;
    }
    if (!(hcr & HCR_VM) || !(hcr & HCR_TSC)) {
        printf("HV: T8152: stage 2 or the SMC trap is off (HCR_EL2 0x%lx)\n", hcr);
        bad++;
    }

    struct verify_ctx ctx = {0};
    hv_pt_for_each_leaf(verify_leaf, &ctx);
    if (ctx.bad) {
        printf("HV: T8152: %u stage-2 leaves break the allowlist\n", ctx.bad);
        bad++;
    }

    if (bad) {
        printf("HV: T8152: %d entry precondition(s) failed; the guest will not start\n", bad);
        return -1;
    }
    printf("HV: T8152: stage 2 verified: %u leaves (%u hw, %u sw map, %u hooks), RAM [0x%lx, "
           "0x%lx), %u exception(s), %u MMIO passthrough (nGnRnE), %u denied ranges\n",
           ctx.leaves, ctx.hw, ctx.sw, ctx.hooks, ram_start, ram_end, allow_count, mmio_allow_count,
           deny_count);
    return 0;
}

u64 hv_t8152_guest_actlr_read(void)
{
    return mrs(SYS_ACTLR_EL12);
}

void hv_t8152_guest_actlr_write(u64 value)
{
    if (mrs(SYS_ACTLR_EL12) != value)
        msr(SYS_ACTLR_EL12, value);
}
