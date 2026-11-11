/* SPDX-License-Identifier: MIT */
/*
 * Host test of the T8122 (J613) internal display handoff in src/kboot_display_handoff.c, checked
 * against what the kernel reads from the device tree it hands over.
 *
 * A fake ADT, a fake PMGR table and fake MMIO/DRAM stand in for the Mac; libfdt is aura's own.
 * The ADT has the J613's structure with synthetic addresses: seven iop-dcp-nub segments (__TEXT,
 * __DATA, __OS_LOG, then four heap segments, the last of them /vram), dart-dcp stream 5 mapping
 * every segment but __OS_LOG, dart-disp0 stream 0 mapping segments 4 and 6 and stream 4 mapping
 * segment 5, and segment 3 mapped by neither display stream. That is the layout two J613s have
 * shown at boot.
 *
 *   t8122-display-host <in.dtb> <scenario> [out.dtb]
 *
 * Scenarios: unarmed, publish, repeat, wrong-image, not-running, outside-segment, power-off,
 * contract-selftest. Each runs dt_set_display_handoff() on <in.dtb>, prints what it checked and
 * exits 0 if every check passed. tests/run-t8122-display-handoff.sh runs them all.
 *
 * What "the contract" is (contract_check() below), read from the Aurora kernel:
 *  - the display enable gate (drivers/soc/apple/t6030-display-gate.c, gate_resolve()): exactly one
 *    apple,t8122-dcp and one apple,t8122-display-subsystem node, apple,t8122-handoff = <1> on
 *    both, the dcp's iommus one t8110 DART stream 5, the display's another t8110 DART stream 0,
 *    the dcp's mboxes one apple,asc-mailbox-v4, and all five nodes still disabled;
 *  - drm/apple's 14.x IOMFB path for the J613 board (iomfb_v14_7.c, dcp.c, apple_drv.c): the
 *    root is apple,j613, apple,firmware-uuid is the J613 image, apple,firmware-compat is 14.7.0,
 *    every RTKit buffer lies in a dcp memory-region (the __OS_LOG one marked apple,dcp-os-log at
 *    its physical address, the others DART-mapped), a "piodma" child of the dcp that is available
 *    and marked, and a "framebuffer" memory-region on the display subsystem that is available and
 *    compatible "framebuffer";
 *  - iommu-addresses on every DART-mapped region, for the stream that maps it, at the DVA the
 *    locked page tables use, so the kernel's DART driver keeps those mappings.
 * apple,notch-height and the simple framebuffer come from dt_set_fb() in kboot.c, not from this
 * file, and are not checked here.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adt.h"
#include "exception.h"
#include "firmware.h"
#include "kboot.h"
#include "mcc.h"
#include "pmgr.h"
#include "soc.h"
#include "t6050_unmodelled.h"
#include "types.h"
#include "utils.h"

#include "libfdt/libfdt.h"

int dt_set_display_handoff(void *dt);

void *adt = (void *)1;
u32 chip_id;
volatile enum exc_guard_t exc_guard;
volatile int exc_count;
struct fw_version_info os_firmware = {V14_7, "14.7", {14, 7, 0}, 3, "iBoot-10151.140.19.700.2"};
size_t mcc_carveout_count;
struct mcc_carveout mcc_carveouts[8];
static u64 ram_base, ram_size, fb_base;

u64 kboot_ram_base(void)
{
    return ram_base;
}
u64 kboot_ram_size(void)
{
    return ram_size;
}
u64 kboot_fb_base(void)
{
    return fb_base;
}
/* Where stage 1 loaded aura: clear of every __OS_LOG buffer below */
void kboot_loaded_file_part(u64 *base, u64 *size)
{
    *base = 0x10010000000ULL;
    *size = 0xe0000;
}

int firmware_set_fdt(void *fdt, int node, const char *prop, const struct fw_version_info *ver)
{
    fdt32_t data[5];
    for (size_t i = 0; i < ver->num_length; i++)
        data[i] = cpu_to_fdt32(ver->num[i]);
    return fdt_setprop(fdt, node, prop, data, ver->num_length * 4) ? -1 : 0;
}

/* ---- fake ADT ---- */

struct fnode {
    char path[96];
    struct adt_property *props[64];
    int nprops;
    u64 reg[8][2];
    int nreg;
};
static struct fnode nodes[64];
static int nnodes;

static int node_find(const char *path)
{
    for (int i = 0; i < nnodes; i++)
        if (!strcmp(nodes[i].path, path))
            return i;
    return -1;
}

static struct fnode *node_get(const char *path)
{
    int i = node_find(path);
    if (i >= 0)
        return &nodes[i];
    snprintf(nodes[nnodes].path, sizeof(nodes[0].path), "%s", path);
    return &nodes[nnodes++];
}

static void prop_set(const char *path, const char *name, const void *val, u32 len)
{
    struct fnode *n = node_get(path);
    struct adt_property *p = calloc(1, sizeof(*p) + len + 8);
    snprintf(p->name, sizeof(p->name), "%s", name);
    p->size = len;
    memcpy(p->value, val, len);
    n->props[n->nprops++] = p;
}

static void prop_u32(const char *path, const char *name, u32 v)
{
    prop_set(path, name, &v, 4);
}

static void prop_u64(const char *path, const char *name, u64 v)
{
    prop_set(path, name, &v, 8);
}

static void prop_pair(const char *path, const char *name, u64 a, u64 b)
{
    u64 v[2] = {a, b};
    prop_set(path, name, v, 16);
}

static void prop_str(const char *path, const char *name, const char *s)
{
    prop_set(path, name, s, strlen(s) + 1);
}

static void reg_add(const char *path, u64 a, u64 s)
{
    struct fnode *n = node_get(path);
    n->reg[n->nreg][0] = a;
    n->reg[n->nreg][1] = s;
    n->nreg++;
}

int adt_path_offset(const void *a, const char *path)
{
    UNUSED(a);
    int i = node_find(path);
    return i < 0 ? -1 : i + 1;
}

int adt_path_offset_trace(const void *a, const char *path, int *offsets)
{
    int n = adt_path_offset(a, path);
    if (n < 0)
        return n;
    offsets[0] = n;
    offsets[1] = 0;
    return n;
}

