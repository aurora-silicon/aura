/* SPDX-License-Identifier: MIT */
/*
 * Internal display handoff, for SoCs whose internal DCP iBoot leaves running.
 *
 * iBoot starts the internal DCP, lights the panel and leaves the coprocessor running. aura does
 * not restart, reconfigure or stop it here (display.c refuses its own DCP modeset path on T6030).
 * This file only describes the state iBoot left, for the next stage. Everything that differs
 * between SoCs (addresses, firmware image, boards, device-tree names, which steps run) is in
 * struct dcp_soc.
 *
 * T6030 (M3 Pro), always, on every boot and for every kernel:
 *  - Reserve (no-map, by physical address only) the __OS_LOG buffers of dcp, dcpext0 and
 *    dcpext1. iBoot places them in RAM that is otherwise handed to the OS, and the running
 *    coprocessors keep writing to them. They are not DART addresses: their "remap" repeats the
 *    physical address, which falls inside the DCP's own framebuffer mapping, so they get no
 *    iommu-addresses.
 *  - Log the DCP CPU state and the dart-dcp and dart-disp0 PROTECT registers, then lock dart-dcp
 *    (as dt_set_display() already does for dart-disp0), so that an OS DART driver cannot reset
 *    the page tables the running DCP uses.
 *  - Walk the dart-dcp SID 5 and dart-disp0 SID 0 / SID 4 page tables, read only, and check every
 *    mapping against the ADT firmware segments. Unarmed this is a dry run: the result is only
 *    logged.
 *
 * T6030 armed (boot.bin config line chosen.asahi,t6030-dcp=1), and only if every check below
 * passes:
 *  - one /reserved-memory node (no-map) per iop-dcp-nub segment, with iommu-addresses for exactly
 *    the mappings found in the page tables (SID 5 for dcp, SID 0 for the display, SID 4 for the
 *    piodma child), in the 1 TiB alias form of the DARTs' apple,dma-range;
 *  - memory-region(-names) on dcp, display and piodma, apple,firmware-uuid/-version/-compat on
 *    dcp;
 *  - the marker apple,t6030-handoff = <1> on dcp, display and piodma.
 *  The display nodes stay "disabled": the shared boot.bin DTB also boots kernels that cannot adopt
 *  locked DARTs, so only a kernel that knows the marker may enable them.
 *
 * T6030 external display processors, dcpext0 and, if the ADT and the device tree have it,
 * dcpext1 (see dcpext6_insts[]): with chosen asahi,t6030-dcpext=1 and the panel handoff
 * published, each processor's firmware memory (apple,t6030-dcpext-memory-ready); with chosen
 * asahi,t6030-dispext=1, that memory plus its empty, locked display DART and a scanout child
 * (apple,t6030-dispext-handoff).
 *
 * T8122 (M3): the __OS_LOG reservations on every boot (oslog_always), and nothing else unless the
 * boot.bin config line chosen.asahi,t8122-dcp=1 is set. Armed, it does a dry run: the ADT values a
 * T8122 handoff and device tree need are logged (including every clock-gate and power-gate of the
 * display nodes resolved through the ADT's PMGR table, the panel properties and the PMP nub's
 * values), the __OS_LOG buffers are reserved, and the DCP state and page tables are read and
 * checked as on T6030, with addresses, firmware segments and DART windows taken from the ADT
 * instead of fixed values. The display power state is taken from the ADT too: the power state
 * registers the clock-gates of /arm-io/dcp resolve to (a virtual device by its nearest real
 * parents). The DCP and its DARTs are only read while all of them are on.
 *
 * T8122 armed now pins the DCP image the J613/J615 iBoot loads and leaves running (the
 * iop-dcp-nub uuid below), so the same generalised handoff publishes for the Air as for T6030:
 * with every check passing it locks dart-dcp, reserves the firmware segments, adds the
 * iommu-addresses from the live page tables, and sets memory-region(-names),
 * apple,firmware-uuid/-version/-compat and the marker apple,t8122-handoff on dcp, display and
 * piodma. Everything (addresses, segments, DART windows, the display power state) is read from
 * the live ADT at boot, nothing is fixed from a capture. Fail-safe: if the running image's uuid
 * is not the pinned one, the display power domain is not on, or a page-table walk maps outside
 * the firmware segments, it publishes nothing and the Mac boots on the framebuffer. The display
 * nodes stay "disabled": only a kernel that sees apple,t8122-handoff may enable them, and a T8122
 * kernel device tree without display nodes is normal. No T8122 PMP is handed off here.
 *
 * T6031 (M3 Max, J514C/J516C only; nothing at all on another board or chip, so not on a T6034):
 * the T8122 handoff with T6031 values, armed by chosen.asahi,t6031-dcp=1, as on T6030 and T8122.
 * On a J516C dt_set_m3_defaults() (kboot.c) sets it unless a config line set it; a J514C still
 * needs the boot.bin config line. The kernel decides whether to take the display over (a kernel
 * may leave that to its command line), so the handoff must be harmless when ignored: the nodes
 * stay disabled and only the marker and the memory description are added. On every boot, armed or
 * not, the __OS_LOG buffers of dcp and dcpext0..3, taken from the ADT, are reserved. Armed, the
 * DCP, DART and display power state are read and checked as on T8122, against the T6031 addresses
 * (the display block sits at 0x3_8400_0000), and only with every check passing is dart-dcp locked
 * and the handoff published with marker apple,t6031-handoff. The DCP image is not pinned here:
 * apple,firmware-uuid is the running image's ADT uuid, and the kernel's board record decides
 * whether it knows that image. No ADT description is logged (the ADT reaches Linux whole, and the
 * log ring is 16K); the DART stream registers are.
 *
 * T6041 display block of the M4 Pro (chip T6040, J616s and J614s), with the desktop 26.6.2 DCP firmware:
 * the T6031 handoff with T6041 values. There is no /chosen opt-in: the handoff is attempted on
 * every J616s and J614s boot whose OS firmware matches the known
 * profile, and whose device tree describes the T6041 display block (aliases dcp and disp0,
 * compatible apple,t6041-dcp and apple,display-subsystem). Anything else gets nothing at all: no
 * __OS_LOG reservation, no register access, no DT edit. On a supported boot the __OS_LOG
 * reservation and every check below run as on T6031, and the handoff is published only if all of
 * them pass. The DCP maps through dart-dcp SID 23 and the display and its piodma through dart-disp0
 * SIDs 0 and 16, of 32 streams each, so these SIDs are per SoC (struct dcp_soc .sids). Marker
 * apple,t6041-handoff; the nodes stay disabled.
 *
 * Fail-soft: dt_set_display_handoff() always returns 0; a failed check is logged and changes
 * nothing. Transactional: DT edits are made on a scratch copy that replaces the DT only if every
 * step succeeded. The __OS_LOG reservations and the armed handoff are separate transactions.
 */

#include "kboot.h"
#include "adt.h"
#include "exception.h"
#include "firmware.h"
#include "malloc.h"
#include "mcc.h"
#include "pmgr.h"
#include "soc.h"
#include "string.h"
#include "t8122_25g83.h"
#include "utils.h"

#include "libfdt/libfdt.h"

#define dlog(fmt, ...) printf("DCP: %s: " fmt, dcp6.soc->name, ##__VA_ARGS__)
/*
 * Routine details of a handoff that went as expected (registers, segments, mappings). They
 * are printed only in DEBUG builds: on a boot console they cost about a second.
 */
#define dlog_debug(fmt, ...) dprintf("DCP: %s: " fmt, dcp6.soc->name, ##__VA_ARGS__)

#define bail(...)                                                                                  \
    do {                                                                                           \
        dlog(__VA_ARGS__);                                                                         \
        return -1;                                                                                 \
    } while (0)

/* T6030 firmware image the handoff is written for (iop-dcp-nub uuid, the 14.x DCP interface) */
#define DCP6_UUID "DDF38191-93B3-324A-BC8F-643006F5AC82"

/* T8122 (J613/J615) DCP image iBoot loads and leaves running (iop-dcp-nub uuid, 14.7 interface) */
#define DCP8_UUID "90F849E1-B422-367E-B389-50246F8DEC47"

/* T6030 J516S/J514S runtime ADT (arm-io ranges applied); checked against the ADT first */
#define DCP6_DART_VM_BASE 0x10000000000UL /* DARTs' DVA alias: 1 TiB + 36-bit DVA */
#define DCP6_DART_VM_SIZE 0x1000000000UL

#define DCP6_SID_DCP    5
#define DCP6_SID_DISP   0
#define DCP6_SID_PIODMA 4

#define ASC_CPU_CONTROL     0x44
#define ASC_CPU_STATUS      0x48
#define ASC_CPU_CONTROL_RUN BIT(4)

#define PMGR_AUTO_ENABLE BIT(28)
#define PMGR_PS_ACTUAL   GENMASK(7, 4)
#define PMGR_PS_TARGET   GENMASK(3, 0)

#define DART_T8110_PROTECT          0x200
#define DART_T8110_PROTECT_TTBR_TCR BIT(0)
#define DART_T8110_TCR(sid)         (0x1000 + 4 * (sid))
#define DART_T8110_TCR_TRANSLATE    BIT(0)
#define DART_T8110_TCR_BYPASS_DART  BIT(1)
#define DART_T8110_TCR_BYPASS_DAPF  BIT(2)
#define DART_T8110_TCR_FOUR_LEVEL   BIT(3)
#define DART_T8110_TTBR(sid)        (0x1400 + 4 * (sid))
#define DART_T8110_TTBR_VALID       BIT(0)
#define DART_T8110_TTBR_ADDR        GENMASK(29, 2)
#define DART_T8110_TTBR_SHIFT       14
#define DART_T8110_STREAMS          16 /* T6030, T8122, T6031 */
#define DART_STREAMS_MAX            32

/* Two-level tables of 16K pages cover the 36-bit DVA space: 2048 x 32 MiB, 2048 x 16 KiB */
#define DART_PTE_VALID   BIT(0)
#define DART_PTE_ADDR    GENMASK(39, 10)
#define DART_PTE_SHIFT   4
#define DART_PAGE        SZ_16K
#define DART_ENTRIES     2048
#define DART_L1_SHIFT    25
#define DART_L2_SHIFT    14
#define DART_DVA_MASK    ((1UL << 36) - 1)
#define DART_PT_MAX_SIZE SZ_1M

#define DCP6_SEGS         8 /* T6030 */
#define DCP6_SEGS_MAX     12
#define DCP6_SEG_MAX_SIZE (256 * SZ_1M)
#define DCP6_OSLOG_MAX    SZ_1M
#define DCP6_MAX_RUNS     16
#define DCP6_NAME_LEN     48
#define DCP6_DEF_LEN      16
#define DCP6_NUBS_MAX     10
#define DCP6_NUB          "/arm-io/dcp/iop-dcp-nub"
#define DCP6_PS_MAX       8  /* power states (or parents) followed per ADT gate list */
#define DCP6_GATE_DEPTH   8  /* PMGR parent levels followed */
#define DCP6_GATES_MAX    32 /* entries per clock-gates/power-gates list */
#define DCP6_VALUE_MAX    64 /* longest panel/dcp property value printed */
#define DCP6_PMP_VALUE    16 /* longest iop-pmp-nub property value printed */

#define SEG_FLAGS_DATA  0
#define SEG_FLAGS_TEXT  1
#define SEG_FLAGS_HEAP  2
#define SEG_FLAGS_OSLOG 0xa

/* Segment indices of the T6030 dcp layout, and of the T6030 dcpext0 layout (its first four) */
enum { SEG_TEXT, SEG_DATA, SEG_OSLOG, SEG_3, SEG_4, SEG_5, SEG_VRAM, SEG_7 };

struct dcp6_seg_def {
    u32 flags;
    int region_id;
    const char *node;   /* /reserved-memory node name, before the unit address */
    const char *name;   /* memory-region-names entry */
    const char *compat; /* compatible of the reserved-memory node, or NULL */
};

/*
 * T6030 iop-dcp-nub segment-ranges, in ADT order. Every segment except __OS_LOG is also a carveout
 * from /chosen/carveout-memory-map (region-id-14 is /vram, the boot framebuffer).
 */
static const struct dcp6_seg_def dcp6_seg_defs_t6030[DCP6_SEGS] = {
    {SEG_FLAGS_TEXT, 49, "dcp-text", "asc-firmware", "apple,asc-mem"},
    {SEG_FLAGS_DATA, 50, "dcp-data", "dcp_data", "apple,asc-mem"},
    {SEG_FLAGS_OSLOG, -1, "dcp-oslog", "os_log", NULL},
    {SEG_FLAGS_HEAP, 57, "dcp-seg3", "region57", "apple,asc-mem"},
    {SEG_FLAGS_HEAP, 94, "dcp-seg4", "region94", "apple,asc-mem"},
    {SEG_FLAGS_HEAP, 95, "dcp-seg5", "region95", "apple,asc-mem"},
    {SEG_FLAGS_HEAP, 14, "framebuffer", "framebuffer", "framebuffer"},
    {SEG_FLAGS_HEAP, 157, "dcp-seg7", "region157", "apple,asc-mem"},
};

#define DCP6_OSLOG_NODE "dcp-oslog"
#define DCP6_OSLOG_PROP "apple,dcp-os-log"
/* (address, size) of each __OS_LOG buffer found on the loaded file part of aura */
#define DCP6_OVERLAP_PROP "asahi,aura-oslog-overlap"

/* Owners of the nubs whose __OS_LOG is reserved: /arm-io/<owner>/iop-<owner>-nub */
static const char *const dcp6_nubs_t6030[] = {"dcp", "dcpext0", "dcpext1"};

/* One SoC's internal display handoff */
struct dcp_soc {
    const char *name;          /* log prefix */
    u32 chip;                  /* chip_id */
    const char *compat;        /* root compatible of the device trees this entry serves */
    const char *const *boards; /* root compatibles the handoff is written for, NULL-terminated */
    const char *boards_desc;
    const char *dcp_compat;
    const char *disp_compat;
    const char *marker; /* set to <1> on dcp, display and piodma by the handoff */
    const char *optin;  /* /chosen property: the boot.bin config line chosen.<optin>=1 arms;
                           NULL: none, the chip, board, firmware and device tree gates decide */
    bool always;        /* reserve, lock and check on every boot, not only when armed; otherwise
                           dart-dcp is only locked by a handoff whose checks have all passed */
    bool oslog_always;  /* reserve the __OS_LOG buffers on every boot, even when not armed */
    bool describe;      /* log the ADT values the handoff and the device tree need */
    bool adt_psreg;     /* the display power state is what /arm-io/dcp's ADT clock-gates name */
    bool fw25;          /* exact J613 25G83 profile */
    bool ext;           /* T6030 dcpextN memory and dispextN DART handoffs */
    bool board_gate;    /* nothing at all unless chip_id is .chip and the root is one of .boards */
    bool any_uuid;      /* no image pinned: the running image's ADT uuid is published */
    bool log_streams;   /* log the DART stream registers, without the ADT description */
    bool sids;          /* the stream IDs and stream count below; otherwise SIDs 5/0/4 of 16 */
    bool oslog_in_rsv;  /* an __OS_LOG wholly inside a /memreserve/ is still given its node */
    u8 sid_dcp, sid_disp, sid_piodma, streams;
    const char *uuid; /* DCP firmware image; NULL: none pinned, dry run only (unless any_uuid) */
    u32 os_fw;        /* enum fw_version the kernel's DCP interface is written for */
    const char *os_fw_name;
    /* Nonzero: the ADT must say the same. Zero: taken from the ADT. */
    u64 coproc, dart_dcp, dart_disp;
    u64 vm_base, vm_size; /* both DARTs' DVA window (vm-base, vm-size, apple,dma-range) */
    u64 pmgr_disp_cpu;    /* display CPU power state register (with adt_psreg: compared only) */
    const struct dcp6_seg_def *segs; /* fixed iop-dcp-nub layout, or NULL: derived from the ADT */
    int nsegs;
    const char *const *nubs; /* owners whose __OS_LOG is reserved, or NULL: every dcp/dcpextN */
    int nnubs;
};

static const char *const dcp_boards_t6030[] = {"apple,j516s", "apple,j514s", NULL};

static const struct dcp_soc dcp_soc_t6030 = {
    .name = "T6030",
    .chip = T6030,
    .compat = "apple,t6030",
    .boards = dcp_boards_t6030,
    .boards_desc = "J516S/J514S",
    .dcp_compat = "apple,t6030-dcp",
    .disp_compat = "apple,t6030-display-subsystem",
    .marker = "apple,t6030-handoff",
    .optin = "asahi,t6030-dcp",
    .always = true,
    .ext = true,
    .uuid = DCP6_UUID,
    .os_fw = V14_7,
    .os_fw_name = "14.7",
    .coproc = 0x28ec00000UL,
    .dart_dcp = 0x28d30c000UL,
    .dart_disp = 0x28d304000UL,
    .vm_base = DCP6_DART_VM_BASE,
    .vm_size = DCP6_DART_VM_SIZE,
    .pmgr_disp_cpu = 0x350710000UL,
    .segs = dcp6_seg_defs_t6030,
    .nsegs = DCP6_SEGS,
    .nubs = dcp6_nubs_t6030,
    .nnubs = ARRAY_SIZE(dcp6_nubs_t6030),
};

/*
 * T8122 (M3 MacBook Air J613/J615). Addresses, segments, DART windows and the display power state
 * come from the ADT. pmgr_disp_cpu is ps_disp_cpu of the open T8122 PMGR device tree (pmgr
 * 0x2d0700000, state 0x10000); it is only compared with the ADT's, for the device tree. The DCP
 * image iBoot leaves running is pinned (DCP8_UUID), so the generalised handoff publishes when
 * armed and every check passes; otherwise it publishes nothing and the Mac keeps the framebuffer.
 */
static const char *const dcp_boards_t8122[] = {"apple,j613", "apple,j615", NULL};

static const struct dcp_soc dcp_soc_t8122 = {
    .name = "T8122",
    .chip = T8122,
    .compat = "apple,t8122",
    .boards = dcp_boards_t8122,
    .boards_desc = "J613/J615",
    .dcp_compat = "apple,t8122-dcp",
    .disp_compat = "apple,t8122-display-subsystem",
    .marker = "apple,t8122-handoff",
    .optin = "asahi,t8122-dcp",
    /*
     * On a J613 the dcp __OS_LOG buffer was seen inside the RAM handed to Linux on every boot
     * checked, and armed J613 boots have shown the reservation. Reserve it on every T8122 boot,
     * as on T6030: it only keeps the running DCP's buffer away from Linux.
     */
    .oslog_always = true,
    .describe = true,
    .adt_psreg = true,
    .uuid = DCP8_UUID,
    .os_fw = V14_7,
    .os_fw_name = "14.7",
    .pmgr_disp_cpu = 0x2d0710000UL,
};

