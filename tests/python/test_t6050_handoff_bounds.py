# SPDX-License-Identifier: MIT
"""The T6050 display handoff refuses an empty, overflowing or out-of-/vram boot framebuffer and a
zero, unaligned or out-of-range bandwidth scratch, and the T6041 headless handoff refuses an empty
or overflowing boot framebuffer.

src/kboot_display_handoff.c is compiled unchanged into a host program with the stand-in headers of
tests/t8122_display_host/ and aura's libfdt (ASan, UBSan). The ADT, the registers and the device
tree are synthetic. A panel-less board goes through dt_set_display_handoff(): the device tree
afterwards says whether the engine and the display subsystem were handed off. The panel path's ADT
checks are run by themselves (t6050_check_adt()), since no fixture models the rest of that path.
The explicit cases are the edges of each check; seeded cases compare with the rule written here
in whole numbers, which cannot wrap.

AURA_T6050_SOURCE names another kboot_display_handoff.c to build (the negative controls use it).
"""
import os
from pathlib import Path
import random
import shutil
import subprocess
import sys

import pytest

from host_compiler import host_cc

ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path(os.environ.get("AURA_T6050_SOURCE", ROOT / "src/kboot_display_handoff.c"))
LIBFDT = ["fdt.c", "fdt_ro.c", "fdt_rw.c", "fdt_wip.c", "fdt_sw.c", "fdt_empty_tree.c",
          "fdt_strerror.c", "fdt_addresses.c"]

U64 = 1 << 64
SZ_16K = 0x4000
RAM, RAM_SIZE = 0x10000000000, 0x400000000
# /vram above the OS's RAM, as iBoot places it
VRAM, VRAM_SIZE = RAM + RAM_SIZE + 0x4000000, 0x2000000
VRAM_END = VRAM + VRAM_SIZE
META_SIZE, BW = 0x40000, 0x20000

