# SPDX-License-Identifier: MIT
"""The stock-SPTM AMCC shadow reads /chosen/lock-regs/amcc on every chip.

The reader and the shadow are the production source, compiled against a
modeled ADT and MCC driver: a well-formed node of any geometry is admitted,
and a missing, malformed or MCC-inconsistent one is refused.
"""
from test_smp_timeouts import REPO, compile_and_run


def reader_source():
    source = (REPO / "src/sptm_virt_s2.c").read_text()
    start = source.index("static u64 amcc_lock_shadow[")
    end = source.index("/* sptm_virt_amcc_nolock is a mask")
    return source[start:end]


def test_no_chip_or_aperture_numbers_in_reader():
    source = reader_source()
    for token in ("T8122", "0x220000000", "0x222000000", "0x690", "0x1c00", "25G83", "J613"):
        assert token not in source
    full = (REPO / "src/sptm_virt_s2.c").read_text()
    assert "return amcc_uses_plane_offsets() || amcc_lockregs_parse(&lr);" in full
    assert 'panic("SPTM-virt[amcc]: lock-regs shadow setup failed' in full


HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint16_t u16;
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#pragma GCC diagnostic ignored "-Wunused-variable"
static const void *adt = (const void *)1;

/* A modeled ADT: node 1 is /chosen/lock-regs/amcc, nodes 2.. its children. */
struct prop { int node; const char *name; u32 len; unsigned char data[32]; };
static struct prop props[64];
static int nprops, nchildren, have_node;
static int adt_path_offset(const void *t, const char *path)
{
    (void)t;
    return have_node && !strcmp(path, "/chosen/lock-regs/amcc") ? 1 : -1;
}
static const void *adt_getprop(const void *t, int node, const char *name, u32 *len)
{
    (void)t;
    for (int i = 0; i < nprops; i++)
        if (props[i].node == node && !strcmp(props[i].name, name)) {
            if (len)
                *len = props[i].len;
            return props[i].data;
        }
    return NULL;
}
static int adt_get_child_count(const void *t, int node) { (void)t; return node == 1 ? nchildren : 0; }
static int adt_first_child_offset(const void *t, int node) { (void)t; (void)node; return 2; }
static int adt_next_sibling_offset(const void *t, int node) { (void)t; return node + 1; }
#define ADT_FOREACH_CHILD(adt, node)                                                     \
    for (int _child_count = adt_get_child_count(adt, node); _child_count; _child_count = 0) \
        for (node = adt_first_child_offset(adt, node); _child_count--;                   \
             node = adt_next_sibling_offset(adt, node))

static u64 mcc_base[4], mcc_stride;
static int mcc_n, mcc_planes;
static int mcc_plane_info(int mcc, u64 *base, u64 *stride, int *count)
{
    if (mcc < 0 || mcc >= mcc_n)
        return -1;
    *base = mcc_base[mcc];
    *stride = mcc_stride;
    *count = mcc_planes;
    return 0;
}

static void del(int node, const char *name)
{
    for (int i = 0; i < nprops; i++)
        if (props[i].node == node && !strcmp(props[i].name, name))
            props[i] = props[--nprops];
}
static void set(int node, const char *name, const void *data, u32 len)
{
    del(node, name);
    props[nprops].node = node;
    props[nprops].name = name;
    props[nprops].len = len;
    memcpy(props[nprops].data, data, len);
    nprops++;
}
static void set32(int node, const char *name, u32 v) { set(node, name, &v, 4); }
static void bank(int node, u32 first)
{
    static const char *const names[] = {"lower-limit-reg-offset", "upper-limit-reg-offset",
        "enable-reg-offset", "lock-reg-offset", "write-disable-reg-offset"};
    for (int i = 0; i < 5; i++)
        set32(node, names[i], first + 4 * i);
    set32(node, "lock-reg-mask", 1); /* unrelated properties are ignored */
}
/* Geometry A: two apertures of two planes, one bank, a cache status. */
static void geometry_a(void)
{
    const u64 ap[2] = {0x200000000ULL, 0x204000000ULL};
    nprops = 0;
    have_node = 1;
    nchildren = 1;
    set(1, "aperture-phys-addr", ap, sizeof(ap));
    set32(1, "aperture-count", 2);
    set32(1, "aperture-size", 0x200000);
    set32(1, "plane-count", 2);
    set32(1, "plane-stride", 0x40000);
    set32(1, "cache-status-reg-offset", 0x1800);
    set32(1, "cache-status-reg-mask", 1);
    set32(1, "cache-status-reg-value", 0);
    bank(2, 0x600);
    mcc_n = 2;
    mcc_base[0] = ap[0];
    mcc_base[1] = ap[1];
    mcc_stride = 0x40000;
    mcc_planes = 2;
}
'''


def test_lockregs_reader(tmp_path):
    harness = HARNESS + reader_source() + r'''