/* Both Air models use the exact 25G83 DCP profile; J615 also requires its opt-in. */
static const char *const dcp_boards_j613[] = {"apple,j613", "apple,j615", NULL};

static const struct dcp_soc dcp_soc_j613_25g83 = {
    .name = "J613-25G83",
    .chip = T8122,
    .compat = "apple,t8122",
    .boards = dcp_boards_j613,
    .boards_desc = "J613/J615",
    .dcp_compat = "apple,t8122-dcp",
    .disp_compat = "apple,t8122-display-subsystem",
    .marker = "apple,j613-25g83-mapping-handoff",
    .optin = "asahi,t8122-dcp",
    .oslog_always = true,
    .describe = true,
    .adt_psreg = true,
    .fw25 = true,
    .uuid = "C042E95C-B9D8-3F0E-94B3-582A08AA6FDD",
    .os_fw = V26_6_2,
    .os_fw_name = "26.6.2",
    .pmgr_disp_cpu = 0x2d0710000UL,
};

/* Explicit kernel profile; neither system firmware nor a topology test selects it. */
static bool dcp25_selected(void *dt)
{
    int node = fdt_path_offset(dt, "dcp"), len;
    const fdt32_t *profile =
        node < 0 ? NULL : fdt_getprop(dt, node, "apple,j613-25g83-profile", &len);
    return profile && len == 4 && fdt32_ld(profile) == 1;
}

/*
 * T6031 (M3 Max J514C/J516C). The display block is T6030's moved to 0x3_8400_0000: dcp, dart-dcp
 * and dart-disp0 are checked against these addresses, both DARTs' DVA window against T6030's.
 * pmgr_disp_cpu is ps_disp_cpu of t6031-pmgr.dtsi; the display power state itself is read from
 * the PMGR devices /arm-io/dcp's ADT clock-gates name. Segments come from the ADT, as on T8122.
 */
static const char *const dcp_boards_t6031[] = {"apple,j514c", "apple,j516c", NULL};

static const struct dcp_soc dcp_soc_t6031 = {
    .name = "T6031",
    .chip = T6031,
    .compat = "apple,t6031",
    .boards = dcp_boards_t6031,
    .boards_desc = "J514C/J516C",
    .dcp_compat = "apple,t6031-dcp",
    .disp_compat = "apple,t6031-display-subsystem",
    .marker = "apple,t6031-handoff",
    .optin = "asahi,t6031-dcp",
    .oslog_always = true,
    .adt_psreg = true,
    .board_gate = true,
    .any_uuid = true,
    .log_streams = true,
    .os_fw = V14_7,
    .os_fw_name = "14.7",
    .coproc = 0x386c00000UL,
    .dart_dcp = 0x38530c000UL,
    .dart_disp = 0x385304000UL,
    .vm_base = DCP6_DART_VM_BASE,
    .vm_size = DCP6_DART_VM_SIZE,
    .pmgr_disp_cpu = 0x2922802a0UL,
};

/*
 * T6041 display block of the M4 Pro (T6040, J616s), with the desktop 26.6.2 DCP firmware. The DCP
 * maps through dart-dcp SID 23 and the display and its piodma through dart-disp0 SIDs 0 and 16, of
 * 32 streams each (ADT sid, sid-count, l2-tt-N). The addresses are this ADT's, after the arm-io
 * ranges; segments and the display power state come from the ADT. The DCP image is not pinned:
 * the kernel accepts only the 26.6.2 firmware. No opt-in: nothing happens unless the device tree
 * describes this display block (dcp6_dt_describes()), and the display nodes stay disabled.
 */
static const char *const dcp_boards_t6041[] = {"apple,j616s", "apple,j614s", NULL};

static const struct dcp_soc dcp_soc_t6041 = {
    .name = "T6041",
    .chip = T6040,
    .compat = "apple,t6040",
    .boards = dcp_boards_t6041,
    .boards_desc = "J616s/J614s",
    .dcp_compat = "apple,t6041-dcp",
    .disp_compat = "apple,display-subsystem",
    .marker = "apple,t6041-handoff",
    .adt_psreg = true,
    .board_gate = true,
    .any_uuid = true,
    .log_streams = true,
    .sids = true,
    .oslog_in_rsv = true,
    .sid_dcp = 23,
    .sid_disp = 0,
    .sid_piodma = 16,
    .streams = 32,
    .os_fw = V26_6_2,
    .os_fw_name = "26.6.2",
    .coproc = 0x402e00000UL,
    .dart_dcp = 0x402340000UL,
    .dart_disp = 0x402300000UL,
    .vm_base = DCP6_DART_VM_BASE,
    .vm_size = DCP6_DART_VM_SIZE,
};

static const struct dcp_soc *const dcp_socs[] = {&dcp_soc_t6030, &dcp_soc_t8122, &dcp_soc_t6031,
                                                 &dcp_soc_t6041};

struct dcp6_seg {
    u64 phys;
    u64 size; /* rounded up to 16K */
    u64 remap;
    u32 flags;
};

struct dcp6_run {
    u64 dva; /* 36-bit DART address */
    u64 phys;
    u64 size;
    int seg;
};

struct dcp6_sid {
    const char *what;
    int sid;
    bool ok;
    int nruns;
    struct dcp6_run runs[DCP6_MAX_RUNS];
};

enum { VM_DCP, VM_DISP }; /* dart-dcp, dart-disp0 */

/* Per-kboot state, used by the armed part. Reset on every call. */
static struct {
    const struct dcp_soc *soc;
    u64 coproc, dart_dcp, dart_disp; /* as checked against the ADT */
    u64 vm_base[2], vm_size[2];      /* DVA windows of dart-dcp and dart-disp0 */
    bool vm_ok;
    int nnubs;
    const char *nubs[DCP6_NUBS_MAX];
    int nsegs, seg_oslog, seg_vram;
    const struct dcp6_seg_def *defs; /* the SoC's layout, or derived[] */
    struct dcp6_seg_def derived[DCP6_SEGS_MAX];
    char derived_names[DCP6_SEGS_MAX][2][DCP6_DEF_LEN]; /* heap segments' names */
    bool mmio_ok;     /* display domain on and the register reads below did not fault */
    bool dcp_running; /* ASC CPU_CONTROL RUN */
    bool dcp_locked;  /* dart-dcp PROTECT_TTBR_TCR set (by iBoot or by us) */
    bool lock_due;    /* not always: dart-dcp is to be locked once the handoff's checks pass */
    bool disp_locked; /* dart-disp0 PROTECT_TTBR_TCR set */
    bool segs_ok;     /* iop-dcp-nub segments parsed and checked */
    struct dcp6_seg segs[DCP6_SEGS_MAX];
    struct dcp6_sid sid[3]; /* dcp SID 5, display SID 0, piodma SID 4 */
    char uuid[40];          /* the DCP image the handoff publishes, once checked */
} dcp6;

/* ---- small helpers ---- */

static int dcp6_sid_dcp(void)
{
    return dcp6.soc->sids ? dcp6.soc->sid_dcp : DCP6_SID_DCP;
}

static int dcp6_sid_disp(void)
{
    return dcp6.soc->sids ? dcp6.soc->sid_disp : DCP6_SID_DISP;
}

static int dcp6_sid_piodma(void)
{
    return dcp6.soc->sids ? dcp6.soc->sid_piodma : DCP6_SID_PIODMA;
}

static int dcp6_streams(void)
{
    return dcp6.soc->sids ? dcp6.soc->streams : DART_T8110_STREAMS;
}

static bool dcp6_overlaps(u64 a, u64 a_size, u64 b, u64 b_size)
{
    u64 a_end = a + a_size < a ? ~0UL : a + a_size;
    u64 b_end = b + b_size < b ? ~0UL : b + b_size;

    return a_size && b_size && a < b_end && b < a_end;
}

static int dcp6_adt_u32(int node, const char *name, u32 *val)
{
    u32 len = 0;
    const void *p = adt_getprop(adt, node, name, &len);
    if (!p || len != sizeof(*val))
        return -1;
    memcpy(val, p, sizeof(*val));
    return 0;
}

static int dcp6_adt_u64(int node, const char *name, u64 *val)
{
    u32 len = 0;
    const void *p = adt_getprop(adt, node, name, &len);
    if (!p || len != sizeof(*val))
        return -1;
    memcpy(val, p, sizeof(*val));
    return 0;
}

static int dcp6_adt_u64_pair(int node, const char *name, u64 *a, u64 *b)
{
    u64 v[2];
    u32 len = 0;
    const void *p = adt_getprop(adt, node, name, &len);
    if (!p || len != sizeof(v))
        return -1;
    memcpy(v, p, sizeof(v));
    *a = v[0];
    *b = v[1];
    return 0;
}

/* reg[idx] of an ADT node, with the /arm-io ranges applied */
static int dcp6_adt_reg(const char *path, int idx, u64 *addr)
{
    int adt_path[8];
    int node = adt_path_offset_trace(adt, path, adt_path);
    if (node < 0)
        return -1;
    return adt_get_reg(adt, adt_path, "reg", idx, addr, NULL);
}

/* An ADT address against the SoC's: equal to it, or, with none given, a 16K-aligned address */
static bool dcp6_addr_is(u64 adt_addr, u64 want)
{
    if (want)
        return adt_addr == want;
    return adt_addr && !(adt_addr & (SZ_16K - 1));
}

static int dcp6_dram(u64 *base, u64 *size)
{
    int chosen = adt_path_offset(adt, "/chosen");
    if (chosen < 0 || dcp6_adt_u64(chosen, "dram-base", base) ||
        dcp6_adt_u64(chosen, "dram-size", size) || !*size || *base + *size <= *base)
        bail("no usable /chosen dram-base/dram-size\n");
    return 0;
}

/* Name number idx in a ';'-separated ADT segment-names list */
static bool dcp6_seg_name_is(const char *names, u32 len, int idx, const char *want)
{
    u32 i = 0;
    while (idx--) {
        while (i < len && names[i] && names[i] != ';')
            i++;
        if (i >= len || names[i] != ';')
            return false;
        i++;
    }
    size_t n = strlen(want);
    return i + n <= len && !memcmp(names + i, want, n) &&
           (i + n == len || names[i + n] == ';' || names[i + n] == 0);
}

/* An ADT uuid string: 8-4-4-4-12 hexadecimal digits and its NUL, exactly */
static bool dcp6_is_uuid(const char *s, u32 len)
{
    if (!s || len != 37 || s[36])
        return false;
    for (int i = 0; i < 36; i++) {
        char c = s[i];
        bool hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
        if ((i == 8 || i == 13 || i == 18 || i == 23) ? c != '-' : !hex)
            return false;
    }
    return true;
}

/* DART address of a segment's remap: the 36-bit DVA */
static u64 dcp6_dva(u64 remap)
{
    return remap & DART_DVA_MASK;
}

/* A 36-bit DVA in the form a DART's apple,dma-range and iommu-addresses use */
static u64 dcp6_alias(int vm, u64 dva)
{
    return (dcp6.vm_base[vm] & ~DART_DVA_MASK) | dva;
}

static int dcp6_guard_begin(void)
{
    int before = exc_count;
    exc_guard = GUARD_MARK | GUARD_SILENT;
    return before;
}

/* Returns -1 if a synchronous abort or an SError was taken since dcp6_guard_begin() */
static int dcp6_guard_end(int before)
{
    sysop("dsb sy");
    sysop("isb");
    exc_guard = GUARD_OFF;
    return exc_count == before ? 0 : -1;
}

/* Whether [base, base+size) overlaps an enabled /reserved-memory node or a /memreserve/ entry */
static int dcp6_resv_conflict(void *dt, u64 base, u64 size, const char *what)
{
    int n = fdt_num_mem_rsv(dt);
    for (int i = 0; i < n; i++) {
        u64 a, s;
        if (!fdt_get_mem_rsv(dt, i, &a, &s) && dcp6_overlaps(base, size, a, s))
            bail("%s 0x%lx+0x%lx overlaps /memreserve/ 0x%lx+0x%lx\n", what, base, size, a, s);
    }

    int resv = fdt_path_offset(dt, "/reserved-memory");
    if (resv < 0)
        return 0;
    int node;
    fdt_for_each_subnode(node, dt, resv)
    {
        const char *status = fdt_getprop(dt, node, "status", NULL);
        if (status && !strcmp(status, "disabled"))
            continue;
        int len;
        const fdt64_t *reg = fdt_getprop(dt, node, "reg", &len);
        if (!reg || len <= 0)
            continue;
        for (int k = 0; (k + 2) * (int)sizeof(fdt64_t) <= len; k += 2) {
            u64 a = fdt64_ld(&reg[k]), s = fdt64_ld(&reg[k + 1]);
            if (dcp6_overlaps(base, size, a, s)) {
                const char *name = fdt_get_name(dt, node, NULL);
                bail("%s 0x%lx+0x%lx overlaps /reserved-memory/%s\n", what, base, size,
                     name ? name : "?");
            }
        }
    }
    return 0;
}

/*
 * As dcp6_resv_conflict(), for an __OS_LOG buffer: with .oslog_in_rsv, a /memreserve/ that
 * contains it whole is no conflict (the buffer is already kept from the OS; the no-map node only
 * describes it). On the J616s the buffers sit inside the aura retained range on tether boots.
 */
static void *dcp6_scratch(void *dt);

static int dcp6_oslog_conflict(void *dt, u64 base, u64 size)
{
    if (!dcp6.soc->oslog_in_rsv)
        return dcp6_resv_conflict(dt, base, size, "__OS_LOG");

    int n = fdt_num_mem_rsv(dt);
    for (int i = 0; i < n; i++) {
        u64 a, s;
        if (fdt_get_mem_rsv(dt, i, &a, &s) || !dcp6_overlaps(base, size, a, s))
            continue;
        if (base < a || base + size > a + s)
            bail("__OS_LOG 0x%lx+0x%lx straddles /memreserve/ 0x%lx+0x%lx\n", base, size, a, s);
        dlog_debug("__OS_LOG 0x%lx+0x%lx lies inside /memreserve/ 0x%lx+0x%lx\n", base, size, a, s);
    }

    /* the /reserved-memory part, with no /memreserve/ entries in the way */
    void *copy = dcp6_scratch(dt);
    if (!copy)
        return -1;
    while (fdt_num_mem_rsv(copy) > 0)
        fdt_del_mem_rsv(copy, 0);
    int ret = dcp6_resv_conflict(copy, base, size, "__OS_LOG");
    free(copy);
    return ret;
}

static int dcp6_carveout_conflict(u64 base, u64 size, const char *what)
{
    for (size_t j = 0; j < mcc_carveout_count; j++)
        if (dcp6_overlaps(base, size, mcc_carveouts[j].base, mcc_carveouts[j].size))
            bail("%s 0x%lx+0x%lx overlaps memory carveout 0x%lx+0x%lx\n", what, base, size,
                 mcc_carveouts[j].base, mcc_carveouts[j].size);
    return 0;
}

/*
 * Existing /reserved-memory/<name> with exactly this no-map reg: returns its offset (the
 * reservation is already there), -FDT_ERR_NOTFOUND if there is no such node, or -FDT_ERR_EXISTS
 * if a node of that name exists with other contents (or the lookup failed).
 */
static int dcp6_find_resv(void *dt, const char *name, u64 base, u64 size)
{
    int resv = fdt_path_offset(dt, "/reserved-memory");
    if (resv < 0)
        return -FDT_ERR_EXISTS;
    int node = fdt_subnode_offset(dt, resv, name);
    if (node == -FDT_ERR_NOTFOUND)
        return node;
    if (node < 0)
        return -FDT_ERR_EXISTS;
    int len;
    const fdt64_t *reg = fdt_getprop(dt, node, "reg", &len);
    if (!reg || len != 2 * sizeof(fdt64_t) || fdt64_ld(&reg[0]) != base ||
        fdt64_ld(&reg[1]) != size || !fdt_getprop(dt, node, "no-map", NULL))
        return -FDT_ERR_EXISTS;
    return node;
}

/* Add /reserved-memory/<name> (reg, no-map, phandle). Returns its phandle, 0 on failure. */
static u32 dcp6_add_resv(void *dt, const char *name, u64 base, u64 size, const char *compat)
{
    fdt64_t reg[2];
    u32 phandle;

    int resv = fdt_path_offset(dt, "/reserved-memory");
    if (resv < 0)
        return 0;
    int node = fdt_add_subnode(dt, resv, name);
    if (node < 0)
        return 0;
    fdt64_st(&reg[0], base);
    fdt64_st(&reg[1], size);
    int err = fdt_generate_phandle(dt, &phandle);
    if (!err)
        err = fdt_setprop_u32(dt, node, "phandle", phandle);
    if (!err)
        err = fdt_setprop(dt, node, "reg", reg, sizeof(reg));
    if (!err && compat)
        err = fdt_setprop_string(dt, node, "compatible", compat);
    if (!err)
        err = fdt_setprop_empty(dt, node, "no-map");
    return err ? 0 : phandle;
}

/* A scratch copy of the DT, for an all-or-nothing edit */
static void *dcp6_scratch(void *dt)
{
    int size = fdt_totalsize(dt);
    void *scratch = malloc(size);
    if (!scratch) {
        dlog("no memory for a DT copy (%d bytes)\n", size);
        return NULL;
    }
    memcpy(scratch, dt, size);
    return scratch;
}

/* Replace dt by scratch (same buffer size) if it is a valid DT; frees scratch either way */
static int dcp6_commit(void *dt, void *scratch)
{
    int ret = -1;
    if (fdt_check_header(scratch) == 0 && fdt_totalsize(scratch) == fdt_totalsize(dt)) {
        memcpy(dt, scratch, fdt_totalsize(scratch));
        ret = 0;
    } else {
        dlog("DT copy is not valid, nothing changed\n");
    }
    free(scratch);
    return ret;
}

/* ---- the SoC's nubs ---- */

/* The iop nub path of an owner such as dcp or dcpext0 */
static void dcp6_nub_path(char *path, size_t len, const char *owner)
{
    snprintf(path, len, "/arm-io/%s/iop-%s-nub", owner, owner);
}

/*
 * The nubs whose __OS_LOG is reserved: the SoC's list, or every iop nub of dcp, dcpext and
 * dcpext0..7 that the ADT has.
 */
