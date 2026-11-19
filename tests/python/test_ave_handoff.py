# SPDX-License-Identifier: MIT
"""Hand every video encoder's preloaded firmware over, each matched to its ADT node by address.

kboot.c's dt_set_ave() runs unchanged (copied out of src/kboot.c with the reserved-memory helpers
it calls) on device trees built here, against a small ADT model, with src/devicetree.c and
libfdt. Covered:

  1. a lone encoder (the M1 Pro's, the M2's) is handed over as before: ADT ave0 or ave, its
     firmware reserved, its CRCs in /chosen, its status left as the DT has it;
  2. both M1 Max encoders are handed over, each from its own ADT node;
  3. an M1 Max encoder whose ADT node is absent, not preloaded or has a bad segment list is
     disabled, and encoder 0 is not;
  4. an encoder whose preloaded copy differs from encoder 0's is disabled; the CRCs stay;
  5. the M1 Ultra: two dies with the same die-local addresses, each node matched to the ADT
     encoder at its translated address;
  6. the M2 encoder's DART window base is read from the DART named after its ADT encoder;
  7. ADT names other than "ave" and "ave<N>" are never taken for an encoder;
  8. dt_set_ave_dapf() programs the CPU DART filter (dart-ave<N>) of every encoder after the
     first that the next stage gets, leaves encoder 0's to dapf.c, disables an encoder whose
     filter fails, and leaves T8140's inherited filters alone;
  9. the M3 (T8122) encoder of a J615, with the segment list of its live ADT, is handed over at
     its DART's window (vm-base plus the segment's offset, where iBoot left it) with no SoC list
     naming it; with an IPSW ADT (no preload) or an offset past the window it is disabled;
  10. both M3 Max (T6031) encoders of a J514c (IPSW ADT, modelled segment lists) are handed over,
     each at its own DART's window, and dart-ave1 is programmed only when the DT enables ave1;
  11. a node is an encoder by its compatible naming the AVE block ("apple,ave" or
     "apple,t<SoC>-ave"), never the decoder or a longer name; every SoC reads its window base
     from its DART, and an encoder whose DART has no vm-base is disabled (M1 Pro and M2 alike).

The harness also runs one DTB against an ADT modelled from it (`dtb <in> <out> <mode>`), which
compares two versions of the handoff on a kernel's device trees.
"""

from pathlib import Path
import subprocess

from host_compiler import host_cc


def region(source, start_marker, end_marker):
    start = source.index(start_marker)
    return source[start:source.index(end_marker, start)]


def handoff_source(source):
    """The handoff and the reserved-memory helpers it calls, from a kboot.c."""
    return "\n".join([
        region(source, "static int dt_device_set_reserved_mem(int node",
               "static int dt_device_set_reserved_mem_from_dart("),
        region(source, "static int dt_get_or_add_reserved_mem(const char *node_name",
               "static int dt_device_add_mem_region("),
        region(source, "static int dt_reserve_asc_segments_at(int fdt_node",
               "static int dt_reserve_asc_segments(const char"),
        region(source, "/*\n * The AVE video encoders run firmware that iBoot preloads",
               "/*\n * The M4 Pro, M5 Pro and A18 Pro Neural Engines"),
    ])


HARNESS = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "adt.h"
#include "devicetree.h"
#include "libfdt/libfdt.h"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((u64)(a) - 1))
#ifndef UINT64_MAX
#define UINT64_MAX (~0ULL)
#endif
enum { T6030 = 0x6030, T8132 = 0x8132, T8140 = 0x8140 };
static u32 chip_id;
static void *dt;
static int dt_bufsize __attribute__((unused)) = 1 << 20;

static char programmed[256];
static const char *failing_dart;
static int dapf_init(const char *path, int index)
{
    assert(index == 3);
    strcat(programmed, path);
    strcat(programmed, " ");
    return failing_dart && !strcmp(path, failing_dart) ? -1 : 0;
}
void *adt;
static struct { u64 phys_base; } cur_boot_args = {0x800000000000ULL};

static char logbuf[16384];
static size_t loglen;
static int log_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(logbuf + loglen, sizeof(logbuf) - loglen, fmt, ap);
    va_end(ap);
    if (n > 0)
        loglen += (size_t)n < sizeof(logbuf) - loglen ? (size_t)n : sizeof(logbuf) - loglen - 1;
    return n;
}
#define printf log_printf
#define bail(...) do { printf(__VA_ARGS__); return -1; } while (0)

/* ---------- the ADT model: /arm-io with encoders and DARTs, and /chosen ---------- */
#define ADT_ARM_IO 1
#define ADT_CHOSEN 2
#define ADT_NODE0 100
struct anode {
    char name[24];
    u64 addr;
    int preloaded;
    int nseg;
    struct adt_segment_ranges seg[4];
    char names[32];
    u64 vm_base; /* DARTs */
    int has_vm_base;
    u32 crc[4];
};
static struct anode anodes[24];
static int nanodes;
static u64 dram_base = 0x10000000000ULL;

static struct anode *anode_at(int off)
{
    return off >= ADT_NODE0 && off < ADT_NODE0 + nanodes ? &anodes[off - ADT_NODE0] : NULL;
}

int adt_path_offset(const void *a, const char *path)
{
    (void)a;
    if (!strcmp(path, "/arm-io"))
        return ADT_ARM_IO;
    if (!strcmp(path, "/chosen"))
        return ADT_CHOSEN;
    if (strncmp(path, "/arm-io/", 8))
        return -1;
    for (int i = 0; i < nanodes; i++)
        if (!strcmp(anodes[i].name, path + 8))
            return ADT_NODE0 + i;
    return -1;
}