HOST = r'''
#include <stdlib.h>

static int fail_malloc;
static void *test_malloc(size_t size)
{
    return fail_malloc ? NULL : malloc(size);
}
#define malloc test_malloc
#include "kboot_display_handoff.c"
#undef malloc

void *adt = (void *)1;
u32 chip_id = T6050;
volatile enum exc_guard_t exc_guard;
volatile int exc_count;
struct fw_version_info os_firmware;
const struct fw_version_info fw_versions[1];
struct boot_args cur_boot_args;
size_t mcc_carveout_count;
struct mcc_carveout mcc_carveouts[1];

#define DIE(...)                                                                                   \
    do {                                                                                           \
        printf("host: " __VA_ARGS__);                                                              \
        exit(3);                                                                                   \
    } while (0)
#define OK(x)                                                                                      \
    do {                                                                                           \
        if ((x) < 0)                                                                               \
            DIE("%s failed\n", #x);                                                                \
    } while (0)

/* ---- the ADT: nodes by path, each with properties and reg entries ---- */

static struct node {
    char path[64];
    struct {
        char name[32];
        u8 data[96];
        u32 len;
    } props[10];
    int nprops, nreg;
    u64 reg[5][2];
} nodes[32];
static int nnodes;

static struct node *node(const char *path)
{
    for (int i = 0; i < nnodes; i++)
        if (!strcmp(nodes[i].path, path))
            return &nodes[i];
    if (nnodes == (int)ARRAY_SIZE(nodes))
        DIE("too many ADT nodes\n");
    snprintf(nodes[nnodes].path, sizeof(nodes[nnodes].path), "%s", path);
    return &nodes[nnodes++];
}

static void prop(const char *path, const char *name, const void *data, u32 len)
{
    struct node *n = node(path);
    if (n->nprops == (int)ARRAY_SIZE(n->props) || len > sizeof(n->props[0].data))
        DIE("no room for %s %s\n", path, name);
    snprintf(n->props[n->nprops].name, sizeof(n->props[0].name), "%s", name);
    memcpy(n->props[n->nprops].data, data, len);
    n->props[n->nprops++].len = len;
}

static void prop_u32(const char *path, const char *name, u32 val)
{
    prop(path, name, &val, sizeof(val));
}

static void prop_u64s(const char *path, const char *name, u64 a, u64 b, int count)
{
    u64 v[2] = {a, b};
    prop(path, name, v, count * sizeof(u64));
}

static void reg(const char *path, int idx, u64 addr, u64 size)
{
    struct node *n = node(path);
    n->reg[idx][0] = addr;
    n->reg[idx][1] = size;
    if (n->nreg <= idx)
        n->nreg = idx + 1;
}

int adt_path_offset(const void *a, const char *path)
{
    UNUSED(a);
    for (int i = 0; i < nnodes; i++)
        if (!strcmp(nodes[i].path, path))
            return i + 1;
    return -1;
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

const void *adt_getprop(const void *a, int n, const char *name, u32 *lenp)
{
    UNUSED(a);
    if (n < 1 || n > nnodes)
        return NULL;
    for (int i = 0; i < nodes[n - 1].nprops; i++)
        if (!strcmp(nodes[n - 1].props[i].name, name)) {
            if (lenp)
                *lenp = nodes[n - 1].props[i].len;
            return nodes[n - 1].props[i].data;
        }
    return NULL;
}

int adt_getprop_copy(const void *a, int n, const char *name, void *out, size_t len)
{
    u32 have = 0;
    const void *p = adt_getprop(a, n, name, &have);
    if (!p || have != len)
        return -1;
    memcpy(out, p, len);
    return len;
}

int adt_get_reg(const void *a, int *path, const char *name, int idx, u64 *addr, u64 *size)
{
    UNUSED(a);
    UNUSED(name);
    struct node *n = &nodes[path[0] - 1];
    if (path[0] < 1 || path[0] > nnodes || idx < 0 || idx >= n->nreg)
        return -1;
    if (addr)
        *addr = n->reg[idx][0];
    if (size)
        *size = n->reg[idx][1];
    return 0;
}

bool adt_is_compatible(const void *a, int n, const char *compat)
{
    DIE("adt_is_compatible(%d, %s)\n", n, compat);
}

int adt_get_property_count(const void *a, int n)
{
    UNUSED(a);
    return n < 1 || n > nnodes ? -1 : nodes[n - 1].nprops;
}

/* The tree walk: a node's children are the nodes whose path is its path plus one component. */
static bool child_of(int parent, int n)
{
    const char *p = parent ? nodes[parent - 1].path : "";
    size_t len = strlen(p);
    const char *c = nodes[n - 1].path;
    return !strncmp(c, p, len) && c[len] == '/' && c[len + 1] && !strchr(c + len + 1, '/');
}

static int parent_of(int n)
{
    const char *c = nodes[n - 1].path;
    const char *slash = strrchr(c, '/');
    for (int i = 1; i <= nnodes; i++)
        if (strlen(nodes[i - 1].path) == (size_t)(slash - c) &&
            !strncmp(nodes[i - 1].path, c, slash - c))
            return i;
    return 0;
}

static int child_after(int parent, int after)
{
    for (int i = after + 1; i <= nnodes; i++)
        if (child_of(parent, i))
            return i;
    return -1;
}

int adt_get_child_count(const void *a, int n)
{
    UNUSED(a);
    int count = 0;
    for (int i = 1; i <= nnodes; i++)
        count += child_of(n, i);
    return count;
}

int adt_first_child_offset(const void *a, int n)
{
    UNUSED(a);
    return child_after(n, 0);
}

int adt_next_sibling_offset(const void *a, int n)
{
    UNUSED(a);
    return n < 1 || n > nnodes ? -1 : child_after(parent_of(n), n);
}

const char *adt_get_name(const void *a, int n)
{
    UNUSED(a);
    return n < 1 || n > nnodes ? NULL : strrchr(nodes[n - 1].path, '/') + 1;
}

int host_adt_nprops(int n)
{
    return 0;
}

const struct adt_property *host_adt_prop(int n, int i)
{
    return NULL;
}

/* ---- the registers: only the words the handoff reads; it writes none ---- */

static struct {
    u64 addr;
    u32 val;
} words[32];
static int nwords;

static void word(u64 addr, u32 val)
{
    words[nwords].addr = addr;
    words[nwords++].val = val;
}

u32 read32(u64 addr)
{
    for (int i = 0; i < nwords; i++)
        if (words[i].addr == addr)
            return words[i].val;
    DIE("read32(0x%llx)\n", (unsigned long long)addr);
}

u64 read64(u64 addr)
{
    DIE("read64(0x%llx)\n", (unsigned long long)addr);
}

void write32(u64 addr, u32 val)
{
    DIE("write32(0x%llx, 0x%x)\n", (unsigned long long)addr, val);
}

/* ---- the rest of aura ---- */

enum fw_version firmware_os_components(void)
{
    return V_UNKNOWN;
}

int firmware_set_fdt(void *fdt, int n, const char *name, const struct fw_version_info *ver)
{
    fdt32_t data[5];
    for (size_t i = 0; i < ver->num_length; i++)
        data[i] = cpu_to_fdt32(ver->num[i]);
    return fdt_setprop(fdt, n, name, data, ver->num_length * 4) ? -1 : 0;
}

#define PS_WORD 0x290000100UL
int pmgr_adt_collect_psregs(const char *path, u64 *out, u8 *flags, int max)
{
    UNUSED(flags);
    if (strcmp(path, "/arm-io/dcpext0") || max < 1)
        return -1;
    out[0] = PS_WORD;
    return 1;
}

int pmgr_adt_gate(const char *path, const char *name, u32 index, struct pmgr_gate *gate)
{
    DIE("pmgr_adt_gate(%s)\n", path);
}

int pmgr_device_gate(u8 die, u16 id, struct pmgr_gate *gate)
{
    DIE("pmgr_device_gate(%u, %u)\n", die, id);
}

int pmgr_adt_is_powered(const char *path)
{
    if (!strcmp(path, "/arm-io/dcp") || !strcmp(path, "/arm-io/disp0"))
        return 1;
    DIE("pmgr_adt_is_powered(%s)\n", path);
}

u64 kboot_ram_base(void)
{
    return cur_boot_args.phys_base;
}

u64 kboot_ram_size(void)
{
    return cur_boot_args.mem_size;
}

u64 kboot_fb_base(void)
{
    return cur_boot_args.video.base;
}

void kboot_loaded_file_part(u64 *base, u64 *size)
{
    *base = cur_boot_args.phys_base;
    *size = 0x120000;
}

int kboot_fdt_path_exact(const void *fdt, const char *path)
{
    return fdt_path_offset(fdt, path);
}

/* ---- the cases ---- */

static u64 arg(int argc, char **argv, const char *name, u64 otherwise)
{
    size_t n = strlen(name);
    for (int i = 2; i < argc; i++)
        if (!strncmp(argv[i], name, n) && argv[i][n] == '=')
            return strtoull(argv[i] + n + 1, NULL, 16);
    return otherwise;
}

/* The first image row whose engines are handed off (or not): this boot's OS firmware and uuid */
static const char *image(bool ext)
{
    for (size_t i = 0; i < ARRAY_SIZE(t6050_images); i++)
        if (t6050_images[i].ext == ext) {
            os_firmware.version = t6050_images[i].os_fw;
            os_firmware.string = "the OS firmware";
            os_firmware.num[0] = 27;
            os_firmware.num_length = 3;
            return t6050_images[i].uuid;
        }
    DIE("no image row\n");
}

/*
 * A nub's segments: its text outside the OS's RAM, its __OS_LOG buffer in it (aura reserves that
 * one on every boot) and, if `vram_size`, /vram.
 */
#define OSLOG 0x10008000000UL
static void segments(const char *nub, u64 vram, u64 vram_size)
{
    static const char names[] = "__TEXT;__OS_LOG;__VRAM";
    struct adt_segment_ranges seg[3] = {
        {.phys = 0x20000000000, .iova = 0x10000000, .remap = 0x10000000, .size = 0x100000},
        {.phys = OSLOG, .iova = 0x18000000, .remap = OSLOG, .size = 0x4000, .unk = SEG_FLAGS_OSLOG},
        {.phys = vram, .iova = 0x20000000, .remap = 0x20000000, .size = vram_size},
    };
    prop(nub, "segment-ranges", seg, (vram_size ? 3 : 2) * sizeof(seg[0]));
    prop(nub, "segment-names", names, sizeof(names));
    prop_u64s("/chosen", "dram-base", 0x10000000000, 0, 1);
    prop_u64s("/chosen", "dram-size", 0x400000000, 0, 1);
}

static void fdt_reg(void *dt, int n, u64 addr, u64 size)
{
    fdt64_t v[2];
    fdt64_st(&v[0], addr);
    fdt64_st(&v[1], size);
    OK(fdt_setprop(dt, n, "reg", v, sizeof(v)));
}

static void fdt_u32s(void *dt, int n, const char *name, int count, u32 a, u32 b, u32 c, u32 d)
{
    fdt32_t v[4] = {cpu_to_fdt32(a), cpu_to_fdt32(b), cpu_to_fdt32(c), cpu_to_fdt32(d)};
    OK(fdt_setprop(dt, n, name, v, count * 4));
}

static int fdt_dev(void *dt, int parent, const char *name, u64 addr, u32 phandle)
{
    int n = fdt_add_subnode(dt, parent, name);
    OK(n);
    fdt_reg(dt, n, addr, 0x4000);
    OK(fdt_setprop_string(dt, n, "status", "disabled"));
    if (phandle)
        OK(fdt_setprop_u32(dt, n, "phandle", phandle));
    return n;
}

static const char *status(void *dt, const char *path)
{
    int n = fdt_path_offset(dt, path);
    const char *s = n < 0 ? NULL : fdt_getprop(dt, n, "status", NULL);
    return s ? s : "none";
}

#define COPROC   0x389c00000UL
#define DART_DCP 0x389304000UL
#define DART_DISP 0x389308000UL
#define VM_BASE  0x10000000000UL
#define VM_SIZE  0xf00000000UL
#define L1_DCP   0x20100004000UL
#define L1_DISP  0x20100104000UL
#define PMC      0x510f00000UL
enum { PH_DART_DCP = 1, PH_DART_DISP, PH_MBOX, PH_PMC };

/*
 * A board without a panel whose display subsystem scans out through dcpext0: every check of the
 * engine and the subsystem holds but the ones the arguments decide (vram= vsize= fb= stride=
 * height= for /vram and the boot framebuffer, meta= for the size of dispext0's PMC metadata in
 * the ADT, dtmeta= for the size the device tree's node gives, bw= for the scratch offset).
 */
static int headless(int argc, char **argv)
{
    static _Alignas(8) char dt[0x10000];
    u64 vram = arg(argc, argv, "vram", 0), vram_size = arg(argc, argv, "vsize", 0);
    u64 meta_size = arg(argc, argv, "meta", 0), bw = arg(argc, argv, "bw", 0);
    u64 no_pt_region = arg(argc, argv, "no_pt_region", 0);
    u64 dup_ext = arg(argc, argv, "dup_ext", 0);
    const char *nub = "/arm-io/dcpext0/iop-dcpext0-nub";

    prop(nub, "uuid", image(true), 37);
    prop_u32(nub, "running", 1);
    segments(nub, 0, 0);
    reg("/arm-io/dcpext0", 0, COPROC, 0x4000);
    for (int i = 0; i < 2; i++) {
        const char *d = i ? "/arm-io/dart-dispext0" : "/arm-io/dart-dcpext0";
        u64 base = i ? DART_DISP : DART_DCP, l1 = i ? L1_DISP : L1_DCP;
        u32 sid = i ? 16 : 23;
        reg(d, 0, base, 0x4000);
        prop_u64s(d, "vm-base", VM_BASE, 0, 1);
        prop_u64s(d, "vm-size", VM_SIZE, 0, 1);
        prop_u64s(d, i ? "l1-tt-16" : "l1-tt-23", l1, 1, 2);
        if (!no_pt_region)
            prop_u64s(d, i ? "pt-region-16" : "pt-region-23", l1 - 0x4000, l1 + 0x40000, 2);
        word(base + DART_T8110_PROTECT, DART_T8110_PROTECT_TTBR_TCR);
        word(base + DART_T8110_TCR(sid), T6050_TCR_WANT);
        word(base + DART_T8110_TTBR(sid),
             DART_T8110_TTBR_VALID |
                 FIELD_PREP(DART_T8110_TTBR_ADDR, l1 >> DART_T8110_TTBR_SHIFT));
    }
    word(PS_WORD, FIELD_PREP(PMGR_PS_ACTUAL, PMGR_PS_ACTIVE) |
                      FIELD_PREP(PMGR_PS_TARGET, PMGR_PS_ACTIVE));
    word(COPROC + ASC_CPU_CONTROL, ASC_CPU_CONTROL_RUN);
    for (int i = 0; i < 4; i++)
        reg("/arm-io/dispext0", i, 0x389200000UL + i * 0x10000UL, 0x4000);
    reg("/arm-io/dispext0", 4, PMC, meta_size);
    reg("/vram", 0, vram, vram_size);

    OK(fdt_create_empty_tree(dt, sizeof(dt)));
    OK(fdt_setprop_string(dt, 0, "compatible", "apple,t6050"));
    OK(fdt_setprop_u32(dt, 0, "#address-cells", 2));
    OK(fdt_setprop_u32(dt, 0, "#size-cells", 2));
    OK(fdt_add_subnode(dt, 0, "chosen"));
    OK(fdt_add_subnode(dt, 0, "reserved-memory"));
    int soc = fdt_add_subnode(dt, 0, "soc");
    OK(soc);
    OK(fdt_setprop_u32(dt, soc, "#address-cells", 2));
    OK(fdt_setprop_u32(dt, soc, "#size-cells", 2));
    OK(fdt_setprop_empty(dt, soc, "ranges"));
    for (int i = 0; i < 2; i++) {
        int d = fdt_dev(dt, soc, i ? "iommu@389308000" : "iommu@389304000",
                        i ? DART_DISP : DART_DCP, i ? PH_DART_DISP : PH_DART_DCP);
        OK(fdt_setprop_string(dt, d, "compatible", "apple,t8110-dart"));
        OK(fdt_setprop_u32(dt, d, "#iommu-cells", 1));
        fdt_u32s(dt, d, "apple,dma-range", 4, VM_BASE >> 32, (u32)VM_BASE, VM_SIZE >> 32,
                 (u32)VM_SIZE);
    }
    fdt_dev(dt, soc, "mbox@389c08000", COPROC + T6050_MBOX, PH_MBOX);
    int pmc = fdt_dev(dt, soc, "pmc-metadata@510f00000", PMC, PH_PMC);
    fdt_reg(dt, pmc, PMC, arg(argc, argv, "dtmeta", meta_size));
    int ext = fdt_dev(dt, soc, "dcpext@389c00000", COPROC, 0);
    OK(fdt_setprop_string(dt, ext, "compatible", "apple,t6050-dcpext"));
    fdt_u32s(dt, ext, "iommus", 2, PH_DART_DCP, 23, 0, 0);
    fdt_u32s(dt, ext, "mboxes", 1, PH_MBOX, 0, 0, 0);
    fdt_u32s(dt, ext, "apple,bw-scratch", 4, PH_PMC, 0, T6050_NDISP, bw);
    int piodma = fdt_add_subnode(dt, ext, "piodma");
    OK(piodma);
    fdt_u32s(dt, piodma, "iommus", 2, PH_DART_DISP, 16, 0, 0);
    if (dup_ext) {
        int dup = fdt_dev(dt, soc, "dcpext-dup@389c00000", COPROC, 0);
        OK(fdt_setprop_string(dt, dup, "compatible", "apple,t6050-dcpext"));
        OK(fdt_setprop_string(dt, dup, "status", "okay"));
        fdt_u32s(dt, dup, "mboxes", 1, PH_MBOX, 0, 0, 0);
        int dpio = fdt_add_subnode(dt, dup, "piodma");
        OK(dpio);
        fdt_u32s(dt, dpio, "iommus", 2, PH_DART_DISP, 16, 0, 0);
    }
    int disp = fdt_add_subnode(dt, soc, "display-subsystem");
    OK(disp);
    OK(fdt_setprop_string(dt, disp, "compatible", "apple,display-subsystem"));
    OK(fdt_setprop_string(dt, disp, "status", "disabled"));
    fdt_u32s(dt, disp, "iommus", 2, PH_DART_DISP, 0, 0, 0);

    if (dt_set_display_handoff(dt))
        DIE("dt_set_display_handoff() failed\n");
    if (fdt_check_header(dt))
        DIE("the device tree is not valid\n");

    int chosen = fdt_path_offset(dt, "/chosen");
    const char *said = fdt_getprop(dt, chosen, T6050_STATUS, NULL);
    printf("result chosen=%s engine=%s subsystem=%s dart-dcp=%s dart-disp=%s mbox=%s pmc=%s",
           said ? said : "none", status(dt, "/soc/dcpext@389c00000"),
           status(dt, "/soc/display-subsystem"), status(dt, "/soc/iommu@389304000"),
           status(dt, "/soc/iommu@389308000"), status(dt, "/soc/mbox@389c08000"),
           status(dt, "/soc/pmc-metadata@510f00000"));
    if (dup_ext)
        printf(" dup=%s", status(dt, "/soc/dcpext-dup@389c00000"));
    printf("\n");
    int len = 0, n, count = 0;
    const fdt32_t *s = fdt_getprop(dt, fdt_path_offset(dt, "/soc/dcpext@389c00000"),
                                   "apple,bw-scratch", &len);
    if (!s || len != 16)
        DIE("apple,bw-scratch is gone\n");
    printf("scratch 0x%x 0x%x 0x%x 0x%x\n", fdt32_ld(&s[0]), fdt32_ld(&s[1]), fdt32_ld(&s[2]),
           fdt32_ld(&s[3]));
    fdt_for_each_subnode(n, dt, fdt_path_offset(dt, "/reserved-memory"))
    {
        const fdt64_t *r = fdt_getprop(dt, n, "reg", &len);
        if (!r || len != 16)
            DIE("a reservation without reg\n");
        printf("reserved %s 0x%llx 0x%llx\n", fdt_get_name(dt, n, NULL),
               (unsigned long long)fdt64_ld(&r[0]), (unsigned long long)fdt64_ld(&r[1]));
        count++;
    }
    printf("reservations %d\n", count);
    return 0;
}

/*
 * A board with a panel, the ADT side alone: every check of t6050_check_adt() holds but the ones of
 * /vram and the boot framebuffer, which the arguments decide.
 */
static int panel(int argc, char **argv)
{
    u64 vram = arg(argc, argv, "vram", 0), vram_size = arg(argc, argv, "vsize", 0);
    u8 agent[24] = {1, 0, 3};
    u32 fn[3] = {0x77, 0, 0x2a5}, clock = 256 + 7, freq[8] = {0};

    prop(DCP6_NUB, "uuid", image(false), 37);
    prop_u32(DCP6_NUB, "running", 1);
    segments(DCP6_NUB, vram, (u32)vram_size == vram_size ? vram_size : 0);
    reg("/arm-io/dcp", 0, COPROC, 0x4000);
    for (int i = 0; i < 4; i++)
        reg("/arm-io/disp0", i, 0x389200000UL + i * 0x10000UL, 0x4000);
    reg("/arm-io/disp0", 4, PMC, 0x40000);
    prop("/arm-io/disp0", "function-bw_req_interrupt0", fn, sizeof(fn));
    prop_u32("/arm-io/disp0", "clock-ids", clock);
    memcpy(agent + 4, &fn[2], 4);
    prop("/arm-io/pmgr", "bwr-agent", agent, sizeof(agent));
    prop_u32("/arm-io/pmgr", "AAPL,phandle", fn[0]);
    freq[7] = 594000000;
    prop("/arm-io", "clock-frequencies", freq, sizeof(freq));
    prop_u32("/backlight", "user-accessible-max-nits", 600 << 16);
    reg("/vram", 0, vram, vram_size);

    dcp6.soc = &dcp_soc_t6050;
    memset(&t6050, 0, sizeof(t6050));
    int rc = t6050_check_adt();
    printf("result rc=%d vram=0x%llx size=0x%llx scratch=0x%x nits=%u\n", rc,
           (unsigned long long)t6050.vram, (unsigned long long)t6050.vram_size, t6050.bw_offset,
           t6050.nits);
    return 0;
}

/*
 * A prewired panel tree with two enabled external engines (dcpext0 healthy, dcpext1 optionally
 * failing running/uuid/segment/pt-region checks via fail=0..4). Runs the full
 * dt_set_display_handoff() and reports the published DT state.
 */
static int prewired(int argc, char **argv)
{
    static _Alignas(8) char dt[0x10000];
    u64 fail = arg(argc, argv, "fail", 0);
    u64 unknown = arg(argc, argv, "unknown", 0);
    u64 nocompat = arg(argc, argv, "nocompat", 0);
    u64 absent_status = arg(argc, argv, "absent_status", 0);
    u64 extra_status = arg(argc, argv, "extra_status", 0);
    u64 disp_enabled = arg(argc, argv, "disp_enabled", 0);
    u64 oversized_iommus = arg(argc, argv, "oversized_iommus", 0);
    u64 fail_scratch = arg(argc, argv, "fail_scratch", 0);
    u64 pack = arg(argc, argv, "pack", 0);
    u64 nested = arg(argc, argv, "nested", 0);
    u64 dup_ext = arg(argc, argv, "dup_ext", 0);
    u64 no_piodma = arg(argc, argv, "no_piodma", 0);
    u64 vram = arg(argc, argv, "vram", 0), vram_size = arg(argc, argv, "vsize", 0);
    u64 meta_size = arg(argc, argv, "meta", 0), ram = cur_boot_args.phys_base;
    const char *uuid = image(true);
    if (unknown) {
        os_firmware = (struct fw_version_info){V_UNKNOWN, "unknown", {0}, 0, NULL};
    } else {
        os_firmware.version = V27_2;
        os_firmware.string = "27.2";
        os_firmware.num[0] = 27;
        os_firmware.num[1] = 2;
        os_firmware.num[2] = 0;
        os_firmware.num_length = 3;
    }
    /* Use the 27.2 ext=true UUID row */
    uuid = NULL;
    for (size_t i = 0; i < ARRAY_SIZE(t6050_images); i++)
        if (t6050_images[i].os_fw == V27_2 && t6050_images[i].ext)
            uuid = t6050_images[i].uuid;
    if (!uuid)
        DIE("no 27.2 ext row\n");

    u64 dcp_coproc = 0x48ae00000UL, dart_dcp = 0x48a340000UL, dart_disp = 0x48a300000UL;
    u64 ext_coproc[2] = {0x2b2e00000UL, 0x2b3e00000UL};
    u64 ext_ddcp[2] = {0x2b2400000UL, 0x2b3400000UL};
    u64 ext_ddisp[2] = {0x2b2300000UL, 0x2b3300000UL};
    u64 l1_dcp = 0x20100004000UL, l1_disp = 0x20100104000UL;
    u8 agent[24] = {1, 0, 3};
    u32 fn[3] = {0x77, 0, 0x2a5}, clock_id = 256 + 7, freq[8] = {0};

    prop(DCP6_NUB, "uuid", uuid, 37);
    prop_u32(DCP6_NUB, "running", 1);
    segments(DCP6_NUB, vram, vram_size);
    reg("/arm-io/dcp", 0, dcp_coproc, 0x4000);
    reg("/arm-io/dcp", 1, 0x48a000000UL, 0x4000);
    for (int i = 0; i < 4; i++)
        reg("/arm-io/disp0", i, 0x489200000UL + i * 0x10000UL, 0x4000);
    reg("/arm-io/disp0", 4, PMC, meta_size);
    prop("/arm-io/disp0", "function-bw_req_interrupt0", fn, sizeof(fn));
    prop_u32("/arm-io/disp0", "clock-ids", clock_id);
    memcpy(agent + 4, &fn[2], 4);
    prop("/arm-io/pmgr", "bwr-agent", agent, sizeof(agent));
    prop_u32("/arm-io/pmgr", "AAPL,phandle", fn[0]);
    freq[7] = 594000000;
    prop("/arm-io", "clock-frequencies", freq, sizeof(freq));
    prop_u32("/backlight", "user-accessible-max-nits", 600 << 16);
    reg("/vram", 0, vram, vram_size);
    word(dcp_coproc + ASC_CPU_CONTROL, ASC_CPU_CONTROL_RUN);

    const char *idarts[2] = {"/arm-io/dart-dcp", "/arm-io/dart-disp0"};
    for (int i = 0; i < 2; i++) {
        u64 base = i ? dart_disp : dart_dcp, l1 = i ? l1_disp : l1_dcp;
        reg(idarts[i], 0, base, 0x4000);
        prop_u64s(idarts[i], "vm-base", VM_BASE, 0, 1);
        prop_u64s(idarts[i], "vm-size", VM_SIZE, 0, 1);
        u32 sid = i ? 0 : 23;
        prop_u64s(idarts[i], i ? "l1-tt-0" : "l1-tt-23", l1, 1, 2);
        prop_u64s(idarts[i], i ? "pt-region-0" : "pt-region-23", l1 - 0x4000, l1 + 0x40000, 2);
        word(base + DART_T8110_PROTECT, DART_T8110_PROTECT_TTBR_TCR);
        word(base + DART_T8110_TCR(sid), T6050_TCR_WANT);
        word(base + DART_T8110_TTBR(sid),
             DART_T8110_TTBR_VALID |
                 FIELD_PREP(DART_T8110_TTBR_ADDR, l1 >> DART_T8110_TTBR_SHIFT));
        if (i) {
            prop_u64s(idarts[i], "l1-tt-16", l1 + 0x4000, 1, 2);
            prop_u64s(idarts[i], "pt-region-16", l1 - 0x4000, l1 + 0x40000, 2);
            word(base + DART_T8110_TCR(16), T6050_TCR_WANT);
            word(base + DART_T8110_TTBR(16),
                 DART_T8110_TTBR_VALID |
                     FIELD_PREP(DART_T8110_TTBR_ADDR, (l1 + 0x4000) >> DART_T8110_TTBR_SHIFT));
        }
    }

    for (int e = 0; e < 2; e++) {
        char nubp[48], dcpp[32], ddcpp[32], ddispp[32];
        snprintf(nubp, sizeof(nubp), "/arm-io/dcpext%d/iop-dcpext%d-nub", e, e);
        snprintf(dcpp, sizeof(dcpp), "/arm-io/dcpext%d", e);
        snprintf(ddcpp, sizeof(ddcpp), "/arm-io/dart-dcpext%d", e);
        snprintf(ddispp, sizeof(ddispp), "/arm-io/dart-dispext%d", e);
        const char *euuid = (e == 1 && fail == 2) ? "64863924-B56B-3E32-87FE-038677F52709" : uuid;
        prop(nubp, "uuid", euuid, 37);
        prop_u32(nubp, "running", (e == 1 && fail == 1) ? 0 : 1);
        struct adt_segment_ranges eseg = {
            .phys = (e == 1 && fail == 3) ? ram + 0x100000 : 0x20000000000UL + (u64)e * 0x1000000UL,
            .iova = 0x10000000,
            .remap = 0x10000000,
            .size = 0x4000,
        };
        static const char enames[] = "__TEXT";
        prop(nubp, "segment-ranges", &eseg, sizeof(eseg));
        prop(nubp, "segment-names", enames, sizeof(enames));
        reg(dcpp, 0, ext_coproc[e], 0x4000);
        reg(ddcpp, 0, ext_ddcp[e], 0x4000);
        reg(ddispp, 0, ext_ddisp[e], 0x4000);
        u64 l1 = 0x20200000000UL + (u64)e * 0x1000000UL;
        prop_u64s(ddcpp, "l1-tt-23", l1, 1, 2);
        prop_u64s(ddcpp, "pt-region-23", l1 - 0x4000, l1 + 0x40000, 2);
        u64 pt16_lo = (e == 1 && fail == 4) ? ram + 0x200000 : l1 + 0x100000;
        prop_u64s(ddispp, "l1-tt-16", pt16_lo, 1, 2);
        prop_u64s(ddispp, "pt-region-16", pt16_lo, pt16_lo + 0x40000, 2);
    }

    enum { PH_CLK = 10, PH_EXT0_DISP, PH_EXT1_DISP };
    OK(fdt_create_empty_tree(dt, sizeof(dt)));
    OK(fdt_setprop_string(dt, 0, "compatible", "apple,t6050"));
    OK(fdt_setprop_u32(dt, 0, "#address-cells", 2));
    OK(fdt_setprop_u32(dt, 0, "#size-cells", 2));
    OK(fdt_add_subnode(dt, 0, "chosen"));
    OK(fdt_add_subnode(dt, 0, "reserved-memory"));
    int clk = fdt_add_subnode(dt, 0, "clock-dcp-fw");
    OK(clk);
    OK(fdt_setprop_string(dt, clk, "compatible", "fixed-clock"));
    OK(fdt_setprop_u32(dt, clk, "#clock-cells", 0));
    OK(fdt_setprop_u32(dt, clk, "phandle", PH_CLK));

    int soc = fdt_add_subnode(dt, 0, "soc");
    OK(soc);
    OK(fdt_setprop_u32(dt, soc, "#address-cells", 2));
    OK(fdt_setprop_u32(dt, soc, "#size-cells", 2));
    OK(fdt_setprop_empty(dt, soc, "ranges"));

    int ddcp = fdt_dev(dt, soc, "iommu@48a340000", dart_dcp, PH_DART_DCP);
    OK(fdt_setprop_string(dt, ddcp, "compatible", "apple,t8110-dart"));
    OK(fdt_setprop_u32(dt, ddcp, "#iommu-cells", 1));
    fdt_u32s(dt, ddcp, "apple,dma-range", 4, VM_BASE >> 32, (u32)VM_BASE, VM_SIZE >> 32,
             (u32)VM_SIZE);
    int ddisp = fdt_dev(dt, soc, "iommu@48a300000", dart_disp, PH_DART_DISP);
    OK(fdt_setprop_string(dt, ddisp, "compatible", "apple,t8110-dart"));
    OK(fdt_setprop_u32(dt, ddisp, "#iommu-cells", 1));
    fdt_u32s(dt, ddisp, "apple,dma-range", 4, VM_BASE >> 32, (u32)VM_BASE, VM_SIZE >> 32,
             (u32)VM_SIZE);

    int mbox = fdt_dev(dt, soc, "mbox@48ae08000", dcp_coproc + T6050_MBOX, PH_MBOX);
    OK(fdt_setprop_string(dt, mbox, "compatible", "apple,asc-mailbox-v4"));
    int pmc = fdt_dev(dt, soc, "pmc-metadata@510f00000", PMC, PH_PMC);
    fdt_reg(dt, pmc, PMC, meta_size);
    OK(fdt_setprop_u32(dt, pmc, "#apple,bw-scratch-cells", 3));

    int dcp = fdt_dev(dt, soc, "dcp@48ae00000", dcp_coproc, 0);
    OK(fdt_setprop_string(dt, dcp, "compatible", "apple,t6050-dcp"));
    {
        fdt64_t r[14] = {
            cpu_to_fdt64(dcp_coproc),     cpu_to_fdt64(0x4000),
            cpu_to_fdt64(0x48a000000UL),  cpu_to_fdt64(0x4000),
            cpu_to_fdt64(0x489200000UL),  cpu_to_fdt64(0x4000),
            cpu_to_fdt64(0x489210000UL),  cpu_to_fdt64(0x4000),
            cpu_to_fdt64(0x489220000UL),  cpu_to_fdt64(0x4000),
            cpu_to_fdt64(0x489230000UL),  cpu_to_fdt64(0x4000),
            cpu_to_fdt64(PMC),            cpu_to_fdt64(meta_size),
        };
        OK(fdt_setprop(dt, dcp, "reg", r, sizeof(r)));
        static const char rnames[] = "coproc\0iop-vbar\0disp-0\0disp-1\0disp-2\0disp-3\0disp-4";
        OK(fdt_setprop(dt, dcp, "reg-names", rnames, sizeof(rnames)));
    }
    fdt_u32s(dt, dcp, "clocks", 1, PH_CLK, 0, 0, 0);
    fdt_u32s(dt, dcp, "apple,clock-ids", 1, clock_id, 0, 0, 0);
    fdt_u32s(dt, dcp, "apple,bw-scratch", 4, PH_PMC, 0, T6050_NDISP, 0x20000 + 3 * 0x4000);
    fdt_u32s(dt, dcp, "mboxes", 1, PH_MBOX, 0, 0, 0);
    fdt_u32s(dt, dcp, "iommus", 2, PH_DART_DCP, 23, 0, 0);
    OK(fdt_add_subnode(dt, dcp, "panel"));
    int piodma = fdt_add_subnode(dt, dcp, "piodma");
    OK(piodma);
    fdt_u32s(dt, piodma, "iommus", 2, PH_DART_DISP, 16, 0, 0);

    const char *ext1_path = "/soc/dcpext1";
    for (int e = 0; e < 2; e++) {
        char dn[32], en[32];
        u32 dph = e ? PH_EXT1_DISP : PH_EXT0_DISP;
        snprintf(dn, sizeof(dn), "iommu@%lx", (unsigned long)ext_ddisp[e]);
        int ed = fdt_add_subnode(dt, soc, dn);
        OK(ed);
        fdt_reg(dt, ed, ext_ddisp[e], 0x4000);
        OK(fdt_setprop_string(dt, ed, "compatible", "apple,t8110-dart"));
        OK(fdt_setprop_u32(dt, ed, "#iommu-cells", 1));
        OK(fdt_setprop_u32(dt, ed, "phandle", dph));

        int parent = soc;
        if (e == 1 && nested) {
            int b1 = fdt_add_subnode(dt, soc, "bus-level-one-display-fabric");
            OK(b1);
            OK(fdt_setprop_u32(dt, b1, "#address-cells", 2));
            OK(fdt_setprop_u32(dt, b1, "#size-cells", 2));
            fdt32_t idr[6] = {cpu_to_fdt32(2), 0, cpu_to_fdt32(2), 0, cpu_to_fdt32(4), 0};
            OK(fdt_setprop(dt, b1, "ranges", idr, sizeof(idr)));
            int b2 = fdt_add_subnode(dt, b1, "bus-level-two-external-engines");
            OK(b2);
            OK(fdt_setprop_u32(dt, b2, "#address-cells", 2));
            OK(fdt_setprop_u32(dt, b2, "#size-cells", 2));
            OK(fdt_setprop_empty(dt, b2, "ranges"));
            parent = b2;
            snprintf(en, sizeof(en), "dcpext@%lx", (unsigned long)ext_coproc[e]);
            ext1_path = "/soc/bus-level-one-display-fabric/bus-level-two-external-engines/"
                        "dcpext@2b3e00000";
        } else {
            snprintf(en, sizeof(en), "dcpext%d", e);
        }
        int ext = fdt_add_subnode(dt, parent, en);
        OK(ext);
        fdt_reg(dt, ext, ext_coproc[e], 0x4000);
        OK(fdt_setprop_string(dt, ext, "compatible", "apple,t6050-dcpext"));
        if (e == 1 && extra_status) {
            static const char extra[] = "okay\0disabled";
            OK(fdt_setprop(dt, ext, "status", extra, sizeof(extra)));
        } else if (!(e == 1 && absent_status)) {
            OK(fdt_setprop_string(dt, ext, "status", "okay"));
        }
        if (!(e == 0 && nocompat)) {
            fdt_u32s(dt, ext, "apple,firmware-compat", 3, 27, 0, 0, 0);
            fdt_u32s(dt, ext, "apple,firmware-version", 3, 27, 0, 0, 0);
        }
        if (!(e == 1 && no_piodma)) {
            int epio = fdt_add_subnode(dt, ext, "piodma");
            OK(epio);
            fdt_u32s(dt, epio, "iommus", 2, dph, 16, 0, 0);
        }
        if (e == 1 && dup_ext) {
            int dup = fdt_dev(dt, soc, "dcpext1-dup", ext_coproc[e], 0);
            OK(fdt_setprop_string(dt, dup, "compatible", "apple,t6050-dcpext"));
            OK(fdt_setprop_string(dt, dup, "status", "okay"));
            fdt_u32s(dt, dup, "apple,firmware-compat", 3, 27, 0, 0, 0);
            fdt_u32s(dt, dup, "apple,firmware-version", 3, 27, 0, 0, 0);
            if (!no_piodma) {
                int dpio = fdt_add_subnode(dt, dup, "piodma");
                OK(dpio);
                fdt_u32s(dt, dpio, "iommus", 2, dph, 16, 0, 0);
            }
        }
    }

    int disp = fdt_add_subnode(dt, soc, "display-subsystem");
    OK(disp);
    OK(fdt_setprop_string(dt, disp, "compatible", "apple,display-subsystem"));
    OK(fdt_setprop_string(dt, disp, "status", disp_enabled ? "okay" : "disabled"));
    if (oversized_iommus) {
        fdt32_t io[36];
        io[0] = cpu_to_fdt32(PH_DART_DISP);
        io[1] = 0;
        io[2] = cpu_to_fdt32(PH_EXT0_DISP);
        io[3] = 0;
        io[4] = cpu_to_fdt32(PH_EXT1_DISP);
        io[5] = 0;
        for (int i = 3; i < 18; i++) {
            io[2 * i] = cpu_to_fdt32(100 + i);
            io[2 * i + 1] = 0;
        }
        OK(fdt_setprop(dt, disp, "iommus", io, sizeof(io)));
    } else {
        fdt32_t io[6] = {
            cpu_to_fdt32(PH_DART_DISP), 0,
            cpu_to_fdt32(PH_EXT0_DISP), 0,
            cpu_to_fdt32(PH_EXT1_DISP), 0,
        };
        OK(fdt_setprop(dt, disp, "iommus", io, sizeof(io)));
    }

    if (pack)
        OK(fdt_pack(dt));

    fail_malloc = fail_scratch ? 1 : 0;
    int rc = dt_set_display_handoff(dt);
    fail_malloc = 0;
    if (rc)
        DIE("dt_set_display_handoff() failed\n");
    if (fdt_check_header(dt))
        DIE("the device tree is not valid\n");

    int chosen = fdt_path_offset(dt, "/chosen");
    const char *said = fdt_getprop(dt, chosen, T6050_STATUS, NULL);
    int e0 = fdt_path_offset(dt, "/soc/dcpext0"), e1 = fdt_path_offset(dt, ext1_path);
    int len = 0;
    const fdt32_t *c0 = e0 < 0 ? NULL : fdt_getprop(dt, e0, "apple,firmware-compat", &len);
    const fdt32_t *c1 = e1 < 0 ? NULL : fdt_getprop(dt, e1, "apple,firmware-compat", &len);
    const fdt32_t *v1 = e1 < 0 ? NULL : fdt_getprop(dt, e1, "apple,firmware-version", &len);
    char c0s[32] = "none", c1s[32] = "none", v1s[32] = "none";
    if (c0)
        snprintf(c0s, sizeof(c0s), "%u.%u.%u", fdt32_ld(&c0[0]), fdt32_ld(&c0[1]), fdt32_ld(&c0[2]));
    if (c1)
        snprintf(c1s, sizeof(c1s), "%u.%u.%u", fdt32_ld(&c1[0]), fdt32_ld(&c1[1]), fdt32_ld(&c1[2]));
    if (v1)
        snprintf(v1s, sizeof(v1s), "%u.%u.%u", fdt32_ld(&v1[0]), fdt32_ld(&v1[1]), fdt32_ld(&v1[2]));
    printf("result chosen=%s dcp=%s subsystem=%s ext0=%s ext1=%s ext0_fw=%s ext1_fw=%s "
           "ext1_ver=%s",
           said ? said : "none", status(dt, "/soc/dcp@48ae00000"),
           status(dt, "/soc/display-subsystem"), status(dt, "/soc/dcpext0"),
           e1 < 0 ? "deleted" : status(dt, ext1_path), c0s, c1s, v1s);
    if (dup_ext) {
        int dup = fdt_path_offset(dt, "/soc/dcpext1-dup");
        const fdt32_t *dc = dup < 0 ? NULL : fdt_getprop(dt, dup, "apple,firmware-compat", &len);
        char dcs[32] = "none";
        if (dc)
            snprintf(dcs, sizeof(dcs), "%u.%u.%u", fdt32_ld(&dc[0]), fdt32_ld(&dc[1]),
                     fdt32_ld(&dc[2]));
        printf(" dup=%s dup_fw=%s", dup < 0 ? "deleted" : status(dt, "/soc/dcpext1-dup"), dcs);
    }
    printf("\n");
    disp = fdt_path_offset(dt, "/soc/display-subsystem");
    const fdt32_t *io = fdt_getprop(dt, disp, "iommus", &len);
    printf("iommus");
    for (int i = 0; io && i < len / 4; i++)
        printf(" %u", fdt32_ld(&io[i]));
    printf("\n");
    return 0;
}

static int t6041_headless_fb(void)
{
    static char dt[0x10000];
    OK(fdt_create_empty_tree(dt, sizeof(dt)));
    OK(fdt_setprop_u32(dt, 0, "#address-cells", 2));
    OK(fdt_setprop_u32(dt, 0, "#size-cells", 2));
    int aliases = fdt_add_subnode(dt, 0, "aliases");
    OK(aliases);
    OK(fdt_setprop_string(dt, aliases, "disp0", "/soc/display-subsystem"));
    OK(fdt_add_subnode(dt, 0, "reserved-memory"));
    int soc = fdt_add_subnode(dt, 0, "soc");
    OK(soc);
    OK(fdt_setprop_u32(dt, soc, "#address-cells", 2));
    OK(fdt_setprop_u32(dt, soc, "#size-cells", 2));
    int dart = fdt_dev(dt, soc, "iommu@389308000", DART_DISP, PH_DART_DISP);
    OK(fdt_setprop_string(dt, dart, "compatible", "apple,t8110-dart"));
    int disp = fdt_add_subnode(dt, soc, "display-subsystem");
    OK(disp);
    OK(fdt_setprop_string(dt, disp, "compatible", dcp_soc_t6041_max.disp_compat));
    OK(fdt_setprop_string(dt, disp, "status", "disabled"));
    fdt_u32s(dt, disp, "iommus", 2, PH_DART_DISP, 0, 0, 0);

    memset(&dcp6, 0, sizeof(dcp6));
    dcp6.soc = &dcp_soc_t6041_max;
    int rc = dcp6_headless_display(dt, dcp6.soc->disp_compat);
    printf("result rc=%d\n", rc);
    int len = 0, n, count = 0;
    fdt_for_each_subnode(n, dt, fdt_path_offset(dt, "/reserved-memory"))
    {
        const fdt64_t *r = fdt_getprop(dt, n, "reg", &len);
        if (!r || len != 16)
            DIE("a reservation without reg\n");
        printf("reserved %s 0x%llx 0x%llx\n", fdt_get_name(dt, n, NULL),
               (unsigned long long)fdt64_ld(&r[0]), (unsigned long long)fdt64_ld(&r[1]));
        count++;
    }
    printf("reservations %d\n", count);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    cur_boot_args.phys_base = arg(argc, argv, "ram", 0);
    cur_boot_args.mem_size = arg(argc, argv, "ramsize", 0);
    cur_boot_args.video.base = arg(argc, argv, "fb", 0);
    cur_boot_args.video.stride = arg(argc, argv, "stride", 0);
    cur_boot_args.video.height = arg(argc, argv, "height", 0);
    if (!strcmp(argv[1], "headless"))
        return headless(argc, argv);
    if (!strcmp(argv[1], "panel"))
        return panel(argc, argv);
    if (!strcmp(argv[1], "prewired"))
        return prewired(argc, argv);
    if (!strcmp(argv[1], "t6041-headless"))
        return t6041_headless_fb();
    return 2;
}
'''


