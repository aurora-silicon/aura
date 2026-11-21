/* SPDX-License-Identifier: MIT */
/*
 * Host test of the atc-phy3 lane tunable rule (src/kboot_atc.c) on T6030. Synthetic ADTs in the
 * boot loader's binary layout, an ADT library with the boot loader's bounds (a lookup the boot
 * loader would panic on fails the test) and real libfdt. Any register access fails the test. Run
 * by tests/run-t6030-display.sh.
 *
 *   t6030-display-host checks       every scenario's own checks
 *   t6030-display-host atc <name>   one ATC scenario: a digest per PHY node and the log, for the
 *                                   comparison with a base tree
 */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adt.h"
#include "kboot_atc.h"
#include "utils.h"

#include "libfdt/libfdt.h"

u32 board_id, chip_id;
void *adt;
static u32 adt_size;
static char logbuf[1 << 16];
static size_t loglen;
static int fail_malloc;

void *__real_malloc(size_t size);
void *__wrap_malloc(size_t size)
{
    return fail_malloc ? NULL : __real_malloc(size);
}

int debug_printf(const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(logbuf + loglen, sizeof(logbuf) - loglen, fmt, ap);
    va_end(ap);
    if (n > 0)
        loglen = min(loglen + n, sizeof(logbuf) - 1);
    return n;
}

void host_mmio(u64 addr)
{
    fprintf(stderr, "FAIL: register access at 0x%llx\n", (unsigned long long)addr);
    abort();
}

#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\nlog:\n%s\n", __FILE__, __LINE__, #c, logbuf);         \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

/* ---- ADT builder, in the boot loader's layout ---- */

static u8 *bld;
static size_t blen, bcap;
static struct {
    size_t hdr;
    int kids;
} open_nodes[32];
static int depth;

static void b_put(const void *p, size_t n)
{
    if (blen + n > bcap) {
        bcap = (blen + n) * 2;
        bld = realloc(bld, bcap);
        CHECK(bld);
    }
    memcpy(bld + blen, p, n);
    blen += n;
}

static u32 *b_u32(size_t off)
{
    return (u32 *)(bld + off);
}

/* A property; a name of 32 characters is stored without a terminator */
static void prop(const char *name, const void *val, u32 len)
{
    char field[32] = {0};
    static const u8 zero[4];

    CHECK(depth > 0 && !open_nodes[depth - 1].kids);
    (*b_u32(open_nodes[depth - 1].hdr))++;
    memcpy(field, name, min(strlen(name), sizeof(field)));
    b_put(field, sizeof(field));
    b_put(&len, sizeof(len));
    if (len)
        b_put(val, len);
    b_put(zero, (4 - len % 4) % 4);
}

static void prop_str(const char *name, const char *s)
{
    prop(name, s, strlen(s) + 1);
}

static void prop_u32s(const char *name, int n, ...)
{
    u32 v[64];
    va_list ap;

    CHECK(n <= 64);
    va_start(ap, n);
    for (int i = 0; i < n; i++)
        v[i] = va_arg(ap, u32);
    va_end(ap);
    prop(name, v, n * 4);
}

static void prop_fill(const char *name, u32 len, u8 fill)
{
    u8 *v = malloc(len ? len : 1);
    memset(v, fill, len);
    prop(name, v, len);
    free(v);
}

/* A node without its name property yet */
static void node_open(void)
{
    u32 hdr[2] = {0, 0};

    if (depth) {
        (*b_u32(open_nodes[depth - 1].hdr + 4))++;
        open_nodes[depth - 1].kids++;
    }
    open_nodes[depth].hdr = blen;
    open_nodes[depth].kids = 0;
    depth++;
    b_put(hdr, sizeof(hdr));
}

static void node_begin(const char *name)
{
    node_open();
    prop_str("name", name);
}

static void node_end(void)
{
    depth--;
}

/* A property header that claims `size` bytes of value, followed by `have` bytes of zeros */
static void prop_claim(const char *name, size_t namelen, u32 size, u32 have)
{
    char field[32];
    static const u8 zero[64];

    CHECK(depth > 0 && !open_nodes[depth - 1].kids && have <= sizeof(zero));
    (*b_u32(open_nodes[depth - 1].hdr))++;
    memset(field, 0, sizeof(field));
    memcpy(field, name, min(namelen, sizeof(field)));
    b_put(field, sizeof(field));
    b_put(&size, sizeof(size));
    b_put(zero, have);
}

