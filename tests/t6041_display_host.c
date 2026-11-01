/* SPDX-License-Identifier: MIT */
/*
 * Host test of the T6041 (M4 Pro J616s) internal display handoff in src/kboot_display_handoff.c.
 *
 * The T6041 handoff has no /chosen opt-in: the chip (T6040), the board (apple,j616s) and the
 * device tree (aliases dcp and disp0 naming apple,t6041-dcp and apple,display-subsystem nodes)
 * decide whether it is attempted. This checks that a boot with no chosen property publishes, that
 * an unsupported chip, board or device tree gets nothing at all (no register access, the device
 * tree byte for byte unchanged), and that every failed validation publishes nothing: no marker, no
 * memory-region, no firmware property and no reserved-memory node other than the __OS_LOG one,
 * which is its own transaction and is kept from the OS on every supported boot.
 *
 * A fake ADT, a fake PMGR table and fake MMIO/DRAM stand in for the Mac (the stand-in headers are
 * tests/t8122_display_host/); libfdt is aura's own. The device tree is built here with libfdt, so
 * no dtc is needed. Addresses are synthetic, except for the T6041 register blocks the handoff
 * checks against fixed values (dcp, dart-dcp, dart-disp0); stream IDs are T6041's: dart-dcp SID
 * 23, dart-disp0 SIDs 0 (display) and 16 (piodma), of 32.
 *
 *   t6041-display-host list
 *   t6041-display-host <scenario>
 *
 * Each scenario runs dt_set_display_handoff() once (twice for "repeat"), prints what it checked
 * and exits 0 if every check passed. tests/python/test_t6041_display_handoff.py runs them all.
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
#include "types.h"
#include "utils.h"

#include "libfdt/libfdt.h"

int dt_set_display_handoff(void *dt);

void *adt = (void *)1;
u32 chip_id;
volatile enum exc_guard_t exc_guard;
volatile int exc_count;
struct fw_version_info os_firmware;
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
/* Where stage 1 loaded aura: clear of the __OS_LOG buffer below */
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

/* Remove a node and everything below it */
static void node_drop(const char *path)
{
    size_t n = strlen(path);
    for (int i = 0; i < nnodes; i++)
        if (!strncmp(nodes[i].path, path, n) && (!nodes[i].path[n] || nodes[i].path[n] == '/'))
            nodes[i].path[0] = 0;
}

static void prop_set(const char *path, const char *name, const void *val, u32 len)
{
    struct fnode *n = node_get(path);
    for (int i = 0; i < n->nprops; i++)
        if (!strcmp(n->props[i]->name, name)) {
            free(n->props[i]);
            n->props[i] = n->props[--n->nprops];
            break;
        }
    struct adt_property *p = calloc(1, sizeof(*p) + len + 8);
    snprintf(p->name, sizeof(p->name), "%s", name);
    p->size = len;
    memcpy(p->value, val, len);
    n->props[n->nprops++] = p;
}