static void dcp6_find_nubs(void)
{
    static const char *const owners[] = {
        "dcp",     "dcpext",  "dcpext0", "dcpext1", "dcpext2",
        "dcpext3", "dcpext4", "dcpext5", "dcpext6", "dcpext7",
    };
    const struct dcp_soc *soc = dcp6.soc;

    dcp6.nnubs = 0;
    if (soc->nubs) {
        for (int i = 0; i < soc->nnubs && i < DCP6_NUBS_MAX; i++)
            dcp6.nubs[dcp6.nnubs++] = soc->nubs[i];
        return;
    }
    for (size_t i = 0; i < ARRAY_SIZE(owners) && dcp6.nnubs < DCP6_NUBS_MAX; i++) {
        char path[DCP6_NAME_LEN];
        dcp6_nub_path(path, sizeof(path), owners[i]);
        if (adt_path_offset(adt, path) >= 0)
            dcp6.nubs[dcp6.nnubs++] = owners[i];
    }
}

/* ---- the reserved __OS_LOG buffers ---- */

/*
 * The __OS_LOG segment of one nub: flags 0xa, named __OS_LOG, remap == phys (a physical buffer),
 * 16K aligned, inside DRAM. Returns 0 and the reservation (size rounded up to 16K).
 */
static int dcp6_get_oslog(const char *owner, const char *nub_path, u64 *phys, u64 *size)
{
    int nub = adt_path_offset(adt, nub_path);
    if (nub < 0)
        bail("%s: no %s\n", owner, nub_path);

    u32 len = 0, names_len = 0;
    const struct adt_segment_ranges *seg = adt_getprop(adt, nub, "segment-ranges", &len);
    const char *names = adt_getprop(adt, nub, "segment-names", &names_len);
    if (!seg || !len || len % sizeof(*seg) || len > 16 * sizeof(*seg) || !names)
        bail("%s: missing or bad segment-ranges/-names (len %u)\n", owner, len);

    int found = -1;
    for (u32 i = 0; i < len / sizeof(*seg); i++) {
        struct adt_segment_ranges r;
        memcpy(&r, &seg[i], sizeof(r));
        if (r.unk != SEG_FLAGS_OSLOG)
            continue;
        if (found >= 0)
            bail("%s: more than one __OS_LOG segment\n", owner);
        found = i;
        if (!dcp6_seg_name_is(names, names_len, i, "__OS_LOG"))
            bail("%s: segment %u has __OS_LOG flags but another name\n", owner, i);
        if (r.remap != r.phys)
            bail("%s: __OS_LOG remap 0x%lx is not its physical address 0x%lx\n", owner, r.remap,
                 r.phys);
        *phys = r.phys;
        *size = ALIGN_UP((u64)r.size, SZ_16K);
    }
    if (found < 0)
        bail("%s: no __OS_LOG segment\n", owner);

    u64 dram_base, dram_size;
    if (dcp6_dram(&dram_base, &dram_size))
        return -1;
    if (!*phys || !*size || *size > DCP6_OSLOG_MAX || (*phys & (SZ_16K - 1)) || *phys < dram_base ||
        *phys + *size > dram_base + dram_size)
        bail("%s: bad __OS_LOG 0x%lx+0x%lx\n", owner, *phys, *size);
    return 0;
}

/*
 * The running coprocessors write their __OS_LOG buffers from before aura starts. Stage 1 loads
 * this image at its own base and start.S then copies the file part of it away (aura-raw.ld keeps
 * that part below 0x120000, where the panel DCP's buffer was seen with the stage 1 these builds
 * were checked with). If a buffer lies on the loaded file part, the coprocessor may have written
 * over aura's code or data before the copy: say so loudly, and mark it in /chosen for the OS
 * side. The buffer is still reserved, because the coprocessor keeps writing it; nothing else can
 * be undone at this point.
 */
static bool dcp6_oslog_on_file_part(const char *owner, u64 phys, u64 size, fdt64_t *marker)
{
    u64 base, len;

    kboot_loaded_file_part(&base, &len);
    if (!dcp6_overlaps(phys, size, base, len))
        return false;
    dlog("WARNING: %s __OS_LOG 0x%lx+0x%lx overlaps aura where stage 1 loaded it (0x%lx+0x%lx)\n",
         owner, phys, size, base, len);
    dlog("WARNING: its coprocessor may have changed aura there; aura was only checked with a "
         "stage 1 that leaves the first 0x120000 bytes clear\n");
    marker[0] = cpu_to_fdt64(phys);
    marker[1] = cpu_to_fdt64(size);
    return true;
}

static __attribute__((noinline)) void dcp6_reserve_oslogs(void *dt)
{
    struct {
        u64 phys, size;
        bool add;
        char name[DCP6_NAME_LEN];
    } logs[DCP6_NUBS_MAX];
    int nadd = 0, have = 0, nover = 0;
    fdt64_t over[DCP6_NUBS_MAX][2];

    for (int i = 0; i < dcp6.nnubs; i++) {
        char nub[DCP6_NAME_LEN];
        logs[i].add = false;
        dcp6_nub_path(nub, sizeof(nub), dcp6.nubs[i]);
        if (dcp6_get_oslog(dcp6.nubs[i], nub, &logs[i].phys, &logs[i].size))
            continue;
        if (dcp6_oslog_on_file_part(dcp6.nubs[i], logs[i].phys, logs[i].size, over[nover]))
            nover++;
        snprintf(logs[i].name, sizeof(logs[i].name), DCP6_OSLOG_NODE "@%lx", logs[i].phys);

        int existing = dcp6_find_resv(dt, logs[i].name, logs[i].phys, logs[i].size);
        if (existing >= 0) {
            have++;
            continue;
        }
        if (existing != -FDT_ERR_NOTFOUND) {
            dlog("%s: /reserved-memory/%s exists with other contents\n", dcp6.nubs[i],
                 logs[i].name);
            continue;
        }

        bool bad = false;
        for (int j = 0; j < i; j++)
            if (logs[j].add &&
                dcp6_overlaps(logs[i].phys, logs[i].size, logs[j].phys, logs[j].size)) {
                dlog("%s: __OS_LOG overlaps %s's\n", dcp6.nubs[i], dcp6.nubs[j]);
                bad = true;
            }
        if (bad || dcp6_oslog_conflict(dt, logs[i].phys, logs[i].size) ||
            dcp6_carveout_conflict(logs[i].phys, logs[i].size, "__OS_LOG"))
            continue;
        logs[i].add = true;
        nadd++;
    }

    if (nover) {
        int chosen = fdt_path_offset(dt, "/chosen");
        if (chosen < 0 || fdt_setprop(dt, chosen, DCP6_OVERLAP_PROP, over, nover * sizeof(over[0])))
            dlog("could not set /chosen/" DCP6_OVERLAP_PROP "\n");
    }

    if (!nadd) {
        if (have)
            dlog("%d __OS_LOG reservation(s) already present\n", have);
        return;
    }

    void *scratch = dcp6_scratch(dt);
    if (!scratch)
        return;
    for (int i = 0; i < dcp6.nnubs; i++) {
        if (!logs[i].add)
            continue;
        u32 ph = dcp6_add_resv(scratch, logs[i].name, logs[i].phys, logs[i].size, NULL);
        int resv = fdt_path_offset(scratch, "/reserved-memory");
        int node = resv < 0 ? resv : fdt_subnode_offset(scratch, resv, logs[i].name);
        if (!ph || node < 0 || fdt_setprop_empty(scratch, node, DCP6_OSLOG_PROP)) {
            dlog("could not add the __OS_LOG reservations (DT full?), none added\n");
            free(scratch);
            return;
        }
    }
    if (dcp6_commit(dt, scratch))
        return;

    for (int i = 0; i < dcp6.nnubs; i++)
        if (logs[i].add)
            dlog_debug("%s __OS_LOG reserved: /reserved-memory/%s (0x%lx+0x%lx, no-map)\n",
                       dcp6.nubs[i], logs[i].name, logs[i].phys, logs[i].size);
}

/* ---- ADT description (dry run) ---- */

static void dcp6_describe_u32s(int node, const char *path, const char *prop)
{
    u32 len = 0;
    const u32 *v = adt_getprop(adt, node, prop, &len);
    if (!v || !len || len % 4) {
        dlog("ADT %s %s: %s\n", path, prop, v ? "not a u32 list" : "absent");
        return;
    }
    dlog("ADT %s %s:", path, prop);
    for (u32 i = 0; i < len / 4 && i < 32; i++) {
        u32 x;
        memcpy(&x, &v[i], sizeof(x));
        printf(" %u", x);
    }
    printf("%s\n", len / 4 > 32 ? " ..." : "");
}

static void dcp6_describe_str(int node, const char *path, const char *prop)
{
    u32 len = 0;
    const char *s = adt_getprop(adt, node, prop, &len);
    if (!s || !len) {
        dlog("ADT %s %s: absent\n", path, prop);
        return;
    }
    /* NUL-separated lists are shown with ',' */
    char buf[96];
    u32 n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    for (u32 i = 0; i < n; i++)
        buf[i] = s[i] ? s[i] : ',';
    while (n && buf[n - 1] == ',')
        n--;
    buf[n] = 0;
    dlog("ADT %s %s: \"%s\"%s\n", path, prop, buf, len > sizeof(buf) - 1 ? " ..." : "");
}

static void dcp6_describe_u64(int node, const char *path, const char *prop)
{
    u64 v;
    if (dcp6_adt_u64(node, prop, &v))
        dlog("ADT %s %s: absent\n", path, prop);
    else
        dlog("ADT %s %s: 0x%lx\n", path, prop, v);
}

/* Property names, eight per line, each line with the log prefix */
static void dcp6_describe_names(int node, const char *path)
{
    int n = 0;
    ADT_FOREACH_PROPERTY(adt, node, prop)
    {
        if (n % 8 == 0)
            dlog("ADT %s properties:", path);
        printf(" %s", prop->name);
        if (++n % 8 == 0)
            printf("\n");
    }
    if (n % 8)
        printf("\n");
}

/* Text: printable characters, NUL-terminated, NULs only between non-empty strings */
static bool dcp6_is_text(const u8 *v, u32 len)
{
    if (len < 2 || !v[0] || v[len - 1])
        return false;
    for (u32 i = 0; i + 1 < len; i++) {
        if (!v[i] ? !v[i + 1] : (v[i] < 0x20 || v[i] > 0x7e))
            return false;
    }
    return true;
}

/*
 * One ADT property: its size and, if it is at most `max` bytes, its value as little-endian u32
 * words (or bytes, for a length that is not a multiple of 4), followed by the text if it is text
 */
static void dcp6_describe_bytes(const char *path, const char *name, const u8 *v, u32 len, u32 max)
{
    dlog("ADT %s %s: %u bytes", path, name, len);
    if (!len || len > max || len > DCP6_VALUE_MAX) {
        printf("\n");
        return;
    }
    printf(",");
    if (len % 4 == 0) {
        for (u32 i = 0; i < len; i += 4) {
            u32 x;
            memcpy(&x, v + i, sizeof(x));
            printf(" 0x%x", x);
        }
    } else {
        printf(" ");
        for (u32 i = 0; i < len; i++)
            printf("%02x", v[i]);
    }
    if (dcp6_is_text(v, len)) {
        char text[DCP6_VALUE_MAX];
        for (u32 i = 0; i + 1 < len; i++)
            text[i] = v[i] ? v[i] : ',';
        text[len - 1] = 0;
        printf(" \"%s\"", text);
    }
    printf("\n");
}

static void dcp6_describe_value(int node, const char *path, const char *name, u32 max)
{
    u32 len = 0;
    const u8 *v = adt_getprop(adt, node, name, &len);
    if (!v)
        dlog("ADT %s %s: absent\n", path, name);
    else
        dcp6_describe_bytes(path, name, v, len, max);
}

/*
 * The PMGR devices that `g` depends on: its parents, their parents and so on, breadth first, at
 * most DCP6_PS_MAX of them. ADT and PMGR table only.
 */
static void dcp6_describe_parents(const struct pmgr_gate *g)
{
    u16 ids[DCP6_PS_MAX];
    int n = 0, more = 0;

    /* Pass 0 takes g's parents, pass k the parents of ids[k - 1]; n never exceeds DCP6_PS_MAX */
    for (int pass = 0; pass <= n; pass++) {
        const u16 *parent = g->parent;
        struct pmgr_gate p;
        if (pass > 0) {
            if (pmgr_device_gate(g->die, ids[pass - 1], &p) < 0 || !p.found)
                continue;
            parent = p.parent;
        }
        for (int k = 0; k < 2; k++) {
            bool seen = !parent[k] || parent[k] == g->id;
            for (int j = 0; j < n && !seen; j++)
                seen = ids[j] == parent[k];
            if (seen)
                continue;
            if (n < DCP6_PS_MAX)
                ids[n++] = parent[k];
            else
                more = 1;
        }
    }
    for (int j = 0; j < n; j++) {
        struct pmgr_gate p;
        printf("%s", j ? ", " : "; parents ");
        if (pmgr_device_gate(g->die, ids[j], &p) < 0 || !p.found)
            printf("0x%x (not a PMGR device)", ids[j]);
        else if (p.virt)
            printf("%.16s (virtual)", p.name);
        else
            printf("%.16s 0x%lx", p.name, p.psreg);
    }
    if (more)
        printf(", ...");
}

/*
 * Every entry of the clock-gates or power-gates list of an ADT node, resolved through the ADT's
 * PMGR table: the entry, the device's name, its power state register (or that it is virtual) and
 * the devices it depends on. ADT and PMGR table only: no register is read here.
 */
static void dcp6_describe_gates(const char *path, const char *prop)
{
    for (u32 i = 0; i < DCP6_GATES_MAX; i++) {
        struct pmgr_gate g;
        if (pmgr_adt_gate(path, prop, i, &g) < 0)
            break;
        dlog("ADT %s %s[%u]: 0x%x", path, prop, i, g.entry);
        if (!g.found) {
            printf(" not a PMGR device\n");
            continue;
        }
        if (g.virt)
            printf(" %.16s, virtual", g.name);
        else if (g.psreg)
            printf(" %.16s, power state 0x%lx", g.name, g.psreg);
        else
            printf(" %.16s, no power state register", g.name);
        dcp6_describe_parents(&g);
        printf("\n");
    }
}

/* compatible, every reg (translated), interrupts, clock-gates and power-gates (resolved) */
static int dcp6_describe_node(const char *path)
{
    int p[8];
    int node = adt_path_offset_trace(adt, path, p);
    if (node < 0) {
        dlog("ADT %s: absent\n", path);
        return -1;
    }
    dcp6_describe_str(node, path, "compatible");
    for (int i = 0; i < 16; i++) {
        u64 a, s;
        if (adt_get_reg(adt, p, "reg", i, &a, &s) < 0)
            break;
        dlog("ADT %s reg[%d]: 0x%lx+0x%lx\n", path, i, a, s);
    }
    if (adt_getprop(adt, node, "interrupts", NULL))
        dcp6_describe_u32s(node, path, "interrupts");
    dcp6_describe_gates(path, "clock-gates");
    dcp6_describe_gates(path, "power-gates");
    return node;
}

/*
 * The panel properties of /arm-io/disp0 and the small properties of /arm-io/dcp: values of the
 * small ones, sizes only of the tables
 */
static void dcp6_describe_panel(void)
{
    static const char *const disp0_values[] = {
        "dot-pitch",   "display-default-color", "max-scaling-ratio", "color-accuracy-index",
        "KSF_version",
    };
    static const char *const disp0_sizes[] = {
        "display-timing-info",
        "temperature-compensation",
        "bics-param-set",
    };
    static const char *const dcp_values[] = {"hdcp-channels", "role", "iop-version"};
    static const char dp_switch[] = "dp-switch-";

    int node = adt_path_offset(adt, "/arm-io/disp0");
    if (node >= 0) {
        for (size_t i = 0; i < ARRAY_SIZE(disp0_values); i++)
            dcp6_describe_value(node, "/arm-io/disp0", disp0_values[i], DCP6_VALUE_MAX);
        for (size_t i = 0; i < ARRAY_SIZE(disp0_sizes); i++)
            dcp6_describe_value(node, "/arm-io/disp0", disp0_sizes[i], 0);
    }
    node = adt_path_offset(adt, "/arm-io/dcp");
    if (node >= 0) {
        for (size_t i = 0; i < ARRAY_SIZE(dcp_values); i++)
            dcp6_describe_value(node, "/arm-io/dcp", dcp_values[i], DCP6_VALUE_MAX);
        ADT_FOREACH_PROPERTY(adt, node, prop)
        {
            if (!strncmp(prop->name, dp_switch, sizeof(dp_switch) - 1))
                dcp6_describe_bytes("/arm-io/dcp", prop->name, prop->value, prop->size,
                                    DCP6_VALUE_MAX);
        }
        dcp6_describe_value(node, "/arm-io/dcp", "audio", 0);
    }
}

/*
 * The values the T6030 PMP export (dt_fill_pmp() in kboot.c) passes, read the same way: board-id
 * and dram-vendor-id of /chosen, and every property of iop-pmp-nub, with its value if it is at
 * most 16 bytes. Logged only: nothing is passed to the OS for this SoC.
 */
static void dcp6_describe_pmp(void)
{
    static const char path[] = "/arm-io/pmp/iop-pmp-nub";

    int chosen = adt_path_offset(adt, "/chosen");
    if (chosen >= 0) {
        dcp6_describe_value(chosen, "/chosen", "board-id", DCP6_PMP_VALUE);
        dcp6_describe_value(chosen, "/chosen", "dram-vendor-id", DCP6_PMP_VALUE);
    }
    int nub = adt_path_offset(adt, path);
    if (nub < 0) {
        dlog("ADT %s: absent\n", path);
        return;
    }
    dlog("ADT %s: %d properties\n", path, adt_get_property_count(adt, nub));
    ADT_FOREACH_PROPERTY(adt, nub, prop)
    {
        dcp6_describe_bytes(path, prop->name, prop->value, prop->size, DCP6_PMP_VALUE);
    }
}

static void dcp6_describe_dart(const char *path)
{
    int node = dcp6_describe_node(path);
    if (node < 0)
        return;
    dcp6_describe_u64(node, path, "vm-base");
    dcp6_describe_u64(node, path, "vm-size");
    for (int sid = 0; sid < dcp6_streams(); sid++) {
        char prop[24];
        u64 lo, hi;
        snprintf(prop, sizeof(prop), "pt-region-%d", sid);
        if (!dcp6_adt_u64_pair(node, prop, &lo, &hi))
            dlog("ADT %s %s: 0x%lx..0x%lx\n", path, prop, lo, hi);
    }
}