/* A property whose 32-byte name field has no terminator and bytes that are not UTF-8 */
static void prop_bad_name(void)
{
    char name[32];

    memset(name, 0xff, sizeof(name));
    memcpy(name, "bad", 3);
    prop_claim(name, sizeof(name), 4, 4);
}

/* The ADT in an allocation of exactly its size, so that a read past its end is caught */
static void adt_finish(void)
{
    CHECK(!depth);
    free(adt);
    adt = malloc(blen);
    memcpy(adt, bld, blen);
    adt_size = blen;
    blen = 0;
}

/* ---- ADT reader, with the boot loader's checks ---- */

static const u8 *A(int off)
{
    return (const u8 *)adt + off;
}

static u32 rd32(int off)
{
    u32 v;
    memcpy(&v, A(off), 4);
    return v;
}

u32 adt_get_size(void)
{
    return adt_size;
}

/* A node the boot loader accepts; anything else would be a panic there */
static void node_ok(int off)
{
    CHECK(off >= 0 && off % 4 == 0 && (u32)off + 8 <= adt_size);
    u32 pc = rd32(off), cc = rd32(off + 4);
    CHECK(pc >= 1 && pc <= 2048 && cc <= 2048);
}

static int prop_next(int poff)
{
    CHECK(poff % 4 == 0 && (u32)poff + 36 <= adt_size);
    return poff + 36 + ((rd32(poff + 32) + 3) & ~3u);
}

static int props_end(int off)
{
    int p = off + 8;
    node_ok(off);
    for (u32 i = 0; i < rd32(off); i++) {
        p = prop_next(p);
        CHECK((u32)p <= adt_size);
    }
    return p;
}

static int subtree_end(int off)
{
    int p = props_end(off);
    for (u32 i = 0; i < rd32(off + 4); i++)
        p = subtree_end(p);
    return p;
}

int adt_get_property_count(const void *a, int off)
{
    (void)a;
    node_ok(off);
    return rd32(off);
}

int adt_first_property_offset(const void *a, int off)
{
    (void)a;
    node_ok(off);
    return off + 8;
}

int adt_next_property_offset(const void *a, int poff)
{
    (void)a;
    return prop_next(poff);
}

int adt_get_child_count(const void *a, int off)
{
    (void)a;
    node_ok(off);
    return rd32(off + 4);
}

int adt_first_child_offset(const void *a, int off)
{
    (void)a;
    node_ok(off);
    CHECK(rd32(off + 4) > 0); /* the boot loader panics on a childless node */
    return props_end(off);
}

int adt_next_sibling_offset(const void *a, int off)
{
    (void)a;
    int n = subtree_end(off);
    node_ok(n); /* past the last node, the boot loader panics */
    return n;
}

const void *adt_getprop(const void *a, int node, const char *name, u32 *lenp)
{
    (void)a;
    int p = node + 8;
    node_ok(node);
    for (u32 i = 0; i < rd32(node); i++) {
        const char *pn = (const char *)A(p);
        if (strnlen(pn, 32) == strlen(name) && !memcmp(pn, name, strlen(name))) {
            if (lenp)
                *lenp = rd32(p + 32);
            return A(p + 36);
        }
        p = prop_next(p);
    }
    return NULL;
}

const char *adt_get_name(const void *a, int node)
{
    const char *n = adt_getprop(a, node, "name", NULL);
    CHECK(n);
    return n;
}

bool adt_is_compatible_at(const void *a, int node, const char *compat, size_t index)
{
    u32 len = 0;
    const char *c = adt_getprop(a, node, "compatible", &len);
    size_t i = 0;

    while (c && len) {
        size_t n = strnlen(c, len);
        if (i++ == index)
            return n == strlen(compat) && !memcmp(c, compat, n);
        if (n + 1 > len)
            break;
        c += n + 1;
        len -= n + 1;
    }
    return false;
}

bool adt_is_compatible(const void *a, int node, const char *compat)
{
    for (size_t i = 0; i < 8; i++)
        if (adt_is_compatible_at(a, node, compat, i))
            return true;
    return false;
}