int adt_path_offset_trace(const void *a, const char *path, int *trace)
{
    int off = adt_path_offset(a, path);
    if (off < ADT_NODE0)
        return -1;
    trace[0] = ADT_ARM_IO;
    trace[1] = off;
    trace[2] = 0;
    return off;
}

int adt_get_reg(const void *a, int *path, const char *prop, int idx, u64 *addr, u64 *size)
{
    (void)a;
    struct anode *n = anode_at(path[1]);
    if (path[0] != ADT_ARM_IO || path[2] || !n || strcmp(prop, "reg") || idx || !n->addr)
        return -1;
    *addr = n->addr;
    if (size)
        *size = 0x4000;
    return 0;
}

int adt_get_child_count(const void *a, int off)
{
    (void)a;
    return off == ADT_ARM_IO ? nanodes : 0;
}
int adt_first_child_offset(const void *a, int off)
{
    (void)a; (void)off;
    return ADT_NODE0;
}
int adt_next_sibling_offset(const void *a, int off)
{
    (void)a;
    return off + 1;
}
const char *adt_get_name(const void *a, int off)
{
    (void)a;
    struct anode *n = anode_at(off);
    return n ? n->name : NULL;
}

static u32 one = 1;
const void *adt_getprop(const void *a, int off, const char *name, u32 *len)
{
    (void)a;
    struct anode *n = anode_at(off);
    if (off == ADT_CHOSEN && !strcmp(name, "dram-base")) {
        *len = sizeof(dram_base);
        return &dram_base;
    }
    if (!n)
        return NULL;
    if (!strcmp(name, "pre-loaded") && n->preloaded) {
        *len = sizeof(one);
        return &one;
    }
    if (!strcmp(name, "segment-ranges") && n->nseg) {
        *len = n->nseg * sizeof(n->seg[0]);
        return n->seg;
    }
    if (!strcmp(name, "segment-names") && n->nseg) {
        *len = strlen(n->names) + 1;
        return n->names;
    }
    if (!strcmp(name, "vm-base") && n->has_vm_base) {
        *len = sizeof(n->vm_base);
        return &n->vm_base;
    }
    return NULL;
}

int adt_getprop_copy(const void *a, int off, const char *name, void *out, size_t len)
{
    u32 plen;
    const void *p = adt_getprop(a, off, name, &plen);
    if (!p || plen != len)
        return -1;
    memcpy(out, p, len);
    return plen;
}

/* The firmware copies are not in host memory: each segment's CRC is the model's. */
u32 tinf_crc32(const void *data, u32 size)
{
    (void)size;
    for (int i = 0; i < nanodes; i++)
        for (int j = 0; j < anodes[i].nseg; j++)
            if (anodes[i].seg[j].phys == (u64)data)
                return anodes[i].crc[j];
    return 0;
}

static struct anode *add_dart(const char *name, u64 vm_base);

/* An encoder and its DART (dart-<name>, vm-base 0 as on the M1 Pro; add_dart() changes it). */
static struct anode *add_encoder(const char *name, u64 addr)
{
    char dart[32];
    snprintf(dart, sizeof(dart), "dart-%s", name);
    add_dart(dart, 0);
    assert(nanodes < (int)ARRAY_SIZE(anodes));
    struct anode *n = &anodes[nanodes];
    memset(n, 0, sizeof(*n));
    snprintf(n->name, sizeof(n->name), "%s", name);
    n->addr = addr;
    n->preloaded = 1;
    n->nseg = 2;
    /* Each encoder its own copy, below the OS. */
    u64 phys = dram_base + 0x100000000ULL + nanodes * 0x1000000ULL;
    n->seg[0] = (struct adt_segment_ranges){phys, 0, 0x10000000ULL, 0xd0000, 0};
    n->seg[1] = (struct adt_segment_ranges){phys + 0x100000, 0xd0000, 0x10100000ULL, 0x128000, 0};
    strcpy(n->names, "__TEXT;__DATA");
    n->crc[0] = 0x460c7a7c;
    n->crc[1] = 0x12345678;
    nanodes++;
    return n;
}

static struct anode *add_dart(const char *name, u64 vm_base)
{
    for (int i = 0; i < nanodes; i++) {
        if (!strcmp(anodes[i].name, name)) {
            anodes[i].vm_base = vm_base;
            return &anodes[i];
        }
    }
    assert(nanodes < (int)ARRAY_SIZE(anodes));
    struct anode *n = &anodes[nanodes++];
    memset(n, 0, sizeof(*n));
    snprintf(n->name, sizeof(n->name), "%s", name);
    n->addr = 0x1000;
    n->vm_base = vm_base;
    n->has_vm_base = 1;
    return n;
}

/* The DART's ADT node has no vm-base. */
static void no_vm_base(const char *name)
{
    for (int i = 0; i < nanodes; i++)
        if (!strcmp(anodes[i].name, name))
            anodes[i].has_vm_base = 0;
}
''' + "@REGION@" + r'''

/* ---------- device trees ---------- */
static unsigned char blob[1 << 20];

static void reg2(fdt32_t *v, u64 a, u64 s)
{
    v[0] = cpu_to_fdt32(a >> 32); v[1] = cpu_to_fdt32(a);
    v[2] = cpu_to_fdt32(s >> 32); v[3] = cpu_to_fdt32(s);
}