static void dcp6_describe_segs(const char *path)
{
    int nub = adt_path_offset(adt, path);
    if (nub < 0) {
        dlog("ADT %s: absent\n", path);
        return;
    }
    dcp6_describe_str(nub, path, "uuid");
    u32 v;
    dlog("ADT %s pre-loaded: %d, running: %d\n", path,
         dcp6_adt_u32(nub, "pre-loaded", &v) ? -1 : (int)v,
         dcp6_adt_u32(nub, "running", &v) ? -1 : (int)v);
    dcp6_describe_str(nub, path, "segment-names");

    u32 len = 0;
    const struct adt_segment_ranges *seg = adt_getprop(adt, nub, "segment-ranges", &len);
    if (!seg || len % sizeof(*seg)) {
        dlog("ADT %s segment-ranges: absent or not whole entries (len %u)\n", path, len);
        return;
    }
    for (u32 i = 0; i < len / sizeof(*seg) && i < DCP6_SEGS_MAX; i++) {
        struct adt_segment_ranges r;
        memcpy(&r, &seg[i], sizeof(r));
        dlog("ADT %s segment %u: phys 0x%lx iova 0x%lx remap 0x%lx size 0x%x flags 0x%x\n", path, i,
             r.phys, r.iova, r.remap, r.size, r.unk);
    }
}

/*
 * The ADT values a device tree and a handoff for this SoC need, for the serial console. Read
 * only, ADT memory only: no device is accessed here.
 */
static __attribute__((noinline)) void dcp6_describe(void)
{
    static const char *const nodes[] = {
        "/arm-io/dcp",      "/arm-io/disp0", "/arm-io/dcpext", "/arm-io/dcpext0",
        "/arm-io/dispext0", "/arm-io/pmp",   "/arm-io/smc",
    };
    static const char *const darts[] = {
        "/arm-io/dart-dcp",     "/arm-io/dart-disp0",    "/arm-io/dart-dcpext",
        "/arm-io/dart-dcpext0", "/arm-io/dart-dispext0", "/arm-io/dart-pmp",
    };
    static const char *const nubs[] = {
        DCP6_NUB,
        "/arm-io/dcpext/iop-dcpext-nub",
        "/arm-io/dcpext0/iop-dcpext0-nub",
        "/arm-io/pmp/iop-pmp-nub",
    };

    dlog("dry run: ADT description follows\n");
    for (size_t i = 0; i < ARRAY_SIZE(nodes); i++)
        dcp6_describe_node(nodes[i]);
    for (size_t i = 0; i < ARRAY_SIZE(darts); i++)
        dcp6_describe_dart(darts[i]);
    for (size_t i = 0; i < ARRAY_SIZE(nubs); i++)
        dcp6_describe_segs(nubs[i]);
    dcp6_describe_pmp();

    int node = adt_path_offset(adt, "/arm-io/disp0");
    if (node >= 0)
        dcp6_describe_names(node, "/arm-io/disp0");
    node = adt_path_offset(adt, "/arm-io/dcp");
    if (node >= 0)
        dcp6_describe_names(node, "/arm-io/dcp");
    dcp6_describe_panel();

    u64 vram;
    if (dcp6_adt_reg("/vram", 0, &vram))
        dlog("ADT /vram: absent\n");
    else
        dlog("ADT /vram reg[0]: 0x%lx; boot framebuffer 0x%lx\n", vram, kboot_fb_base());
    dlog("usable RAM 0x%lx+0x%lx; %zu memory carveouts\n", kboot_ram_base(), kboot_ram_size(),
         mcc_carveout_count);
    dlog("dry run: ADT description ends\n");
}

/* ---- DCP CPU state, DART lock state, dart-dcp lock ---- */

/* A power state is on: actually on, or targeted on with automatic power management */
static bool dcp6_ps_on(u32 ps)
{
    return FIELD_GET(PMGR_PS_ACTUAL, ps) == PMGR_PS_ACTIVE ||
           (FIELD_GET(PMGR_PS_TARGET, ps) == PMGR_PS_ACTIVE && (ps & PMGR_AUTO_ENABLE));
}

struct dcp6_ps {
    u64 addr;
    const char *name;
};

/* Add g's power state register, or, for a virtual device, those of its nearest real parents */
static int dcp6_ps_add(struct dcp6_ps *ps, int *n, const struct pmgr_gate *g, int depth)
{
    if (!g->found)
        return -1;
    if (!g->virt) {
        if (!g->psreg)
            return -1;
        for (int i = 0; i < *n; i++)
            if (ps[i].addr == g->psreg)
                return 0;
        if (*n >= DCP6_PS_MAX)
            return -1;
        ps[(*n)++] = (struct dcp6_ps){g->psreg, g->name};
        return 0;
    }
    if (depth >= DCP6_GATE_DEPTH)
        return -1;
    for (int k = 0; k < 2; k++) {
        struct pmgr_gate p;
        if (!g->parent[k])
            continue;
        if (pmgr_device_gate(g->die, g->parent[k], &p) < 0 || dcp6_ps_add(ps, n, &p, depth + 1))
            return -1;
    }
    return 0;
}

/*
 * The display power state from the ADT: the power state registers that the clock-gates of
 * /arm-io/dcp resolve to through the ADT's PMGR table, a virtual device by its nearest real
 * parents. Returns how many, or -1 if an entry does not resolve or none has a register.
 */
static int dcp6_adt_ps(struct dcp6_ps *ps)
{
    int n = 0;
    for (u32 i = 0; i < DCP6_GATES_MAX; i++) {
        struct pmgr_gate g;
        if (pmgr_adt_gate("/arm-io/dcp", "clock-gates", i, &g) < 0)
            break;
        if (dcp6_ps_add(ps, &n, &g, 0))
            bail("ADT /arm-io/dcp clock-gates[%u] 0x%x does not resolve to PMGR power states, not "
                 "touching the DCP\n",
                 i, g.entry);
    }
    if (!n)
        bail("no ADT /arm-io/dcp clock-gate has a PMGR power state, not touching the DCP\n");
    return n;
}

/*
 * Read (only) every display power state register the ADT names. The DCP and its DARTs are only
 * accessed if all of them are on.
 */
static int dcp6_adt_power_on(void)
{
    struct dcp6_ps ps[DCP6_PS_MAX];
    u32 val[DCP6_PS_MAX];
    bool all_on = true, dt_same = false;

    int n = dcp6_adt_ps(ps);
    if (n < 0)
        return -1;

    int g = dcp6_guard_begin();
    for (int i = 0; i < n; i++)
        val[i] = read32(ps[i].addr);
    if (dcp6_guard_end(g))
        bail("reading the display power state faulted, not touching the DCP\n");

    for (int i = 0; i < n; i++) {
        bool on = dcp6_ps_on(val[i]);
        dlog("display power state (ADT /arm-io/dcp): %.16s 0x%lx = 0x%x, %s\n", ps[i].name,
             ps[i].addr, val[i], on ? "on" : "NOT on");
        all_on &= on;
        dt_same |= ps[i].addr == dcp6.soc->pmgr_disp_cpu;
    }
    if (dcp6.soc->pmgr_disp_cpu)
        dlog("open PMGR DT display CPU state 0x%lx: %s\n", dcp6.soc->pmgr_disp_cpu,
             dt_same ? "the same register" : "NOT one of these");
    if (!all_on)
        bail("display power domain not active, not touching the DCP\n");
    return 0;
}

/* Dry run: the DART streams in use, read only */
static void dcp6_describe_streams(const char *what, u64 base)
{
    u32 tcr[DART_STREAMS_MAX], ttbr[DART_STREAMS_MAX];

    int g = dcp6_guard_begin();
    for (int sid = 0; sid < dcp6_streams(); sid++) {
        tcr[sid] = read32(base + DART_T8110_TCR(sid));
        ttbr[sid] = read32(base + DART_T8110_TTBR(sid));
    }
    if (dcp6_guard_end(g)) {
        dlog("%s: reading the stream registers faulted\n", what);
        return;
    }
    for (int sid = 0; sid < dcp6_streams(); sid++)
        if (tcr[sid] || ttbr[sid])
            dlog_debug("%s SID %d: TCR 0x%x TTBR 0x%x\n", what, sid, tcr[sid], ttbr[sid]);
}

/* Sets dart-dcp's PROTECT_TTBR_TCR (prot: PROTECT as read), which stays set until reset */
static bool dcp6_lock_dcp(u32 prot)
{
    int g = dcp6_guard_begin();
    write32(dcp6.dart_dcp + DART_T8110_PROTECT, prot | DART_T8110_PROTECT_TTBR_TCR);
    u32 after = read32(dcp6.dart_dcp + DART_T8110_PROTECT);
    if (dcp6_guard_end(g) || !(after & DART_T8110_PROTECT_TTBR_TCR)) {
        dlog("locking dart-dcp failed (PROTECT 0x%x)\n", after);
        return false;
    }
    dcp6.dcp_locked = true;
    dlog("dart-dcp locked (PROTECT 0x%x)\n", after);
    return true;
}

static void dcp6_check_hw(void)
{
    const struct dcp_soc *soc = dcp6.soc;
    u64 coproc, dart_dcp, dart_disp;
    int node = adt_path_offset(adt, "/arm-io/dart-dcp");
    int node2 = adt_path_offset(adt, "/arm-io/dart-disp0");

    if (node < 0 || node2 < 0 || !adt_is_compatible(adt, node, "dart,t8110") ||
        !adt_is_compatible(adt, node2, "dart,t8110") || dcp6_adt_reg("/arm-io/dcp", 0, &coproc) ||
        dcp6_adt_reg("/arm-io/dart-dcp", 0, &dart_dcp) ||
        dcp6_adt_reg("/arm-io/dart-disp0", 0, &dart_disp) || !dcp6_addr_is(coproc, soc->coproc) ||
        !dcp6_addr_is(dart_dcp, soc->dart_dcp) || !dcp6_addr_is(dart_disp, soc->dart_disp)) {
        dlog("ADT dcp/dart-dcp/dart-disp0 not as expected, not touching them\n");
        return;
    }
    dcp6.coproc = coproc;
    dcp6.dart_dcp = dart_dcp;
    dcp6.dart_disp = dart_disp;

    /*
     * The DCP blocks are only accessed while the display power domain is on: actually on, or
     * targeted on with automatic power management (which ungates on access)
     */
    int g;
    if (soc->adt_psreg) {
        if (dcp6_adt_power_on())
            return;
    } else {
        g = dcp6_guard_begin();
        u32 ps = read32(soc->pmgr_disp_cpu);
        bool on = dcp6_ps_on(ps);
        if (dcp6_guard_end(g) || !on) {
            dlog("display power domain not active (0x%x), not touching the DCP\n", ps);
            return;
        }
    }

    g = dcp6_guard_begin();
    u32 ctl = read32(dcp6.coproc + ASC_CPU_CONTROL);
    u32 status = read32(dcp6.coproc + ASC_CPU_STATUS);
    u32 prot_dcp = read32(dcp6.dart_dcp + DART_T8110_PROTECT);
    u32 prot_disp = read32(dcp6.dart_disp + DART_T8110_PROTECT);
    if (dcp6_guard_end(g)) {
        dlog("reading the DCP/DART registers faulted, not touching them again\n");
        return;
    }

    dcp6.mmio_ok = true;
    dcp6.dcp_running = ctl & ASC_CPU_CONTROL_RUN;
    dcp6.disp_locked = prot_disp & DART_T8110_PROTECT_TTBR_TCR;
    dlog("CPU_CONTROL 0x%x (%s) STATUS 0x%x; PROTECT dart-dcp 0x%x dart-disp0 0x%x\n", ctl,
         dcp6.dcp_running ? "running" : "NOT running", status, prot_dcp, prot_disp);

    if (soc->describe || soc->log_streams) {
        dcp6_describe_streams("dart-dcp", dcp6.dart_dcp);
        dcp6_describe_streams("dart-disp0", dcp6.dart_disp);
    }

    if (prot_dcp & DART_T8110_PROTECT_TTBR_TCR) {
        dcp6.dcp_locked = true;
        return;
    }
    if (!dcp6.dcp_running) {
        dlog("DCP not running, leaving dart-dcp unlocked\n");
        return;
    }
    if (!soc->uuid && !soc->any_uuid) {
        dlog("dry run: no %s DCP firmware image is pinned, leaving dart-dcp unlocked\n", soc->name);
        return;
    }
    if (!soc->always) {
        /* The lock stays until reset: only a handoff that passed every check sets it */
        dcp6.lock_due = true;
        dlog("dart-dcp left unlocked until the handoff's checks pass\n");
        return;
    }
    dcp6_lock_dcp(prot_dcp);
}

/* The lock for a handoff whose checks all passed: PROTECT is read again, then set if needed */
static bool dcp6_lock_now(void)
{
    int g = dcp6_guard_begin();
    u32 prot = read32(dcp6.dart_dcp + DART_T8110_PROTECT);
    if (dcp6_guard_end(g)) {
        dlog("reading dart-dcp PROTECT faulted, not locking it\n");
        return false;
    }
    dcp6.lock_due = false;
    if (prot & DART_T8110_PROTECT_TTBR_TCR) {
        dcp6.dcp_locked = true;
        return true;
    }
    return dcp6_lock_dcp(prot);
}

/* ---- firmware segments of the internal DCP ---- */

/* The /chosen/carveout-memory-map region-id-N that is exactly base+size, or -1 */
static int dcp6_find_carveout(int cmap, u64 base, u64 size)
{
    int id = -1, found = 0;

    ADT_FOREACH_PROPERTY(adt, cmap, prop)
    {
        static const char prefix[] = "region-id-";
        u64 v[2];
        if (strncmp(prop->name, prefix, sizeof(prefix) - 1) || prop->size != sizeof(v))
            continue;
        memcpy(v, prop->value, sizeof(v));
        if (v[0] != base || v[1] != size)
            continue;
        const char *d = prop->name + sizeof(prefix) - 1;
        int n = 0, digits = 0;
        while (*d >= '0' && *d <= '9' && digits < 5) {
            n = n * 10 + (*d++ - '0');
            digits++;
        }
        if (!digits || *d)
            continue;
        id = n;
        found++;
    }
    return found == 1 ? id : -1;
}

/*
 * A layout for an SoC without a fixed one: exactly one text, one __DATA and one __OS_LOG segment,
 * every other one a heap segment, the one at /vram the framebuffer. Each segment except __OS_LOG
 * must be exactly one carveout, whose region id names it as on T6030.
 */
static int dcp6_derive_segs(const struct adt_segment_ranges *seg, int n, const char *names,
                            u32 names_len, int cmap)
{
    int text = -1, data = -1, oslog = -1, vram_seg = -1;
    u64 vram;

    if (dcp6_adt_reg("/vram", 0, &vram))
        bail("no /vram\n");

    for (int i = 0; i < n; i++) {
        struct adt_segment_ranges r;
        struct dcp6_seg_def *d = &dcp6.derived[i];

        memcpy(&r, &seg[i], sizeof(r));
        d->flags = r.unk;
        d->region_id = -1;
        d->compat = "apple,asc-mem";
        switch (r.unk) {
            case SEG_FLAGS_TEXT:
                if (text >= 0)
                    bail("dcp segments %d and %d both have text flags\n", text, i);
                text = i;
                d->node = "dcp-text";
                d->name = "asc-firmware";
                break;
            case SEG_FLAGS_DATA:
                if (data >= 0 || !dcp6_seg_name_is(names, names_len, i, "__DATA"))
                    bail("dcp segment %d: data flags, but not the only __DATA\n", i);
                data = i;
                d->node = "dcp-data";
                d->name = "dcp_data";
                break;
            case SEG_FLAGS_OSLOG:
                if (oslog >= 0 || !dcp6_seg_name_is(names, names_len, i, "__OS_LOG"))
                    bail("dcp segment %d: __OS_LOG flags, but not the only __OS_LOG\n", i);
                oslog = i;
                d->node = DCP6_OSLOG_NODE;
                d->name = "os_log";
                d->compat = NULL;
                break;
            case SEG_FLAGS_HEAP:
                if (r.phys == vram) {
                    if (vram_seg >= 0)
                        bail("dcp segments %d and %d are both /vram\n", vram_seg, i);
                    vram_seg = i;
                    d->node = "framebuffer";
                    d->name = "framebuffer";
                    d->compat = "framebuffer";
                }
                break;
            default:
                bail("dcp segment %d has flags 0x%x, not text, data, heap or __OS_LOG\n", i, r.unk);
        }
        if (r.unk == SEG_FLAGS_OSLOG)
            continue;
        d->region_id = dcp6_find_carveout(cmap, r.phys, ALIGN_UP((u64)r.size, SZ_16K));
        if (d->region_id < 0)
            bail("dcp segment %d 0x%lx+0x%x is not exactly one carveout\n", i, r.phys, r.size);
        if (r.unk == SEG_FLAGS_HEAP && i != vram_seg) {
            char *node = dcp6.derived_names[i][0], *name = dcp6.derived_names[i][1];
            snprintf(node, DCP6_DEF_LEN, "dcp-seg%u", (unsigned)i % 100);
            snprintf(name, DCP6_DEF_LEN, "region%u", (unsigned)d->region_id % 100000);
            d->node = node;
            d->name = name;
        }
    }
    if (text < 0 || data < 0 || oslog < 0 || vram_seg < 0)
        bail("dcp segments lack text, __DATA, __OS_LOG or /vram\n");

    dcp6.nsegs = n;
    dcp6.seg_oslog = oslog;
    dcp6.seg_vram = vram_seg;
    dcp6.defs = dcp6.derived;
    for (int i = 0; i < n; i++) {
        if (i == oslog)
            dlog_debug("dcp segment %d: %s, physical buffer\n", i, dcp6.derived[i].name);
        else
            dlog_debug("dcp segment %d: %s, region-id-%d\n", i, dcp6.derived[i].name,
                       dcp6.derived[i].region_id);
    }
    return 0;
}

/*
 * The DVA windows of dart-dcp and dart-disp0: the SoC's, or, for an SoC without one, each DART's
 * ADT vm-base/vm-size, which must lie in one alias of the 36-bit DVA space that two-level tables
 * of 16K pages cover.
 */