/* Child lookup by name; a miss past the last child is an error, not a panic */
static int child_named(int node, const char *name, size_t n)
{
    u32 cc = rd32(node + 4);
    int c = props_end(node);
    for (u32 i = 0; i < cc; i++) {
        u32 len = 0;
        const char *cn = adt_getprop(adt, c, "name", &len);
        if (cn && strnlen(cn, len) == n && !memcmp(cn, name, n))
            return c;
        c = subtree_end(c);
    }
    return -1;
}

int adt_path_offset_trace(const void *a, const char *path, int *offsets)
{
    (void)a;
    int node = 0, d = 0;

    CHECK(path[0] == '/');
    for (const char *p = path + 1; *p;) {
        const char *e = strchr(p, '/');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        node = child_named(node, p, n);
        if (node < 0)
            return -1;
        CHECK(d < 8);
        if (offsets)
            offsets[d] = node;
        d++;
        p += n + (e ? 1 : 0);
    }
    if (offsets && d < 8)
        offsets[d] = 0;
    return node;
}

int adt_path_offset(const void *a, const char *path)
{
    return adt_path_offset_trace(a, path, NULL);
}

static u64 cells(const u8 *p, u32 n)
{
    u64 v = 0;
    for (u32 i = 0; i < n; i++) {
        u32 x;
        memcpy(&x, p + 4 * i, 4);
        v |= (u64)x << (32 * i);
    }
    return v;
}

static int ncells(int node, const char *name, u32 *v)
{
    u32 len = 0;
    const void *p = adt_getprop(adt, node, name, &len);
    if (!p || len != 4)
        return -1;
    memcpy(v, p, 4);
    return 0;
}

int adt_get_reg(const void *a, int *path, const char *pname, int idx, u64 *paddr, u64 *psize)
{
    (void)a;
    int n = 0;
    u32 ac, sc, len = 0;

    if (!path[0])
        return -1;
    while (path[n])
        n++;
    CHECK(n < 8); /* the boot loader indexes an 8-entry array */
    int cur = n - 1, node = path[cur], parent = cur ? path[cur - 1] : 0;
    if (ncells(parent, "#address-cells", &ac) || ncells(parent, "#size-cells", &sc) || ac < 1 ||
        ac > 2 || sc > 2)
        return -1;
    const u8 *reg = adt_getprop(adt, node, pname, &len);
    if (!reg || !len || len < (u32)(idx + 1) * (ac + sc) * 4)
        return -1;
    u64 addr = cells(reg + idx * (ac + sc) * 4, ac);
    u64 size = cells(reg + idx * (ac + sc) * 4 + ac * 4, sc);
    while (parent) {
        cur--;
        node = parent;
        parent = cur > 0 ? path[cur - 1] : 0;
        const u8 *ranges = adt_getprop(adt, node, "ranges", &len);
        if (!ranges)
            break;
        u32 pac;
        if (ncells(parent, "#address-cells", &pac) || pac < 1 || pac > 2)
            return -1;
        u32 entry = 4 * (pac + ac + sc);
        for (u32 o = 0; o + entry <= len; o += entry) {
            u64 ca = cells(ranges + o, ac), pa = cells(ranges + o + 4 * ac, pac);
            u64 cs = cells(ranges + o + 4 * (ac + pac), sc);
            if (addr >= ca && addr + size <= ca + cs) {
                addr = addr - ca + pa;
                break;
            }
        }
        if (ncells(parent, "#size-cells", &sc))
            return -1;
        ac = pac;
    }
    if (paddr)
        *paddr = addr;
    if (psize)
        *psize = size;
    return 0;
}

/* ---- the synthetic T6030 ADT ---- */

#define ARMIO_BASE 0x200000000ULL

static void reg_entries(int n, const u64 *phys, const u64 *size)
{
    u32 v[64];
    for (int i = 0; i < n; i++) {
        u64 c = phys[i] - ARMIO_BASE;
        v[4 * i] = (u32)c;
        v[4 * i + 1] = (u32)(c >> 32);
        v[4 * i + 2] = (u32)size[i];
        v[4 * i + 3] = (u32)(size[i] >> 32);
    }
    prop("reg", v, n * 16);
}