const void *adt_getprop(const void *a, int node, const char *name, u32 *lenp)
{
    UNUSED(a);
    if (node < 1 || node > nnodes)
        return NULL;
    struct fnode *n = &nodes[node - 1];
    for (int i = 0; i < n->nprops; i++)
        if (!strcmp(n->props[i]->name, name)) {
            if (lenp)
                *lenp = n->props[i]->size;
            return n->props[i]->value;
        }
    return NULL;
}

int adt_get_reg(const void *a, int *path, const char *prop, int idx, u64 *addr, u64 *size)
{
    UNUSED(a);
    UNUSED(prop);
    int last = 0;
    for (int i = 0; path[i]; i++)
        last = path[i];
    if (last < 1)
        return -1;
    struct fnode *n = &nodes[last - 1];
    if (idx < 0 || idx >= n->nreg)
        return -1;
    if (addr)
        *addr = n->reg[idx][0];
    if (size)
        *size = n->reg[idx][1];
    return 0;
}

bool adt_is_compatible(const void *a, int node, const char *compat)
{
    u32 len = 0;
    const char *c = adt_getprop(a, node, "compatible", &len);
    for (u32 i = 0; c && i < len; i += strlen(c + i) + 1)
        if (!strcmp(c + i, compat))
            return true;
    return false;
}

int host_adt_nprops(int node)
{
    return node >= 1 && node <= nnodes ? nodes[node - 1].nprops : 0;
}

const struct adt_property *host_adt_prop(int node, int i)
{
    return nodes[node - 1].props[i];
}

int adt_get_property_count(const void *a, int node)
{
    UNUSED(a);
    return host_adt_nprops(node);
}

/* ---- fake PMGR table: device id -> name, virtual or power state register, parents ---- */

static struct fdev {
    u16 id;
    const char *name;
    bool virt;
    u64 psreg;
    u16 parent[2];
} fdevs[16];
static int nfdevs;

static void fdev_add(u16 id, const char *name, bool virt, u64 psreg, u16 p0)
{
    fdevs[nfdevs++] = (struct fdev){id, name, virt, psreg, {p0, 0}};
}

int pmgr_device_gate(u8 die, u16 id, struct pmgr_gate *g)
{
    if (!id)
        return -1;
    memset(g, 0, sizeof(*g));
    g->entry = ((u32)die << 28) | id;
    g->id = id;
    g->die = die;
    for (int i = 0; i < nfdevs; i++)
        if (fdevs[i].id == id) {
            g->found = true;
            g->virt = fdevs[i].virt;
            g->name = fdevs[i].name;
            g->psreg = fdevs[i].virt ? 0 : fdevs[i].psreg;
            g->parent[0] = fdevs[i].parent[0];
            g->parent[1] = fdevs[i].parent[1];
        }
    return 0;
}

int pmgr_adt_gate(const char *path, const char *prop, u32 index, struct pmgr_gate *g)
{
    u32 len = 0;
    int node = adt_path_offset(adt, path);
    const u32 *e = node < 0 ? NULL : adt_getprop(adt, node, prop, &len);
    if (!e || index >= len / 4)
        return -1;
    pmgr_device_gate(e[index] >> 28, e[index] & 0xffff, g);
    g->entry = e[index];
    return 0;
}

/* ---- fake MMIO and DRAM ---- */

struct region {
    u64 base, size;
    u8 *buf;
};
static struct region regions[16];
static int nregions, mmio_writes;

static void region_add(u64 base, u64 size)
{
    regions[nregions++] = (struct region){base, size, calloc(1, size)};
}

static u8 *mem(u64 a, int len)
{
    for (int i = 0; i < nregions; i++)
        if (a >= regions[i].base && a + len <= regions[i].base + regions[i].size)
            return regions[i].buf + (a - regions[i].base);
    return NULL;
}

static void poke32(u64 a, u32 v)
{
    memcpy(mem(a, 4), &v, 4);
}

static void poke64(u64 a, u64 v)
{
    memcpy(mem(a, 8), &v, 8);
}

static u64 peek64(u64 a)
{
    u64 v;
    memcpy(&v, mem(a, 8), 8);
    return v;
}

static void fault(const char *what, u64 a)
{
    if (exc_guard) {
        exc_count++;
        return;
    }
    printf("FAIL unguarded %s of unmapped 0x%lx\n", what, (unsigned long)a);
    exit(3);
}

u32 read32(u64 a)
{
    u8 *p = mem(a, 4);
    u32 v = 0;
    if (!p)
        fault("read32", a);
    else
        memcpy(&v, p, 4);
    return v;
}

u64 read64(u64 a)
{
    u8 *p = mem(a, 8);
    u64 v = 0;
    if (!p)
        fault("read64", a);
    else
        memcpy(&v, p, 8);
    return v;
}

void write32(u64 a, u32 v)
{
    u8 *p = mem(a, 4);
    mmio_writes++;
    if (!p)
        fault("write32", a);
    else
        memcpy(p, &v, 4);
}

/* ---- DART page tables (T8110 two-level format) ---- */

#define PTE_ADDR(x) (((x) >> 4) & 0xffffffffc00ULL)
struct pt {
    u64 lo, next;
};

static void pt_init(struct pt *t, u64 lo, u64 size)
{
    region_add(lo, size);
    t->lo = lo;
    t->next = lo + 0x4000;
}

static void pt_map(struct pt *t, u64 dva, u64 pa, u64 size)
{
    for (u64 o = 0; o < size; o += 0x4000) {
        u64 d = dva + o, i = d >> 25, j = (d >> 14) & 2047;
        u64 e1 = peek64(t->lo + 8 * i);
        if (!(e1 & 1)) {
            e1 = PTE_ADDR(t->next) | 1;
            t->next += 0x4000;
            poke64(t->lo + 8 * i, e1);
        }
        u64 l2 = (e1 & 0xffffffffc00ULL) << 4;
        poke64(l2 + 8 * j, PTE_ADDR(pa + o) | 3);
    }
}

static u32 ttbr_of(u64 l1)
{
    return (u32)(((l1 >> 14) << 2) | 1);
}