@pytest.fixture(scope="module")
def host(tmp_path_factory):
    cc = host_cc()
    if not shutil.which(cc[0]):
        pytest.skip("no host C compiler")
    tmp = tmp_path_factory.mktemp("t6050-bounds")
    src = tmp / "src"
    src.mkdir()
    # The code under test next to the stand-ins, so that its #include "utils.h" finds them
    code = SOURCE.read_text()
    if sys.platform == "darwin":
        # The host fixture has no aura ELF linker script; retain a dedicated Mach-O section.
        code = code.replace('section(".init.t6050")', 'section("__TEXT,__t6050_init")')
    (src / "kboot_display_handoff.c").write_text(code)
    (src / "t6050_bounds_host.c").write_text(HOST)
    for header in (ROOT / "tests/t8122_display_host").glob("*.h"):
        shutil.copy(header, src)
    binary = tmp / "t6050-bounds-host"
    subprocess.run(
        [*cc, "-std=gnu11", "-D_GNU_SOURCE", "-O1", "-g", "-Wall", "-Wno-unused-function",
         "-Wno-unused-parameter", "-Wno-format", "-Wno-format-truncation",
         "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
         "-fno-sanitize-recover=undefined", "-I", str(src), "-I", str(ROOT / "src"),
         str(src / "t6050_bounds_host.c"),
         *(str(ROOT / "src/libfdt" / f) for f in LIBFDT), "-o", str(binary)],
        check=True,
    )
    return binary