static void simple(const char *name, const char *compat, u64 phys, u64 size, u32 irq)
{
    node_begin(name);
    prop_str("compatible", compat);
    reg_entries(1, &phys, &size);
    prop_u32s("interrupts", 1, irq);
    node_end();
}

static const char *const common_tunables[] = {
    "tunable_ATC0AXI2AF",  "tunable_ATC_FABRIC",         "tunable_CIO3PLL_CORE",
    "tunable_CIO3PLL_TOP", "tunable_ACIOPHY_LANE_USBC0", "tunable_ACIOPHY_PLL_TOP",
    "tunable_ACIOPHY_TOP", "tunable_AUSCMN_DIG",         "tunable_AUSPLL_CORE",
    "tunable_AUX_TOP",     "tunable_AUSCMN_SHM",         "tunable_CLKMON_CFG",
    "tunable_CIO_SHIM",
};

static const char *const lane_tunables[2][11] = {
    {"tunable_LN0_RX_TOP_USB_DFLT", "tunable_LN0_RX_TOP_USB_EQA", "tunable_LN0_RX_EQ_USB_EQA",
     "tunable_LN0_RX_SHM_USB_DFLT", "tunable_LN0_TX_TOP_USB_DFLT", "tunable_LN0_TX_SHM_USB_DFLT",
     "tunable_LN0_RX_TOP_CIO_DFLT", "tunable_LN0_RX_EQ_CIO_EQA", "tunable_LN0_RX_SHM_CIO_DFLT",
     "tunable_LN0_TX_TOP_CIO_DFLT", "tunable_LN0_TX_SHM_CIO_DFLT"},
    {"tunable_LN1_RX_TOP_USB_DFLT", "tunable_LN1_RX_TOP_USB_EQA", "tunable_LN1_RX_EQ_USB_EQA",
     "tunable_LN1_RX_SHM_USB_DFLT", "tunable_LN1_TX_TOP_USB_DFLT", "tunable_LN1_TX_SHM_USB_DFLT",
     "tunable_LN1_RX_TOP_CIO_DFLT", "tunable_LN1_RX_EQ_CIO_EQA", "tunable_LN1_RX_SHM_CIO_DFLT",
     "tunable_LN1_TX_TOP_CIO_DFLT", "tunable_LN1_TX_SHM_CIO_DFLT"},
};

/* One ATC tunable entry: offset 8, 32 bits, mask and value derived from the name */
static void tunable(const char *name, u32 salt)
{
    u32 v[3] = {8 | (32u << 24), 0xff00ff00u ^ salt, 0x12345678u ^ salt};
    prop(name, v, sizeof(v));
}

struct shape {
    bool atc3;            /* atc-phy3 present */
    bool atc3_lanes[2];   /* its lane 0 / lane 1 tunables */
    bool atc3_cio_only0;  /* lane 0: only the CIO tunables */
    bool atc3_no_aux_top; /* a required common entry missing */
    bool ext1;            /* dcpext1, dispext1 and their DARTs */
    bool finds;           /* crossbar, hdmi and dptx nodes */
    int extra_xbars;      /* more crossbar nodes */
    int many_props;       /* this many extra properties on every PHY */
    bool huge_reg;        /* dcpext0 reg over 1 KiB */
    bool no_armio;
    bool bad_names; /* properties with unreadable names before the ones that are looked up */
    int malformed;  /* a malformed /arm-io node after dart-dispext0: MAL_* */
    int nest;       /* a chain of this many nested nodes below /arm-io, before dcpext1 */
    bool dense;     /* two hdmi nodes with 500 small values with long names each */
};

enum {
    MAL_NONE,
    MAL_SIZE,      /* a property size past the end of the ADT */
    MAL_SIZE_FLAG, /* a property size with its top bit set */
    MAL_NOPROPS,   /* a property count of 0 */
    MAL_KIDS,      /* a child count over 2048 */
    MAL_NR,
};

static void broken_node(int kind)
{
    node_begin("broken");
    switch (kind) {
        case MAL_SIZE:
            prop_claim("x", 1, 0xfff00, 4);
            break;
        case MAL_SIZE_FLAG:
            prop_claim("x", 1, 0x80000004u, 4);
            break;
        case MAL_NOPROPS:
            *b_u32(open_nodes[depth - 1].hdr) = 0;
            break;
        case MAL_KIDS:
            *b_u32(open_nodes[depth - 1].hdr + 4) = 3000;
            break;
    }
    node_end();
}