/* ---- the J613-shaped world ---- */

#define TB         0x10000000000ULL
static bool fw25;
#define J613_UUID (fw25 ? "C042E95C-B9D8-3F0E-94B3-582A08AA6FDD" : "90F849E1-B422-367E-B389-50246F8DEC47")
#define HANDOFF (fw25 ? "apple,j613-25g83-mapping-handoff" : "apple,t8122-handoff")
#define DART_DCP   0x28d30c000ULL
#define DART_DISP0 0x28d304000ULL
#define DCP_COPROC 0x28ec00000ULL

struct seg {
    u64 phys, iova, remap;
    u32 size, flags;
};

enum { SEG_TEXT, SEG_DATA, SEG_OSLOG, SEG_HEAP3, SEG_HEAP4, SEG_HEAP5, SEG_VRAM, NSEGS };

/* Synthetic addresses; flags, order and stream coverage as on a J613 */
static const struct seg segs[NSEGS] = {
    {TB + 0x280000000ULL, 0, TB + 0x40000000, 0x10000, 1},
    {TB + 0x280010000ULL, 0x10000, TB + 0x80000000, 0x8000, 0},
    {TB + 0x24a4000ULL, 0x18000, TB + 0x24a4000ULL, 0x20000, 0xa},
    {TB + 0x280020000ULL, ~0ULL, TB + 0x968000, 0x40000, 2},
    {TB + 0x280060000ULL, ~0ULL, TB + 0x1990000, 0x4000, 2},
    {TB + 0x280064000ULL, ~0ULL, TB + 0x1994000, 0xc000, 2},
    {TB + 0x280100000ULL, ~0ULL, TB + 0x1a54000, 0x100000, 2},
};
static const int region_ids[NSEGS] = {49, 50, -1, 57, 94, 95, 14};

/* Which display stream maps each segment: SID0 segments 4 and 6, SID4 segment 5 */
#define SID0_SEGS ((1u << SEG_HEAP4) | (1u << SEG_VRAM))
#define SID4_SEGS (1u << SEG_HEAP5)

struct opts {
    bool armed, twice, wrong_uuid, not_running, stray_map, power_off;
};

static void world_j613(const struct opts *o)
{
    chip_id = T8122;
    ram_base = TB + 0x800000;
    ram_size = 0x200000000ULL - 0x800000;
    prop_u64("/chosen", "dram-base", TB);
    prop_u64("/chosen", "dram-size", 0x300000000ULL);
    mcc_carveout_count = 0;

    prop_set("/arm-io/dcp/iop-dcp-nub", "segment-ranges", segs, sizeof(segs));
    prop_str("/arm-io/dcp/iop-dcp-nub", "segment-names",
             "__TEXT;__DATA;__OS_LOG;unknown;unknown;unknown;unknown");
    prop_str("/arm-io/dcp/iop-dcp-nub", "uuid",
             o->wrong_uuid ? "11111111-2222-3333-4444-555555555555" : J613_UUID);
    prop_u32("/arm-io/dcp/iop-dcp-nub", "pre-loaded", 1);
    prop_u32("/arm-io/dcp/iop-dcp-nub", "running", 1);
    for (int i = 0; i < NSEGS; i++) {
        if (region_ids[i] < 0)
            continue;
        char p[32];
        snprintf(p, sizeof(p), "region-id-%d", region_ids[i]);
        prop_pair("/chosen/carveout-memory-map", p, segs[i].phys,
                  ALIGN_UP((u64)segs[i].size, SZ_16K));
    }
    reg_add("/vram", segs[SEG_VRAM].phys, segs[SEG_VRAM].size);
    fb_base = segs[SEG_VRAM].phys + 0x4000;

    reg_add("/arm-io/dcp", DCP_COPROC, 0x6c000);
    prop_str("/arm-io/dcp", "compatible", "iop,ascwrap-v6");
    u32 irqs[4] = {538, 537, 540, 539};
    prop_set("/arm-io/dcp", "interrupts", irqs, sizeof(irqs));
    prop_str("/arm-io/dart-dcp", "compatible", "dart,t8110");
    reg_add("/arm-io/dart-dcp", DART_DCP, 0x4000);
    prop_u64("/arm-io/dart-dcp", "vm-base", TB);
    prop_u64("/arm-io/dart-dcp", "vm-size", 0x1000000000ULL);
    prop_pair("/arm-io/dart-dcp", "pt-region-5", TB + 0x2f0000000ULL, TB + 0x2f0020000ULL);
    prop_str("/arm-io/dart-disp0", "compatible", "dart,t8110");
    reg_add("/arm-io/dart-disp0", DART_DISP0, 0x4000);
    prop_u64("/arm-io/dart-disp0", "vm-base", TB);
    prop_u64("/arm-io/dart-disp0", "vm-size", 0x1000000000ULL);
    prop_pair("/arm-io/dart-disp0", "pt-region-0", TB + 0x2f0100000ULL, TB + 0x2f0110000ULL);
    prop_pair("/arm-io/dart-disp0", "pt-region-4", TB + 0x2f0200000ULL, TB + 0x2f0210000ULL);
    prop_str("/arm-io/disp0", "compatible", "disp0,t8122");
    reg_add("/arm-io/disp0", 0x28c000000ULL, 0x690000);

    /* PMGR: /arm-io/dcp's clock-gates name a virtual device whose parent is the display CPU */
    nfdevs = 0;
    fdev_add(0x10, "SOC_TOP", false, 0x2d0700100ULL, 0);
    fdev_add(0x11, "DISP_SYS", false, 0x2d07001d8ULL, 0x10);
    fdev_add(0x12, "DISP_FE", false, 0x2d0700208ULL, 0x11);
    fdev_add(0x13, "DISP_CPU", false, 0x2d0710000ULL, 0x12);
    fdev_add(0x14, "DCP_VIRT", true, 0, 0x13);
    u32 dcp_cg[1] = {0x14};
    prop_set("/arm-io/dcp", "clock-gates", dcp_cg, sizeof(dcp_cg));
    prop_set("/arm-io/dcp", "power-gates", dcp_cg, sizeof(dcp_cg));
    u32 disp_cg[1] = {0x12};
    prop_set("/arm-io/disp0", "clock-gates", disp_cg, sizeof(disp_cg));

    region_add(0x2d0700000ULL, 0x14000);
    poke32(0x2d0710000ULL, o->power_off ? 0x0 : 0xff);
    poke32(0x2d0700100ULL, 0xff);
    poke32(0x2d07001d8ULL, 0xff);
    poke32(0x2d0700208ULL, 0xff);
    region_add(DCP_COPROC, 0x4000);
    poke32(DCP_COPROC + 0x44, o->not_running ? 0 : 0x10);
    poke32(DCP_COPROC + 0x48, 0x3000ac);
    region_add(DART_DCP, 0x4000);
    region_add(DART_DISP0, 0x4000);
    poke32(DART_DCP + 0x200, 0x1e7);
    poke32(DART_DISP0 + 0x200, 0x1e1);

    struct pt p5, p0, p4;
    pt_init(&p5, TB + 0x2f0000000ULL, 0x20000);
    pt_init(&p0, TB + 0x2f0100000ULL, 0x10000);
    pt_init(&p4, TB + 0x2f0200000ULL, 0x10000);
    for (int i = 0; i < NSEGS; i++) {
        u64 size = ALIGN_UP((u64)segs[i].size, SZ_16K);
        if (i != SEG_OSLOG)
            pt_map(&p5, segs[i].remap - TB, segs[i].phys, size);
        if (SID0_SEGS & (1u << i))
            pt_map(&p0, segs[i].remap - TB, segs[i].phys, size);
        if (SID4_SEGS & (1u << i))
            pt_map(&p4, segs[i].remap - TB, segs[i].phys, size);
    }
    if (o->stray_map) /* a scanout page outside every firmware segment */
        pt_map(&p0, 0x5000000, TB + 0x10000000, 0x4000);
    poke32(DART_DCP + 0x1000 + 4 * 5, 1);
    poke32(DART_DCP + 0x1400 + 4 * 5, ttbr_of(p5.lo));
    poke32(DART_DISP0 + 0x1000, 1);
    poke32(DART_DISP0 + 0x1400, ttbr_of(p0.lo));
    poke32(DART_DISP0 + 0x1000 + 4 * 4, 1);
    poke32(DART_DISP0 + 0x1400 + 4 * 4, ttbr_of(p4.lo));
}