static int dcp6_get_vm(void)
{
    static const char *const darts[2] = {"/arm-io/dart-dcp", "/arm-io/dart-disp0"};
    const struct dcp_soc *soc = dcp6.soc;

    for (int k = 0; k < 2; k++) {
        if (soc->vm_size) {
            dcp6.vm_base[k] = soc->vm_base;
            dcp6.vm_size[k] = soc->vm_size;
            continue;
        }
        u64 base, size;
        int node = adt_path_offset(adt, darts[k]);
        if (node < 0 || dcp6_adt_u64(node, "vm-base", &base) ||
            dcp6_adt_u64(node, "vm-size", &size))
            bail("%s: no vm-base/vm-size\n", darts[k]);
        if (!size || ((base | size) & (SZ_16K - 1)) || (base & DART_DVA_MASK) + size < size ||
            (base & DART_DVA_MASK) + size > DART_DVA_MASK + 1)
            bail("%s: vm-base 0x%lx vm-size 0x%lx is not one 36-bit DVA window\n", darts[k], base,
                 size);
        dcp6.vm_base[k] = base;
        dcp6.vm_size[k] = size;
    }
    dcp6.vm_ok = true;
    return 0;
}

static int dcp6_get_segs(void)
{
    const struct dcp_soc *soc = dcp6.soc;
    int nub = adt_path_offset(adt, DCP6_NUB);
    if (nub < 0)
        bail("no %s\n", DCP6_NUB);

    u32 len = 0, names_len = 0;
    const struct adt_segment_ranges *seg = adt_getprop(adt, nub, "segment-ranges", &len);
    const char *names = adt_getprop(adt, nub, "segment-names", &names_len);
    if (soc->segs) {
        if (!seg || len != soc->nsegs * sizeof(*seg) || !names)
            bail("dcp segment-ranges missing or not %d entries (len %u)\n", soc->nsegs, len);
        /* Segment 0 (text) is checked by its flags below */
        if (!dcp6_seg_name_is(names, names_len, SEG_DATA, "__DATA") ||
            !dcp6_seg_name_is(names, names_len, SEG_OSLOG, "__OS_LOG"))
            bail("dcp segments 1 and 2 are not named __DATA and __OS_LOG\n");
    } else if (!seg || !len || len % sizeof(*seg) || len > DCP6_SEGS_MAX * sizeof(*seg) || !names) {
        bail("dcp segment-ranges missing or not 1 to %d entries (len %u)\n", DCP6_SEGS_MAX, len);
    }

    u64 dram_base, dram_size;
    if (dcp6_dram(&dram_base, &dram_size))
        return -1;
    u64 ram_min = kboot_ram_base();
    u64 ram_max = kboot_ram_base() + kboot_ram_size();

    int cmap = adt_path_offset(adt, "/chosen/carveout-memory-map");
    if (cmap < 0)
        bail("no /chosen/carveout-memory-map\n");

    if (soc->segs) {
        dcp6.nsegs = soc->nsegs;
        dcp6.seg_oslog = SEG_OSLOG;
        dcp6.seg_vram = SEG_VRAM;
        dcp6.defs = soc->segs;
    } else if (dcp6_derive_segs(seg, len / sizeof(*seg), names, names_len, cmap)) {
        return -1;
    }
    if (!dcp6.vm_ok && dcp6_get_vm())
        return -1;

    for (int i = 0; i < dcp6.nsegs; i++) {
        struct adt_segment_ranges r;
        struct dcp6_seg *s = &dcp6.segs[i];
        const struct dcp6_seg_def *d = &dcp6.defs[i];

        memcpy(&r, &seg[i], sizeof(r));
        s->phys = r.phys;
        s->size = ALIGN_UP((u64)r.size, SZ_16K);
        s->remap = r.remap;
        s->flags = r.unk;

        if (s->flags != d->flags)
            bail("dcp segment %d flags 0x%x, expected 0x%x\n", i, s->flags, d->flags);
        if (!r.size || s->size > DCP6_SEG_MAX_SIZE || (s->phys & (SZ_16K - 1)) ||
            s->phys < dram_base || s->phys + s->size < s->phys ||
            s->phys + s->size > dram_base + dram_size)
            bail("bad dcp segment %d 0x%lx+0x%x\n", i, s->phys, r.size);

        if (i == dcp6.seg_oslog) {
            if (s->remap != s->phys)
                bail("dcp __OS_LOG remap 0x%lx is not physical\n", s->remap);
        } else {
            /* A DVA in dart-dcp's window, which is one alias of the 36-bit space */
            u64 vm_base = dcp6.vm_base[VM_DCP], vm_end = vm_base + dcp6.vm_size[VM_DCP];
            if (s->remap < vm_base || (s->remap & (SZ_16K - 1)) || s->remap + s->size < s->remap ||
                s->remap + s->size > vm_end ||
                (s->remap & ~DART_DVA_MASK) != (vm_base & ~DART_DVA_MASK))
                bail("dcp segment %d remap 0x%lx is not a DART alias address\n", i, s->remap);
            /* carveouts are outside the RAM the OS gets */
            if (!(s->phys + s->size <= ram_min || s->phys >= ram_max))
                bail("dcp segment %d 0x%lx+0x%lx is in usable RAM\n", i, s->phys, s->size);
            if (dcp6_carveout_conflict(s->phys, s->size, "dcp segment"))
                return -1;

            /* The matching carveout: same base, same (16K rounded) size */
            char prop[32];
            u64 cbase, csize;
            snprintf(prop, sizeof(prop), "region-id-%d", d->region_id);
            if (dcp6_adt_u64_pair(cmap, prop, &cbase, &csize) || cbase != s->phys ||
                csize != s->size)
                bail("dcp segment %d does not match carveout %s\n", i, prop);
        }

        for (int j = 0; j < i; j++) {
            if (dcp6_overlaps(s->phys, s->size, dcp6.segs[j].phys, dcp6.segs[j].size))
                bail("dcp segments %d and %d overlap\n", j, i);
            if (i != dcp6.seg_oslog && j != dcp6.seg_oslog &&
                dcp6_overlaps(s->remap, s->size, dcp6.segs[j].remap, dcp6.segs[j].size))
                bail("dcp segments %d and %d overlap in DVA space\n", j, i);
        }
    }

    /* The framebuffer segment is /vram, where iBoot's boot framebuffer lives */
    u64 vram;
    const struct dcp6_seg *fb = &dcp6.segs[dcp6.seg_vram];
    if (dcp6_adt_reg("/vram", 0, &vram) || vram != fb->phys || kboot_fb_base() < fb->phys ||
        kboot_fb_base() >= fb->phys + fb->size)
        bail("/vram or the boot framebuffer is not dcp segment %d\n", dcp6.seg_vram);

    return 0;
}

/* ---- read-only page-table walk ---- */

static int dcp6_seg_of(u64 phys)
{
    for (int i = 0; i < dcp6.nsegs; i++)
        if (phys >= dcp6.segs[i].phys && phys < dcp6.segs[i].phys + dcp6.segs[i].size)
            return i;
    return -1;
}

/* Record one valid PTE. Runs are contiguous in DVA and PA, and never span two segments. */
static int dcp6_run_add(struct dcp6_sid *s, u64 dva, u64 pa)
{
    int seg = dcp6_seg_of(pa);
    if (seg < 0)
        bail("%s SID %d maps DVA 0x%lx to 0x%lx, outside the firmware segments\n", s->what, s->sid,
             dva, pa);

    if (s->nruns) {
        struct dcp6_run *r = &s->runs[s->nruns - 1];
        if (r->seg == seg && r->dva + r->size == dva && r->phys + r->size == pa) {
            r->size += DART_PAGE;
            return 0;
        }
    }
    if (s->nruns >= DCP6_MAX_RUNS)
        bail("%s SID %d has too many mappings\n", s->what, s->sid);
    s->runs[s->nruns++] = (struct dcp6_run){dva, pa, DART_PAGE, seg};
    return 0;
}

static int dcp6_walk_inner(struct dcp6_sid *s, u64 base, u64 pt_lo, u64 pt_hi)
{
    u32 tcr = read32(base + DART_T8110_TCR(s->sid));
    u32 ttbr = read32(base + DART_T8110_TTBR(s->sid));

    if (!(tcr & DART_T8110_TCR_TRANSLATE) ||
        (tcr & (DART_T8110_TCR_BYPASS_DART | DART_T8110_TCR_FOUR_LEVEL)) ||
        !(ttbr & DART_T8110_TTBR_VALID))
        bail("%s SID %d: TCR 0x%x TTBR 0x%x not a two-level translation\n", s->what, s->sid, tcr,
             ttbr);

    u64 l1 = (u64)FIELD_GET(DART_T8110_TTBR_ADDR, ttbr) << DART_T8110_TTBR_SHIFT;
    if (l1 < pt_lo || l1 + DART_PAGE > pt_hi)
        bail("%s SID %d: L1 table 0x%lx outside pt-region 0x%lx..0x%lx\n", s->what, s->sid, l1,
             pt_lo, pt_hi);

    for (u64 i = 0; i < DART_ENTRIES; i++) {
        u64 e1 = read64(l1 + 8 * i);
        if (!(e1 & DART_PTE_VALID))
            continue;
        u64 l2 = (e1 & DART_PTE_ADDR) << DART_PTE_SHIFT;
        if (l2 < pt_lo || l2 + DART_PAGE > pt_hi)
            bail("%s SID %d: L2 table 0x%lx outside pt-region\n", s->what, s->sid, l2);

        for (u64 j = 0; j < DART_ENTRIES; j++) {
            u64 e2 = read64(l2 + 8 * j);
            if (!(e2 & DART_PTE_VALID))
                continue;
            u64 dva = (i << DART_L1_SHIFT) | (j << DART_L2_SHIFT);
            if (dcp6_run_add(s, dva, (e2 & DART_PTE_ADDR) << DART_PTE_SHIFT))
                return -1;
        }
    }
    return 0;
}

/*
 * Walk one stream's page tables. The TTBR/TCR reads and the table reads are guarded, and a table
 * is only read after its address was found inside the ADT pt-region of that stream, which must be
 * a 16K-aligned DRAM range outside usable RAM and outside the protected carveouts.
 */
static int dcp6_walk(struct dcp6_sid *s, const char *adt_dart, int vm, u64 base)
{
    char prop[24];
    u64 pt_lo, pt_hi, vm_base, vm_size;

    s->nruns = 0;
    int node = adt_path_offset(adt, adt_dart);
    snprintf(prop, sizeof(prop), "pt-region-%d", s->sid);
    if (node < 0 || dcp6_adt_u64_pair(node, prop, &pt_lo, &pt_hi))
        bail("%s: no %s/%s\n", s->what, adt_dart, prop);
    if (dcp6_adt_u64(node, "vm-base", &vm_base) || dcp6_adt_u64(node, "vm-size", &vm_size) ||
        vm_base != dcp6.vm_base[vm] || vm_size != dcp6.vm_size[vm])
        bail("%s: %s vm-base/vm-size not 0x%lx/0x%lx\n", s->what, adt_dart, dcp6.vm_base[vm],
             dcp6.vm_size[vm]);

    u64 dram_base, dram_size;
    u64 ram_min = kboot_ram_base();
    u64 ram_max = kboot_ram_base() + kboot_ram_size();
    if (dcp6_dram(&dram_base, &dram_size))
        return -1;
    if (pt_hi <= pt_lo || pt_hi - pt_lo > DART_PT_MAX_SIZE || ((pt_lo | pt_hi) & (SZ_16K - 1)) ||
        pt_lo < dram_base || pt_hi > dram_base + dram_size ||
        !(pt_hi <= ram_min || pt_lo >= ram_max))
        bail("%s: bad %s 0x%lx..0x%lx\n", s->what, prop, pt_lo, pt_hi);
    if (dcp6_carveout_conflict(pt_lo, pt_hi - pt_lo, prop))
        return -1;

    int g = dcp6_guard_begin();
    int ret = dcp6_walk_inner(s, base, pt_lo, pt_hi);
    if (dcp6_guard_end(g))
        bail("%s SID %d: reading the page tables faulted\n", s->what, s->sid);
    return ret;
}

/*
 * dcp (SID 5) must map exactly the non-log segments, each at its ADT remap address and in full.
 * display (SID 0) and piodma (SID 4) may map only the heap segments (flags 2), each at most once,
 * in full, starting at the segment base, so that one iommu-addresses entry per mapping describes
 * it; the display must map the framebuffer segment.
 */
static int dcp6_check_sid(struct dcp6_sid *s)
{
    bool seen[DCP6_SEGS_MAX] = {false};

    for (int i = 0; i < s->nruns; i++) {
        struct dcp6_run *r = &s->runs[i];
        struct dcp6_seg *g = &dcp6.segs[r->seg];

        if (r->seg == dcp6.seg_oslog)
            bail("%s SID %d maps the __OS_LOG buffer\n", s->what, s->sid);
        if (s != &dcp6.sid[0] && g->flags != SEG_FLAGS_HEAP)
            bail("%s SID %d maps dcp segment %d\n", s->what, s->sid, r->seg);
        if (seen[r->seg])
            bail("%s SID %d maps dcp segment %d twice\n", s->what, s->sid, r->seg);
        seen[r->seg] = true;
        if (r->phys != g->phys || r->size != g->size)
            bail("%s SID %d maps 0x%lx+0x%lx of dcp segment %d (0x%lx+0x%lx)\n", s->what, s->sid,
                 r->phys, r->size, r->seg, g->phys, g->size);
        if (s == &dcp6.sid[0] && r->dva != dcp6_dva(g->remap))
            bail("dcp SID %d maps segment %d at DVA 0x%lx, ADT says 0x%lx\n", s->sid, r->seg,
                 r->dva, g->remap);
    }

    if (s == &dcp6.sid[0]) {
        for (int i = 0; i < dcp6.nsegs; i++)
            if (i != dcp6.seg_oslog && !seen[i])
                bail("dcp SID %d does not map dcp segment %d\n", s->sid, i);
    } else if (s == &dcp6.sid[1] && !seen[dcp6.seg_vram]) {
        bail("display SID %d does not map the framebuffer\n", s->sid);
    }
    return 0;
}

static void dcp6_dry_run(void)
{
    const struct {
        const char *what;
        int sid;
        const char *dart;
        int vm;
        u64 base;
    } sids[3] = {
        {"dcp", dcp6_sid_dcp(), "/arm-io/dart-dcp", VM_DCP, dcp6.dart_dcp},
        {"display", dcp6_sid_disp(), "/arm-io/dart-disp0", VM_DISP, dcp6.dart_disp},
        {"piodma", dcp6_sid_piodma(), "/arm-io/dart-disp0", VM_DISP, dcp6.dart_disp},
    };

    dcp6.segs_ok = !dcp6_get_segs();
    if (!dcp6.segs_ok || !dcp6.mmio_ok)
        return;

    for (int i = 0; i < 3; i++) {
        struct dcp6_sid *s = &dcp6.sid[i];
        s->what = sids[i].what;
        s->sid = sids[i].sid;
        s->ok = !dcp6_walk(s, sids[i].dart, sids[i].vm, sids[i].base) && !dcp6_check_sid(s);
        if (!s->ok)
            continue;
#ifdef DEBUG
        u64 pages = 0;
        for (int j = 0; j < s->nruns; j++)
            pages += s->runs[j].size / DART_PAGE;
        dlog_debug("%s SID %d: %d mappings, %lu pages, all in firmware segments (", s->what, s->sid,
                   s->nruns, pages);
        for (int j = 0; j < s->nruns; j++)
            dprintf("%s%d@0x%lx", j ? " " : "", s->runs[j].seg, s->runs[j].dva);
        dprintf(")\n");
#endif
    }
}

/* ---- armed: the handoff ---- */

struct dcp6_dt {
    char dcp[DCP6_NAME_LEN * 2];
    char display[DCP6_NAME_LEN * 2];
    char piodma[DCP6_NAME_LEN * 2];
};

static int dcp6_status_disabled(void *dt, int node, const char *what)
{
    const char *status = fdt_getprop(dt, node, "status", NULL);
    if (!status || strcmp(status, "disabled"))
        bail("%s is not status \"disabled\"\n", what);
    return 0;
}

/* Parent of `node` has 2 address and 2 size cells */
static int dcp6_cells_2_2(void *dt, int node, const char *what)
{
    int parent = fdt_parent_offset(dt, node);
    if (parent < 0 || fdt_address_cells(dt, parent) != 2 || fdt_size_cells(dt, parent) != 2)
        bail("%s: parent does not have 2 address and 2 size cells\n", what);
    return 0;
}

/*
 * iommus = <&dart sid> with a disabled t8110 DART at `base` whose apple,dma-range is the DVA window
 * vm_base+vm_size. Returns the DART's offset.
 */
static int dcp6_check_iommu(void *dt, int node, const char *what, u32 sid, u64 base, u64 vm_base,
                            u64 vm_size)
{
    int len;
    const fdt32_t *iommus = fdt_getprop(dt, node, "iommus", &len);
    if (!iommus || len != 2 * sizeof(fdt32_t) || fdt32_ld(&iommus[1]) != sid)
        bail("%s iommus is not <&dart %u>\n", what, sid);

    int dart = fdt_node_offset_by_phandle(dt, fdt32_ld(&iommus[0]));
    if (dart < 0 || fdt_node_check_compatible(dt, dart, "apple,t8110-dart"))
        bail("%s: iommus does not point to a t8110 DART\n", what);

    const fdt32_t *cells = fdt_getprop(dt, dart, "#iommu-cells", &len);
    if (!cells || len != sizeof(fdt32_t) || fdt32_ld(cells) != 1)
        bail("%s: DART #iommu-cells is not 1\n", what);

    const fdt64_t *reg = fdt_getprop(dt, dart, "reg", &len);
    if (dcp6_cells_2_2(dt, dart, what) || !reg || len < (int)(2 * sizeof(fdt64_t)) ||
        fdt64_ld(&reg[0]) != base)
        bail("%s: DART reg is not 0x%lx\n", what, base);

    const fdt32_t *range = fdt_getprop(dt, dart, "apple,dma-range", &len);
    if (!range || len != 4 * sizeof(fdt32_t) ||
        (((u64)fdt32_ld(&range[0]) << 32) | fdt32_ld(&range[1])) != vm_base ||
        (((u64)fdt32_ld(&range[2]) << 32) | fdt32_ld(&range[3])) != vm_size)
        bail("%s: DART apple,dma-range is not <0x%lx 0x%lx>\n", what, vm_base, vm_size);

    if (dcp6_status_disabled(dt, dart, "the DART of the display nodes"))
        return -1;
    return dart;
}