static void nested(int n)
{
    char name[16];

    if (!n)
        return;
    snprintf(name, sizeof(name), "nest%d", n);
    node_begin(name);
    nested(n - 1);
    node_end();
}

static void atc_phy(int i, const struct shape *s)
{
    static const u64 base[4] = {0x703000000ULL, 0xb03000000ULL, 0xf03000000ULL, 0x1303000000ULL};
    char name[16];
    u64 phys[5], size[5] = {0x4c000, 0x8000, 0x8000, 0x4000, 0x4000};

    snprintf(name, sizeof(name), "atc-phy%d", i);
    node_begin(name);
    prop("compatible", "atc-phy,t8122\0atc-phy", 22);
    phys[0] = base[i];
    phys[1] = base[i] + 0x50000;
    phys[2] = base[i] - 0x3000000;
    phys[3] = base[i] - 0x570000;
    phys[4] = base[i] - 0x57c000;
    reg_entries(5, phys, size);
    prop_u32s("interrupts", 2, 1000 + i, 1001 + i);
    for (size_t k = 0; k < ARRAY_SIZE(common_tunables); k++) {
        if (i == 3 && s->atc3_no_aux_top && !strcmp(common_tunables[k], "tunable_AUX_TOP"))
            continue;
        tunable(common_tunables[k], i * 100 + k);
    }
    for (int l = 0; l < 2; l++)
        for (int k = 0; k < 11; k++) {
            bool want = i != 3 || s->atc3_lanes[l];
            if (i == 3 && l == 0 && s->atc3_cio_only0)
                want = k >= 6;
            if (want && !strstr(lane_tunables[l][k], "TOP_USB_EQA"))
                tunable(lane_tunables[l][k], i * 100 + 50 + l * 11 + k);
        }
    if (i == 3) {
        prop_fill("tunable_DP_LN0_TX", 24, 0x11);
        prop_fill("dp-big-table", 300, 0x22);
        prop_fill("hdmi-mode", 4, 0x33);
        prop_fill("aux-config", 8, 0x44);
        prop_fill("unrelated", 8, 0x55);
    }
    if (i == 1)
        prop_fill("abcdefghijklmnopqrstuvwxyz012345", 4, 0x66); /* 32 characters */
    for (int k = 0; k < s->many_props; k++) {
        char pn[40];
        snprintf(pn, sizeof(pn), "padding-property-number-%07d", k);
        prop_fill(pn, 4, 0);
    }
    if (i == 2 && s->finds) {
        simple("dp-crossbar2", "dp-xbar,t8122", base[i] + 0x4c000, 0x4000, 7);
    }
    node_end();
}