/*
 * ---- the T6030 (J516S) world, for comparing two trees' handoff output ----
 *
 * The J613 world with T6030's fixed eight-segment layout and region numbers, the dcp, dcpext0
 * and dcpext1 __OS_LOG buffers, the T6030 DCP image and the display CPU power state read directly
 * at 0x350710000. The t6030-* scenarios only run the handoff and keep its device tree and log;
 * tests/run-t6031-unchanged.sh checks that another tree gives the same bytes.
 */
static const struct seg segs6[8] = {
    {TB + 0x280000000ULL, 0, TB + 0x40000000, 0x10000, 1},
    {TB + 0x280010000ULL, 0x10000, TB + 0x80000000, 0x8000, 0},
    {TB + 0x24a4000ULL, 0x18000, TB + 0x24a4000ULL, 0x20000, 0xa},
    {TB + 0x280020000ULL, ~0ULL, TB + 0x968000, 0x40000, 2},
    {TB + 0x280060000ULL, ~0ULL, TB + 0x1990000, 0x4000, 2},
    {TB + 0x280064000ULL, ~0ULL, TB + 0x1994000, 0xc000, 2},
    {TB + 0x280100000ULL, ~0ULL, TB + 0x1a54000, 0x100000, 2},
    {TB + 0x280200000ULL, ~0ULL, TB + 0x1b60000, 0x4000, 2},
};
static const int region_ids6[8] = {49, 50, -1, 57, 94, 95, 14, 157};