class Result:
    """What the host program printed: aura's log lines and the state afterwards."""

    def __init__(self, text):
        self.log, self.reserved, self.scratch = [], [], None
        for line in text.splitlines():
            kind, _, rest = line.partition(" ")
            if kind == "DCP:":
                self.log.append(rest.partition(": ")[2])
            elif kind == "result":
                self.state = dict(item.split("=") for item in rest.split())
            elif kind == "scratch":
                self.scratch = [int(x, 16) for x in rest.split()]
            elif kind == "reserved":
                name, base, size = rest.split()
                self.reserved.append((name, int(base, 16), int(size, 16)))
            elif kind == "reservations":
                assert int(rest) == len(self.reserved)
            elif kind == "iommus":
                self.iommus = [int(x) for x in rest.split()]
            else:
                raise AssertionError(line)

    def said(self, text):
        return any(text in line for line in self.log)


def run(host, mode, **values):
    values = {"ram": RAM, "ramsize": RAM_SIZE, **values}
    env = {"ASAN_OPTIONS": "detect_leaks=0", "PATH": "/usr/bin:/bin"}
    out = subprocess.run([str(host), mode, *(f"{k}={v:x}" for k, v in values.items())],
                         capture_output=True, text=True, env=env)
    assert out.returncode == 0, out.stdout + out.stderr
    return Result(out.stdout)