/*
 * A tree with /chosen, /reserved-memory and one bus per die, as the kernel's M1 DTs. libfdt adds
 * a subnode before its siblings, so nodes are added last first to get the kernel DT's order.
 */
static void tree(int dies)
{
    dt = blob;
    assert(!fdt_create_empty_tree(dt, sizeof(blob)));
    assert(!fdt_setprop_u32(dt, 0, "#address-cells", 2));
    assert(!fdt_setprop_u32(dt, 0, "#size-cells", 2));
    assert(fdt_add_subnode(dt, 0, "chosen") >= 0);
    int rm = fdt_add_subnode(dt, 0, "reserved-memory");
    assert(rm >= 0);
    for (int d = dies - 1; d >= 0; d--) {
        char name[32];
        snprintf(name, sizeof(name), d ? "soc@2200000000" : "soc@200000000");
        int soc = fdt_add_subnode(dt, 0, name);
        assert(soc >= 0);
        assert(!fdt_setprop_u32(dt, soc, "#address-cells", 2));
        assert(!fdt_setprop_u32(dt, soc, "#size-cells", 2));
        fdt32_t r[6] = {cpu_to_fdt32(2), 0, cpu_to_fdt32(d ? 0x22 : 2), 0, cpu_to_fdt32(4), 0};
        assert(!fdt_setprop(dt, soc, "ranges", r, sizeof(r)));
    }
    nanodes = 0;
    loglen = 0;
    logbuf[0] = 0;
    programmed[0] = 0;
    failing_dart = NULL;
    chip_id = 0;
}

static int bus(int die)
{
    int n = fdt_path_offset(dt, die ? "/soc@2200000000" : "/soc@200000000");
    assert(n >= 0);
    return n;
}

static void encoder(int die, const char *compat, u64 addr, const char *status)
{
    char name[48];
    snprintf(name, sizeof(name), "video-codec@%llx", (unsigned long long)addr);
    int n = fdt_add_subnode(dt, bus(die), name);
    assert(n >= 0);
    assert(!fdt_setprop_string(dt, n, "compatible", compat));
    fdt32_t r[4];
    reg2(r, addr, 0x45c000);
    assert(!fdt_setprop(dt, n, "reg", r, sizeof(r)));
    if (status)
        assert(!fdt_setprop_string(dt, n, "status", status));
}

/* An encoder node with the SoC's compatible and the block's ("apple,t8122-ave", "apple,ave"). */
static void encoder2(int die, const char *soc, const char *block, u64 addr, const char *status)
{
    encoder(die, soc, addr, status);
    char path[64];
    snprintf(path, sizeof(path), "%s/video-codec@%llx", die ? "/soc@2200000000" : "/soc@200000000",
             (unsigned long long)addr);
    int n = fdt_path_offset(dt, path);
    assert(n >= 0 && !fdt_appendprop_string(dt, n, "compatible", block));
}

static int enc(int die, u64 addr)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/video-codec@%llx", die ? "/soc@2200000000" : "/soc@200000000",
             (unsigned long long)addr);
    int n = fdt_path_offset(dt, path);
    assert(n >= 0);
    return n;
}

static const char *status(int die, u64 addr)
{
    const char *s = fdt_getprop(dt, enc(die, addr), "status", NULL);
    return s ? s : "(none)";
}

/* The physical address of the first firmware region the node was given, or 0. */
static u64 firmware(int die, u64 addr, int idx)
{
    int len;
    const fdt32_t *mr = fdt_getprop(dt, enc(die, addr), "memory-region", &len);
    if (!mr || len < (idx + 1) * 4)
        return 0;
    int rn = fdt_node_offset_by_phandle(dt, fdt32_to_cpu(mr[idx]));
    assert(rn >= 0);
    const fdt64_t *r = fdt_getprop(dt, rn, "reg", NULL);
    return fdt64_to_cpu(r[0]);
}

/* The device address the node's firmware region @idx is mapped at (its iommu-addresses). */
static u64 iova_at(int die, u64 addr, int idx)
{
    int len;
    const fdt32_t *mr = fdt_getprop(dt, enc(die, addr), "memory-region", &len);
    assert(mr && len >= (idx + 1) * 4);
    int rn = fdt_node_offset_by_phandle(dt, fdt32_to_cpu(mr[idx]));
    const fdt32_t *ia = fdt_getprop(dt, rn, "iommu-addresses", &len);
    assert(ia && len >= 20);
    return ((u64)fdt32_to_cpu(ia[1]) << 32) | fdt32_to_cpu(ia[2]);
}

static u64 iova(int die, u64 addr)
{
    return iova_at(die, addr, 0);
}

static const char *chosen(const char *prop)
{
    const char *s = fdt_getprop(dt, fdt_path_offset(dt, "/chosen"), prop, NULL);
    return s ? s : "";
}

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\nlog:\n%s\n", __FILE__, __LINE__, #c, logbuf); exit(1); } } while (0)

#define M1_ENC0 0x40d100000ULL
#define M1_ENC1 0x507100000ULL

static void m1max(void)
{
    tree(1);
    encoder(0, "apple,t6000-ave", M1_ENC1, "okay");
    encoder(0, "apple,t6000-ave", M1_ENC0, "okay");
}