int main(void)
{
    struct amcc_lockregs lr;

    geometry_a();
    assert(amcc_lockregs_parse(&lr));
    assert(lr.napertures == 2 && lr.nplanes == 2 && lr.nbanks == 1);
    assert(lr.aperture[1] == 0x204000000ULL && lr.plane_stride == 0x40000);
    assert(lr.bank_reg[0][0] == 0x600 && lr.bank_reg[0][4] == 0x610);
    assert(lr.has_cache_status && lr.cache_status == 0x1800 && lr.cache_status_mask == 1);
    /* 8-byte counts are read as mcc_get_count() reads them. */
    { u64 v = 2; set(1, "plane-count", &v, 8); }
    assert(amcc_lockregs_parse(&lr) && lr.nplanes == 2);

    /* Geometry B: one aperture of four planes, two banks, no cache status. */
    {
        const u64 ap[1] = {0x300000000ULL};
        geometry_a();
        set(1, "aperture-phys-addr", ap, sizeof(ap));
        set32(1, "aperture-count", 1);
        set32(1, "plane-count", 4);
        set32(1, "plane-stride", 0x8000);
        del(1, "cache-status-reg-offset");
        del(1, "cache-status-reg-mask");
        del(1, "cache-status-reg-value");
        nchildren = 2;
        bank(3, 0x700);
        mcc_n = 1;
        mcc_base[0] = ap[0];
        mcc_stride = 0x8000;
        mcc_planes = 4;
        assert(amcc_lockregs_parse(&lr));
        assert(lr.nbanks == 2 && lr.nplanes == 4 && !lr.has_cache_status);
        amcc_lr = lr;
        assert(amcc_lr_is_shadow(0x300008000ULL + 0x70c));
        assert(!amcc_lr_is_shadow(0x300008000ULL + 0x614));
    }

    /* Refusals. Each starts from the admitted geometry A. */
    geometry_a(); have_node = 0; assert(!amcc_lockregs_parse(&lr));
    geometry_a(); set32(1, "aperture-count", 3); assert(!amcc_lockregs_parse(&lr));
    geometry_a(); del(1, "aperture-size"); assert(!amcc_lockregs_parse(&lr));
    geometry_a(); set(1, "aperture-phys-addr", "\0\0\0", 3); assert(!amcc_lockregs_parse(&lr));
    geometry_a(); set32(1, "plane-stride", 0x41000); assert(!amcc_lockregs_parse(&lr));
    geometry_a(); set32(1, "plane-count", 0); assert(!amcc_lockregs_parse(&lr));
    geometry_a(); set32(1, "plane-stride", 0x200000); assert(!amcc_lockregs_parse(&lr));
    geometry_a(); { u64 v = 1ULL << 32; set(1, "plane-count", &v, 8); }
    assert(!amcc_lockregs_parse(&lr));
    geometry_a(); { u16 v = 2; set(1, "plane-count", &v, 2); } assert(!amcc_lockregs_parse(&lr));
    geometry_a(); del(1, "cache-status-reg-value"); assert(!amcc_lockregs_parse(&lr));
    geometry_a(); set32(1, "cache-status-reg-value", 2); assert(!amcc_lockregs_parse(&lr));
    geometry_a(); del(2, "lock-reg-offset"); assert(!amcc_lockregs_parse(&lr));
    geometry_a(); set32(2, "lock-reg-offset", 0x4000); assert(!amcc_lockregs_parse(&lr));
    geometry_a(); set32(2, "lock-reg-offset", 0x602); assert(!amcc_lockregs_parse(&lr));
    geometry_a(); set32(2, "lock-reg-offset", 0x600); assert(!amcc_lockregs_parse(&lr));
    geometry_a(); set32(2, "lock-reg-offset", 0x1800); assert(!amcc_lockregs_parse(&lr));
    geometry_a(); nchildren = 0; assert(!amcc_lockregs_parse(&lr));
    geometry_a(); nchildren = 2; bank(3, 0x600); assert(!amcc_lockregs_parse(&lr));
    geometry_a(); nchildren = 5; for (int n = 2; n < 7; n++) bank(n, 0x100 * n);
    assert(!amcc_lockregs_parse(&lr));
    geometry_a(); mcc_base[1] += 0x4000; assert(!amcc_lockregs_parse(&lr));
    geometry_a(); mcc_planes = 4; assert(!amcc_lockregs_parse(&lr));
    geometry_a(); mcc_stride = 0x80000; assert(!amcc_lockregs_parse(&lr));
    geometry_a(); mcc_n = 1; assert(!amcc_lockregs_parse(&lr));
    geometry_a(); mcc_n = 3; assert(!amcc_lockregs_parse(&lr));
    return 0;
}
'''
    compile_and_run(tmp_path, "amcc-lockregs", harness)


def test_lockregs_shadow(tmp_path):
    harness = HARNESS + reader_source() + r'''
int main(void)
{
    geometry_a();
    assert(amcc_lockregs_parse(&amcc_lr));
    const u64 pages[] = {0x200000000ULL, 0x200040000ULL, 0x204000000ULL, 0x204040000ULL};
    for (unsigned p = 0; p < ARRAY_SIZE(pages); p++) {
        for (u32 r = 0; r < 5; r++) {
            u64 pa = pages[p] + 0x600 + 4 * r, value = 0xfeed0000 + p * 16 + r;
            assert(amcc_lr_is_shadow(pa));
            assert(amcc_shadow_rw(pa, &value, true));
            value = ~0ULL;
            assert(amcc_shadow_rw(pa, &value, false));
            assert(value == 0xfeed0000 + p * 16 + r);
        }
    }
    /* A bank register the guest reads before writing starts at zero (unlocked). */
    u64 fresh = ~0ULL;
    assert(amcc_shadow_rw(0x208000600ULL, &fresh, false) && fresh == 0);
    assert(!amcc_lr_is_shadow(0x200001800ULL));
    assert(!amcc_lr_is_shadow(0x200000614ULL));
    return 0;
}
'''
    compile_and_run(tmp_path, "amcc-lockregs-shadow", harness)