static void build_adt(const struct shape *s)
{
    node_begin("device-tree");
    prop_u32s("#address-cells", 1, 2);
    prop_u32s("#size-cells", 1, 2);
    prop_str("compatible", "J516sAP");
    node_begin("chosen");
    prop_u32s("board-id", 1, 0x28);
    node_end();
    if (!s->no_armio) {
        node_begin("arm-io");
        prop_u32s("#address-cells", 1, 2);
        prop_u32s("#size-cells", 1, 2);
        prop_u32s("ranges", 6, 0, 0, (u32)ARMIO_BASE, (u32)(ARMIO_BASE >> 32), 0, 0x100);
        if (s->bad_names) {
            /* Before its name: the library's lookup of any later /arm-io node reads it first */
            u64 p = 0x200004000ULL, z = 0x1000;
            node_open();
            prop_bad_name();
            prop_str("name", "xbar-bad");
            reg_entries(1, &p, &z);
            node_end();
        }
        for (int i = 0; i < 4; i++)
            if (i < 3 || s->atc3)
                atc_phy(i, s);
        if (s->huge_reg) {
            node_begin("dcpext0");
            prop_str("compatible", "dcpext,t6030");
            prop_fill("reg", 2000, 1);
            node_end();
        } else if (s->bad_names) {
            u64 p = 0x2d2c00000ULL, z = 0x4000;
            node_begin("dcpext0");
            prop_bad_name();
            prop_str("compatible", "dcpext,t6030");
            reg_entries(1, &p, &z);
            prop_u32s("interrupts", 1, 634);
            node_end();
        } else {
            simple("dcpext0", "dcpext,t6030", 0x2d2c00000ULL, 0x4000, 634);
        }
        simple("dispext0", "dispext,t6030", 0x2d2000000ULL, 0x100000, 640);
        simple("dart-dcpext0", "dart,t8110", 0x2d130c000ULL, 0x4000, 651);
        node_begin("dart-dispext0");
        prop_str("compatible", "dart,t8110");
        u64 p2[2] = {0x2d1304000ULL, 0x2d1300000ULL}, z2[2] = {0x4000, 0x4000};
        reg_entries(2, p2, z2);
        prop_u32s("interrupts", 1, 651);
        node_end();
        if (s->malformed)
            broken_node(s->malformed);
        nested(s->nest);
        if (s->ext1) {
            simple("dcpext1", "dcpext,t6030", 0x2d6c00000ULL, 0x4000, 672);
            simple("dispext1", "dispext,t6030", 0x2d6000000ULL, 0x100000, 678);
            simple("dart-dcpext1", "dart,t8110", 0x2d530c000ULL, 0x4000, 689);
            simple("dart-dispext1", "dart,t8110", 0x2d5304000ULL, 0x4000, 689);
        }
        if (s->finds) {
            node_begin("dp2hdmi-gpio0");
            prop_str("compatible", "dp2hdmi,gpio");
            prop_fill("function-hdmi_pwr_en", 16, 0x0f);
            prop_fill("function-dp2hdmi_pwr_en", 16, 0x1a);
            prop_fill("function-hpd", 16, 0x2f);
            prop_fill("big-table", 600, 0x77);
            for (int k = 0; s->dense && k < 500; k++) {
                char pn[40];
                snprintf(pn, sizeof(pn), "function-hdmi-padding-%09d", k);
                prop_fill(pn, 4, k);
            }
            node_end();
            if (s->dense) {
                node_begin("dp2hdmi-gpio1");
                for (int k = 0; k < 500; k++) {
                    char pn[40];
                    snprintf(pn, sizeof(pn), "function-hdmi-padding-%09d", k);
                    prop_fill(pn, 4, k);
                }
                node_end();
            }
            simple("lpdptx-phy0", "lpdptx-phy,t6030", 0x2d4000000ULL, 0x8000, 700);
            simple("atc3-dpxbar", "dp-xbar,t8122", 0x130304c000ULL, 0x4000, 9);
            simple("weird@1-xbar", "x", 0x200000000ULL, 0x1000, 1); /* not a usable node name */
            /* Four levels below /arm-io is found, five is not */
            node_begin("l1");
            node_begin("l2");
            node_begin("l3");
            simple("l4-xbar", "x", 0x200001000ULL, 0x1000, 2);
            node_begin("l4");
            simple("l5-xbar", "x", 0x200002000ULL, 0x1000, 3);
            node_end();
            node_end();
            node_end();
            node_end();
        }
        for (int k = 0; k < s->extra_xbars; k++) {
            char n[16];
            snprintf(n, sizeof(n), "xbar%d", k);
            simple(n, "x", 0x200003000ULL + 0x1000 * k, 0x1000, 4);
        }
        /* The last node of the ADT: a walk must not look past it */
        node_begin("zz-last");
        prop_u32s("x", 1, 1);
        node_end();
        node_end();
    }
    node_end();
    adt_finish();
}

/* ---- FDTs ---- */