static int scenarios(void)
{
    struct anode *a;

    /* 1: the M1 Pro, one encoder, ADT ave0 */
    tree(1);
    encoder(0, "apple,t6000-ave", M1_ENC0, "okay");
    a = add_encoder("ave0", M1_ENC0);
    add_dart("dart-ave0", 0);
    CHECK(!dt_set_ave());
    CHECK(!strcmp(status(0, M1_ENC0), "okay"));
    CHECK(firmware(0, M1_ENC0, 0) == a->seg[0].phys && firmware(0, M1_ENC0, 1) == a->seg[1].phys);
    CHECK(iova(0, M1_ENC0) == 0); /* the 6070 firmware runs from DART address zero */
    CHECK(!strcmp(chosen("aurora,ave-text-crc32"), "460c7a7c"));
    CHECK(!strcmp(chosen("aurora,ave-data-crc32"), "12345678"));
    CHECK(!strstr(logbuf, "AVE"));
    puts("PASS: M1 Pro encoder handed over from ADT ave0");

    /* A lone encoder the DT leaves off is still given its firmware, and left off. */
    tree(1);
    encoder(0, "apple,t6000-ave", M1_ENC0, "disabled");
    add_encoder("ave0", M1_ENC0);
    CHECK(!dt_set_ave());
    CHECK(!strcmp(status(0, M1_ENC0), "disabled") && firmware(0, M1_ENC0, 0));
    puts("PASS: lone encoder left as the DT has it");

    /* 2: the M1 Max, both encoders */
    m1max();
    a = add_encoder("ave0", M1_ENC0);
    struct anode *b = add_encoder("ave1", M1_ENC1);
    CHECK(!dt_set_ave());
    CHECK(!strcmp(status(0, M1_ENC0), "okay") && !strcmp(status(0, M1_ENC1), "okay"));
    CHECK(firmware(0, M1_ENC0, 0) == a->seg[0].phys && firmware(0, M1_ENC1, 0) == b->seg[0].phys);
    CHECK(firmware(0, M1_ENC1, 1) == b->seg[1].phys);
    CHECK(strstr(logbuf, "video-codec@507100000 handed over from ADT /arm-io/ave1"));
    puts("PASS: M1 Max encoders 0 and 1 handed over, each from its own ADT node");

    /* The ADT lists its encoders in any order; addresses decide. */
    m1max();
    b = add_encoder("ave1", M1_ENC1);
    a = add_encoder("ave0", M1_ENC0);
    CHECK(!dt_set_ave());
    CHECK(firmware(0, M1_ENC0, 0) == a->seg[0].phys && firmware(0, M1_ENC1, 0) == b->seg[0].phys);
    puts("PASS: ADT order does not matter");

    /* 3: encoder 1 absent from the ADT, not preloaded, or with a bad segment list */
    for (int c = 0; c < 4; c++) {
        m1max();
        a = add_encoder("ave0", M1_ENC0);
        if (c == 1) {
            add_encoder("ave1", M1_ENC1)->preloaded = 0;
        } else if (c == 2) {
            b = add_encoder("ave1", M1_ENC1);
            b->seg[1].phys = b->seg[0].phys; /* overlapping */
        } else if (c == 3) {
            add_encoder("ave1", M1_ENC1 + 0x100000); /* another block's address */
        }
        CHECK(!dt_set_ave());
        CHECK(!strcmp(status(0, M1_ENC0), "okay") && firmware(0, M1_ENC0, 0) == a->seg[0].phys);
        CHECK(!strcmp(status(0, M1_ENC1), "disabled") && !firmware(0, M1_ENC1, 0));
        CHECK(strstr(logbuf, "video-codec@507100000 disabled"));
        CHECK(!strcmp(chosen("aurora,ave-text-crc32"), "460c7a7c"));
    }
    puts("PASS: encoder 1 absent, not preloaded, bad segments or elsewhere: disabled; encoder 0 kept");

    /* Encoder 0 absent: encoder 1 is handed over alone. */
    m1max();
    b = add_encoder("ave1", M1_ENC1);
    CHECK(!dt_set_ave());
    CHECK(!strcmp(status(0, M1_ENC0), "disabled") && firmware(0, M1_ENC1, 0) == b->seg[0].phys);
    puts("PASS: encoder 0 absent: disabled, encoder 1 handed over");

    /* 4: encoder 1's copy differs: it is disabled, encoder 0's CRCs stay. */
    m1max();
    add_encoder("ave0", M1_ENC0);
    add_encoder("ave1", M1_ENC1)->crc[1] = 0xdeadbeef;
    CHECK(!dt_set_ave());
    CHECK(!strcmp(status(0, M1_ENC0), "okay") && !strcmp(status(0, M1_ENC1), "disabled"));
    CHECK(!firmware(0, M1_ENC1, 0));
    CHECK(!strcmp(chosen("aurora,ave-data-crc32"), "12345678"));
    CHECK(strstr(logbuf, "differs from the encoder already handed over"));
    puts("PASS: a differing firmware copy is refused, the published CRCs kept");

    /* 5: the M1 Ultra: die 1 repeats die 0's die-local addresses. */
    tree(2);
    encoder(1, "apple,t6000-ave", M1_ENC1, "disabled");
    encoder(1, "apple,t6000-ave", M1_ENC0, "okay");
    encoder(0, "apple,t6000-ave", M1_ENC1, "okay");
    encoder(0, "apple,t6000-ave", M1_ENC0, "okay");
    struct anode *u[4] = {
        add_encoder("ave0", M1_ENC0), add_encoder("ave1", M1_ENC1),
        add_encoder("ave2", M1_ENC0 + 0x2000000000ULL), add_encoder("ave3", M1_ENC1 + 0x2000000000ULL),
    };
    CHECK(!dt_set_ave());
    CHECK(firmware(0, M1_ENC0, 0) == u[0]->seg[0].phys && firmware(0, M1_ENC1, 0) == u[1]->seg[0].phys);
    CHECK(firmware(1, M1_ENC0, 0) == u[2]->seg[0].phys && firmware(1, M1_ENC1, 0) == u[3]->seg[0].phys);
    CHECK(!strcmp(status(1, M1_ENC0), "okay") && !strcmp(status(1, M1_ENC1), "disabled"));
    puts("PASS: M1 Ultra: four encoders, each from the ADT node at its translated address");

    /* Die 1's ADT encoders missing: only die 1's nodes are disabled. */
    tree(2);
    encoder(1, "apple,t6000-ave", M1_ENC0, "okay");
    encoder(0, "apple,t6000-ave", M1_ENC0, "okay");
    add_encoder("ave0", M1_ENC0);
    CHECK(!dt_set_ave());
    CHECK(!strcmp(status(0, M1_ENC0), "okay") && !strcmp(status(1, M1_ENC0), "disabled"));
    puts("PASS: M1 Ultra die 1 without ADT encoders: die 1 disabled, die 0 kept");

    /* 6: the M2: ADT ave, its DART window at dart-ave's vm-base. */
    tree(1);
    encoder(0, "apple,t8112-ave", 0x267100000ULL, "okay");
    a = add_encoder("ave", 0x267100000ULL);
    add_dart("dart-ave", 0x800000000ULL);
    add_dart("dart-ave0", 0x1230000000ULL); /* not the encoder's */
    CHECK(!dt_set_ave());
    CHECK(!strcmp(status(0, 0x267100000ULL), "okay") && firmware(0, 0x267100000ULL, 0) == a->seg[0].phys);
    CHECK(iova(0, 0x267100000ULL) == 0x800000000ULL);
    puts("PASS: M2 encoder at its DART's vm-base");

    tree(1);
    encoder(0, "apple,t8112-ave", 0x267100000ULL, "okay");
    add_encoder("ave", 0x267100000ULL);
    no_vm_base("dart-ave");
    CHECK(!dt_set_ave());
    CHECK(!strcmp(status(0, 0x267100000ULL), "disabled") && strstr(logbuf, "vm-base missing"));
    puts("PASS: M2 encoder without its DART's vm-base: disabled");

    /* The M1 Pro's window base is read the same way: vm-base 0 is a base, no vm-base is not. */
    tree(1);
    encoder(0, "apple,t6000-ave", M1_ENC0, "okay");
    add_encoder("ave0", M1_ENC0);
    no_vm_base("dart-ave0");
    CHECK(!dt_set_ave());
    CHECK(!strcmp(status(0, M1_ENC0), "disabled") && strstr(logbuf, "vm-base missing"));
    CHECK(!firmware(0, M1_ENC0, 0) && !chosen("aurora,ave-text-crc32")[0]);
    puts("PASS: M1 Pro encoder without its DART's vm-base: disabled");

    /*
     * 9: the M3 (T8122) of a J615 under macOS 27.2, from its live ADT: a lone encoder named ave at
     * 0x315100000, compatible "apple,t8122-ave", "apple,ave", left disabled by the DT. iBoot's
     * segment list: TEXT 0x134000 bytes at DART offset 0, DATA 0x128000 bytes after it, both
     * remapped to dart-ave's vm-base (0x10000000000) plus their offset.
     */
#define J615_AVE 0x315100000ULL
#define M3_VM_BASE 0x10000000000ULL
    for (int on = 0; on < 2; on++) {
        tree(1);
        encoder2(0, "apple,t8122-ave", "apple,ave", J615_AVE, on ? "okay" : "disabled");
        a = add_encoder("ave", J615_AVE);
        a->seg[0] = (struct adt_segment_ranges){0x10000a8c000ULL, 0, 0x10000000000ULL, 0x134000, 1};
        a->seg[1] = (struct adt_segment_ranges){0x10001a2c000ULL, 0x134000, 0x10000134000ULL,
                                                0x128000, 0};
        add_dart("dart-ave", M3_VM_BASE);
        CHECK(!dt_set_ave());
        CHECK(!strcmp(status(0, J615_AVE), on ? "okay" : "disabled"));
        CHECK(firmware(0, J615_AVE, 0) == 0x10000a8c000ULL);
        CHECK(firmware(0, J615_AVE, 1) == 0x10001a2c000ULL);
        /* Where iBoot left the coprocessor: the remap addresses, the window base plus offset. */
        CHECK(iova_at(0, J615_AVE, 0) == a->seg[0].remap);
        CHECK(iova_at(0, J615_AVE, 1) == a->seg[1].remap);
        CHECK(!strcmp(chosen("aurora,ave-text-crc32"), "460c7a7c"));
        CHECK(!strcmp(chosen("aurora,ave-data-crc32"), "12345678"));
        CHECK(!strstr(logbuf, "AVE"));
    }
    puts("PASS: J615 (T8122) encoder handed over at dart-ave's window, status kept");

    /* The same J615 with an IPSW ADT: no iBoot preload, no segment list. Disabled, no CRCs. */
    tree(1);
    encoder2(0, "apple,t8122-ave", "apple,ave", J615_AVE, "okay");
    a = add_encoder("ave", J615_AVE);
    a->preloaded = 0;
    a->nseg = 0;
    add_dart("dart-ave", M3_VM_BASE);
    CHECK(!dt_set_ave());
    CHECK(!strcmp(status(0, J615_AVE), "disabled") && strstr(logbuf, "firmware not preloaded"));
    CHECK(!firmware(0, J615_AVE, 0) && !chosen("aurora,ave-text-crc32")[0]);
    puts("PASS: J615 encoder without an iBoot preload: disabled");

    /* An offset that already carries the window base is not the layout this handoff knows. */
    tree(1);
    encoder2(0, "apple,t8122-ave", "apple,ave", J615_AVE, "okay");
    a = add_encoder("ave", J615_AVE);
    a->seg[1].iova = M3_VM_BASE + 0xd0000;
    add_dart("dart-ave", M3_VM_BASE);
    CHECK(!dt_set_ave());
    CHECK(!strcmp(status(0, J615_AVE), "disabled") && strstr(logbuf, "outside its DART window"));
    CHECK(!firmware(0, J615_AVE, 0));
    puts("PASS: segment offset beyond the DART window: disabled");

    /*
     * 10: the M3 Max (T6031) of a J514c, from the IPSW ADT: ave0 at 0x285100000 and ave1 at
     * 0x3c5100000, each with its own DART at vm-base 0x10000000000. IPSW ADTs carry no segment
     * lists, so each encoder gets the model's, remapped at its window as on the J615.
     */
#define T6031_AVE0 0x285100000ULL
#define T6031_AVE1 0x3c5100000ULL
    tree(1);
    encoder2(0, "apple,t6031-ave", "apple,ave", T6031_AVE1, "okay");
    encoder2(0, "apple,t6031-ave", "apple,ave", T6031_AVE0, "okay");
    a = add_encoder("ave0", T6031_AVE0);
    b = add_encoder("ave1", T6031_AVE1);
    for (int i = 0; i < 2; i++) {
        a->seg[i].remap = M3_VM_BASE + a->seg[i].iova;
        b->seg[i].remap = M3_VM_BASE + b->seg[i].iova;
    }
    add_dart("dart-ave0", M3_VM_BASE);
    add_dart("dart-ave1", M3_VM_BASE);
    CHECK(!dt_set_ave());
    CHECK(!strcmp(status(0, T6031_AVE0), "okay") && !strcmp(status(0, T6031_AVE1), "okay"));
    CHECK(firmware(0, T6031_AVE0, 0) == a->seg[0].phys && firmware(0, T6031_AVE1, 0) == b->seg[0].phys);
    CHECK(firmware(0, T6031_AVE1, 1) == b->seg[1].phys);
    CHECK(iova_at(0, T6031_AVE0, 0) == M3_VM_BASE && iova_at(0, T6031_AVE1, 0) == M3_VM_BASE);
    CHECK(iova_at(0, T6031_AVE1, 1) == b->seg[1].remap);
    CHECK(strstr(logbuf, "video-codec@285100000 handed over from ADT /arm-io/ave0"));
    CHECK(strstr(logbuf, "video-codec@3c5100000 handed over from ADT /arm-io/ave1"));
#ifndef NO_AVE_DAPF
    CHECK(!fdt_pack(dt) && !dt_set_ave_dapf() && !strcmp(programmed, "/arm-io/dart-ave1 "));
#endif
    puts("PASS: J514c (T6031) encoders 0 and 1 handed over, each at its own DART's window");

    /* As the kernel DT ships them, both off: handed over, left off, dart-ave1 left alone. */
    tree(1);
    encoder2(0, "apple,t6031-ave", "apple,ave", T6031_AVE1, "disabled");
    encoder2(0, "apple,t6031-ave", "apple,ave", T6031_AVE0, "disabled");
    add_encoder("ave0", T6031_AVE0);
    add_encoder("ave1", T6031_AVE1);
    add_dart("dart-ave0", M3_VM_BASE);
    add_dart("dart-ave1", M3_VM_BASE);
    CHECK(!dt_set_ave());
    CHECK(!strcmp(status(0, T6031_AVE0), "disabled") && !strcmp(status(0, T6031_AVE1), "disabled"));
    CHECK(firmware(0, T6031_AVE0, 0) && firmware(0, T6031_AVE1, 0));
#ifndef NO_AVE_DAPF
    CHECK(!fdt_pack(dt) && !dt_set_ave_dapf() && !programmed[0]);
#endif
    puts("PASS: J514c encoders the DT leaves off: handed over, left off");

    /* 11: only the encoder block is an encoder: not the decoder, nor a name that ends otherwise. */
    tree(1);
    encoder2(0, "apple,t8122-avd", "apple,avd", J615_AVE, "okay");
    encoder(0, "apple,t8122-ave-dart", J615_AVE + 0x100000, "okay");
    encoder(0, "apple,ave2", J615_AVE + 0x200000, "okay");
    add_encoder("ave", J615_AVE);
    add_dart("dart-ave", M3_VM_BASE);
    CHECK(!dt_set_ave());
    CHECK(!strcmp(status(0, J615_AVE), "okay") && !firmware(0, J615_AVE, 0));
    CHECK(!strcmp(status(0, J615_AVE + 0x100000), "okay") && !strcmp(status(0, J615_AVE + 0x200000), "okay"));
    CHECK(!chosen("aurora,ave-text-crc32")[0] && !strstr(logbuf, "AVE"));
    puts("PASS: nodes that are not the encoder block are left alone");

    /* A node with only the block's compatible is an encoder. */
    tree(1);
    encoder(0, "apple,ave", J615_AVE, "okay");
    a = add_encoder("ave", J615_AVE);
    add_dart("dart-ave", M3_VM_BASE);
    CHECK(!dt_set_ave());
    CHECK(firmware(0, J615_AVE, 0) == a->seg[0].phys && iova(0, J615_AVE) == M3_VM_BASE);
    puts("PASS: \"apple,ave\" alone is an encoder");

    /* 7: names that are not an encoder's */
    tree(1);
    encoder(0, "apple,t6000-ave", M1_ENC0, "okay");
    add_dart("dart-ave0", 0)->addr = M1_ENC0;
    add_encoder("avef", M1_ENC0);
    add_encoder("ave0x", M1_ENC0);
    add_encoder("ave-0", M1_ENC0);
    CHECK(!dt_set_ave());
    CHECK(!strcmp(status(0, M1_ENC0), "disabled") && strstr(logbuf, "ADT node absent"));
    puts("PASS: only ave and ave<N> are encoders");

#ifndef NO_AVE_DAPF
    /* 8: the CPU DART filters of the encoders after the first */
    tree(1);
    encoder(0, "apple,t6000-ave", M1_ENC0, "okay");
    add_encoder("ave0", M1_ENC0);
    add_dart("dart-ave0", 0);
    CHECK(!dt_set_ave() && !fdt_pack(dt));
    u32 size = fdt_totalsize(dt);
    CHECK(!dt_set_ave_dapf() && !programmed[0] && fdt_totalsize(dt) == size);
    puts("PASS: one encoder: no filter beyond dapf.c's dart-ave0");

    m1max();
    add_encoder("ave0", M1_ENC0); add_dart("dart-ave0", 0);
    add_encoder("ave1", M1_ENC1); add_dart("dart-ave1", 0);
    CHECK(!dt_set_ave() && !fdt_pack(dt));
    CHECK(!dt_set_ave_dapf() && !strcmp(programmed, "/arm-io/dart-ave1 "));
    CHECK(!strcmp(status(0, M1_ENC1), "okay"));
    puts("PASS: M1 Max: dart-ave1 programmed for encoder 1");

    m1max();
    add_encoder("ave0", M1_ENC0); add_dart("dart-ave0", 0);
    add_encoder("ave1", M1_ENC1)->preloaded = 0; add_dart("dart-ave1", 0);
    CHECK(!dt_set_ave() && !fdt_pack(dt));
    CHECK(!dt_set_ave_dapf() && !programmed[0]);
    puts("PASS: encoder 1 not handed over: dart-ave1 left alone");

    m1max();
    add_encoder("ave0", M1_ENC0); add_dart("dart-ave0", 0);
    add_encoder("ave1", M1_ENC1); add_dart("dart-ave1", 0);
    CHECK(!dt_set_ave() && !fdt_pack(dt));
    failing_dart = "/arm-io/dart-ave1";
    CHECK(dt_set_ave_dapf() < 0);
    CHECK(!strcmp(status(0, M1_ENC1), "disabled") && !strcmp(status(0, M1_ENC0), "okay"));
    CHECK(fdt_totalsize(dt) < (u32)dt_bufsize); /* packed again */
    puts("PASS: dart-ave1 failure leaves encoder 1 off");

    tree(2);
    encoder(1, "apple,t6000-ave", M1_ENC1, "okay");
    encoder(1, "apple,t6000-ave", M1_ENC0, "okay");
    encoder(0, "apple,t6000-ave", M1_ENC1, "okay");
    encoder(0, "apple,t6000-ave", M1_ENC0, "okay");
    add_encoder("ave0", M1_ENC0); add_encoder("ave1", M1_ENC1);
    add_encoder("ave2", M1_ENC0 + 0x2000000000ULL); add_encoder("ave3", M1_ENC1 + 0x2000000000ULL);
    add_dart("dart-ave0", 0); add_dart("dart-ave1", 0); add_dart("dart-ave2", 0);
    add_dart("dart-ave3", 0);
    CHECK(!dt_set_ave() && !fdt_pack(dt));
    CHECK(!dt_set_ave_dapf());
    CHECK(!strcmp(programmed, "/arm-io/dart-ave1 /arm-io/dart-ave2 /arm-io/dart-ave3 "));
    puts("PASS: M1 Ultra: dart-ave1, dart-ave2 and dart-ave3 programmed");

    tree(1);
    encoder(0, "apple,t8140-ave", 0x289100000ULL, "okay");
    add_encoder("ave1", 0x289100000ULL); add_dart("dart-ave1", 0);
    chip_id = T8140;
    CHECK(!dt_set_ave_dapf() && !programmed[0]);
    puts("PASS: T8140 inherited filters preserved");
#endif
    return 0;
}