/* Node for alias `name`, and its path. Returns the offset or a negative error. */
static int dcp6_alias_path(void *dt, const char *name, char *path, size_t len)
{
    const char *p = fdt_get_alias(dt, name);
    if (!p)
        return -FDT_ERR_NOTFOUND;
    int node = fdt_path_offset(dt, p);
    if (node < 0 || fdt_get_path(dt, node, path, len))
        return -FDT_ERR_BADPATH;
    return node;
}

static bool dcp6_board_ok(void *dt)
{
    for (const char *const *b = dcp6.soc->boards; *b; b++)
        if (!fdt_node_check_compatible(dt, 0, *b))
            return true;
    return false;
}

/* The DT side of the gates: nodes, aliases, cells, iommus, DARTs, status, nothing published yet */
static int dcp6_check_dt(void *dt, struct dcp6_dt *n)
{
    const struct dcp_soc *soc = dcp6.soc;

    if (!dcp6_board_ok(dt))
        bail("not a %s device tree\n", soc->boards_desc);
    if (soc->fw25 && !t8122_25g83_board(dt))
        bail(
            "J615 25G83 is experimental: no display handoff without chosen." T8122_25G83_J615_OPT_IN
            "=1\n");

    char p1[sizeof(n->piodma)], p2[sizeof(n->piodma)];
    int dcp = dcp6_alias_path(dt, "dcp", n->dcp, sizeof(n->dcp));
    int disp = dcp6_alias_path(dt, "disp0", n->display, sizeof(n->display));
    int pio1 = dcp6_alias_path(dt, "disp0-piodma", p1, sizeof(p1));
    int pio2 = dcp6_alias_path(dt, "disp0_piodma", p2, sizeof(p2));
    if (dcp < 0 || disp < 0 || (pio1 < 0 && pio2 < 0))
        bail("device tree has no dcp/disp0/disp0-piodma aliases\n");
    if (pio1 >= 0 && pio2 >= 0 && pio1 != pio2)
        bail("disp0-piodma and disp0_piodma aliases differ\n");
    int pio = pio1 >= 0 ? pio1 : pio2;
    memcpy(n->piodma, pio1 >= 0 ? p1 : p2, sizeof(n->piodma));

    if (fdt_node_check_compatible(dt, dcp, soc->dcp_compat) ||
        fdt_node_check_compatible(dt, disp, soc->disp_compat))
        bail("dcp/disp0 are not %s/%s\n", soc->dcp_compat, soc->disp_compat);
    if (fdt_parent_offset(dt, pio) != dcp)
        bail("piodma is not a child of dcp\n");

    if (dcp6_cells_2_2(dt, dcp, "dcp") || dcp6_cells_2_2(dt, disp, "display") ||
        dcp6_cells_2_2(dt, pio, "piodma"))
        return -1;
    int resv = fdt_path_offset(dt, "/reserved-memory");
    if (resv < 0 || fdt_address_cells(dt, resv) != 2 || fdt_size_cells(dt, resv) != 2)
        bail("/reserved-memory missing or not 2+2 cells\n");

    if (dcp6_status_disabled(dt, dcp, "dcp") || dcp6_status_disabled(dt, disp, "display"))
        return -1;
    if (soc->fw25) {
        int len;
        const fdt32_t *compat = fdt_getprop(dt, dcp, "apple,firmware-compat", &len);
        if (!dcp25_selected(dt) || !compat || len != 12 || fdt32_ld(compat) != 26 ||
            fdt32_ld(compat + 1) != 6 || fdt32_ld(compat + 2) != 2 ||
            dcp6_status_disabled(dt, pio, "25G83 piodma"))
            bail("25G83 DT profile/compatibility or PIODMA status is invalid\n");
    }
    int dart_dcp = dcp6_check_iommu(dt, dcp, "dcp", dcp6_sid_dcp(), dcp6.dart_dcp,
                                    dcp6.vm_base[VM_DCP], dcp6.vm_size[VM_DCP]);
    int dart_disp = dcp6_check_iommu(dt, disp, "display", dcp6_sid_disp(), dcp6.dart_disp,
                                     dcp6.vm_base[VM_DISP], dcp6.vm_size[VM_DISP]);
    int dart_pio = dcp6_check_iommu(dt, pio, "piodma", dcp6_sid_piodma(), dcp6.dart_disp,
                                    dcp6.vm_base[VM_DISP], dcp6.vm_size[VM_DISP]);
    if (dart_dcp < 0 || dart_disp < 0 || dart_pio < 0)
        return -1;
    if (dart_disp != dart_pio || dart_dcp == dart_disp)
        bail("display and piodma must share one DART, dcp another\n");

    int len;
    const fdt32_t *mbox = fdt_getprop(dt, dcp, "mboxes", &len);
    int mb = mbox && len == sizeof(fdt32_t) ? fdt_node_offset_by_phandle(dt, fdt32_ld(mbox)) : -1;
    if (mb < 0 || dcp6_status_disabled(dt, mb, "the dcp mailbox"))
        bail("dcp mailbox missing or enabled\n");

    const char *const new_props[] = {
        "memory-region",
        "memory-region-names",
        soc->marker,
        "apple,firmware-uuid",
    };
    const int nodes[3] = {dcp, disp, pio};
    for (int i = 0; i < 3; i++)
        for (size_t j = 0; j < ARRAY_SIZE(new_props); j++)
            if (fdt_getprop(dt, nodes[i], new_props[j], NULL))
                bail("%s already has %s\n",
                     i == 0   ? "dcp"
                     : i == 1 ? "display"
                              : "piodma",
                     new_props[j]);
    return 0;
}

/* The ADT and hardware side of the gates */
static int dcp6_check_adt(void)
{
    const struct dcp_soc *soc = dcp6.soc;

    if (chip_id != soc->chip)
        bail("chip 0x%x is not %s\n", chip_id, soc->name);
    if (os_firmware.version != soc->os_fw)
        bail("OS firmware %s is not %s\n", os_firmware.string, soc->os_fw_name);

    int nub = adt_path_offset(adt, DCP6_NUB);
    u32 len = 0, preloaded, running;
    const char *uuid = nub < 0 ? NULL : adt_getprop(adt, nub, "uuid", &len);
    if (!soc->uuid && !soc->any_uuid)
        bail("no %s DCP firmware image is pinned (the ADT names %.40s)\n", soc->name,
             uuid && len ? uuid : "none");
    if (soc->any_uuid) {
        if (!dcp6_is_uuid(uuid, len))
            bail("the dcp firmware image has no well-formed ADT uuid\n");
        dlog_debug("dcp firmware image %s, not pinned here: the kernel's board record decides\n",
                   uuid);
    } else if (!uuid || len != strlen(soc->uuid) + 1 || memcmp(uuid, soc->uuid, len)) {
        bail("dcp firmware is not image %s\n", soc->uuid);
    }
    memcpy(dcp6.uuid, uuid, len);
    if (dcp6_adt_u32(nub, "pre-loaded", &preloaded) || preloaded != 1 ||
        dcp6_adt_u32(nub, "running", &running) || running != 1)
        bail("iBoot did not leave the dcp firmware loaded and running\n");

    if (!dcp6.mmio_ok || !dcp6.dcp_running || !(dcp6.dcp_locked || dcp6.lock_due) ||
        !dcp6.disp_locked)
        bail("DCP not running or its DARTs not locked\n");
    if (!dcp6.segs_ok)
        bail("dcp firmware segments not usable\n");
    for (int i = 0; i < 3; i++)
        if (!dcp6.sid[i].ok)
            bail("%s page tables not validated\n", dcp6.sid[i].what);
    return 0;
}

/* Phandle of the node at `path` in dt, adding one if it has none; 0 on failure */
static u32 dcp6_phandle(void *dt, const char *path)
{
    int node = fdt_path_offset(dt, path);
    if (node < 0)
        return 0;
    u32 ph = fdt_get_phandle(dt, node);
    if (ph)
        return ph;
    if (fdt_generate_phandle(dt, &ph))
        return 0;
    node = fdt_path_offset(dt, path);
    if (node < 0 || fdt_setprop_u32(dt, node, "phandle", ph))
        return 0;
    return ph;
}

/* An iommu-addresses entry: the DVA in the alias form `alias_base` gives it */
static int dcp6_append_iommu_addr(void *dt, int node, u32 phandle, u64 alias_base, u64 dva,
                                  u64 size)
{
    fdt32_t tuple[5];
    tuple[0] = cpu_to_fdt32(phandle);
    fdt64_st(&tuple[1], alias_base | dva);
    fdt64_st(&tuple[3], size);
    return fdt_appendprop(dt, node, "iommu-addresses", tuple, sizeof(tuple));
}

/* The run of stream `sid_idx` that maps segment `seg`, or NULL */
static const struct dcp6_run *dcp6_run_of(int sid_idx, int seg)
{
    for (int i = 0; i < dcp6.sid[sid_idx].nruns; i++)
        if (dcp6.sid[sid_idx].runs[i].seg == seg)
            return &dcp6.sid[sid_idx].runs[i];
    return NULL;
}

static int dcp6_set_list(void *dt, const char *path, const fdt32_t *ph, int count,
                         const char *names, int names_len)
{
    int node = fdt_path_offset(dt, path);
    if (node < 0)
        return -1;
    if (count && fdt_setprop(dt, node, "memory-region", ph, count * sizeof(*ph)))
        return -1;
    node = fdt_path_offset(dt, path);
    if (count && (node < 0 || fdt_setprop(dt, node, "memory-region-names", names, names_len)))
        return -1;
    node = fdt_path_offset(dt, path);
    if (node < 0 || fdt_setprop_u32(dt, node, dcp6.soc->marker, 1))
        return -1;
    return 0;
}

/* Same-boot ADT clock cells. Refusal is local to the scratch handoff. */
static int dcp25_publish_clock(void *dt)
{
    int disp = adt_path_offset(adt, "/arm-io/disp0");
    int arm = adt_path_offset(adt, "/arm-io");
    u32 len = 0;
    const u32 *ids = disp < 0 ? NULL : adt_getprop(adt, disp, "clock-ids", &len);
    u32 clock_ids[2];
    if (!ids || len != sizeof(clock_ids))
        bail("25G83 display clock IDs are missing\n");
    memcpy(clock_ids, ids, sizeof(clock_ids));
    if (clock_ids[0] != 345 || clock_ids[1] != 416)
        bail("25G83 display clock IDs are invalid\n");
    const u32 *freq = arm < 0 ? NULL : adt_getprop(adt, arm, "clock-frequencies", &len);
    if (!freq || len % 4 || len / 4 <= 416 - 256 || !freq[345 - 256] || freq[416 - 256])
        bail("25G83 display clock values are unavailable\n");
    fdt32_t cells[] = {cpu_to_fdt32(1), cpu_to_fdt32(345), cpu_to_fdt32(416),
                       cpu_to_fdt32(freq[345 - 256]), cpu_to_fdt32(freq[416 - 256])};
    int chosen = fdt_path_offset(dt, "/chosen");
    return chosen < 0 ? -1
                      : fdt_setprop(dt, chosen, "apple,j613-25g83-display-clock-adt", cells,
                                    sizeof(cells));
}

/* Write the whole handoff into `dt` (a scratch copy). Any failure leaves it half-written. */
static int dcp6_publish(void *dt, const struct dcp6_dt *n)
{
    if (dcp6.soc->fw25 && dcp25_publish_clock(dt))
        return -1;
    u32 ph_dcp = dcp6_phandle(dt, n->dcp);
    u32 ph_disp = dcp6_phandle(dt, n->display);
    u32 ph_pio = dcp6_phandle(dt, n->piodma);
    if (!ph_dcp || !ph_disp || !ph_pio)
        return -1;

    fdt32_t mr[3][DCP6_SEGS_MAX];
    char names[3][DCP6_SEGS_MAX * DCP6_DEF_LEN];
    int count[3] = {0}, names_len[3] = {0};
    /* dcp's stream is in dart-dcp, display's and piodma's in dart-disp0 */
    const u64 alias_base[3] = {dcp6_alias(VM_DCP, 0), dcp6_alias(VM_DISP, 0),
                               dcp6_alias(VM_DISP, 0)};

    for (int i = 0; i < dcp6.nsegs; i++) {
        const struct dcp6_seg_def *d = &dcp6.defs[i];
        const struct dcp6_seg *s = &dcp6.segs[i];
        char name[DCP6_NAME_LEN];
        u32 ph;

        snprintf(name, sizeof(name), "%s@%lx", d->node, s->phys);
        if (i == dcp6.seg_oslog) {
            /* Added by the always-on part of this kboot */
            int resv = fdt_path_offset(dt, "/reserved-memory");
            int node = resv < 0 ? resv : fdt_subnode_offset(dt, resv, name);
            ph = node < 0 ? 0 : fdt_get_phandle(dt, node);
        } else {
            ph = dcp6_add_resv(dt, name, s->phys, s->size, d->compat);
        }
        if (!ph)
            return -1;

        int resv = fdt_path_offset(dt, "/reserved-memory");
        int node = resv < 0 ? resv : fdt_subnode_offset(dt, resv, name);
        if (node < 0)
            return -1;

        const struct dcp6_run *r;
        const u32 owner_ph[3] = {ph_dcp, ph_disp, ph_pio};
        for (int k = 0; k < 3; k++) {
            if (k == 0 && i == dcp6.seg_oslog)
                r = NULL; /* physical buffer: memory-region entry only, no DART address */
            else if (!(r = dcp6_run_of(k, i)))
                continue;
            if (r) {
                node = fdt_subnode_offset(dt, fdt_path_offset(dt, "/reserved-memory"), name);
                if (node < 0 ||
                    dcp6_append_iommu_addr(dt, node, owner_ph[k], alias_base[k], r->dva, r->size))
                    return -1;
            }
            size_t nl = strlen(d->name) + 1;
            mr[k][count[k]++] = cpu_to_fdt32(ph);
            memcpy(names[k] + names_len[k], d->name, nl);
            names_len[k] += nl;
        }
    }

    if (dcp6_set_list(dt, n->dcp, mr[0], count[0], names[0], names_len[0]) ||
        dcp6_set_list(dt, n->display, mr[1], count[1], names[1], names_len[1]) ||
        dcp6_set_list(dt, n->piodma, mr[2], count[2], names[2], names_len[2]))
        return -1;

    int dcp = fdt_path_offset(dt, n->dcp);
    if (dcp < 0 || fdt_setprop_string(dt, dcp, "apple,firmware-uuid", dcp6.uuid))
        return -1;
    dcp = fdt_path_offset(dt, n->dcp);
    if (dcp < 0 || firmware_set_fdt(dt, dcp, "apple,firmware-version", &os_firmware))
        return -1;
    dcp = fdt_path_offset(dt, n->dcp);
    if (dcp < 0 || firmware_set_fdt(dt, dcp, "apple,firmware-compat", &os_firmware))
        return -1;
    return 0;
}

static bool dcp6_armed(void *dt)
{
    if (!dcp6.soc->optin)
        return true;
    int chosen = fdt_path_offset(dt, "/chosen");
    int len = 0;
    const char *optin = chosen >= 0 ? fdt_getprop(dt, chosen, dcp6.soc->optin, &len) : NULL;
    return optin && len == 2 && !strcmp(optin, "1");
}

/* The segments' reservations: __OS_LOG's is in place, the others do not collide with anything */
static int dcp6_check_resv(void *dt)
{
    for (int i = 0; i < dcp6.nsegs; i++) {
        const struct dcp6_seg *s = &dcp6.segs[i];
        char name[DCP6_NAME_LEN];

        snprintf(name, sizeof(name), "%s@%lx", dcp6.defs[i].node, s->phys);
        int node = dcp6_find_resv(dt, name, s->phys, s->size);
        if (i == dcp6.seg_oslog) {
            if (node < 0 || !fdt_get_phandle(dt, node) ||
                !fdt_getprop(dt, node, DCP6_OSLOG_PROP, NULL))
                bail("dcp __OS_LOG is not reserved as /reserved-memory/%s\n", name);
        } else if (node != -FDT_ERR_NOTFOUND) {
            bail("/reserved-memory/%s already exists\n", name);
        } else if (dcp6_resv_conflict(dt, s->phys, s->size, "dcp segment")) {
            return -1;
        }
    }
    return 0;
}

static __attribute__((noinline)) void dcp6_handoff(void *dt)
{
    struct dcp6_dt n;

    int dcp = fdt_path_offset(dt, "dcp");
    if (dcp >= 0 && fdt_getprop(dt, dcp, dcp6.soc->marker, NULL)) {
        dlog("handoff already published\n");
        return;
    }
    if (dcp6_check_adt() || dcp6_check_dt(dt, &n) || dcp6_check_resv(dt)) {
        dlog("armed, but handoff NOT published; display nodes untouched\n");
        return;
    }

    void *scratch = dcp6_scratch(dt);
    if (!scratch)
        return;
    if (dcp6_publish(scratch, &n)) {
        dlog("DT write failed (full?); handoff NOT published, nothing changed\n");
        free(scratch);
        return;
    }
    /* Every check passed and the device tree is ready: now the lock, if it is still due */
    if (dcp6.lock_due && !dcp6_lock_now()) {
        dlog("armed, but handoff NOT published; display nodes untouched\n");
        free(scratch);
        return;
    }
    if (dcp6_commit(dt, scratch))
        return;
    dlog("handoff published (firmware %s, %s); display nodes left disabled, marker %s set\n",
         dcp6.uuid, os_firmware.string, dcp6.soc->marker);
}

/*
 * T6030 external display processors (dcpext0, dcpext1), independent of the running panel
 * transaction. The memory handoff only describes the preloaded firmware memory; it does not
 * access the external ASC or its DART. The display DART handoff only reads that DART's state, and
 * only while the processor's CPU power domain is on; it never restarts or locks it. Every external
 * node stays disabled until a kernel that knows the markers enables it.
 *
 * Both processors are armed by the same /chosen flags: asahi,t6030-dcpext for memory only and
 * asahi,t6030-dispext for the display DART and its prerequisite memory. dcpext1 is handed off
 * only if the ADT has it and the device tree describes it (alias dcpext1); otherwise it is
 * skipped and nothing changes. Each
 * handoff is its own transaction, so a refused dcpext1 leaves dcpext0's in place.
 */
#define DCPEXT6_MARKER  "apple,t6030-dcpext-memory-ready"
#define DCPEXT6_SEGS    4
#define DISPEXT6_MARKER "apple,t6030-dispext-handoff"
#define DISPEXT6_PMGR   0x350700000UL /* PMGR base of the dispextN_cpu power states */