static void world_t6030(const struct opts *o)
{
    chip_id = T6030;
    ram_base = TB + 0x800000;
    ram_size = 0x200000000ULL - 0x800000;
    prop_u64("/chosen", "dram-base", TB);
    prop_u64("/chosen", "dram-size", 0x300000000ULL);
    mcc_carveout_count = 0;

    prop_set("/arm-io/dcp/iop-dcp-nub", "segment-ranges", segs6, sizeof(segs6));
    prop_str("/arm-io/dcp/iop-dcp-nub", "segment-names",
             "__TEXT;__DATA;__OS_LOG;unknown;unknown;unknown;unknown;unknown");
    prop_str("/arm-io/dcp/iop-dcp-nub", "uuid",
             o->wrong_uuid ? "11111111-2222-3333-4444-555555555555"
                           : "DDF38191-93B3-324A-BC8F-643006F5AC82");
    prop_u32("/arm-io/dcp/iop-dcp-nub", "pre-loaded", 1);
    prop_u32("/arm-io/dcp/iop-dcp-nub", "running", 1);
    for (int i = 0; i < 2; i++) {
        /* dcpext0 and dcpext1: text, data, __OS_LOG and a heap segment */
        char path[64];
        struct seg ext[4] = {
            segs6[0],
            {TB + 0x281000000ULL + i * 0x100000ULL, 0x10000, TB + 0x80000000, 0x8000, 0},
            {TB + 0x24c4000ULL + i * 0x20000ULL, 0x18000, TB + 0x24c4000ULL + i * 0x20000ULL,
             0x20000, 0xa},
            {TB + 0x281800000ULL + i * 0x100000ULL, ~0ULL, TB + 0xc0000000, 0x40000, 2},
        };
        snprintf(path, sizeof(path), "/arm-io/dcpext%d/iop-dcpext%d-nub", i, i);
        prop_set(path, "segment-ranges", ext, sizeof(ext));
        prop_str(path, "segment-names", "__TEXT;__DATA;__OS_LOG;unknown");
    }
    for (int i = 0; i < 8; i++) {
        if (region_ids6[i] < 0)
            continue;
        char p[32];
        snprintf(p, sizeof(p), "region-id-%d", region_ids6[i]);
        prop_pair("/chosen/carveout-memory-map", p, segs6[i].phys,
                  ALIGN_UP((u64)segs6[i].size, SZ_16K));
    }
    reg_add("/vram", segs6[6].phys, segs6[6].size);
    fb_base = segs6[6].phys + 0x4000;

    reg_add("/arm-io/dcp", DCP_COPROC, 0x6c000);
    prop_str("/arm-io/dart-dcp", "compatible", "dart,t8110");
    reg_add("/arm-io/dart-dcp", DART_DCP, 0x4000);
    prop_u64("/arm-io/dart-dcp", "vm-base", TB);
    prop_u64("/arm-io/dart-dcp", "vm-size", 0x1000000000ULL);
    prop_pair("/arm-io/dart-dcp", "pt-region-5", TB + 0x2f0000000ULL, TB + 0x2f0020000ULL);
    prop_str("/arm-io/dart-disp0", "compatible", "dart,t8110");
    reg_add("/arm-io/dart-disp0", DART_DISP0, 0x4000);
    prop_u64("/arm-io/dart-disp0", "vm-base", TB);
    prop_u64("/arm-io/dart-disp0", "vm-size", 0x1000000000ULL);
    prop_pair("/arm-io/dart-disp0", "pt-region-0", TB + 0x2f0100000ULL, TB + 0x2f0110000ULL);
    prop_pair("/arm-io/dart-disp0", "pt-region-4", TB + 0x2f0200000ULL, TB + 0x2f0210000ULL);

    region_add(0x350700000ULL, 0x14000);
    poke32(0x350710000ULL, o->power_off ? 0x0 : 0xff);
    region_add(DCP_COPROC, 0x4000);
    poke32(DCP_COPROC + 0x44, o->not_running ? 0 : 0x10);
    poke32(DCP_COPROC + 0x48, 0x3000ac);
    region_add(DART_DCP, 0x4000);
    region_add(DART_DISP0, 0x4000);
    poke32(DART_DCP + 0x200, 0x1e0);
    poke32(DART_DISP0 + 0x200, 0x1e1);

    struct pt p5, p0, p4;
    pt_init(&p5, TB + 0x2f0000000ULL, 0x20000);
    pt_init(&p0, TB + 0x2f0100000ULL, 0x10000);
    pt_init(&p4, TB + 0x2f0200000ULL, 0x10000);
    for (int i = 0; i < 8; i++) {
        u64 size = ALIGN_UP((u64)segs6[i].size, SZ_16K);
        if (i != SEG_OSLOG)
            pt_map(&p5, segs6[i].remap - TB, segs6[i].phys, size);
        if (SID0_SEGS & (1u << i))
            pt_map(&p0, segs6[i].remap - TB, segs6[i].phys, size);
        if (SID4_SEGS & (1u << i))
            pt_map(&p4, segs6[i].remap - TB, segs6[i].phys, size);
    }
    if (o->stray_map)
        pt_map(&p0, 0x5000000, TB + 0x10000000, 0x4000);
    poke32(DART_DCP + 0x1000 + 4 * 5, 1);
    poke32(DART_DCP + 0x1400 + 4 * 5, ttbr_of(p5.lo));
    poke32(DART_DISP0 + 0x1000, 1);
    poke32(DART_DISP0 + 0x1400, ttbr_of(p0.lo));
    poke32(DART_DISP0 + 0x1000 + 4 * 4, 1);
    poke32(DART_DISP0 + 0x1400 + 4 * 4, ttbr_of(p4.lo));
}

/* ---- checks ---- */

static int failures;
static bool quiet;