/* ---------- one kernel DTB against an ADT modelled from it ---------- */
/*
 * The first compatible of an encoder node, or NULL: the harness's own reading of the kernel DTs
 * (a node naming "apple,ave" or "apple,t<SoC>-ave"), so it runs any version of the handoff.
 */
static const char *dtb_encoder(int node)
{
    int len;
    const char *list = fdt_getprop(dt, node, "compatible", &len);
    for (int pos = 0; list && pos < len; pos += strlen(list + pos) + 1) {
        const char *c = list + pos;
        size_t n = strlen(c);
        if (!strcmp(c, "apple,ave") ||
            (!strncmp(c, "apple,t", 7) && n > 11 && !strcmp(c + n - 4, "-ave")))
            return list;
    }
    return NULL;
}

static bool compat_is(const char *compat, const char *soc)
{
    return !strncmp(compat, soc, strlen(soc));
}

/*
 * <mode>: "all" every encoder preloaded; "first" only the first; "none" no encoder in the ADT;
 * "cold" all present, none preloaded. Encoders are named ave on SoCs whose ADT has one (or, on the
 * A18 Pro, one of two) and ave<N> elsewhere. The A18 Pro ADT has encoder 0 only, or with "first"
 * encoder 1 only.
 */
static int run_dtb(const char *in, const char *out, const char *mode)
{
    FILE *f = fopen(in, "rb");
    assert(f);
    static unsigned char raw[1 << 20];
    size_t n = fread(raw, 1, sizeof(raw), f);
    fclose(f);
    assert(n > 0 && !fdt_open_into(raw, blob, sizeof(blob)));
    dt = blob;
    nanodes = 0;
    if (fdt_path_offset(dt, "/reserved-memory") < 0) {
        int rm = fdt_add_subnode(dt, 0, "reserved-memory");
        assert(rm >= 0);
    }
    int k = 0, total = 0;
    for (int node = fdt_next_node(dt, -1, NULL); node >= 0; node = fdt_next_node(dt, node, NULL))
        total += dtb_encoder(node) != NULL;
    for (int node = fdt_next_node(dt, -1, NULL); node >= 0; node = fdt_next_node(dt, node, NULL)) {
        const char *compat = dtb_encoder(node);
        if (!compat || !strcmp(mode, "none"))
            continue;
        u64 addr = dt_get_address(dt, node);
        char name[16];
        /* The ADT names a lone encoder ave, except on the M1 Pro and M2 Pro (ave0). */
        bool lone = total == 1 && !compat_is(compat, "apple,t6000-ave") &&
                    !compat_is(compat, "apple,t6020-ave") && !compat_is(compat, "apple,t6041-ave");
        if (!strcmp(compat, "apple,t8140-ave")) {
            if (!strcmp(mode, "first") ? addr != 0x289100000ULL : addr != 0x285100000ULL)
                continue;
            snprintf(name, sizeof(name), addr == 0x285100000ULL ? "ave" : "ave1");
        } else {
            snprintf(name, sizeof(name), lone ? "ave" : "ave%d", k);
        }
        if (k++ && !strcmp(mode, "first"))
            continue;
        struct anode *a = add_encoder(name, addr);
        a->preloaded = strcmp(mode, "cold") != 0;
        /*
         * The ADTs' DART windows: vm-base 0 on the M1 family, 0x800000000 on the M2 and
         * 0x10000000000 on every later encoder. iBoot remaps the segments to vm-base plus their
         * offset, except on the M1 family, where TEXT's remap address is its physical address.
         */
        u64 vm_base = compat_is(compat, "apple,t8112-ave") ? 0x800000000ULL : 0x10000000000ULL;
        bool m1 = compat_is(compat, "apple,t6000-ave") || compat_is(compat, "apple,t8103-ave");
        if (m1)
            vm_base = 0;
        for (int i = 0; i < a->nseg; i++)
            a->seg[i].remap = m1 ? a->seg[i].phys : vm_base + a->seg[i].iova;
        char dart[24];
        snprintf(dart, sizeof(dart), "dart-%s", name);
        add_dart(dart, vm_base);
    }
    int ret = dt_set_ave();
    assert(!fdt_pack(dt));
    f = fopen(out, "wb");
    assert(f);
    fwrite(dt, 1, fdt_totalsize(dt), f);
    fclose(f);
    fputs(logbuf, stdout);
    return ret;
}