struct dcpext6_inst {
    const char *name;     /* log name */
    const char *alias;    /* device tree alias of the processor */
    bool optional;        /* skipped if the ADT or the device tree does not have it */
    const char *adt_dcp;  /* ADT processor node */
    const char *adt_nub;  /* its iop nub */
    const char *adt_dart; /* ADT firmware DART (stream 5) */
    const char *adt_disp; /* ADT display DART (streams 0 and 4) */
    const char *resv;     /* /reserved-memory name prefix of the data and heap segments */
    u64 coproc, dart;     /* processor and firmware DART registers */
    const char *dcp_path; /* device tree path of the processor */
    /* Display DART, added as dart_path, with scanout_path as the processor's scanout child */
    const char *disp; /* /reserved-memory name prefix of its page tables */
    u64 disp_dart, disp_reg1;
    u32 disp_irq;
    const char *dart_name, *dart_path, *scanout_path;
    /* The processor's CPU power state: PMGR offset, device tree node and label */
    u32 ps_reg;
    const char *ps_path, *ps_label;
    const char *log; /* prefix of the display DART messages */
};

static const struct dcpext6_inst dcpext6_insts[] = {
    {
        .name = "dcpext",
        .alias = "dcpext0",
        .adt_dcp = "/arm-io/dcpext0",
        .adt_nub = "/arm-io/dcpext0/iop-dcpext0-nub",
        .adt_dart = "/arm-io/dart-dcpext0",
        .adt_disp = "/arm-io/dart-dispext0",
        .resv = "dcpext0",
        .coproc = 0x2d2c00000UL,
        .dart = 0x2d130c000UL,
        .dcp_path = "/soc/dcp@2d2c00000",
        .disp = "dispext0",
        .disp_dart = 0x2d1304000UL,
        .disp_reg1 = 0x2d1300000UL,
        .disp_irq = 651,
        .dart_name = "iommu@2d1304000",
        .dart_path = "/soc/iommu@2d1304000",
        .scanout_path = "/soc/dcp@2d2c00000/scanout",
        .ps_reg = 0x3e0,
        .ps_path = "/soc/power-management@350700000/power-controller@3e0",
        .ps_label = "dispext0_cpu",
        .log = "",
    },
    {
        /* Registers 0x4000000 above dcpext0's; display DART interrupt as its firmware DART's */
        .name = "dcpext1",
        .alias = "dcpext1",
        .optional = true,
        .adt_dcp = "/arm-io/dcpext1",
        .adt_nub = "/arm-io/dcpext1/iop-dcpext1-nub",
        .adt_dart = "/arm-io/dart-dcpext1",
        .adt_disp = "/arm-io/dart-dispext1",
        .resv = "dcpext1",
        .coproc = 0x2d6c00000UL,
        .dart = 0x2d530c000UL,
        .dcp_path = "/soc/dcp@2d6c00000",
        .disp = "dispext1",
        .disp_dart = 0x2d5304000UL,
        .disp_reg1 = 0x2d5300000UL,
        .disp_irq = 689,
        .dart_name = "iommu@2d5304000",
        .dart_path = "/soc/iommu@2d5304000",
        .scanout_path = "/soc/dcp@2d6c00000/scanout",
        .ps_reg = 0x3f8,
        .ps_path = "/soc/power-management@350700000/power-controller@3f8",
        .ps_label = "dispext1_cpu",
        .log = "dispext1: ",
    },
};

static int dcpext6_publish(void *dt, const struct dcpext6_inst *inst)
{
    const char *alias = inst->alias;
    const u32 flags[DCPEXT6_SEGS] = {SEG_FLAGS_TEXT, SEG_FLAGS_DATA, SEG_FLAGS_OSLOG,
                                     SEG_FLAGS_HEAP};
    char path[96];
    int node = dcp6_alias_path(dt, alias, path, sizeof(path));
    if (node < 0 || dcp6_status_disabled(dt, node, alias) || dcp6_cells_2_2(dt, node, alias))
        return -1;
    int panel = fdt_path_offset(dt, "dcp");
    if (panel < 0 || !fdt_getprop(dt, panel, dcp6.soc->marker, NULL))
        bail("%s needs the validated panel handoff first\n", inst->name);
    if (chip_id != T6030 || os_firmware.version != V14_7)
        return -1;
    if (fdt_getprop(dt, node, "memory-region", NULL) || fdt_getprop(dt, node, DCPEXT6_MARKER, NULL))
        bail("%s already has memory metadata\n", inst->name);

    u64 base;
    if (dcp6_adt_reg(inst->adt_dart, 0, &base) || base != inst->dart ||
        dcp6_check_iommu(dt, node, alias, DCP6_SID_DCP, base, DCP6_DART_VM_BASE,
                         DCP6_DART_VM_SIZE) < 0)
        return -1;
    const fdt64_t *reg;
    int len;
    reg = fdt_getprop(dt, node, "reg", &len);
    if (dcp6_adt_reg(inst->adt_dcp, 0, &base) || base != inst->coproc || !reg || len < 16 ||
        fdt64_ld(reg) != base)
        bail("%s ASC address mismatch\n", inst->name);

    int nub = adt_path_offset(adt, inst->adt_nub);
    if (nub < 0)
        return -1;
    u32 seglen = 0, nameslen = 0, uuidlen = 0, loaded;
    const struct adt_segment_ranges *ranges = adt_getprop(adt, nub, "segment-ranges", &seglen);
    const char *names = adt_getprop(adt, nub, "segment-names", &nameslen);
    const char *uuid = adt_getprop(adt, nub, "uuid", &uuidlen);
    if (nub < 0 || !ranges || seglen != DCPEXT6_SEGS * sizeof(*ranges) || !names || !uuid ||
        uuidlen != sizeof(DCP6_UUID) || memcmp(uuid, DCP6_UUID, sizeof(DCP6_UUID)) ||
        dcp6_adt_u32(nub, "pre-loaded", &loaded) || loaded != 1)
        bail("%s firmware metadata invalid\n", inst->name);
    if (!dcp6_seg_name_is(names, nameslen, 1, "__DATA") ||
        !dcp6_seg_name_is(names, nameslen, 2, "__OS_LOG"))
        return -1;

    struct dcp6_seg segs[DCPEXT6_SEGS];
    u64 dram_base, dram_size;
    if (dcp6_dram(&dram_base, &dram_size))
        return -1;
    for (int i = 0; i < DCPEXT6_SEGS; i++) {
        struct adt_segment_ranges r;
        memcpy(&r, &ranges[i], sizeof(r));
        struct dcp6_seg *s = &segs[i];
        *s = (struct dcp6_seg){r.phys, ALIGN_UP((u64)r.size, SZ_16K), r.remap, r.unk};
        if (s->flags != flags[i] || !r.size || s->size > DCP6_SEG_MAX_SIZE ||
            (s->phys & (SZ_16K - 1)) || s->phys < dram_base || s->phys + s->size < s->phys ||
            s->phys + s->size > dram_base + dram_size)
            return -1;
        if (i == SEG_OSLOG) {
            if (s->remap != s->phys || s->size > DCP6_OSLOG_MAX)
                return -1;
        } else {
            if (s->remap < DCP6_DART_VM_BASE || (s->remap & (SZ_16K - 1)) ||
                s->remap - DCP6_DART_VM_BASE + s->size > DCP6_DART_VM_SIZE)
                return -1;
            /* Firmware is already excluded from the usable RAM window. */
            u64 ram_end = kboot_ram_base() + kboot_ram_size();
            if (!(s->phys + s->size <= kboot_ram_base() || s->phys >= ram_end) ||
                dcp6_carveout_conflict(s->phys, s->size, alias))
                return -1;
        }
        for (int j = 0; j < i; j++) {
            if (dcp6_overlaps(s->phys, s->size, segs[j].phys, segs[j].size) ||
                (i != SEG_OSLOG && j != SEG_OSLOG &&
                 dcp6_overlaps(s->remap, s->size, segs[j].remap, segs[j].size)))
                return -1;
        }
    }
    /* Only the immutable text may alias the panel's firmware allocation. */
    if (segs[0].phys != dcp6.segs[0].phys || segs[0].size != dcp6.segs[0].size ||
        segs[0].remap != dcp6.segs[0].remap)
        return -1;

    u32 owner = dcp6_phandle(dt, path);
    if (!owner)
        return -1;
    fdt32_t mr[DCPEXT6_SEGS];
    for (int i = 0; i < DCPEXT6_SEGS; i++) {
        struct dcp6_seg *s = &segs[i];
        char name[64];
        if (i == SEG_TEXT || i == SEG_OSLOG) {
            snprintf(name, sizeof(name), "%s@%lx", i == SEG_TEXT ? "dcp-text" : DCP6_OSLOG_NODE,
                     s->phys);
            node = dcp6_find_resv(dt, name, s->phys, s->size);
            if (node < 0 ||
                (i == SEG_TEXT && fdt_node_check_compatible(dt, node, "apple,asc-mem")) ||
                (i == SEG_OSLOG && (!fdt_getprop(dt, node, DCP6_OSLOG_PROP, NULL) ||
                                    fdt_getprop(dt, node, "iommu-addresses", NULL))))
                return -1;
            mr[i] = cpu_to_fdt32(fdt_get_phandle(dt, node));
        } else {
            if (dcp6_resv_conflict(dt, s->phys, s->size, alias))
                return -1;
            snprintf(name, sizeof(name), "%s-%s@%lx", inst->resv, i == SEG_DATA ? "data" : "heap",
                     s->phys);
            mr[i] = cpu_to_fdt32(dcp6_add_resv(dt, name, s->phys, s->size, "apple,asc-mem"));
            node = fdt_node_offset_by_phandle(dt, fdt32_to_cpu(mr[i]));
        }
        if (!mr[i] || node < 0)
            return -1;
        if (i != SEG_OSLOG && dcp6_append_iommu_addr(dt, node, owner, DCP6_DART_VM_BASE,
                                                     s->remap - DCP6_DART_VM_BASE, s->size))
            return -1;
    }
    static const char mr_names[] = "asc-firmware\0dcp_data\0os_log\0heap";
    node = fdt_path_offset(dt, path);
    if (node < 0 || fdt_setprop(dt, node, "memory-region", mr, sizeof(mr)) ||
        fdt_setprop(dt, node, "memory-region-names", mr_names, sizeof(mr_names)) ||
        fdt_setprop_string(dt, node, "apple,firmware-uuid", DCP6_UUID) ||
        firmware_set_fdt(dt, node, "apple,firmware-version", &os_firmware) ||
        firmware_set_fdt(dt, node, "apple,firmware-compat", &os_firmware) ||
        fdt_setprop_u32(dt, node, DCPEXT6_MARKER, 1))
        return -1;
    return 0;
}

static void dcpext6_handoff(void *dt, const struct dcpext6_inst *inst)
{
    void *scratch = dcp6_scratch(dt);
    if (!scratch)
        return;
    if (dcpext6_publish(scratch, inst)) {
        dlog("%s memory handoff rejected; panel handoff unchanged\n", inst->name);
        free(scratch);
        return;
    }
    if (!dcp6_commit(dt, scratch))
        dlog("%s memory handoff published; CPU untouched, node disabled\n", inst->name);
}

struct dispext6_tables {
    u64 lo[2], hi[2];
    u32 tcr[2], ttbr[2];
};

/* Read only, including on refusal. Never restart or lock the display DART. */
static int dispext6_check_inner(const struct dcpext6_inst *inst, struct dispext6_tables *t)
{
    const u32 sid[2] = {0, 4};
    /* Do not access the external display aperture unless its CPU domain is on. */
    if ((read32(DISPEXT6_PMGR + inst->ps_reg) & 0xf0) != 0xf0)
        return -1;
    if (!(read32(inst->disp_dart + DART_T8110_PROTECT) & DART_T8110_PROTECT_TTBR_TCR))
        return -1;
    for (int i = 0; i < 2; i++) {
        t->tcr[i] = read32(inst->disp_dart + DART_T8110_TCR(sid[i]));
        t->ttbr[i] = read32(inst->disp_dart + DART_T8110_TTBR(sid[i]));
        if (!(t->tcr[i] & DART_T8110_TCR_TRANSLATE) ||
            (t->tcr[i] & (DART_T8110_TCR_BYPASS_DART | DART_T8110_TCR_FOUR_LEVEL)) ||
            !(t->ttbr[i] & DART_T8110_TTBR_VALID))
            return -1;
        u64 root = (u64)FIELD_GET(DART_T8110_TTBR_ADDR, t->ttbr[i]) << DART_T8110_TTBR_SHIFT;
        if (root < t->lo[i] || root + DART_PAGE > t->hi[i])
            return -1;
        /* Refuse even invalid-but-nonzero entries: ownership must be unambiguous. */
        for (u32 n = 0; n < DART_ENTRIES; n++)
            if (read64(root + 8 * n))
                return -1;
    }
    return 0;
}

static int dispext6_publish(void *dt, const struct dcpext6_inst *inst)
{
    struct dispext6_tables t;
    const char *adt_path = inst->adt_disp;
    const char *pt_names[2] = {"pt-region-0", "pt-region-4"};
    const char *ext_path = inst->dcp_path;
    u64 base, vm_base, vm_size, dram_base, dram_size;
    u32 irq;
    int a = adt_path_offset(adt, adt_path);
    int ext = fdt_path_offset(dt, ext_path), len;
    const fdt32_t *ready = ext < 0 ? NULL : fdt_getprop(dt, ext, DCPEXT6_MARKER, &len);
    if (chip_id != T6030 || os_firmware.version != V14_7 || a < 0 || ext < 0 || !ready ||
        len != 4 || fdt32_ld(ready) != 1 || dcp6_status_disabled(dt, ext, inst->alias) ||
        fdt_path_offset(dt, inst->dart_path) != -FDT_ERR_NOTFOUND ||
        fdt_path_offset(dt, inst->scanout_path) != -FDT_ERR_NOTFOUND)
        return -1;
    if (dcp6_adt_reg(adt_path, 0, &base) || base != inst->disp_dart ||
        dcp6_adt_reg(adt_path, 1, &base) || base != inst->disp_reg1 ||
        dcp6_adt_u32(a, "interrupts", &irq) || irq != inst->disp_irq ||
        dcp6_adt_u64(a, "vm-base", &vm_base) || vm_base != DCP6_DART_VM_BASE ||
        dcp6_adt_u64(a, "vm-size", &vm_size) || vm_size != DCP6_DART_VM_SIZE ||
        dcp6_dram(&dram_base, &dram_size))
        return -1;
    for (int i = 0; i < 2; i++) {
        if (dcp6_adt_u64_pair(a, pt_names[i], &t.lo[i], &t.hi[i]) || t.hi[i] <= t.lo[i] ||
            t.hi[i] - t.lo[i] > DART_PT_MAX_SIZE || ((t.lo[i] | t.hi[i]) & (DART_PAGE - 1)) ||
            t.lo[i] < dram_base || t.hi[i] > dram_base + dram_size ||
            dcp6_overlaps(t.lo[i], t.hi[i] - t.lo[i], kboot_ram_base(), kboot_ram_size()) ||
            dcp6_carveout_conflict(t.lo[i], t.hi[i] - t.lo[i], pt_names[i]) ||
            dcp6_resv_conflict(dt, t.lo[i], t.hi[i] - t.lo[i], pt_names[i]))
            return -1;
    }
    if (dcp6_overlaps(t.lo[0], t.hi[0] - t.lo[0], t.lo[1], t.hi[1] - t.lo[1]))
        return -1;

    int fw = dcp6_check_iommu(dt, ext, inst->alias, DCP6_SID_DCP, inst->dart, DCP6_DART_VM_BASE,
                              DCP6_DART_VM_SIZE);
    if (fw < 0)
        return -1;
    /* Copy only validated local interrupt-controller and power-domain references. */
    const fdt32_t *prop = fdt_getprop(dt, fw, "interrupt-parent", &len);
    if (!prop || len != 4)
        return -1;
    u32 aic = fdt32_ld(prop);
    int parent = fdt_node_offset_by_phandle(dt, aic);
    if (parent < 0 || !fdt_getprop(dt, parent, "interrupt-controller", NULL))
        return -1;
    prop = fdt_getprop(dt, parent, "#interrupt-cells", &len);
    if (!prop || len != 4 || fdt32_ld(prop) != 3)
        return -1;
    prop = fdt_getprop(dt, fw, "power-domains", &len);
    if (!prop || len != 4)
        return -1;
    u32 pd = fdt32_ld(prop);
    parent = fdt_node_offset_by_phandle(dt, pd);
    const char *label = parent < 0 ? NULL : fdt_getprop(dt, parent, "label", &len);
    if (!label || len < 0 || (size_t)len != strlen(inst->ps_label) + 1 ||
        memcmp(label, inst->ps_label, len))
        return -1;
    if (parent != fdt_path_offset(dt, inst->ps_path) ||
        fdt_node_check_compatible(dt, parent, "apple,t6030-pmgr-pwrstate"))
        return -1;
    prop = fdt_getprop(dt, parent, "#power-domain-cells", &len);
    if (!prop || len != 4 || fdt32_ld(prop))
        return -1;
    prop = fdt_getprop(dt, parent, "reg", &len);
    if (!prop || len != 8 || fdt32_ld(prop) != inst->ps_reg || fdt32_ld(prop + 1) != 4)
        return -1;

    int guard = dcp6_guard_begin();
    int ret = dispext6_check_inner(inst, &t);
    if (dcp6_guard_end(guard) || ret)
        return -1;

    fdt32_t mr[2], state[6];
    for (int i = 0; i < 2; i++) {
        char name[80];
        snprintf(name, sizeof(name), "%s-pt%d@%lx", inst->disp, i ? 4 : 0, t.lo[i]);
        u32 ph = dcp6_add_resv(dt, name, t.lo[i], t.hi[i] - t.lo[i], NULL);
        if (!ph)
            return -1;
        mr[i] = cpu_to_fdt32(ph);
        state[3 * i] = cpu_to_fdt32(i ? 4 : 0);
        state[3 * i + 1] = cpu_to_fdt32(t.tcr[i]);
        state[3 * i + 2] = cpu_to_fdt32(t.ttbr[i]);
    }
    int soc = fdt_path_offset(dt, "/soc");
    int dart = fdt_add_subnode(dt, soc, inst->dart_name);
    const char compat[] = "apple,t6030-dart\0apple,t8110-dart";
    const char names[] = "sid0-page-tables\0sid4-page-tables";
    fdt64_t reg[] = {cpu_to_fdt64(inst->disp_dart), cpu_to_fdt64(0x4000)};
    fdt64_t range[] = {cpu_to_fdt64(vm_base), cpu_to_fdt64(vm_size)};
    fdt32_t interrupts[] = {cpu_to_fdt32(0), cpu_to_fdt32(irq), cpu_to_fdt32(4)};
    if (dart < 0 || fdt_setprop(dt, dart, "compatible", compat, sizeof(compat)) ||
        fdt_setprop_string(dt, dart, "status", "disabled") ||
        fdt_setprop_u32(dt, dart, "#iommu-cells", 1) ||
        fdt_setprop(dt, dart, "reg", reg, sizeof(reg)) ||
        fdt_setprop(dt, dart, "apple,dma-range", range, sizeof(range)) ||
        fdt_setprop_u32(dt, dart, "interrupt-parent", aic) ||
        fdt_setprop(dt, dart, "interrupts", interrupts, sizeof(interrupts)) ||
        fdt_setprop_u32(dt, dart, "power-domains", pd) ||
        fdt_setprop(dt, dart, "memory-region", mr, sizeof(mr)) ||
        fdt_setprop(dt, dart, "memory-region-names", names, sizeof(names)) ||
        fdt_setprop(dt, dart, "apple,inherited-dart-state", state, sizeof(state)) ||
        fdt_setprop_u32(dt, dart, DISPEXT6_MARKER, 1))
        return -1;
    u32 ph = dcp6_phandle(dt, inst->dart_path);
    ext = fdt_path_offset(dt, ext_path);
    int scanout = fdt_add_subnode(dt, ext, "scanout");
    fdt32_t iommu[] = {cpu_to_fdt32(ph), cpu_to_fdt32(0)};
    if (!ph || scanout < 0 ||
        fdt_setprop_string(dt, scanout, "compatible", "apple,t6030-dispext-scanout") ||
        fdt_setprop_string(dt, scanout, "status", "disabled") ||
        fdt_setprop_u32(dt, scanout, "#address-cells", 2) ||
        fdt_setprop_u32(dt, scanout, "#size-cells", 2) ||
        fdt_setprop(dt, scanout, "iommus", iommu, sizeof(iommu)) ||
        fdt_setprop_u32(dt, scanout, DISPEXT6_MARKER, 1))
        return -1;
    return 0;
}