static void check(bool ok, const char *fmt, ...)
{
    va_list ap;
    if (!ok)
        failures++;
    if (quiet)
        return;
    printf(ok ? "ok   " : "FAIL ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

/* The single node compatible with @compat, or -1 if there are none or several */
static int one_compatible(const void *dt, const char *compat)
{
    int node = fdt_node_offset_by_compatible(dt, -1, compat);
    if (node < 0 || fdt_node_offset_by_compatible(dt, node, compat) >= 0)
        return -1;
    return node;
}

static bool status_is(const void *dt, int node, const char *want)
{
    const char *s = fdt_getprop(dt, node, "status", NULL);
    return s && !strcmp(s, want);
}

/* of_device_is_available(): no status, or "okay"/"ok" */
static bool available(const void *dt, int node)
{
    const char *s = fdt_getprop(dt, node, "status", NULL);
    return !s || !strcmp(s, "okay") || !strcmp(s, "ok");
}

static bool marker_set(const void *dt, int node)
{
    int len;
    const fdt32_t *v = fdt_getprop(dt, node, HANDOFF, &len);
    return v && len == 4 && fdt32_ld(v) == 1;
}

/* The target of a single <phandle arg> entry of @list (@cells argument cells), or -1 */
static int single_ref(const void *dt, int node, const char *list, int cells, u32 *arg)
{
    int len;
    const fdt32_t *v = fdt_getprop(dt, node, list, &len);
    if (!v || len != 4 * (1 + cells))
        return -1;
    if (cells && arg)
        *arg = fdt32_ld(v + 1);
    return fdt_node_offset_by_phandle(dt, fdt32_ld(v));
}

/* memory-region entry @i of @node: the /reserved-memory node, or -1 */
static int region_at(const void *dt, int node, int i)
{
    int len;
    const fdt32_t *v = fdt_getprop(dt, node, "memory-region", &len);
    if (!v || len % 4 || i >= len / 4)
        return -1;
    int r = fdt_node_offset_by_phandle(dt, fdt32_ld(v + i));
    int resv = fdt_path_offset(dt, "/reserved-memory");
    return r >= 0 && fdt_parent_offset(dt, r) == resv ? r : -1;
}

static int region_count(const void *dt, int node)
{
    int len;
    const fdt32_t *v = fdt_getprop(dt, node, "memory-region", &len);
    return v && !(len % 4) ? len / 4 : 0;
}

static bool reg_of(const void *dt, int node, u64 *base, u64 *size)
{
    int len;
    const fdt64_t *r = fdt_getprop(dt, node, "reg", &len);
    if (!r || len != 16)
        return false;
    *base = fdt64_ld(r);
    *size = fdt64_ld(r + 1);
    return true;
}

/* The DVA of the iommu-addresses entry of @region for @dev (2+2 cells), or ~0 */
static u64 iommu_dva(const void *dt, int region, int dev, u64 *size)
{
    int len;
    const fdt32_t *v = fdt_getprop(dt, region, "iommu-addresses", &len);
    u32 ph = fdt_get_phandle(dt, dev);
    for (int i = 0; v && ph && i + 20 <= len; i += 20) {
        const fdt32_t *e = v + i / 4;
        if (fdt32_ld(e) != ph)
            continue;
        *size = ((u64)fdt32_ld(e + 3) << 32) | fdt32_ld(e + 4);
        return ((u64)fdt32_ld(e + 1) << 32) | fdt32_ld(e + 2);
    }
    return ~0ULL;
}

/* Mask of the segments @node's memory-region list covers exactly (reg == segment) */
static u32 linked_segments(const void *dt, int node)
{
    u32 mask = 0;
    for (int i = 0; i < region_count(dt, node); i++) {
        u64 base, size;
        int r = region_at(dt, node, i);
        if (r < 0 || !reg_of(dt, r, &base, &size))
            continue;
        for (int s = 0; s < NSEGS; s++)
            if (base == segs[s].phys && size == ALIGN_UP((u64)segs[s].size, SZ_16K))
                mask |= 1u << s;
    }
    return mask;
}

/* The memory-region of @node whose reg is segment @s, or -1 */
static int segment_region(const void *dt, int node, int s)
{
    for (int i = 0; i < region_count(dt, node); i++) {
        u64 base, size;
        int r = region_at(dt, node, i);
        if (r >= 0 && reg_of(dt, r, &base, &size) && base == segs[s].phys &&
            size == ALIGN_UP((u64)segs[s].size, SZ_16K))
            return r;
    }
    return -1;
}

/* Every DART-mapped segment in @mask is linked with an iommu-addresses entry at its DVA */
static bool dvas_match(const void *dt, int dev, u32 mask)
{
    for (int s = 0; s < NSEGS; s++) {
        if (!(mask & (1u << s)))
            continue;
        u64 size = 0;
        int r = segment_region(dt, dev, s);
        if (r < 0 || iommu_dva(dt, r, dev, &size) != segs[s].remap ||
            size != ALIGN_UP((u64)segs[s].size, SZ_16K))
            return false;
    }
    return true;
}

/* memory-region-names has one entry per memory-region */
static bool names_match_regions(const void *dt, int node)
{
    int n = fdt_stringlist_count(dt, node, "memory-region-names");
    return n == region_count(dt, node);
}

/* The handoff contract, as the kernel reads it. Returns the number of failed checks. */
static int contract_check(const void *dt)
{
    int before = failures;

    /* The display enable gate */
    int dcp = one_compatible(dt, "apple,t8122-dcp");
    int disp = one_compatible(dt, "apple,t8122-display-subsystem");
    check(dcp >= 0 && disp >= 0, "gate: exactly one apple,t8122-dcp and one display subsystem");
    if (dcp < 0 || disp < 0)
        return failures - before;
    check(marker_set(dt, dcp) && marker_set(dt, disp),
          "gate: apple,t8122-handoff = <1> on the dcp and the display subsystem");
    u32 sid_dcp = ~0u, sid_disp = ~0u;
    int dart_dcp = single_ref(dt, dcp, "iommus", 1, &sid_dcp);
    int dart_disp = single_ref(dt, disp, "iommus", 1, &sid_disp);
    check(dart_dcp >= 0 && sid_dcp == 5 &&
              !fdt_node_check_compatible(dt, dart_dcp, "apple,t8110-dart"),
          "gate: dcp iommus is one apple,t8110-dart stream 5");
    check(dart_disp >= 0 && sid_disp == 0 &&
              !fdt_node_check_compatible(dt, dart_disp, "apple,t8110-dart") &&
              dart_disp != dart_dcp,
          "gate: display iommus is another apple,t8110-dart, stream 0");
    int mbox = single_ref(dt, dcp, "mboxes", 0, NULL);
    check(mbox >= 0 && !fdt_node_check_compatible(dt, mbox, "apple,asc-mailbox-v4"),
          "gate: dcp mboxes is one apple,asc-mailbox-v4");
    check(status_is(dt, dcp, "disabled") && status_is(dt, disp, "disabled") && dart_dcp >= 0 &&
              status_is(dt, dart_dcp, "disabled") && dart_disp >= 0 &&
              status_is(dt, dart_disp, "disabled") && mbox >= 0 &&
              status_is(dt, mbox, "disabled"),
          "gate: dcp, display, both DARTs and the mailbox are still disabled");

    /* drm/apple, 14.x IOMFB, J613 board */
    check(!fdt_node_check_compatible(dt, 0, "apple,j613"), "drm: the root is apple,j613");
    const char *uuid = fdt_getprop(dt, dcp, "apple,firmware-uuid", NULL);
    check(uuid && !strcmp(uuid, J613_UUID), "drm: dcp apple,firmware-uuid is %s", J613_UUID);
    int len;
    const fdt32_t *compat = fdt_getprop(dt, dcp, "apple,firmware-compat", &len);
    check(compat && len == 12 && fdt32_ld(compat) == (fw25 ? 26 : 14) && fdt32_ld(compat + 1) == (fw25 ? 6 : 7) &&
              fdt32_ld(compat + 2) == (fw25 ? 2 : 0),
          "drm: dcp apple,firmware-compat is 14.7.0");

    int oslog = segment_region(dt, dcp, SEG_OSLOG);
    u64 dummy;
    check(oslog >= 0 && fdt_getprop(dt, oslog, "apple,dcp-os-log", NULL) &&
              iommu_dva(dt, oslog, dcp, &dummy) == ~0ULL,
          "drm: the __OS_LOG buffer is a dcp memory-region at its physical address, marked "
          "apple,dcp-os-log");
    u32 sid5 = ((1u << NSEGS) - 1) & ~(1u << SEG_OSLOG);
    check(linked_segments(dt, dcp) == ((1u << NSEGS) - 1),
          "drm: every iop-dcp-nub segment is a dcp memory-region (mask 0x%x)",
          linked_segments(dt, dcp));
    check(dvas_match(dt, dcp, sid5),
          "drm: every DART-mapped dcp segment has iommu-addresses <&dcp remap size>");
    bool regions_ok = true;
    for (int i = 0; i < region_count(dt, dcp); i++) {
        int r = region_at(dt, dcp, i);
        u64 base, size;
        regions_ok &= r >= 0 && reg_of(dt, r, &base, &size) && fdt_getprop(dt, r, "no-map", NULL);
    }
    check(regions_ok && region_count(dt, dcp) > 0,
          "drm: every dcp memory-region is a no-map /reserved-memory node with a reg");

    int pio = fdt_subnode_offset(dt, dcp, "piodma");
    u32 sid_pio = ~0u;
    int dart_pio = pio >= 0 ? single_ref(dt, pio, "iommus", 1, &sid_pio) : -1;
    check(pio >= 0 && (fw25 ? status_is(dt, pio, "disabled") : available(dt, pio)) && marker_set(dt, pio),
          "drm: the dcp has an available \"piodma\" child marked apple,t8122-handoff");
    check(pio >= 0 && dart_pio == dart_disp && sid_pio == 4,
          "drm: piodma iommus is the display's DART, stream 4");
    check(pio >= 0 && linked_segments(dt, pio) == SID4_SEGS && dvas_match(dt, pio, SID4_SEGS),
          "drm: piodma memory-region is the stream 4 segment, at its DVA");

    int fb_idx = fdt_stringlist_search(dt, disp, "memory-region-names", "framebuffer");
    int fb = fb_idx >= 0 ? region_at(dt, disp, fb_idx) : -1;
    u64 fb_reg, fb_size;
    check(fb >= 0 && available(dt, fb) && !fdt_node_check_compatible(dt, fb, "framebuffer") &&
              reg_of(dt, fb, &fb_reg, &fb_size) && fb_reg == segs[SEG_VRAM].phys &&
              fb_size == ALIGN_UP((u64)segs[SEG_VRAM].size, SZ_16K),
          "drm: the display's \"framebuffer\" memory-region is available, compatible "
          "\"framebuffer\" and is /vram");
    check(linked_segments(dt, disp) == SID0_SEGS && dvas_match(dt, disp, SID0_SEGS),
          "drm: display memory-region is the stream 0 segments (mask 0x%x), at their DVAs",
          linked_segments(dt, disp));
    check(names_match_regions(dt, dcp) && names_match_regions(dt, disp) &&
              (pio < 0 || names_match_regions(dt, pio)),
          "drm: memory-region-names has one name per memory-region on dcp, display and piodma");

    /* One /reserved-memory node per segment, shared by every device that maps it */
    bool shared = true;
    for (int s = 0; s < NSEGS; s++) {
        int a = segment_region(dt, dcp, s);
        int b = (SID0_SEGS & (1u << s)) ? segment_region(dt, disp, s) : a;
        int c = (SID4_SEGS & (1u << s)) && pio >= 0 ? segment_region(dt, pio, s) : a;
        shared &= a >= 0 && a == b && a == c;
    }
    check(shared, "drm: each segment is one /reserved-memory node shared by the devices that "
                  "map it");
    return failures - before;
}

/* No handoff published: no marker anywhere, display nodes untouched, __OS_LOG still reserved */
static void refusal_check(const void *dt)
{
    int dcp = one_compatible(dt, "apple,t8122-dcp");
    int disp = one_compatible(dt, "apple,t8122-display-subsystem");
    int pio = dcp >= 0 ? fdt_subnode_offset(dt, dcp, "piodma") : -1;
    check(dcp >= 0 && disp >= 0 && !marker_set(dt, dcp) && !marker_set(dt, disp) &&
              (pio < 0 || !marker_set(dt, pio)),
          "refused: no apple,t8122-handoff marker on dcp, display or piodma");
    check(dcp >= 0 && disp >= 0 && !fdt_getprop(dt, dcp, "memory-region", NULL) &&
              !fdt_getprop(dt, disp, "memory-region", NULL) &&
              (pio < 0 || !fdt_getprop(dt, pio, "memory-region", NULL)),
          "refused: no memory-region on dcp, display or piodma");
    check(dcp >= 0 && disp >= 0 && status_is(dt, dcp, "disabled") &&
              status_is(dt, disp, "disabled"),
          "refused: dcp and display still disabled");

    int resv = fdt_path_offset(dt, "/reserved-memory"), node, found = 0;
    fdt_for_each_subnode(node, dt, resv)
    {
        u64 base, size;
        if (reg_of(dt, node, &base, &size) && base == segs[SEG_OSLOG].phys &&
            size == segs[SEG_OSLOG].size && fdt_getprop(dt, node, "no-map", NULL))
            found++;
    }
    check(found == 1, "refused: the __OS_LOG buffer is still reserved no-map, once");
}

/* The checker must notice each of these breakages of a published handoff */
static void contract_selftest(const void *published)
{
    static const char *const what[] = {
        "display memory-region-names removed", "piodma disabled",
        "dcp firmware-compat 14.8.3",          "apple,dcp-os-log removed",
        "display marker removed",              "framebuffer not compatible framebuffer",
    };
    static u8 copy[1 << 20];
    for (unsigned m = 0; m < ARRAY_SIZE(what); m++) {
        memcpy(copy, published, fdt_totalsize(published));
        int dcp = one_compatible(copy, "apple,t8122-dcp");
        int disp = one_compatible(copy, "apple,t8122-display-subsystem");
        int pio = fdt_subnode_offset(copy, dcp, "piodma");
        int ret = -1;
        switch (m) {
            case 0:
                ret = fdt_delprop(copy, disp, "memory-region-names");
                break;
            case 1:
                ret = fdt_setprop_string(copy, pio, "status", fw25 ? "okay" : "disabled");
                break;
            case 2: {
                fdt32_t v[3] = {cpu_to_fdt32(14), cpu_to_fdt32(8), cpu_to_fdt32(3)};
                ret = fdt_setprop(copy, dcp, "apple,firmware-compat", v, sizeof(v));
                break;
            }
            case 3:
                ret = fdt_delprop(copy, segment_region(copy, dcp, SEG_OSLOG), "apple,dcp-os-log");
                break;
            case 4:
                ret = fdt_delprop(copy, disp, HANDOFF);
                break;
            case 5: {
                int i = fdt_stringlist_search(copy, disp, "memory-region-names", "framebuffer");
                ret = fdt_setprop_string(copy, region_at(copy, disp, i), "compatible",
                                         "apple,asc-mem");
                break;
            }
        }
        int before = failures;
        quiet = true;
        int caught = ret ? 0 : contract_check(copy);
        quiet = false;
        failures = before;
        check(!ret && caught > 0, "selftest: %s is caught (%d check(s) fail)", what[m], caught);
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <in.dtb> <scenario> [out.dtb]\n", argv[0]);
        return 2;
    }
    const char *sc = argv[2];
    /* t6030-<scenario>: the same scenario in the T6030 world, run and kept, not checked */
    bool t6030 = !strncmp(sc, "t6030-", 6);
    if (t6030)
        sc += 6;
    fw25 = !t6030 && getenv("J613_25G83") != NULL;
    struct opts o = {0};
    bool publish = true;
    if (!strcmp(sc, "unarmed")) {
        publish = false;
    } else if (!strcmp(sc, "publish") || !strcmp(sc, "contract-selftest")) {
        o.armed = true;
    } else if (!strcmp(sc, "repeat")) {
        o.armed = o.twice = true;
    } else if (!strcmp(sc, "wrong-image")) {
        o.armed = o.wrong_uuid = true;
        publish = false;
    } else if (!strcmp(sc, "not-running")) {
        o.armed = o.not_running = true;
        publish = false;
    } else if (!strcmp(sc, "outside-segment")) {
        o.armed = o.stray_map = true;
        publish = false;
    } else if (!strcmp(sc, "power-off")) {
        o.armed = o.power_off = true;
        publish = false;
    } else if (!strcmp(sc, "missing-clock") || !strcmp(sc, "wrong-os") ||
               !strcmp(sc, "missing-profile")) {
        o.armed = true;
        publish = false;
    } else {
        fprintf(stderr, "unknown scenario %s\n", sc);
        return 2;
    }

    FILE *f = fopen(argv[1], "rb");
    static u8 in[1 << 20], dt[1 << 20];
    if (!f || !fread(in, 1, sizeof(in), f) || fdt_check_header(in))
        return 2;
    fclose(f);
    if (fdt_open_into(in, dt, fdt_totalsize(in) + 128 * 1024))
        return 2;

    if (t6030)
        world_t6030(&o);
    else
        world_j613(&o);
    if (fw25) {
        os_firmware = (struct fw_version_info){V26_6_2, "26.6.2", {26, 6, 2}, 3, "mBoot-18000.161.10"};
        int dcp = fdt_path_offset(dt, "dcp");
        fdt_setprop_u32(dt, dcp, "apple,j613-25g83-profile", 1);
        fdt32_t compat[] = {cpu_to_fdt32(26), cpu_to_fdt32(6), cpu_to_fdt32(2)};
        fdt_setprop(dt, fdt_path_offset(dt, "dcp"), "apple,firmware-compat", compat, sizeof(compat));
        fdt_setprop_string(dt, fdt_path_offset(dt, "disp0-piodma"), "status", "disabled");
        u32 ids[2] = {345, 416}, frequencies[161] = {0};
        frequencies[89] = 712000000;
        prop_set("/arm-io/disp0", "clock-ids", ids, sizeof(ids));
        if (strcmp(sc, "missing-clock"))
            prop_set("/arm-io", "clock-frequencies", frequencies, sizeof(frequencies));
        if (!strcmp(sc, "wrong-os"))
            os_firmware.version = V14_7;
        if (!strcmp(sc, "missing-profile"))
            fdt_delprop(dt, fdt_path_offset(dt, "dcp"), "apple,j613-25g83-profile");
    }
    if (o.armed)
        fdt_setprop_string(dt, fdt_path_offset(dt, "/chosen"),
                           t6030 ? "asahi,t6030-dcp" : "asahi,t8122-dcp", "1");

    printf("== %s\n", sc);
    fflush(stdout);
    int ret = dt_set_display_handoff(dt);
    if (o.twice) {
        static u8 first[1 << 20];
        memcpy(first, dt, fdt_totalsize(dt));
        int ret2 = dt_set_display_handoff(dt);
        check(ret2 == 0, "second call returns 0");
        int dcp = one_compatible(dt, "apple,t8122-dcp");
        int dcp1 = one_compatible(first, "apple,t8122-dcp");
        check(region_count(dt, dcp) == region_count(first, dcp1),
              "second call adds no dcp memory-region");
    }
    fflush(stdout);
    check(ret == 0, "dt_set_display_handoff() returns 0");
    check(exc_count == 0, "no MMIO access faulted");
    if (!o.armed && !t6030)
        check(mmio_writes == 0, "unarmed: no MMIO write");

    if (t6030) {
        /* Compared with another tree's output by tests/run-t6031-unchanged.sh */
    } else if (publish) {
        contract_check(dt);
        if (fw25) {
            int len, chosen = fdt_path_offset(dt, "/chosen");
            const fdt32_t *clock = fdt_getprop(dt, chosen, "apple,j613-25g83-display-clock-adt", &len);
            check(clock && len == 20 && fdt32_ld(clock) == 1 && fdt32_ld(clock+1) == 345 &&
                  fdt32_ld(clock+2) == 416 && fdt32_ld(clock+3) == 712000000 && !fdt32_ld(clock+4),
                  "25G83 same-boot clock cells");
            check(!fdt_getprop(dt, fdt_path_offset(dt, "dcp"), "apple,t8122-handoff", NULL),
                  "25G83 never publishes 14.x marker");
        }
    } else
        refusal_check(dt);
    if (!strcmp(sc, "contract-selftest"))
        contract_selftest(dt);

    if (argc > 3) {
        f = fopen(argv[3], "wb");
        if (f) {
            fwrite(dt, 1, fdt_totalsize(dt), f);
            fclose(f);
        }
    }
    printf("%s: %s\n", sc, failures ? "FAIL" : "pass");
    return failures ? 1 : 0;
}