def framebuffer(fb, size):
    """A boot framebuffer of `size` bytes at `fb`, as stride and height."""
    for height in (1080, 16, 3, 1):
        if size % height == 0:
            return {"fb": fb, "stride": size // height, "height": height}
    raise AssertionError(size)


def headless(host, vram=VRAM, vsize=VRAM_SIZE, fb=VRAM, fb_size=VRAM_SIZE, meta=META_SIZE, bw=BW,
             **more):
    return run(host, "headless", vram=vram, vsize=vsize, meta=meta, bw=bw,
               **framebuffer(fb, fb_size), **more)


DISABLED = {"engine": "disabled", "subsystem": "disabled", "dart-dcp": "disabled",
            "dart-disp": "disabled", "mbox": "disabled", "pmc": "disabled"}
ENABLED = dict.fromkeys(DISABLED, "okay")
NO_VRAM = "no /vram holding the boot framebuffer"
NO_SCRATCH = "dcpext0: apple,bw-scratch is not in /arm-io/dispext0's PMC metadata"
REFUSED = "display handoff refused; the boot framebuffer keeps the display"
# the engine's __OS_LOG buffer is reserved whether or not it is handed off
OSLOG = ("dcp-oslog@10008000000", 0x10008000000, 0x4000)


def handed_off(r, vram=VRAM, vsize=VRAM_SIZE, bw=BW):
    """The engine and the subsystem were handed off, with /vram as their framebuffer."""
    assert r.state == {"chosen": "published", **ENABLED}, r.log
    assert sorted(r.reserved) == sorted([OSLOG, (f"framebuffer@{vram:x}", vram, vsize)])
    assert r.said(f"dcpext0 handed off: bw-scratch 0x{bw:x}")
    assert r.said("dcpext0 handed off with the display subsystem")
    assert not r.said(REFUSED) and not r.said(NO_VRAM) and not r.said(NO_SCRATCH)
    assert r.scratch == [4, 0, 5, bw]
    return True


def refused(r, why, bw=BW):
    """Nothing was handed off, for the reason `why`; the device tree is as it was."""
    assert r.state == {"chosen": "refused", **DISABLED}, r.log
    assert r.reserved == [OSLOG]
    assert r.said(why) and r.said(REFUSED)
    assert not r.said("handed off")
    assert r.scratch == [4, 0, 5, bw]
    return True


# ---- the boot framebuffer and /vram ------------------------------------------------------------

# (framebuffer start, its size, taken): /vram is [VRAM, VRAM_END)
FRAMEBUFFERS = [
    # all of /vram, its first page, its last page, its last byte, somewhere inside
    (VRAM, VRAM_SIZE, True),
    (VRAM, SZ_16K, True),
    (VRAM_END - SZ_16K, SZ_16K, True),
    (VRAM_END - 1, 1, True),
    (VRAM + 0x123000, 0x7e9000, True),
    # an empty framebuffer is refused even when its base lies in /vram
    (VRAM, 0, False),
    (VRAM_END - 1, 0, False),
    # starting where /vram ends: nothing of it is in /vram, whatever its size
    (VRAM_END, 0, False),
    (VRAM_END, 1, False),
    (VRAM_END, SZ_16K, False),
    # starting past the end of /vram: `vram + vram_size - fb` wrapped here
    (VRAM_END + 1, 0, False),
    (VRAM_END + SZ_16K, SZ_16K, False),
    (VRAM_END + SZ_16K, VRAM_SIZE, False),
    (VRAM_END + 0x40000000, 0x7e9000, False),
    (U64 - SZ_16K, SZ_16K, False),
    (U64 - 1, 1, False),
    (U64 - 1, 0, False),
    # starting below /vram, or at 0
    (VRAM - SZ_16K, SZ_16K, False),
    (VRAM - SZ_16K, 2 * SZ_16K, False),
    (VRAM - 1, VRAM_SIZE, False),
    (0, SZ_16K, False),
    # starting inside and ending past the end, by a byte and by far
    (VRAM, VRAM_SIZE + 1, False),
    (VRAM_END - SZ_16K, SZ_16K + 1, False),
    (VRAM_END - 1, 2, False),
    (VRAM + SZ_16K, VRAM_SIZE, False),
    (VRAM, 0x100000000, False),
]


@pytest.mark.parametrize("fb,size,taken", FRAMEBUFFERS)
def test_headless_framebuffer_must_lie_in_vram(host, fb, size, taken):
    r = headless(host, fb=fb, fb_size=size)
    assert handed_off(r) if taken else refused(r, NO_VRAM)


@pytest.mark.parametrize("fb,size,taken", FRAMEBUFFERS)
def test_panel_framebuffer_must_lie_in_vram(host, fb, size, taken):
    r = run(host, "panel", vram=VRAM, vsize=VRAM_SIZE, **framebuffer(fb, size))
    assert r.said(NO_VRAM) == (not taken)
    if taken:
        # every other check of the ADT side passed: the panel's /vram and its bandwidth scratch
        assert r.state == {"rc": "0", "vram": f"0x{VRAM:x}", "size": f"0x{VRAM_SIZE:x}",
                           "scratch": f"0x{0x20000 + 3 * 0x4000:x}", "nits": "600"}
        assert r.log == []
    else:
        assert r.state["rc"] == "-1" and len(r.log) == 1


def test_headless_without_pt_region_stays_handed_off(host):
    # t6050_ext_version() / t6050_ext_reject() apply only to engines already enabled in the input
    # DT, not engines Aura just enabled via t6050_ext_one(): omitting pt-region-* from the
    # headless ADT still leaves dcpext0, display-subsystem, and their providers enabled.
    r = headless(host, no_pt_region=1)
    assert handed_off(r)
    assert not r.said("rejected")
    assert not r.said("the device tree enables")


@pytest.mark.parametrize("no_pt_region", [1, 0])
def test_headless_duplicate_ext_node_refused(host, no_pt_region):
    # A second, input-enabled apple,t6050-dcpext node at the same coprocessor address makes the
    # mapping ambiguous: the headless handoff refuses before committing and t6050_ext_reject()
    # disables the duplicate node too, even when pt-region-* is absent.
    r = headless(host, no_pt_region=no_pt_region, dup_ext=1)
    assert r.state == {"chosen": "refused", **DISABLED, "dup": "disabled"}, r.log
    assert r.reserved == [OSLOG]
    assert r.said(REFUSED)
    assert not r.said("handed off")


@pytest.mark.parametrize("fail", [0, 1, 2, 3, 4])
def test_prewired_panel_keeps_rejected_external_engines_out_of_handoff(host, fail):
    r = run(host, "prewired", fail=fail, meta=META_SIZE, vram=VRAM, vsize=VRAM_SIZE,
            **framebuffer(VRAM, VRAM_SIZE))
    assert r.state["chosen"] == "published", r.log
    assert r.state["dcp"] == "okay" and r.state["subsystem"] == "okay"
    assert r.state["ext0"] == "okay" and r.state["ext0_fw"] == "27.2.0"
    if fail == 0:
        assert r.state["ext1"] == "okay" and r.state["ext1_fw"] == "27.2.0"
        assert r.state["ext1_ver"] == "27.2.0"
        assert r.iommus == [2, 0, 11, 0, 12, 0]
    else:
        assert r.state["ext1"] == "disabled"
        assert r.state["ext1_fw"] == "27.0.0" and r.state["ext1_ver"] == "27.0.0"
        assert r.iommus == [2, 0, 11, 0]


def test_prewired_panel_rejects_external_engines_on_unknown_stub_and_packed_dt(host):
    # Unknown stub: dt_set_display_handoff() refuses the internal panel, keeps healthy ext0 at
    # the tree's version, and still disables stopped ext1 and removes its display DART.
    r = run(host, "prewired", unknown=1, fail=1, meta=META_SIZE, vram=VRAM, vsize=VRAM_SIZE,
            **framebuffer(VRAM, VRAM_SIZE))
    assert r.state["chosen"] == "refused", r.log
    assert r.state["dcp"] == "disabled" and r.state["subsystem"] == "disabled"
    assert r.state["ext0"] == "okay" and r.state["ext0_fw"] == "27.0.0"
    assert r.state["ext1"] == "disabled"
    assert r.state["ext1_fw"] == "27.0.0" and r.state["ext1_ver"] == "27.0.0"
    assert r.iommus == [2, 0, 11, 0]

    # Packed DT where ext0's firmware-compat write fails with NOSPACE: rejection is not rolled
    # back, so stopped ext1 stays disabled and out of display-subsystem iommus.
    r = run(host, "prewired", pack=1, nocompat=1, fail=1, meta=META_SIZE, vram=VRAM,
            vsize=VRAM_SIZE, **framebuffer(VRAM, VRAM_SIZE))
    assert r.state["ext0"] == "okay" and r.state["ext0_fw"] == "none"
    assert r.state["ext1"] == "disabled"
    assert r.state["ext1_fw"] == "27.0.0" and r.state["ext1_ver"] == "27.0.0"
    assert r.iommus == [2, 0, 11, 0]

    # Packed DT where ext1 has no status property so status = "disabled" cannot fit: ext1 is
    # deleted instead and its display DART is removed from display-subsystem iommus.
    r = run(host, "prewired", pack=1, nocompat=1, absent_status=1, fail=1, meta=META_SIZE,
            vram=VRAM, vsize=VRAM_SIZE, **framebuffer(VRAM, VRAM_SIZE))
    assert r.state["ext0"] == "okay"
    assert r.state["ext1"] == "deleted"
    assert r.iommus == [2, 0, 11, 0]


def test_prewired_panel_rejects_nested_bus_external_engine(host):
    # Stopped ext1 behind two identity-mapped buses with path > 64 bytes: still disabled and
    # stripped from display-subsystem iommus while healthy ext0 and the panel stay up.
    r = run(host, "prewired", nested=1, fail=1, meta=META_SIZE, vram=VRAM, vsize=VRAM_SIZE,
            **framebuffer(VRAM, VRAM_SIZE))
    assert r.state["chosen"] == "published", r.log
    assert r.state["dcp"] == "okay" and r.state["subsystem"] == "okay"
    assert r.state["ext0"] == "okay" and r.state["ext0_fw"] == "27.2.0"
    assert r.state["ext1"] == "disabled"
    assert r.state["ext1_fw"] == "27.0.0" and r.state["ext1_ver"] == "27.0.0"
    assert r.iommus == [2, 0, 11, 0]

    # Healthy ext1 behind the same two identity-mapped buses stays okay and gets 27.2.0.
    r = run(host, "prewired", nested=1, fail=0, meta=META_SIZE, vram=VRAM, vsize=VRAM_SIZE,
            **framebuffer(VRAM, VRAM_SIZE))
    assert r.state["chosen"] == "published", r.log
    assert r.state["ext1"] == "okay" and r.state["ext1_fw"] == "27.2.0"
    assert r.state["ext1_ver"] == "27.2.0"
    assert r.iommus == [2, 0, 11, 0, 12, 0]

    # Packed DT with absent status on the nested-bus stopped engine deletes it and strips scanout.
    r = run(host, "prewired", pack=1, nocompat=1, absent_status=1, nested=1, fail=1, meta=META_SIZE,
            vram=VRAM, vsize=VRAM_SIZE, **framebuffer(VRAM, VRAM_SIZE))
    assert r.state["ext0"] == "okay"
    assert r.state["ext1"] == "deleted"
    assert r.iommus == [2, 0, 11, 0]


def test_prewired_panel_rejects_external_engines_across_failure_and_status_edges(host):
    # Scratch allocation failure during rejection: falls back to in-place edits so stopped ext1
    # is still disabled and stripped from scanout (and deleted on a packed DT with absent status).
    r = run(host, "prewired", fail_scratch=1, fail=1, meta=META_SIZE, vram=VRAM, vsize=VRAM_SIZE,
            **framebuffer(VRAM, VRAM_SIZE))
    assert r.state["ext1"] == "disabled"
    assert r.state["ext1_fw"] == "27.0.0" and r.state["ext1_ver"] == "27.0.0"
    assert r.iommus == [2, 0, 11, 0]

    r = run(host, "prewired", pack=1, absent_status=1, fail_scratch=1, fail=1, meta=META_SIZE,
            vram=VRAM, vsize=VRAM_SIZE, **framebuffer(VRAM, VRAM_SIZE))
    assert r.state["ext1"] == "deleted"
    assert r.iommus == [2, 0, 11, 0]

    # Oversized display-subsystem iommus (> 128 bytes): compacts iommus in place without
    # allocating, strips ext1's DART while keeping other phandles intact, disables rejected ext1,
    # and updates healthy ext0 to 27.2.0.
    expected_oversized = [2, 0, 11, 0] + [x for i in range(3, 18) for x in (100 + i, 0)]
    r = run(host, "prewired", oversized_iommus=1, fail=1, meta=META_SIZE, vram=VRAM,
            vsize=VRAM_SIZE, **framebuffer(VRAM, VRAM_SIZE))
    assert r.state["ext0"] == "okay" and r.state["ext0_fw"] == "27.2.0"
    assert r.state["ext1"] == "disabled"
    assert r.state["ext1_fw"] == "27.0.0" and r.state["ext1_ver"] == "27.0.0"
    assert r.iommus == expected_oversized

    # Extra-bytes status string ("okay\0disabled"): Linux probes the first string, so rejection
    # still disables ext1 and strips its display DART while keeping the panel and ext0 active.
    r = run(host, "prewired", extra_status=1, fail=1, meta=META_SIZE, vram=VRAM, vsize=VRAM_SIZE,
            **framebuffer(VRAM, VRAM_SIZE))
    assert r.state["chosen"] == "published", r.log
    assert r.state["dcp"] == "okay" and r.state["subsystem"] == "okay"
    assert r.state["ext0"] == "okay" and r.state["ext0_fw"] == "27.2.0"
    assert r.state["ext1"] == "disabled"
    assert r.state["ext1_fw"] == "27.0.0" and r.state["ext1_ver"] == "27.0.0"
    assert r.iommus == [2, 0, 11, 0]


@pytest.mark.parametrize("fail_scratch", [0, 1])
def test_prewired_panel_drops_rejected_scanout_from_enabled_oversized_iommus(host, fail_scratch):
    # Display subsystem already enabled ("okay") with >128-byte iommus: rejected ext1 is disabled
    # AND its DART phandle (12) is removed from iommus in place while all other phandles stay
    # intact, both with scratch allocation and with in-place fallback.
    expected_iommus = [2, 0, 11, 0] + [x for i in range(3, 18) for x in (100 + i, 0)]
    r = run(host, "prewired", disp_enabled=1, oversized_iommus=1, fail_scratch=fail_scratch,
            fail=1, meta=META_SIZE, vram=VRAM, vsize=VRAM_SIZE, **framebuffer(VRAM, VRAM_SIZE))
    assert r.state["subsystem"] == "okay"
    assert r.state["ext1"] == "disabled"
    assert r.state["ext1_fw"] == "27.0.0" and r.state["ext1_ver"] == "27.0.0"
    assert r.iommus == expected_iommus
    if not fail_scratch:
        assert r.state["ext0"] == "okay" and r.state["ext0_fw"] == "27.2.0"


def test_prewired_panel_rejects_packed_and_no_piodma_duplicate_ext_nodes(host):
    # Packed DT with two healthy duplicate ext1 nodes (both having piodma): deleting the first
    # duplicate must not let the second survive as unique, stay okay, or get version-stamped.
    r = run(host, "prewired", pack=1, absent_status=1, dup_ext=1, fail=0, meta=META_SIZE,
            vram=VRAM, vsize=VRAM_SIZE, **framebuffer(VRAM, VRAM_SIZE))
    assert r.state["ext0"] == "okay"
    assert r.state["ext1"] == "deleted"
    assert r.state["dup"] == "disabled" and r.state["dup_fw"] == "27.0.0"
    assert r.iommus == [2, 0, 11, 0]

    # Duplicate healthy ext1 nodes that both lack piodma: both are disabled and PH_EXT1_DISP (12)
    # is still removed from display-subsystem iommus via the preserved ADT index.
    r = run(host, "prewired", dup_ext=1, no_piodma=1, fail=0, meta=META_SIZE, vram=VRAM,
            vsize=VRAM_SIZE, **framebuffer(VRAM, VRAM_SIZE))
    assert r.state["chosen"] == "refused", r.log
    assert r.state["ext0"] == "okay" and r.state["ext0_fw"] == "27.2.0"
    assert r.state["ext1"] == "disabled" and r.state["ext1_fw"] == "27.0.0"
    assert r.state["dup"] == "disabled" and r.state["dup_fw"] == "27.0.0"
    assert r.iommus == [2, 0, 11, 0]


@pytest.mark.parametrize("mode", ["headless", "panel"])
def test_vram_at_the_top_of_the_address_space(host, mode):
    # /vram ends where the address space does: `vram + vram_size` is 0 there, and still right
    vram, vsize = U64 - 0x1000000, 0x1000000
    for fb, size, taken in [(vram, vsize, True), (U64 - SZ_16K, SZ_16K, True),
                            (U64 - SZ_16K, SZ_16K + 1, False), (vram - SZ_16K, SZ_16K, False),
                            (vram, vsize + 1, False), (SZ_16K, SZ_16K, False)]:
        if mode == "headless":
            r = headless(host, vram=vram, vsize=vsize, fb=fb, fb_size=size)
            assert handed_off(r, vram, vsize) if taken else refused(r, NO_VRAM)
        else:
            r = run(host, "panel", vram=vram, vsize=vsize, **framebuffer(fb, size))
            assert (r.state["rc"], r.said(NO_VRAM)) == (("0", False) if taken else ("-1", True))


@pytest.mark.parametrize("vram,vsize", [
    (VRAM, 0), (VRAM + 0x1000, VRAM_SIZE), (VRAM, VRAM_SIZE + 0x1000), (VRAM, SZ_16K - 1),
])
def test_headless_vram_must_be_whole_pages(host, vram, vsize):
    assert refused(headless(host, vram=vram, vsize=vsize, fb=vram, fb_size=min(vsize, SZ_16K)),
                   NO_VRAM)


def framebuffer_in_vram(vram, vsize, fb, size):
    """The rule, in whole numbers: a non-empty framebuffer that starts in /vram and does not leave it."""
    return fb != 0 and size != 0 and vram <= fb < vram + vsize and fb + size <= vram + vsize


# (stride, height, taken): zero or overflowing stride * height must be refused even when the
# wrapped 64-bit product would fit inside /vram
FB_GEOMETRIES = [
    # valid non-empty geometries inside /vram
    (7680, 1080, True),
    (SZ_16K, 1, True),
    # zero stride, zero height, or both
    (0, 1080, False),
    (7680, 0, False),
    (0, 0, False),
    # stride * height overflows 64 bits and wraps to 0
    (1 << 32, 1 << 32, False),
    (1 << 63, 2, False),
    # stride * height overflows 64 bits and wraps to a small non-zero value inside /vram
    ((1 << 63) + 1, 2, False),
    ((1 << 63) + SZ_16K // 2, 2, False),
    ((1 << 62) + 0x1000, 4, False),
    ((1 << 62) + 1, 4, False),
    (U64 - 1, 2, False),
]


@pytest.mark.parametrize("stride,height,taken", FB_GEOMETRIES)
def test_framebuffer_geometry_zero_and_overflow(host, stride, height, taken):
    r = run(host, "headless", vram=VRAM, vsize=VRAM_SIZE, meta=META_SIZE, bw=BW,
            fb=VRAM, stride=stride, height=height)
    assert handed_off(r) if taken else refused(r, NO_VRAM)
    r = run(host, "panel", vram=VRAM, vsize=VRAM_SIZE, fb=VRAM, stride=stride, height=height)
    assert (r.state["rc"], r.said(NO_VRAM)) == (("0", False) if taken else ("-1", True))


@pytest.mark.parametrize("stride,height,taken", [
    *FB_GEOMETRIES,
    # non-overflowing product whose ALIGN_UP(..., SZ_16K) wraps to 0
    (U64 - SZ_16K + 1, 1, False),
])
def test_t6041_headless_framebuffer_zero_and_overflow(host, stride, height, taken):
    r = run(host, "t6041-headless", fb=VRAM, stride=stride, height=height)
    if taken:
        aligned = (stride * height + SZ_16K - 1) & ~(SZ_16K - 1)
        assert r.state == {"rc": "0"}
        assert r.reserved == [(f"framebuffer@{VRAM:x}", VRAM, aligned)]
    else:
        assert r.state == {"rc": "-1"}
        assert r.reserved == []


@pytest.mark.parametrize("seed", range(12))
def test_seeded_framebuffers_match_the_rule(host, seed):
    rng = random.Random(0x408 + seed)
    taken = 0
    for _ in range(12):
        vsize = rng.choice([SZ_16K, 0x800000, VRAM_SIZE, 0x40000000])
        vram = rng.choice([VRAM, VRAM + 0x40000000, U64 - vsize, U64 - vsize - 0x4000000])
        fb = rng.choice([
            vram + rng.randrange(0, vsize), vram + rng.randrange(0, vsize, SZ_16K),
            vram + vsize + rng.choice([0, 1, SZ_16K, rng.randrange(0, 1 << 40)]),
            vram - rng.choice([1, SZ_16K, rng.randrange(1, 1 << 30)]),
        ]) % U64
        size = rng.choice([0, 1, SZ_16K, vsize, max(vram + vsize - fb, 0) % U64,
                           (vram + vsize - fb + 1) % U64, rng.randrange(0, 2 * vsize)])
        want = framebuffer_in_vram(vram, vsize, fb, size)
        taken += want
        r = headless(host, vram=vram, vsize=vsize, fb=fb, fb_size=size)
        assert handed_off(r, vram, vsize) if want else refused(r, NO_VRAM), (vram, vsize, fb, size)
        r = run(host, "panel", vram=vram, vsize=vsize, **framebuffer(fb, size))
        assert (r.state["rc"], r.said(NO_VRAM)) == (("0", False) if want else ("-1", True))
    # the cases are of both kinds
    assert 0 < taken < 12


# ---- the bandwidth scratch of a panel-less board's engine ---------------------------------------

# (size of dispext0's PMC metadata, scratch offset, taken)
SCRATCHES = [
    # zero and unaligned offsets are refused even inside a large block
    (META_SIZE, 0, False),
    (META_SIZE, 1, False),
    (META_SIZE, 2, False),
    (META_SIZE, 3, False),
    (META_SIZE, BW + 1, False),
    (META_SIZE, BW + 2, False),
    (META_SIZE, BW + 3, False),
    # non-zero 32-bit aligned offsets inside the block: smallest, typical, last word
    (META_SIZE, 4, True),
    (META_SIZE, BW, True),
    (META_SIZE, META_SIZE - 4, True),
    # its last byte, or more, is past the block
    (META_SIZE, META_SIZE - 3, False),
    (META_SIZE, META_SIZE, False),
    (META_SIZE, 0xffffffff, False),
    # small blocks: offset 0 and unaligned offsets are refused; 8 bytes holds offset 4
    (4, 0, False),
    (4, 1, False),
    (5, 1, False),
    (5, 2, False),
    (7, 4, False),
    (8, 4, True),
    (9, 4, True),
    (9, 5, False),
    # a block under a word holds none: `meta_size - 4` wrapped here, and any offset passed
    (3, 0, False),
    (3, 4, False),
    (3, BW, False),
    (3, 0xffffffff, False),
    (2, 0, False),
    (1, 0, False),
    (1, 0xfffffffd, False),
    (0, 0, False),
    (0, BW, False),
    (0, 0xfffffffc, False),
    # a 4 GiB block holds the last aligned 32-bit offset; unaligned 0xffffffff is still refused
    (0x100000000, 0xfffffffc, True),
    (0xffffffff, 0xfffffffc, False),
    (0x100000004, 0xffffffff, False),
    (0x100000002, 0xffffffff, False),
]


@pytest.mark.parametrize("meta,bw,taken", SCRATCHES)
def test_headless_scratch_must_lie_in_the_pmc_metadata(host, meta, bw, taken):
    r = headless(host, meta=meta, bw=bw)
    assert handed_off(r, bw=bw) if taken else refused(r, NO_SCRATCH, bw=bw)


def test_headless_scratch_block_must_be_the_adts(host):
    # the device tree's PMC node gives another size than dispext0's metadata: not that block
    assert refused(headless(host, dtmeta=META_SIZE + 0x4000), NO_SCRATCH)
    assert refused(headless(host, meta=3, dtmeta=META_SIZE), NO_SCRATCH)
    assert refused(headless(host, meta=META_SIZE, dtmeta=3), NO_SCRATCH)


@pytest.mark.parametrize("seed", range(6))
def test_seeded_scratches_match_the_rule(host, seed):
    rng = random.Random(0x264 + seed)
    taken = 0
    for _ in range(12):
        meta = rng.choice([0, 3, 4, 5, 8, 0x4000, META_SIZE, 0x100000000,
                           rng.randrange(8, 1 << 33)])
        bw = rng.choice([0, 1, 2, 3, 4, BW, 0xffffffff, 0xfffffffc,
                         (max(meta - 4, 0) & ~3) % (1 << 32),
                         max(meta - 3, 0) % (1 << 32), rng.randrange(0, 1 << 32)])
        # in whole numbers: a non-zero 32-bit aligned word whose four bytes end inside the block
        want = bw != 0 and (bw & 3) == 0 and bw + 4 <= meta
        taken += want
        r = headless(host, meta=meta, bw=bw)
        assert handed_off(r, bw=bw) if want else refused(r, NO_SCRATCH, bw=bw), (meta, bw)
    assert 0 < taken < 12