static void *make_fdt(int size, bool phys_aliases)
{
    void *fdt = calloc(1, size);
    CHECK(!fdt_create_empty_tree(fdt, size));
    int root = 0;
    CHECK(!fdt_setprop_string(fdt, root, "compatible", "apple,j516s"));
    CHECK(fdt_add_subnode(fdt, root, "chosen") >= 0);
    int c = fdt_path_offset(fdt, "/chosen");
    CHECK(!fdt_setprop_string(fdt, c, "bootargs", "x"));
    if (phys_aliases) {
        static const char *const p[] = {"/soc/phy@703000000", "/soc/phy@b03000000",
                                        "/soc/phy@f03000000", "/soc/phy@1303000000"};
        CHECK(fdt_add_subnode(fdt, root, "soc") >= 0);
        CHECK(fdt_add_subnode(fdt, root, "aliases") >= 0);
        for (int i = 0; i < 4; i++) {
            char a[16];
            int soc = fdt_path_offset(fdt, "/soc");
            CHECK(fdt_add_subnode(fdt, soc, p[i] + 5) >= 0);
            snprintf(a, sizeof(a), "atcphy%d", i);
            CHECK(!fdt_setprop_string(fdt, fdt_path_offset(fdt, "/aliases"), a, p[i]));
        }
    }
    return fdt;
}

static int totalsize(const void *fdt)
{
    return fdt_totalsize(fdt);
}

static const void *getp(void *fdt, const char *path, const char *name, int *len)
{
    int node = fdt_path_offset(fdt, path);
    if (node < 0)
        return NULL;
    return fdt_getprop(fdt, node, name, len);
}

static bool has_str(void *fdt, const char *path, const char *name, const char *want)
{
    int len;
    const char *v = getp(fdt, path, name, &len);
    for (int i = 0; v && i < len; i += strlen(v + i) + 1)
        if (strstr(v + i, want))
            return true;
    return false;
}

static int count_strs(void *fdt, const char *path, const char *name)
{
    int len, n = 0;
    const char *v = getp(fdt, path, name, &len);
    for (int i = 0; v && i < len; i += strlen(v + i) + 1)
        n++;
    return n;
}

/* FNV-1a over a node's properties and subnodes, in order */
static u64 digest(void *fdt, int node, u64 h)
{
    int p, sub;
    fdt_for_each_property_offset(p, fdt, node)
    {
        const char *name;
        int len;
        const u8 *v = fdt_getprop_by_offset(fdt, p, &name, &len);
        for (const char *c = name; *c; c++)
            h = (h ^ (u8)*c) * 0x100000001b3ULL;
        for (int i = 0; i < len; i++)
            h = (h ^ v[i]) * 0x100000001b3ULL;
        h = (h ^ (u32)len) * 0x100000001b3ULL;
    }
    fdt_for_each_subnode(sub, fdt, node)
    {
        const char *n = fdt_get_name(fdt, sub, NULL);
        for (const char *c = n; *c; c++)
            h = (h ^ (u8)*c) * 0x100000001b3ULL;
        h = digest(fdt, sub, h);
    }
    return h;
}

static void reset_log(void)
{
    loglen = 0;
    logbuf[0] = 0;
}

/* ---- ATC scenarios ---- */

static const char *const phy_paths[] = {"/soc/phy@703000000", "/soc/phy@b03000000",
                                        "/soc/phy@f03000000", "/soc/phy@1303000000"};

static void *run_atc(u32 chip, const struct shape *s)
{
    chip_id = chip;
    build_adt(s);
    void *fdt = make_fdt(256 * 1024, true);
    reset_log();
    CHECK(!kboot_setup_atc(fdt));
    CHECK(!fdt_check_header(fdt));
    return fdt;
}

static const struct {
    const char *name;
    u32 chip;
    struct shape s;
} atc_scenarios[] = {
    {"t6030-lanes", T6030, {.atc3 = true, .atc3_lanes = {true, true}}},
    {"t6030-nolanes", T6030, {.atc3 = true}},
    {"t6030-lane1", T6030, {.atc3 = true, .atc3_lanes = {false, true}}},
    {"t6030-cio0", T6030, {.atc3 = true, .atc3_lanes = {true, false}, .atc3_cio_only0 = true}},
    {"t6030-nocommon", T6030, {.atc3 = true, .atc3_no_aux_top = true}},
    {"t8122-nolanes", T8122, {.atc3 = true}},
    {"t6020-nolanes", T6020, {.atc3 = true}},
};