int main(int argc, char **argv)
{
    if (argc == 5 && !strcmp(argv[1], "dtb"))
        return run_dtb(argv[2], argv[3], argv[4]) ? 1 : 0;
    return scenarios();
}
'''


def build(tmp_path, kboot_source, name="ave-handoff"):
    repo = Path(__file__).resolve().parents[2]
    harness = tmp_path / f"{name}.c"
    code = HARNESS.replace("@REGION@", handoff_source(kboot_source))
    if "static int dt_set_ave_dapf(" not in kboot_source:
        code = "#define NO_AVE_DAPF\n" + code
    harness.write_text(code)
    binary = tmp_path / name
    lib = repo / "src/libfdt"
    files = ["fdt.c", "fdt_ro.c", "fdt_rw.c", "fdt_wip.c", "fdt_sw.c", "fdt_empty_tree.c",
             "fdt_strerror.c", "fdt_addresses.c"]
    subprocess.run([
        *host_cc(), "-std=gnu11", "-Wall", "-Werror", "-Wno-sign-compare",
        "-Wno-unused-function", "-Wno-format-truncation", "-I", str(repo / "src"),
        "-I", str(repo), str(harness), str(repo / "src/devicetree.c"),
        *[str(lib / f) for f in files], "-o", str(binary),
    ], check=True)
    return binary


def test_ave_handoff(tmp_path):
    repo = Path(__file__).resolve().parents[2]
    binary = build(tmp_path, (repo / "src/kboot.c").read_text())
    run = subprocess.run([str(binary)], capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr
    assert run.stdout.count("PASS:") == 26, run.stdout