static void dispext6_handoff(void *dt, const struct dcpext6_inst *inst)
{
    void *scratch = dcp6_scratch(dt);
    if (!scratch)
        return;
    if (dispext6_publish(scratch, inst)) {
        dlog("%sdisplay DART handoff rejected; existing handoffs unchanged\n", inst->log);
        free(scratch);
        return;
    }
    if (!dcp6_commit(dt, scratch))
        dlog("%sempty external display DART handoff published; hardware untouched\n", inst->log);
}

/* A /chosen flag set to "1" (by the device tree or a boot.bin config line chosen.<name>=1) */
static bool dcpext6_flag(void *dt, const char *name)
{
    int chosen = fdt_path_offset(dt, "/chosen"), len = 0;
    const char *opt = chosen < 0 ? NULL : fdt_getprop(dt, chosen, name, &len);
    return opt && len == 2 && !memcmp(opt, "1", 2);
}

/* An optional processor is handed off only if both the ADT and the device tree have it */
static bool dcpext6_present(void *dt, const struct dcpext6_inst *inst)
{
    if (adt_path_offset(adt, inst->adt_dcp) < 0 || adt_path_offset(adt, inst->adt_nub) < 0) {
        dlog("%s: not in the ADT, no handoff\n", inst->name);
        return false;
    }
    if (!fdt_get_alias(dt, inst->alias)) {
        dlog("%s: the device tree does not describe it, no handoff\n", inst->name);
        return false;
    }
    return true;
}

/* dcpext0, then dcpext1; each processor's memory handoff before its display DART handoff */
static void dcpext6_handoffs(void *dt)
{
    bool disp = dcpext6_flag(dt, "asahi,t6030-dispext");
    bool mem = disp || dcpext6_flag(dt, "asahi,t6030-dcpext");

    if (!mem && !disp)
        return;
    for (size_t i = 0; i < ARRAY_SIZE(dcpext6_insts); i++) {
        const struct dcpext6_inst *inst = &dcpext6_insts[i];
        if (inst->optional && !dcpext6_present(dt, inst))
            continue;
        if (mem)
            dcpext6_handoff(dt, inst);
        if (disp)
            dispext6_handoff(dt, inst);
    }
}

/*
 * J616s external DCPs use the same four-segment ASC layout as the T6030
 * external DCPs. The kernel DT supplies disabled text/data/heap placeholders;
 * fill those only from this boot's ADT. In particular, do not enable the DCP,
 * its DART or any display route here: the kernel's PMP-aware gate owns that.
 */
struct t6041_ext {
    const char *name, *dt_path, *adt_path, *nub_path, *dart_path;
    u64 coproc, dart;
};

static const struct t6041_ext t6041_exts[] = {
    {"dcpext0", "/dcp@3a2e00000", "/arm-io/dcpext0", "/arm-io/dcpext0/iop-dcpext0-nub",
     "/arm-io/dart-dcpext0", 0x3a2e00000UL, 0x3a2340000UL},
    {"dcpext1", "/dcp@602e00000", "/arm-io/dcpext1", "/arm-io/dcpext1/iop-dcpext1-nub",
     "/arm-io/dart-dcpext1", 0x602e00000UL, 0x602340000UL},
};

static int t6041_ext_fill(void *dt, const struct t6041_ext *ext)
{
    static const u32 flags[DCPEXT6_SEGS] = {SEG_FLAGS_TEXT, SEG_FLAGS_DATA, SEG_FLAGS_OSLOG,
                                            SEG_FLAGS_HEAP};
    static const char mr_names[] = "text\0data\0heap";
    static const char *const parts[] = {"text", "data", "heap"};
    static const int seg_index[] = {SEG_TEXT, SEG_DATA, SEG_3};
    u64 coproc, dart, dram, dram_size, ram_end;
    int node = fdt_path_offset(dt, ext->dt_path), len;
    const fdt64_t *reg;
    const fdt32_t *iommu, *mr;
    u32 ph[3];

    /* A DT without #451's external nodes is an ordinary internal-only boot. */
    if (node < 0)
        return 1;
    if (fdt_node_check_compatible(dt, node, "apple,t6041-dcpext") ||
        dcp6_status_disabled(dt, node, ext->name) || dcp6_adt_reg(ext->adt_path, 0, &coproc) ||
        coproc != ext->coproc || dcp6_adt_reg(ext->dart_path, 0, &dart) || dart != ext->dart)
        return -1;
    reg = fdt_getprop(dt, node, "reg", &len);
    if (!reg || len < 2 * (int)sizeof(fdt64_t) || fdt64_ld(reg) != coproc)
        return -1;
    iommu = fdt_getprop(dt, node, "iommus", &len);
    if (!iommu || len != 2 * (int)sizeof(fdt32_t) || fdt32_ld(&iommu[1]) != 23)
        return -1;
    int dart_node = fdt_node_offset_by_phandle(dt, fdt32_ld(iommu));
    if (dart_node < 0 || fdt_node_check_compatible(dt, dart_node, "apple,t8110-dart") ||
        dcp6_status_disabled(dt, dart_node, ext->name))
        return -1;
    reg = fdt_getprop(dt, dart_node, "reg", &len);
    if (!reg || len < 2 * (int)sizeof(fdt64_t) || fdt64_ld(reg) != dart)
        return -1;

    mr = fdt_getprop(dt, node, "memory-region", &len);
    if (!mr || len != 3 * (int)sizeof(fdt32_t))
        return -1;
    for (int i = 0; i < 3; i++)
        ph[i] = fdt32_ld(&mr[i]);
    if (!ph[0] || !ph[1] || !ph[2] || ph[0] == ph[1] || ph[0] == ph[2] || ph[1] == ph[2])
        return -1;
    const char *names = fdt_getprop(dt, node, "memory-region-names", &len);
    if (!names || len != sizeof(mr_names) || memcmp(names, mr_names, sizeof(mr_names)))
        return -1;
    for (int i = 0; i < 3; i++) {
        int region = fdt_node_offset_by_phandle(dt, ph[i]);
        const char *status = region < 0 ? NULL : fdt_getprop(dt, region, "status", NULL);
        const fdt64_t *r = region < 0 ? NULL : fdt_getprop(dt, region, "reg", &len);
        if (!status || strcmp(status, "disabled") ||
            fdt_node_check_compatible(dt, region, "apple,asc-mem") ||
            !fdt_getprop(dt, region, "no-map", NULL) ||
            fdt_getprop(dt, region, "iommu-addresses", NULL) || !r ||
            len != 2 * (int)sizeof(fdt64_t) || fdt64_ld(r) || fdt64_ld(&r[1]) != SZ_16K)
            return -1;
    }

    int nub = adt_path_offset(adt, ext->nub_path);
    u32 seg_len = 0, names_len = 0, preloaded, running;
    const struct adt_segment_ranges *ranges =
        nub < 0 ? NULL : adt_getprop(adt, nub, "segment-ranges", &seg_len);
    const char *seg_names = nub < 0 ? NULL : adt_getprop(adt, nub, "segment-names", &names_len);
    if (!ranges || seg_len != DCPEXT6_SEGS * sizeof(*ranges) || !seg_names ||
        !dcp6_seg_name_is(seg_names, names_len, SEG_TEXT, "__TEXT") ||
        !dcp6_seg_name_is(seg_names, names_len, SEG_DATA, "__DATA") ||
        !dcp6_seg_name_is(seg_names, names_len, SEG_OSLOG, "__OS_LOG") ||
        dcp6_adt_u32(nub, "pre-loaded", &preloaded) || preloaded != 1 ||
        dcp6_adt_u32(nub, "running", &running) || running != 1 || dcp6_dram(&dram, &dram_size))
        return -1;
    ram_end = kboot_ram_base() + kboot_ram_size();
    if (ram_end < kboot_ram_base())
        return -1;
    struct dcp6_seg segs[DCPEXT6_SEGS];
    for (int i = 0; i < DCPEXT6_SEGS; i++) {
        struct adt_segment_ranges r;
        memcpy(&r, &ranges[i], sizeof(r));
        segs[i] = (struct dcp6_seg){r.phys, ALIGN_UP((u64)r.size, SZ_16K), r.remap, r.unk};
        const struct dcp6_seg *s = &segs[i];
        if (s->flags != flags[i] || !r.size || s->size > DCP6_SEG_MAX_SIZE ||
            (s->phys & (SZ_16K - 1)) || s->phys < dram || s->phys + s->size < s->phys ||
            s->phys + s->size > dram + dram_size)
            return -1;
        if (i == SEG_OSLOG) {
            if (s->remap != s->phys || s->size > DCP6_OSLOG_MAX)
                return -1;
        } else if (s->remap < DCP6_DART_VM_BASE ||
                   s->remap - DCP6_DART_VM_BASE > DCP6_DART_VM_SIZE - s->size ||
                   (s->remap & (SZ_16K - 1)) ||
                   !(s->phys + s->size <= kboot_ram_base() || s->phys >= ram_end) ||
                   dcp6_carveout_conflict(s->phys, s->size, ext->name)) {
            return -1;
        }
        for (int j = 0; j < i; j++)
            if (dcp6_overlaps(s->phys, s->size, segs[j].phys, segs[j].size) ||
                (i != SEG_OSLOG && j != SEG_OSLOG &&
                 dcp6_overlaps(s->remap, s->size, segs[j].remap, segs[j].size)))
                return -1;
    }
    /* The two external processors and the panel share immutable TEXT. */
    if (segs[SEG_TEXT].phys != dcp6.segs[SEG_TEXT].phys ||
        segs[SEG_TEXT].size != dcp6.segs[SEG_TEXT].size)
        return -1;

    u32 owner = dcp6_phandle(dt, ext->dt_path);
    if (!owner)
        return -1;
    /*
     * TEXT is the panel's own region: give it this processor's mapping and
     * point the text entry at it, as dcpext6 does, instead of describing the
     * same memory a second time. The text placeholder is then deleted.
     */
    char text_name[64];
    const struct dcp6_seg *text = &segs[SEG_TEXT];
    snprintf(text_name, sizeof(text_name), "dcp-text@%lx", text->phys);
    int shared = dcp6_find_resv(dt, text_name, text->phys, text->size);
    if (shared < 0 || fdt_node_check_compatible(dt, shared, "apple,asc-mem") ||
        dcp6_append_iommu_addr(dt, shared, owner, DCP6_DART_VM_BASE,
                               text->remap - DCP6_DART_VM_BASE, text->size))
        return -1;
    shared = dcp6_find_resv(dt, text_name, text->phys, text->size);
    u32 shared_ph = shared < 0 ? 0 : fdt_get_phandle(dt, shared);
    if (!shared_ph)
        return -1;
    for (int i = 1; i < 3; i++) {
        const struct dcp6_seg *s = &segs[seg_index[i]];
        char name[64];
        fdt64_t region_reg[2] = {cpu_to_fdt64(s->phys), cpu_to_fdt64(s->size)};
        if (dcp6_resv_conflict(dt, s->phys, s->size, ext->name))
            return -1;
        snprintf(name, sizeof(name), "%s-%s@%lx", ext->name, parts[i], s->phys);
        int region = fdt_node_offset_by_phandle(dt, ph[i]);
        if (region < 0 || fdt_set_name(dt, region, name))
            return -1;
        region = fdt_node_offset_by_phandle(dt, ph[i]);
        if (region < 0 || fdt_setprop(dt, region, "reg", region_reg, sizeof(region_reg)) ||
            dcp6_append_iommu_addr(dt, region, owner, DCP6_DART_VM_BASE,
                                   s->remap - DCP6_DART_VM_BASE, s->size) ||
            fdt_setprop_string(dt, region, "status", "okay"))
            return -1;
    }
    fdt32_t mr_new[3] = {cpu_to_fdt32(shared_ph), cpu_to_fdt32(ph[1]), cpu_to_fdt32(ph[2])};
    node = fdt_path_offset(dt, ext->dt_path);
    if (node < 0 || fdt_setprop(dt, node, "memory-region", mr_new, sizeof(mr_new)))
        return -1;
    int placeholder = fdt_node_offset_by_phandle(dt, ph[0]);
    if (placeholder < 0 || fdt_del_node(dt, placeholder))
        return -1;
    node = fdt_path_offset(dt, ext->dt_path);
    if (node < 0 || firmware_set_fdt(dt, node, "apple,firmware-version", &os_firmware) ||
        firmware_set_fdt(dt, node, "apple,firmware-compat", &os_firmware))
        return -1;
    return 0;
}

static void t6041_ext_handoffs(void *dt)
{
    int panel = fdt_path_offset(dt, "dcp");
    if (panel < 0 || !fdt_getprop(dt, panel, dcp6.soc->marker, NULL))
        return;
    for (size_t i = 0; i < ARRAY_SIZE(t6041_exts); i++) {
        void *scratch = dcp6_scratch(dt);
        if (!scratch)
            return;
        int ret = t6041_ext_fill(scratch, &t6041_exts[i]);
        if (ret) {
            if (ret < 0)
                dlog("%s firmware memory refused; placeholder remains disabled\n",
                     t6041_exts[i].name);
            free(scratch);
            continue;
        }
        if (!dcp6_commit(dt, scratch))
            dlog("%s firmware memory published; processor remains disabled\n", t6041_exts[i].name);
    }
}

/*
 * For an SoC without an opt-in: the device tree has the display block the handoff is for, the dcp
 * and disp0 aliases naming nodes of the SoC's compatibles. Only the presence is checked here;
 * dcp6_check_dt() checks the rest before anything is published.
 */
static bool dcp6_dt_describes(void *dt)
{
    const char *dcp = fdt_get_alias(dt, "dcp");
    const char *disp = fdt_get_alias(dt, "disp0");
    int dcp_node = dcp ? fdt_path_offset(dt, dcp) : -1;
    int disp_node = disp ? fdt_path_offset(dt, disp) : -1;
    return dcp_node >= 0 && disp_node >= 0 &&
           !fdt_node_check_compatible(dt, dcp_node, dcp6.soc->dcp_compat) &&
           !fdt_node_check_compatible(dt, disp_node, dcp6.soc->disp_compat);
}

/* Always returns 0: nothing here may stop the boot */
int dt_set_display_handoff(void *dt)
{
    memset(&dcp6, 0, sizeof(dcp6));

    for (size_t i = 0; i < ARRAY_SIZE(dcp_socs) && !dcp6.soc; i++)
        if (!fdt_node_check_compatible(dt, 0, dcp_socs[i]->compat))
            dcp6.soc = dcp_socs[i];
    if (!dcp6.soc)
        return 0;

    if (dcp6.soc->chip == T8122 && dcp25_selected(dt))
        dcp6.soc = &dcp_soc_j613_25g83;

    if (dcp6.soc->board_gate && (chip_id != dcp6.soc->chip || !dcp6_board_ok(dt))) {
        dlog("not a %s (chip 0x%x): nothing done\n", dcp6.soc->boards_desc, chip_id);
        return 0;
    }

    /* Reject unsupported firmware before reserving __OS_LOG or touching registers. */
    if (!dcp6.soc->optin &&
        (dcp6.soc->os_fw == V_UNKNOWN || os_firmware.version != dcp6.soc->os_fw)) {
        dlog("no qualified OS firmware for %s: nothing done\n", dcp6.soc->dcp_compat);
        return 0;
    }

    if (!dcp6.soc->optin && !dcp6_dt_describes(dt)) {
        dlog("the device tree does not describe the display block (dcp %s, disp0 %s): nothing "
             "done\n",
             dcp6.soc->dcp_compat, dcp6.soc->disp_compat);
        return 0;
    }

    bool armed = dcp6_armed(dt);
    if (!dcp6.soc->always && !armed) {
        if (dcp6.soc->oslog_always) {
            dcp6_find_nubs();
            dcp6_reserve_oslogs(dt);
        }
        dlog("not armed (no chosen.%s=1): no display handoff\n", dcp6.soc->optin);
        return 0;
    }

    if (dcp6.soc->describe)
        dcp6_describe();
    dcp6_find_nubs();
    dcp6_reserve_oslogs(dt);
    dcp6_check_hw();
    dcp6_dry_run();

    if (armed)
        dcp6_handoff(dt);
    else
        dlog("not armed (no chosen.%s=1): no display handoff\n", dcp6.soc->optin);
    if (dcp6.soc->ext)
        dcpext6_handoffs(dt);
    if (dcp6.soc == &dcp_soc_t6041)
        t6041_ext_handoffs(dt);
    return 0;
}