static void atc_print(const char *name)
{
    for (size_t i = 0; i < ARRAY_SIZE(atc_scenarios); i++) {
        if (strcmp(atc_scenarios[i].name, name))
            continue;
        void *fdt = run_atc(atc_scenarios[i].chip, &atc_scenarios[i].s);
        for (int p = 0; p < 4; p++)
            fprintf(stdout, "%s phy%d %016llx\n", name, p,
                    (unsigned long long)digest(fdt, fdt_path_offset(fdt, phy_paths[p]),
                                               0xcbf29ce484222325ULL));
        fprintf(stdout, "%s log %s", name, logbuf);
        free(fdt);
        return;
    }
    fprintf(stderr, "no ATC scenario %s\n", name);
    exit(2);
}

static void atc_checks(void)
{
    int len;
    void *fdt;

    /* T6030 atc-phy3 without lane tunables: the common ones, no lane ones, one log line */
    fdt = run_atc(T6030, &(struct shape){.atc3 = true});
    const char *p3 = phy_paths[3];
    CHECK(getp(fdt, p3, "apple,tunable-common-b", &len) && len == 12 * 12);
    CHECK(getp(fdt, p3, "apple,tunable-common", &len) && len == 12 * 12);
    CHECK(getp(fdt, p3, "apple,tunable-axi2af", &len) && len == 12);
    CHECK(getp(fdt, p3, "apple,tunable-common-a", &len) && len == 0);
    for (int l = 0; l < 2; l++) {
        char n[32];
        snprintf(n, sizeof(n), "apple,tunable-lane%d-usb", l);
        CHECK(!getp(fdt, p3, n, &len));
        snprintf(n, sizeof(n), "apple,tunable-lane%d-cio", l);
        CHECK(!getp(fdt, p3, n, &len));
    }
    CHECK(strstr(logbuf, "kboot: /arm-io/atc-phy3: 22 optional lane tunables absent "
                         "(tunable_LN0_RX_TOP_USB_DFLT;"));
    CHECK(strstr(logbuf, ";...); common tunables passed\n"));
    CHECK(!strstr(logbuf, "Unable to setup ATC tunables"));
    /* The other PHYs keep every lane tunable, with the CDR lock entry on CIO */
    CHECK(getp(fdt, phy_paths[0], "apple,tunable-lane1-cio", &len) && len == 6 * 12);
    free(fdt);

    /* Lane 1 only: lane 0 gets nothing, lane 1 everything, including the CDR lock entry */
    fdt = run_atc(T6030, &(struct shape){.atc3 = true, .atc3_lanes = {false, true}});
    CHECK(!getp(fdt, p3, "apple,tunable-lane0-cio", &len));
    CHECK(getp(fdt, p3, "apple,tunable-lane1-cio", &len) && len == 6 * 12);
    CHECK(getp(fdt, p3, "apple,tunable-lane1-usb", &len) && len == 5 * 12);
    /* lane 0 entirely, and lane 1 RX_TOP_USB_EQA, which this ADT never has */
    CHECK(strstr(logbuf, "12 optional lane tunables absent"));
    free(fdt);

    /* Lane 0 with only its CIO tunables */
    fdt = run_atc(
        T6030, &(struct shape){.atc3 = true, .atc3_lanes = {true, false}, .atc3_cio_only0 = true});
    CHECK(getp(fdt, p3, "apple,tunable-lane0-cio", &len) && len == 6 * 12);
    CHECK(!getp(fdt, p3, "apple,tunable-lane0-usb", &len));
    free(fdt);

    /* A missing common entry still drops all of them */
    fdt = run_atc(T6030, &(struct shape){.atc3 = true, .atc3_no_aux_top = true});
    CHECK(!getp(fdt, p3, "apple,tunable-common-b", &len));
    CHECK(strstr(logbuf, "Unable to setup ATC tunables for /arm-io/atc-phy3"));
    free(fdt);

    /* Not T6030: as before, a missing lane entry drops all of them */
    fdt = run_atc(T8122, &(struct shape){.atc3 = true});
    CHECK(!getp(fdt, p3, "apple,tunable-common-b", &len));
    CHECK(!strstr(logbuf, "optional lane tunables absent"));
    free(fdt);
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "atc")) {
        atc_print(argv[2]);
        return 0;
    }
    if (argc != 2 || strcmp(argv[1], "checks"))
        return 2;
    atc_checks();
    free(adt);
    free(bld);
    fprintf(stdout, "t6030 atc-phy3 host test: all checks passed\n");
    return 0;
}