static void prop_drop(const char *path, const char *name)
{
    struct fnode *n = node_get(path);
    for (int i = 0; i < n->nprops; i++)
        if (!strcmp(n->props[i]->name, name)) {
            free(n->props[i]);
            n->props[i] = n->props[--n->nprops];
            return;
        }
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

/* ---- fake PMGR table: device id -> name, virtual or power state register, parent ---- */

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

/* ---- fake MMIO and DRAM; every access is counted ---- */

struct region {
    u64 base, size;
    u8 *buf;
};
static struct region regions[16];
static int nregions, mmio_reads, mmio_writes;
static u64 last_write;

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

static u32 peek32(u64 a)
{
    u32 v;
    memcpy(&v, mem(a, 4), 4);
    return v;
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
    printf("FAIL unguarded %s of unmapped 0x%llx\n", what, (unsigned long long)a);
    exit(3);
}

u32 read32(u64 a)
{
    u8 *p = mem(a, 4);
    u32 v = 0;
    mmio_reads++;
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
    mmio_reads++;
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
    last_write = a;
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

/* ---- the J616s-shaped world ---- */

#define TB         0x10000000000ULL
#define DCP_COPROC 0x402e00000ULL /* the T6041 values the handoff checks the ADT against */
#define DART_DCP   0x402340000ULL
#define DART_DISP0 0x402300000ULL
#define PMGR_BASE  0x400700000ULL /* synthetic */
#define PS_DISP    (PMGR_BASE + 0x10000)
#define SID_DCP    23
#define SID_DISP   0
#define SID_PIODMA 16
#define MARKER     "apple,t6041-handoff"
#define OPTIN      "asahi,t6041-dcp" /* the removed opt-in, for the stale-optin scenario */
#define DCP_UUID   "8E5C2B1A-4F7D-3A6E-9B0C-1D2E3F405162"
#define EXT0       0x3a2e00000ULL
#define EXT1       0x602e00000ULL
#define EXT_DART0  0x3a2340000ULL
#define EXT_DART1  0x602340000ULL

struct seg {
    u64 phys, iova, remap;
    u32 size, flags;
};

enum { SEG_TEXT, SEG_DATA, SEG_OSLOG, SEG_HEAP3, SEG_HEAP4, SEG_HEAP5, SEG_VRAM, NSEGS };

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

/* The display stream maps segments 4 and 6 (/vram), the piodma stream segment 5 */
#define DISP_SEGS   ((1u << SEG_HEAP4) | (1u << SEG_VRAM))
#define PIODMA_SEGS (1u << SEG_HEAP5)

static u64 seg_size(int s)
{
    return ALIGN_UP((u64)segs[s].size, SZ_16K);
}

struct opts {
    bool not_running, power_off, disp_unlocked, stray_map;
};

static void world_j616s(const struct opts *o)
{
    chip_id = T6040;
    os_firmware = (struct fw_version_info){V26_6_2, "26.6.2", {26, 6, 2}, 3, "mBoot-18000.161.10"};
    ram_base = TB + 0x800000;
    ram_size = 0x200000000ULL - 0x800000;
    prop_u64("/chosen", "dram-base", TB);
    prop_u64("/chosen", "dram-size", 0x300000000ULL);
    mcc_carveout_count = 0;

    const char *nub = "/arm-io/dcp/iop-dcp-nub";
    prop_set(nub, "segment-ranges", segs, sizeof(segs));
    prop_str(nub, "segment-names", "__TEXT;__DATA;__OS_LOG;unknown;unknown;unknown;unknown");
    prop_str(nub, "uuid", DCP_UUID);
    prop_u32(nub, "pre-loaded", 1);
    prop_u32(nub, "running", 1);
    for (int i = 0; i < NSEGS; i++) {
        if (region_ids[i] < 0)
            continue;
        char p[32];
        snprintf(p, sizeof(p), "region-id-%d", region_ids[i]);
        prop_pair("/chosen/carveout-memory-map", p, segs[i].phys, seg_size(i));
    }
    reg_add("/vram", segs[SEG_VRAM].phys, segs[SEG_VRAM].size);
    fb_base = segs[SEG_VRAM].phys + 0x4000;

    reg_add("/arm-io/dcp", DCP_COPROC, 0x6c000);
    prop_str("/arm-io/dcp", "compatible", "iop,ascwrap-v6");
    prop_str("/arm-io/dart-dcp", "compatible", "dart,t8110");
    reg_add("/arm-io/dart-dcp", DART_DCP, 0x4000);
    prop_u64("/arm-io/dart-dcp", "vm-base", TB);
    prop_u64("/arm-io/dart-dcp", "vm-size", 0x1000000000ULL);
    prop_pair("/arm-io/dart-dcp", "pt-region-23", TB + 0x2f0000000ULL, TB + 0x2f0020000ULL);
    prop_str("/arm-io/dart-disp0", "compatible", "dart,t8110");
    reg_add("/arm-io/dart-disp0", DART_DISP0, 0x4000);
    prop_u64("/arm-io/dart-disp0", "vm-base", TB);
    prop_u64("/arm-io/dart-disp0", "vm-size", 0x1000000000ULL);
    prop_pair("/arm-io/dart-disp0", "pt-region-0", TB + 0x2f0100000ULL, TB + 0x2f0110000ULL);
    prop_pair("/arm-io/dart-disp0", "pt-region-16", TB + 0x2f0200000ULL, TB + 0x2f0210000ULL);
    prop_str("/arm-io/disp0", "compatible", "disp0,t6041");

    /* /arm-io/dcp's clock-gates name a virtual device whose parent is the display CPU */
    nfdevs = 0;
    fdev_add(0x10, "DISP_FE", false, PMGR_BASE + 0x208, 0);
    fdev_add(0x11, "DISP_CPU", false, PS_DISP, 0x10);
    fdev_add(0x12, "DCP_VIRT", true, 0, 0x11);
    u32 dcp_cg[1] = {0x12};
    prop_set("/arm-io/dcp", "clock-gates", dcp_cg, sizeof(dcp_cg));

    region_add(PMGR_BASE, 0x14000);
    poke32(PS_DISP, o->power_off ? 0x0 : 0xff);
    poke32(PMGR_BASE + 0x208, 0xff);
    region_add(DCP_COPROC, 0x4000);
    poke32(DCP_COPROC + 0x44, o->not_running ? 0 : 0x10);
    poke32(DCP_COPROC + 0x48, 0x3000ac);
    region_add(DART_DCP, 0x4000);
    region_add(DART_DISP0, 0x4000);
    poke32(DART_DCP + 0x200, 0x1e0); /* unlocked: the handoff locks it once every check passed */
    poke32(DART_DISP0 + 0x200, o->disp_unlocked ? 0x1e0 : 0x1e1);

    struct pt pd, p0, pp;
    pt_init(&pd, TB + 0x2f0000000ULL, 0x20000);
    pt_init(&p0, TB + 0x2f0100000ULL, 0x10000);
    pt_init(&pp, TB + 0x2f0200000ULL, 0x10000);
    for (int i = 0; i < NSEGS; i++) {
        if (i != SEG_OSLOG)
            pt_map(&pd, segs[i].remap - TB, segs[i].phys, seg_size(i));
        if (DISP_SEGS & (1u << i))
            pt_map(&p0, segs[i].remap - TB, segs[i].phys, seg_size(i));
        if (PIODMA_SEGS & (1u << i))
            pt_map(&pp, segs[i].remap - TB, segs[i].phys, seg_size(i));
    }
    if (o->stray_map) /* a scanout page outside every firmware segment */
        pt_map(&p0, 0x5000000, TB + 0x10000000, 0x4000);
    poke32(DART_DCP + 0x1000 + 4 * SID_DCP, 1);
    poke32(DART_DCP + 0x1400 + 4 * SID_DCP, ttbr_of(pd.lo));
    poke32(DART_DISP0 + 0x1000 + 4 * SID_DISP, 1);
    poke32(DART_DISP0 + 0x1400 + 4 * SID_DISP, ttbr_of(p0.lo));
    poke32(DART_DISP0 + 0x1000 + 4 * SID_PIODMA, 1);
    poke32(DART_DISP0 + 0x1400 + 4 * SID_PIODMA, ttbr_of(pp.lo));
}

/* ---- the J616s device tree, as the kernel's t6040-j616s.dts describes the display block ---- */

#define DT_SIZE (256 * 1024)

static int ok_(int r, const char *what)
{
    if (r < 0) {
        printf("FAIL building the device tree: %s: %s\n", what, fdt_strerror(r));
        exit(2);
    }
    return r;
}
#define OK(x) ok_((x), #x)

static void reg2(void *dt, int node, u64 base, u64 size)
{
    fdt64_t r[2] = {cpu_to_fdt64(base), cpu_to_fdt64(size)};
    OK(fdt_setprop(dt, node, "reg", r, sizeof(r)));
}

static void cells(void *dt, int node)
{
    OK(fdt_setprop_u32(dt, node, "#address-cells", 2));
    OK(fdt_setprop_u32(dt, node, "#size-cells", 2));
}

static void iommus(void *dt, int node, u32 dart, u32 sid)
{
    fdt32_t v[2] = {cpu_to_fdt32(dart), cpu_to_fdt32(sid)};
    OK(fdt_setprop(dt, node, "iommus", v, sizeof(v)));
}

static int dart(void *dt, int soc, const char *name, u64 base, u32 ph)
{
    int n = OK(fdt_add_subnode(dt, soc, name));
    OK(fdt_setprop_string(dt, n, "compatible", "apple,t8110-dart"));
    reg2(dt, n, base, 0x4000);
    OK(fdt_setprop_u32(dt, n, "#iommu-cells", 1));
    fdt32_t range[4] = {cpu_to_fdt32(TB >> 32), 0, cpu_to_fdt32(0x10), 0};
    OK(fdt_setprop(dt, n, "apple,dma-range", range, sizeof(range)));
    OK(fdt_setprop_string(dt, n, "status", "disabled"));
    OK(fdt_setprop_u32(dt, n, "phandle", ph));
    return n;
}

enum { PH_DART_DCP = 1, PH_DART_DISP, PH_MBOX };

static void build_dt(void *dt, const char *root_compat, size_t root_len)
{
    OK(fdt_create_empty_tree(dt, DT_SIZE));
    OK(fdt_setprop(dt, 0, "compatible", root_compat, root_len));
    cells(dt, 0);
    OK(fdt_add_subnode(dt, 0, "chosen"));
    int resv = OK(fdt_add_subnode(dt, 0, "reserved-memory"));
    cells(dt, resv);
    OK(fdt_setprop_empty(dt, resv, "ranges"));

    int soc = OK(fdt_add_subnode(dt, 0, "soc"));
    cells(dt, soc);
    OK(fdt_setprop_empty(dt, soc, "ranges"));
    dart(dt, soc, "iommu@402340000", DART_DCP, PH_DART_DCP);
    dart(dt, soc, "iommu@402300000", DART_DISP0, PH_DART_DISP);

    int mbox = OK(fdt_add_subnode(dt, soc, "mbox@402e08000"));
    OK(fdt_setprop_string(dt, mbox, "compatible", "apple,asc-mailbox-v4"));
    OK(fdt_setprop_string(dt, mbox, "status", "disabled"));
    OK(fdt_setprop_u32(dt, mbox, "phandle", PH_MBOX));

    int dcp = OK(fdt_add_subnode(dt, soc, "dcp@402e00000"));
    OK(fdt_setprop_string(dt, dcp, "compatible", "apple,t6041-dcp"));
    reg2(dt, dcp, DCP_COPROC, 0x6c000);
    cells(dt, dcp);
    iommus(dt, dcp, PH_DART_DCP, SID_DCP);
    OK(fdt_setprop_u32(dt, dcp, "mboxes", PH_MBOX));
    OK(fdt_setprop_string(dt, dcp, "status", "disabled"));
    int pio = OK(fdt_add_subnode(dt, dcp, "piodma"));
    OK(fdt_setprop_string(dt, pio, "compatible", "apple,dcp-piodma"));
    iommus(dt, pio, PH_DART_DISP, SID_PIODMA);

    int disp = OK(fdt_add_subnode(dt, 0, "display-subsystem"));
    OK(fdt_setprop_string(dt, disp, "compatible", "apple,display-subsystem"));
    iommus(dt, disp, PH_DART_DISP, SID_DISP);
    OK(fdt_setprop_string(dt, disp, "status", "disabled"));

    int aliases = OK(fdt_add_subnode(dt, 0, "aliases"));
    OK(fdt_setprop_string(dt, aliases, "dcp", "/soc/dcp@402e00000"));
    OK(fdt_setprop_string(dt, aliases, "disp0", "/display-subsystem"));
    OK(fdt_setprop_string(dt, aliases, "disp0-piodma", "/soc/dcp@402e00000/piodma"));
}

static void build_ext(void *dt, int which)
{
    const u64 base = which ? EXT1 : EXT0;
    const u64 dart_base = which ? EXT_DART1 : EXT_DART0;
    const u32 dart_ph = 100 + which;
    char name[64], path[96];
    snprintf(name, sizeof(name), "iommu@%llx", (unsigned long long)dart_base);
    dart(dt, 0, name, dart_base, dart_ph);
    snprintf(name, sizeof(name), "dcp@%llx", (unsigned long long)base);
    int dcp = OK(fdt_add_subnode(dt, 0, name));
    OK(fdt_setprop_string(dt, dcp, "compatible", "apple,t6041-dcpext"));
    reg2(dt, dcp, base, 0x4000);
    iommus(dt, dcp, dart_ph, 23);
    OK(fdt_setprop_string(dt, dcp, "status", "disabled"));
    fdt32_t mr[3];
    static const char *const parts[] = {"text", "data", "heap"};
    for (int i = 0; i < 3; i++) {
        snprintf(name, sizeof(name), "dcpext%d-%s", which, parts[i]);
        int region = OK(fdt_add_subnode(dt, fdt_path_offset(dt, "/reserved-memory"), name));
        OK(fdt_setprop_string(dt, region, "compatible", "apple,asc-mem"));
        reg2(dt, region, 0, 0x4000);
        OK(fdt_setprop_empty(dt, region, "no-map"));
        OK(fdt_setprop_string(dt, region, "status", "disabled"));
        OK(fdt_setprop_u32(dt, region, "phandle", 110 + which * 3 + i));
        mr[i] = cpu_to_fdt32(110 + which * 3 + i);
    }
    snprintf(path, sizeof(path), "/dcp@%llx", (unsigned long long)base);
    dcp = fdt_path_offset(dt, path);
    OK(fdt_setprop(dt, dcp, "memory-region", mr, sizeof(mr)));
    static const char names[] = "text\0data\0heap";
    OK(fdt_setprop(dt, dcp, "memory-region-names", names, sizeof(names)));
}

static void world_ext(int which)
{
    const u64 base = which ? EXT1 : EXT0;
    const u64 dart_base = which ? EXT_DART1 : EXT_DART0;
    struct seg ext[4] = {
        {segs[SEG_TEXT].phys, 0, segs[SEG_TEXT].remap, segs[SEG_TEXT].size, 1},
        {TB + 0x281200000ULL + which * 0x200000, 0, TB + 0x51000000 + which * 0x100000, 0x8000, 0},
        {TB + 0x28030000ULL + which * 0x40000, 0, TB + 0x28030000ULL + which * 0x40000, 0x4000,
         0xa},
        {TB + 0x282200000ULL + which * 0x200000, ~0ULL, TB + 0x52000000 + which * 0x100000, 0x10000,
         2},
    };
    char path[96];
    snprintf(path, sizeof(path), "/arm-io/dcpext%d", which);
    reg_add(path, base, 0x4000);
    snprintf(path, sizeof(path), "/arm-io/dart-dcpext%d", which);
    reg_add(path, dart_base, 0x4000);
    snprintf(path, sizeof(path), "/arm-io/dcpext%d/iop-dcpext%d-nub", which, which);
    prop_set(path, "segment-ranges", ext, sizeof(ext));
    prop_str(path, "segment-names", "__TEXT;__DATA;__OS_LOG;unknown");
    prop_u32(path, "pre-loaded", 1);
    prop_u32(path, "running", 1);
}

static const char J616S[] = "apple,j616s\0apple,t6040\0apple,arm-platform";
static const char OTHER[] = "apple,j999s\0apple,t6040\0apple,arm-platform";
static const char J614S[] = "apple,j614s\0apple,t6040\0apple,arm-platform";

/* ---- checks ---- */

static int failures;

static void check(bool ok, const char *fmt, ...)
{
    va_list ap;
    if (!ok)
        failures++;
    printf(ok ? "ok   " : "FAIL ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static int alias_node(const void *dt, const char *name)
{
    const char *p = fdt_get_alias(dt, name);
    return p ? fdt_path_offset(dt, p) : -1;
}

static bool status_is(const void *dt, int node, const char *want)
{
    const char *s = fdt_getprop(dt, node, "status", NULL);
    return s && !strcmp(s, want);
}

static bool marker_set(const void *dt, int node)
{
    int len;
    const fdt32_t *v = fdt_getprop(dt, node, MARKER, &len);
    return v && len == 4 && fdt32_ld(v) == 1;
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

static int region_count(const void *dt, int node)
{
    int len;
    const fdt32_t *v = fdt_getprop(dt, node, "memory-region", &len);
    return v && !(len % 4) ? len / 4 : 0;
}

static int region_at(const void *dt, int node, int i)
{
    int len;
    const fdt32_t *v = fdt_getprop(dt, node, "memory-region", &len);
    if (!v || len % 4 || i >= len / 4)
        return -1;
    int r = fdt_node_offset_by_phandle(dt, fdt32_ld(v + i));
    return r >= 0 && fdt_parent_offset(dt, r) == fdt_path_offset(dt, "/reserved-memory") ? r : -1;
}

/* The memory-region of @node whose reg is segment @s, or -1 */
static int segment_region(const void *dt, int node, int s)
{
    for (int i = 0; i < region_count(dt, node); i++) {
        u64 base, size;
        int r = region_at(dt, node, i);
        if (r >= 0 && reg_of(dt, r, &base, &size) && base == segs[s].phys && size == seg_size(s))
            return r;
    }
    return -1;
}

static u32 linked_segments(const void *dt, int node)
{
    u32 mask = 0;
    for (int s = 0; s < NSEGS; s++)
        if (segment_region(dt, node, s) >= 0)
            mask |= 1u << s;
    return mask;
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

static bool dvas_match(const void *dt, int dev, u32 mask)
{
    for (int s = 0; s < NSEGS; s++) {
        u64 size = 0;
        int r;
        if (!(mask & (1u << s)))
            continue;
        if ((r = segment_region(dt, dev, s)) < 0 || iommu_dva(dt, r, dev, &size) != segs[s].remap ||
            size != seg_size(s))
            return false;
    }
    return true;
}

static bool u32s_are(const void *dt, int node, const char *prop, const u32 *want, int n)
{
    int len;
    const fdt32_t *v = fdt_getprop(dt, node, prop, &len);
    if (!v || len != 4 * n)
        return false;
    for (int i = 0; i < n; i++)
        if (fdt32_ld(v + i) != want[i])
            return false;
    return true;
}

/* What aura publishes for the kernel's T6041 display gate and DCP driver */
static void contract_check(const void *dt)
{
    int dcp = alias_node(dt, "dcp"), disp = alias_node(dt, "disp0");
    int pio = alias_node(dt, "disp0-piodma");
    check(dcp >= 0 && disp >= 0 && pio >= 0, "published: dcp, disp0 and piodma still present");
    if (dcp < 0 || disp < 0 || pio < 0)
        return;
    check(marker_set(dt, dcp) && marker_set(dt, disp) && marker_set(dt, pio),
          "published: " MARKER " = <1> on dcp, display and piodma");
    check(status_is(dt, dcp, "disabled") && status_is(dt, disp, "disabled") &&
              !fdt_getprop(dt, pio, "status", NULL) &&
              status_is(dt, fdt_node_offset_by_phandle(dt, PH_DART_DCP), "disabled") &&
              status_is(dt, fdt_node_offset_by_phandle(dt, PH_DART_DISP), "disabled") &&
              status_is(dt, fdt_node_offset_by_phandle(dt, PH_MBOX), "disabled"),
          "published: dcp, display, both DARTs and the mailbox left disabled, piodma untouched");
    const char *uuid = fdt_getprop(dt, dcp, "apple,firmware-uuid", NULL);
    check(uuid && !strcmp(uuid, DCP_UUID), "published: dcp apple,firmware-uuid is the ADT's");
    const u32 v26[3] = {26, 6, 2};
    check(u32s_are(dt, dcp, "apple,firmware-version", v26, 3) &&
              u32s_are(dt, dcp, "apple,firmware-compat", v26, 3),
          "published: dcp apple,firmware-version and -compat are 26.6.2");

    check(linked_segments(dt, dcp) == (1u << NSEGS) - 1 && region_count(dt, dcp) == NSEGS,
          "published: every iop-dcp-nub segment is one dcp memory-region");
    int oslog = segment_region(dt, dcp, SEG_OSLOG);
    u64 size;
    check(oslog >= 0 && fdt_getprop(dt, oslog, "apple,dcp-os-log", NULL) &&
              iommu_dva(dt, oslog, dcp, &size) == ~0ULL,
          "published: __OS_LOG is a physical dcp memory-region marked apple,dcp-os-log");
    check(dvas_match(dt, dcp, ((1u << NSEGS) - 1) & ~(1u << SEG_OSLOG)),
          "published: every DART-mapped dcp segment has iommu-addresses at its SID %d DVA",
          SID_DCP);
    check(linked_segments(dt, disp) == DISP_SEGS && dvas_match(dt, disp, DISP_SEGS),
          "published: display memory-region is the SID %d segments, at their DVAs", SID_DISP);
    check(linked_segments(dt, pio) == PIODMA_SEGS && dvas_match(dt, pio, PIODMA_SEGS),
          "published: piodma memory-region is the SID %d segment, at its DVA", SID_PIODMA);
    int fb_idx = fdt_stringlist_search(dt, disp, "memory-region-names", "framebuffer");
    int fb = fb_idx >= 0 ? region_at(dt, disp, fb_idx) : -1;
    check(fb >= 0 && !fdt_node_check_compatible(dt, fb, "framebuffer") &&
              fb == segment_region(dt, disp, SEG_VRAM),
          "published: the display's \"framebuffer\" memory-region is /vram");
    check(fdt_stringlist_count(dt, dcp, "memory-region-names") == region_count(dt, dcp) &&
              fdt_stringlist_count(dt, disp, "memory-region-names") == region_count(dt, disp) &&
              fdt_stringlist_count(dt, pio, "memory-region-names") == region_count(dt, pio),
          "published: one memory-region-names entry per memory-region");
    check(peek32(DART_DCP + 0x200) & 1, "published: dart-dcp locked after every check passed");
}

/*
 * Every node and property of @out is in @in with the same value, and the other way round, except
 * for the nodes named in @extra (NULL-terminated), which @out may add.
 */
static int dt_diff(const void *in, const void *out, const char *const *extra)
{
    int diffs = 0;
    for (int pass = 0; pass < 2; pass++) {
        const void *a = pass ? in : out, *b = pass ? out : in;
        for (int node = 0; node >= 0; node = fdt_next_node(a, node, NULL)) {
            char path[256];
            if (fdt_get_path(a, node, path, sizeof(path)))
                return -1;
            int other = fdt_path_offset(b, path);
            if (other < 0) {
                bool allowed = false;
                for (const char *const *e = extra; !pass && *e; e++)
                    allowed |= !strncmp(path, *e, strlen(*e));
                if (!allowed) {
                    printf("     %s node %s\n", pass ? "removed" : "added", path);
                    diffs++;
                }
                continue;
            }
            int prop;
            fdt_for_each_property_offset(prop, a, node)
            {
                const char *name;
                int len, olen;
                const void *v = fdt_getprop_by_offset(a, prop, &name, &len);
                const void *ov = fdt_getprop(b, other, name, &olen);
                if (!ov || olen != len || memcmp(v, ov, len)) {
                    printf("     %s: %s %s\n", path, name, ov ? "changed" : pass ? "removed" : "added");
                    diffs++;
                }
            }
        }
    }
    return diffs;
}

static void oslog_name(char *buf, size_t len)
{
    snprintf(buf, len, "/reserved-memory/dcp-oslog@%llx", (unsigned long long)segs[SEG_OSLOG].phys);
}

/* The __OS_LOG reservation, its own transaction on every supported boot: present, once */
static void oslog_check(const void *dt, bool want)
{
    char path[64];
    oslog_name(path, sizeof(path));
    int node = fdt_path_offset(dt, path);
    u64 base = 0, size = 0;
    bool there = node >= 0 && reg_of(dt, node, &base, &size) && base == segs[SEG_OSLOG].phys &&
                 size == seg_size(SEG_OSLOG) && fdt_getprop(dt, node, "no-map", NULL) &&
                 fdt_getprop(dt, node, "apple,dcp-os-log", NULL);
    check(there == want, "the __OS_LOG buffer is %sreserved no-map", want ? "" : "not ");
}

/* A refusal after the gates: nothing published, nothing changed but the __OS_LOG reservation */
static void refusal_check(const void *in, const void *dt, bool oslog)
{
    char path[64];
    oslog_name(path, sizeof(path));
    const char *const extra[] = {path, NULL};
    int dcp = alias_node(dt, "dcp"), disp = alias_node(dt, "disp0");
    int pio = alias_node(dt, "disp0-piodma");
    check(dcp >= 0 && disp >= 0 && !marker_set(dt, dcp) && !marker_set(dt, disp) &&
              (pio < 0 || !marker_set(dt, pio)),
          "refused: no " MARKER " on dcp, display or piodma");
    check(dt_diff(in, dt, oslog ? extra : extra + 1) == 0,
          "refused: no memory-region, firmware property or reserved-memory node published%s",
          oslog ? " (only the __OS_LOG reservation added)" : " (device tree unchanged)");
    oslog_check(dt, oslog);
    check(mmio_writes == 0, "refused: no MMIO write (dart-dcp not locked)");
}

/* ---- scenarios ---- */

enum kind {
    PUBLISH,
    NOTHING, /* refused by the chip, board, firmware or DT gate: no access at all, the DT
                byte-identical */
    REFUSE,  /* refused after the gates, __OS_LOG reserved */
    REFUSE_NO_OSLOG,
};

struct scenario {
    const char *name;
    enum kind kind;
    const char *what;
};

static const struct scenario scenarios[] = {
    {"publish", PUBLISH, "a supported boot with no /chosen property publishes"},
    {"external", PUBLISH, "both external firmware memories come from the ADT"},
    {"external-refuse", PUBLISH, "one bad external ADT leaves its placeholders disabled"},
    {"stale-optin", PUBLISH, "a left-over chosen.asahi,t6041-dcp=1 changes nothing"},
    {"stale-optin-zero", PUBLISH, "a left-over chosen.asahi,t6041-dcp=0 does not disable it"},
    {"repeat", PUBLISH, "a second call adds nothing"},
    {"oslog-in-memreserve", PUBLISH, "an __OS_LOG wholly inside a /memreserve/ is still described"},
    /* chip, board and device tree */
    {"wrong-chip", NOTHING, "chip T6041 with a T6040 device tree"},
    {"wrong-board", NOTHING, "an unsupported T6040 board"},
    {"j614-no-optin", PUBLISH, "J614s publishes without an explicit opt-in"},
    {"j614-optin", PUBLISH, "J614s ignores a stale explicit opt-in"},
    {"no-display-dt", NOTHING, "a J616s device tree without the display block"},
    {"no-dcp-alias", NOTHING, "a J616s device tree without the dcp alias"},
    {"wrong-dcp-compat", NOTHING, "dcp is not apple,t6041-dcp"},
    {"wrong-disp-compat", NOTHING, "disp0 is not apple,display-subsystem"},
    {"no-disp-alias", NOTHING, "a J616s device tree without the disp0 alias"},
    {"stale-optin-no-dcp", NOTHING, "a stale chosen flag cannot supply the missing dcp alias"},
    {"stale-optin-wrong-disp", NOTHING, "a stale chosen flag cannot repair disp0 compatible"},
    {"wrong-os", NOTHING, "OS firmware is not 26.6.2"},
    {"unknown-os", NOTHING, "OS firmware is unknown"},
    {"stale-optin-wrong-os", NOTHING, "a stale chosen flag cannot qualify OS firmware"},
    /* missing ADT nodes */
    {"no-adt-nub", REFUSE_NO_OSLOG, "no ADT iop-dcp-nub"},
    {"no-adt-dart-dcp", REFUSE, "no ADT dart-dcp"},
    {"no-adt-dart-disp0", REFUSE, "no ADT dart-disp0"},
    {"no-adt-dcp-reg", REFUSE, "no ADT /arm-io/dcp register block"},
    {"no-adt-vram", REFUSE, "no ADT /vram"},
    {"no-adt-carveouts", REFUSE, "no ADT /chosen/carveout-memory-map"},
    {"no-adt-pt-region", REFUSE, "no ADT pt-region for dart-dcp SID 23"},
    {"no-adt-clock-gates", REFUSE, "no ADT clock-gates on /arm-io/dcp"},
    /* failed validation */
    {"bad-uuid", REFUSE, "the ADT dcp image uuid is malformed"},
    {"not-preloaded", REFUSE, "iBoot did not leave the dcp firmware running"},
    {"not-running", REFUSE, "the DCP CPU is not running"},
    {"power-off", REFUSE, "the display power domain is off"},
    {"disp-unlocked", REFUSE, "dart-disp0 is not locked"},
    {"outside-segment", REFUSE, "a display mapping outside the firmware segments"},
    {"wrong-dcp-reg", REFUSE, "the ADT dart-dcp is not at the T6041 address"},
    {"wrong-sid", REFUSE, "dcp iommus is not dart-dcp SID 23"},
    {"dcp-enabled", REFUSE, "the dcp node is not disabled"},
    {"dart-enabled", REFUSE, "the dcp DART is not disabled"},
    {"mbox-enabled", REFUSE, "the dcp mailbox is not disabled"},
    {"no-piodma", REFUSE, "the device tree has no piodma"},
    {"segment-reserved", REFUSE, "a dcp segment collides with a /reserved-memory node"},
    {"oslog-straddles", REFUSE_NO_OSLOG, "an __OS_LOG that straddles a /memreserve/"},
    {"dt-full", REFUSE, "the device tree has no room for the handoff"},
};

static bool is(const char *a, const char *b)
{
    return !strcmp(a, b);
}

int main(int argc, char **argv)
{
    if (argc == 2 && is(argv[1], "list")) {
        for (size_t i = 0; i < ARRAY_SIZE(scenarios); i++)
            printf("%s\n", scenarios[i].name);
        return 0;
    }
    if (argc != 2) {
        fprintf(stderr, "usage: %s list | <scenario>\n", argv[0]);
        return 2;
    }
    const struct scenario *sc = NULL;
    for (size_t i = 0; i < ARRAY_SIZE(scenarios); i++)
        if (is(argv[1], scenarios[i].name))
            sc = &scenarios[i];
    if (!sc) {
        fprintf(stderr, "unknown scenario %s\n", argv[1]);
        return 2;
    }
    const char *s = sc->name;

    struct opts o = {
        .not_running = is(s, "not-running"),
        .power_off = is(s, "power-off"),
        .disp_unlocked = is(s, "disp-unlocked"),
        .stray_map = is(s, "outside-segment"),
    };
    world_j616s(&o);

    static u8 dt[DT_SIZE], in[DT_SIZE];
    const char *board = is(s, "wrong-board") ? OTHER :
                        (is(s, "j614-no-optin") || is(s, "j614-optin")) ? J614S : J616S;
    build_dt(dt, board, sizeof(J616S));
    if (is(s, "external") || is(s, "external-refuse")) {
        for (int i = 0; i < 2; i++) {
            build_ext(dt, i);
            world_ext(i);
        }
        if (is(s, "external-refuse"))
            prop_u32("/arm-io/dcpext0/iop-dcpext0-nub", "running", 0);
    }

    /* ADT side */
    if (is(s, "wrong-chip"))
        chip_id = T6041;
    if (is(s, "no-adt-nub"))
        node_drop("/arm-io/dcp/iop-dcp-nub");
    if (is(s, "no-adt-dart-dcp"))
        node_drop("/arm-io/dart-dcp");
    if (is(s, "no-adt-dart-disp0"))
        node_drop("/arm-io/dart-disp0");
    if (is(s, "no-adt-dcp-reg"))
        node_get("/arm-io/dcp")->nreg = 0;
    if (is(s, "no-adt-vram"))
        node_drop("/vram");
    if (is(s, "no-adt-carveouts"))
        node_drop("/chosen/carveout-memory-map");
    if (is(s, "no-adt-pt-region"))
        prop_drop("/arm-io/dart-dcp", "pt-region-23");
    if (is(s, "no-adt-clock-gates"))
        prop_drop("/arm-io/dcp", "clock-gates");
    if (is(s, "wrong-os") || is(s, "stale-optin-wrong-os"))
        os_firmware = (struct fw_version_info){V14_7, "14.7", {14, 7, 0}, 3, "iBoot"};
    if (is(s, "unknown-os"))
        os_firmware = (struct fw_version_info){V_UNKNOWN, "unknown", {0}, 0, "unknown"};
    if (is(s, "bad-uuid"))
        prop_str("/arm-io/dcp/iop-dcp-nub", "uuid", "not-a-uuid");
    if (is(s, "not-preloaded"))
        prop_u32("/arm-io/dcp/iop-dcp-nub", "running", 0);
    if (is(s, "wrong-dcp-reg"))
        node_get("/arm-io/dart-dcp")->reg[0][0] = DART_DCP + 0x4000;

    /* device tree side */
    int dcp = alias_node(dt, "dcp"), aliases = fdt_path_offset(dt, "/aliases");
    if (is(s, "no-display-dt")) {
        OK(fdt_del_node(dt, alias_node(dt, "disp0")));
        OK(fdt_del_node(dt, alias_node(dt, "dcp")));
        OK(fdt_del_node(dt, aliases));
    }
    if (is(s, "no-dcp-alias") || is(s, "stale-optin-no-dcp"))
        OK(fdt_delprop(dt, aliases, "dcp"));
    if (is(s, "no-disp-alias"))
        OK(fdt_delprop(dt, aliases, "disp0"));
    if (is(s, "wrong-dcp-compat"))
        OK(fdt_setprop_string(dt, dcp, "compatible", "apple,t6031-dcp"));
    if (is(s, "wrong-disp-compat") || is(s, "stale-optin-wrong-disp"))
        OK(fdt_setprop_string(dt, alias_node(dt, "disp0"), "compatible",
                              "apple,t6031-display-subsystem"));
    if (is(s, "wrong-sid"))
        iommus(dt, dcp, PH_DART_DCP, 5);
    if (is(s, "dcp-enabled"))
        OK(fdt_setprop_string(dt, dcp, "status", "okay"));
    if (is(s, "dart-enabled"))
        OK(fdt_setprop_string(dt, fdt_node_offset_by_phandle(dt, PH_DART_DCP), "status", "okay"));
    if (is(s, "mbox-enabled"))
        OK(fdt_setprop_string(dt, fdt_node_offset_by_phandle(dt, PH_MBOX), "status", "okay"));
    if (is(s, "no-piodma")) {
        OK(fdt_del_node(dt, alias_node(dt, "disp0-piodma")));
        OK(fdt_delprop(dt, fdt_path_offset(dt, "/aliases"), "disp0-piodma"));
    }
    if (is(s, "segment-reserved")) {
        int n = OK(fdt_add_subnode(dt, fdt_path_offset(dt, "/reserved-memory"), "other@0"));
        reg2(dt, n, segs[SEG_HEAP3].phys + 0x4000, 0x4000);
    }
    if (is(s, "oslog-in-memreserve"))
        OK(fdt_add_mem_rsv(dt, segs[SEG_OSLOG].phys - 0x4000, seg_size(SEG_OSLOG) + 0x8000));
    if (is(s, "oslog-straddles"))
        OK(fdt_add_mem_rsv(dt, segs[SEG_OSLOG].phys + 0x4000, seg_size(SEG_OSLOG)));
    int chosen = fdt_path_offset(dt, "/chosen");
    if (is(s, "j614-optin") || is(s, "stale-optin") || is(s, "stale-optin-no-dcp") || is(s, "stale-optin-wrong-disp") ||
        is(s, "stale-optin-wrong-os"))
        OK(fdt_setprop_string(dt, chosen, OPTIN, "1"));
    if (is(s, "stale-optin-zero"))
        OK(fdt_setprop_string(dt, chosen, OPTIN, "0"));
    if (is(s, "dt-full")) {
        /* Room for the __OS_LOG node only: the handoff's scratch copy runs out of space */
        OK(fdt_pack(dt));
        OK(fdt_open_into(dt, dt, fdt_totalsize(dt) + 512));
    } else {
        OK(fdt_pack(dt));
        OK(fdt_open_into(dt, dt, DT_SIZE));
    }
    memcpy(in, dt, fdt_totalsize(dt));

    printf("== %s: %s\n", s, sc->what);
    if (!fdt_getprop(in, fdt_path_offset(in, "/chosen"), OPTIN, NULL))
        check(true, "no /chosen/" OPTIN " in the device tree handed to aura");
    fflush(stdout);
    int ret = dt_set_display_handoff(dt);
    fflush(stdout);
    check(ret == 0, "dt_set_display_handoff() returns 0");
    check(exc_count == 0, "no MMIO access faulted");

    if (is(s, "external") || is(s, "external-refuse")) {
        for (int i = 0; i < 2; i++) {
            char path[40];
            snprintf(path, sizeof(path), "/dcp@%llx", (unsigned long long)(i ? EXT1 : EXT0));
            int ext = fdt_path_offset(dt, path);
            const fdt32_t *mr = fdt_getprop(dt, ext, "memory-region", NULL);
            check(ext >= 0 && mr && !strcmp(fdt_getprop(dt, ext, "status", NULL), "disabled"),
                  "external processor stays disabled for the kernel gate");
            bool passed = i != 0 || !is(s, "external-refuse");
            for (int j = 0; j < 3; j++) {
                int region = fdt_node_offset_by_phandle(dt, fdt32_ld(&mr[j]));
                const char *status = fdt_getprop(dt, region, "status", NULL);
                int len;
                const fdt32_t *map = fdt_getprop(dt, region, "iommu-addresses", &len);
                if (j == 0) {
                    /* TEXT is the panel's region, shared: one tuple per processor. */
                    const char *name = fdt_get_name(dt, region, NULL);
                    bool shared = name && !strncmp(name, "dcp-text@", 9);
                    bool mapped = false;
                    for (int k = 0; shared && map && (k + 5) * 4 <= len; k += 5) {
                        fdt64_t dva;
                        memcpy(&dva, &map[k + 1], sizeof(dva));
                        if (fdt32_ld(&map[k]) == fdt_get_phandle(dt, ext) &&
                            fdt64_to_cpu(dva) == segs[SEG_TEXT].remap)
                            mapped = true;
                    }
                    check((shared && mapped) == passed,
                          "external text is the panel's text region, mapped for this processor");
                    char ph_name[24];
                    snprintf(ph_name, sizeof(ph_name), "dcpext%d-text", i);
                    check((fdt_subnode_offset(dt, fdt_path_offset(dt, "/reserved-memory"),
                                              ph_name) < 0) == passed,
                          "the text placeholder is gone exactly when the text is shared");
                    continue;
                }
                bool ready = status && !strcmp(status, "okay") && map && len == 5 * 4;
                check(ready == (i != 0 || !is(s, "external-refuse")),
                      "external memory is ready exactly when its ADT passed");
                if (ready) {
                    u64 want_phys = j == 0 ? segs[SEG_TEXT].phys :
                        j == 1 ? TB + 0x281200000ULL + i * 0x200000 :
                                 TB + 0x282200000ULL + i * 0x200000;
                    u64 want_dva = j == 0 ? segs[SEG_TEXT].remap :
                        j == 1 ? TB + 0x51000000 + i * 0x100000 :
                                 TB + 0x52000000 + i * 0x100000;
                    u64 want_size = j == 0 ? seg_size(SEG_TEXT) : j == 1 ? 0x8000 : 0x10000;
                    const fdt64_t *r = fdt_getprop(dt, region, "reg", &len);
                    fdt64_t map_dva, map_size;
                    memcpy(&map_dva, &map[1], sizeof(map_dva));
                    memcpy(&map_size, &map[3], sizeof(map_size));
                    check(r && len == 16 && fdt64_ld(r) == want_phys &&
                              fdt64_ld(&r[1]) == want_size &&
                              fdt32_ld(map) == fdt_get_phandle(dt, ext) &&
                              fdt64_to_cpu(map_dva) == want_dva &&
                              fdt64_to_cpu(map_size) == want_size,
                          "external region has the ADT physical span and DART alias");
                }
            }
            check(!!fdt_getprop(dt, ext, "apple,firmware-compat", NULL) ==
                      (i != 0 || !is(s, "external-refuse")),
                  "firmware compatibility follows the memory transaction");
        }
    }

    if (is(s, "repeat")) {
        static u8 first[DT_SIZE];
        memcpy(first, dt, fdt_totalsize(dt));
        int writes = mmio_writes;
        fflush(stdout);
        ret = dt_set_display_handoff(dt);
        fflush(stdout);
        check(ret == 0 && !memcmp(first, dt, fdt_totalsize(first)),
              "second call returns 0 and changes nothing in the device tree");
        check(mmio_writes == writes, "second call makes no MMIO write");
    }

    switch (sc->kind) {
        case PUBLISH:
            contract_check(dt);
            oslog_check(dt, true);
            check(mmio_writes == 1 && last_write == DART_DCP + 0x200,
                  "the only MMIO write is the dart-dcp lock");
            break;
        case NOTHING:
            check(fdt_totalsize(dt) == fdt_totalsize(in) && !memcmp(in, dt, fdt_totalsize(in)),
                  "nothing done: the device tree is byte for byte unchanged");
            check(mmio_reads == 0 && mmio_writes == 0, "nothing done: no MMIO access at all");
            break;
        case REFUSE:
        case REFUSE_NO_OSLOG:
            refusal_check(in, dt, sc->kind == REFUSE);
            break;
    }

    printf("%s: %s\n", s, failures ? "FAIL" : "pass");
    return failures ? 1 : 0;
}
