# SPDX-License-Identifier: MIT
"""Run the production MCC initializers against ADTs whose count properties are 4 or 8 bytes,
missing, or malformed.

The whole of src/mcc.c is compiled against a fake ADT whose adt_getprop_copy() keeps the
production contract: the property length on success, -1 for a missing property and -20 for a
size mismatch. The M4 Mac mini (J773g) ADT stores amcc_aperture_count in 4 bytes; aura read it
as a u64, failed, and (with an mcc_init() failure fatal on every chip) panicked at boot.
"""

import re
import subprocess
from pathlib import Path

from host_compiler import host_cc

ROOT = Path(__file__).resolve().parents[2]

PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define BIT(x)                 (1UL << (x))
#define GENMASK(msb, lsb)      ((BIT((msb + 1) - (lsb)) - 1) << (lsb))
#define _FIELD_LSB(field)      ((field) & ~(field - 1))
#define FIELD_PREP(field, val) (((val) * (_FIELD_LSB(field))) & (field))
#define ARRAY_SIZE(s)          (sizeof(s) / sizeof((s)[0]))
#define REGION_RWX_EL0 0x80000000000
#define REGION_RW_EL0  0xa0000000000
#define REGION_RX_EL1  0xc0000000000

#define T6000 0x6000
#define T8103 0x8103
#define T8112 0x8112
#define T6032 0x6032
#define T6040 0x6040
#define T6050 0x6050
#define T8122 0x8122
#define T8132 0x8132
#define T8142 0x8142

static u32 chip_id, board_id;
static u64 ram_base = 0x10000000000ULL, mem_size_actual = 0x200000000ULL;
static u64 get_page_size(void) { return 0x4000; }
static int rm_calls;
static void mmu_rm_mapping(u64 from, u64 size) { (void)from; (void)size; rm_calls++; }

/* MMIO: TZ registers read back what a test put there; cache writes are counted. */
static struct { u64 addr; u32 val; } mmio[16];
static int nmmio, writes, mmio_reads;
static void set_mmio(u64 addr, u32 val) { mmio[nmmio].addr = addr; mmio[nmmio++].val = val; }
static u32 read32(u64 addr)
{
    mmio_reads++;
    for (int i = 0; i < nmmio; i++)
        if (mmio[i].addr == addr)
            return mmio[i].val;
    return 0;
}
static void write32(u64 addr, u32 val) { (void)addr; (void)val; writes++; }
static int poll32(u64 addr, u32 mask, u32 target, u32 timeout)
{
    (void)addr; (void)mask; (void)target; (void)timeout;
    return 0;
}

/* The fake ADT: /arm-io/mcc, /chosen/carveout-memory-map, and a flat "reg" list. */
#define MCC_NODE   10
#define CARVE_NODE 20
struct adt_property {
    char name[32];
    u32 size;
    u8 value[];
};
static void *adt = (void *)1;
static struct { int node; const char *name; u32 len; _Alignas(8) u8 data[16 * 32]; } props[32];
static int nprops, reg_entries;
static bool have_mcc, have_carve;
static const char *mcc_compat;
#define REG_BASE(i) (0x200000000ULL + (u64)(i) * 0x1000000)

static int find(int node, const char *name)
{
    for (int i = 0; i < nprops; i++)
        if (props[i].node == node && !strcmp(props[i].name, name))
            return i;
    return -1;
}
static void set_prop(int node, const char *name, const void *data, u32 len)
{
    int i = find(node, name);
    if (i < 0)
        i = nprops++;
    assert(len <= sizeof(props[i].data));
    props[i].node = node;
    props[i].name = name;
    props[i].len = len;
    /* Bytes past the property are not zero, so reading more than it holds shows. */
    memset(props[i].data, 0xa5, sizeof(props[i].data));
    memcpy(props[i].data, data, len);
}
static void del_prop(int node, const char *name)
{
    int i = find(node, name);
    if (i >= 0)
        props[i] = props[--nprops];
}
/* A little-endian count of len bytes (bytes past 8 stay 0xa5). */
static void set_count(int node, const char *name, u64 value, u32 len)
{
    u8 bytes[16];
    memset(bytes, 0xa5, sizeof(bytes));
    for (u32 i = 0; i < len && i < 8; i++)
        bytes[i] = (u8)(value >> (8 * i));
    set_prop(node, name, bytes, len);
}
static const void *adt_getprop(const void *a, int node, const char *name, u32 *len)
{
    (void)a;
    int i = find(node, name);
    if (i < 0)
        return NULL;
    if (len)
        *len = props[i].len;
    return props[i].data;
}
static int adt_getprop_copy(const void *a, int node, const char *name, void *out, size_t len)
{
    (void)a;
    if (node < 0)
        return -4;
    int i = find(node, name);
    if (i < 0)
        return -1;
    if (props[i].len != len)
        return -20;
    memcpy(out, props[i].data, len);
    return (int)len;
}
#define ADT_GETPROP(adt, nodeoffset, name, val)                                                    \
    adt_getprop_copy(adt, nodeoffset, name, (val), sizeof(*(val)))
static int adt_path_offset(const void *a, const char *path)
{
    (void)a;
    if (!strcmp(path, "/arm-io/mcc"))
        return have_mcc ? MCC_NODE : -1;
    if (!strcmp(path, "/chosen/carveout-memory-map"))
        return have_carve ? CARVE_NODE : -1;
    return -1;
}
static int adt_path_offset_trace(const void *a, const char *path, int *trace)
{
    trace[0] = 0;
    return adt_path_offset(a, path);
}
static bool adt_is_compatible(const void *a, int node, const char *compat)
{
    (void)a;
    return node == MCC_NODE && mcc_compat && !strcmp(mcc_compat, compat);
}
static _Alignas(8) char compat_buf[96];
static const struct adt_property *adt_get_property(const void *a, int node, const char *name)
{
    (void)a; (void)node; (void)name;
    struct adt_property *p = (struct adt_property *)compat_buf;
    strcpy((char *)p->value, mcc_compat);
    return p;
}
static int adt_get_reg(const void *a, int *path, const char *prop, int idx, u64 *addr, u64 *size)
{
    (void)a; (void)path; (void)prop;
    if (idx < 0 || idx >= reg_entries)
        return -1;
    *addr = REG_BASE(idx);
    if (size)
        *size = 0x1000000;
    return 0;
}
'''

CASES = r'''
#define ADT_TZ4_START (0x10000000000ULL + 0x10000000)
#define ADT_TZ4_SIZE  0x4000000ULL
#define ADT_TZ2_START (0x10000000000ULL + 0x20000000)
#define ADT_TZ2_SIZE  0x2000000ULL

static void set_regs(int entries)
{
    u8 reg[16 * 32] = {0};
    assert(entries <= 32);
    reg_entries = entries;
    set_prop(MCC_NODE, "reg", reg, 16 * entries);
}

static void reset(u32 chip, const char *compat)
{
    nprops = nmmio = writes = rm_calls = mmio_reads = 0;
    mcc_initialized = false;
    mcc_count = 0;
    memset(mcc_regs, 0, sizeof(mcc_regs));
    chip_id = chip;
    board_id = 0;
    have_mcc = have_carve = true;
    mcc_compat = compat;
    set_regs(0);
    u64 r4[2] = {ADT_TZ4_START, ADT_TZ4_SIZE}, r2[2] = {ADT_TZ2_START, ADT_TZ2_SIZE};
    set_prop(CARVE_NODE, "region-id-4", r4, sizeof(r4));
    set_prop(CARVE_NODE, "region-id-2", r2, sizeof(r2));
}

/* TZ window n in the T6030 register layout, which T8132 uses, in 4 KiB pages from ram_base. */
static void set_tz(int n, u32 first_page, u32 last_page)
{
    set_mmio(REG_BASE(7) + 0x6dc + 0x14 * n, first_page);
    set_mmio(REG_BASE(7) + 0x6e0 + 0x14 * n, last_page);
    set_mmio(REG_BASE(7) + 0x6e8 + 0x14 * n, 1);
}

/*
 * The T8132 shape of every M4 (base) ADT in 26B131 and 25G83: two AMCCs at reg[7] and reg[8]
 * of 11, two planes and four channels each. TZ0 and TZ1 hold the ADT's region-id-4 and
 * region-id-2; TZ0 is larger than the ADT range, so the TZ registers are seen to be used.
 */
#define TZ0_SIZE 0x5000000ULL
static void t8132(const char *count_name, u32 count_len)
{
    reset(T8132, "mcc,t8132");
    set_regs(11);
    if (count_name)
        set_count(MCC_NODE, count_name, 2, count_len);
    set_count(MCC_NODE, "dcs-count-per-amcc", 4, 4);
    set_count(MCC_NODE, "plane-count-per-amcc", 2, 4);
    set_tz(0, 0x10000, 0x10000 + (TZ0_SIZE >> 12) - 1);
    set_tz(1, 0x20000, 0x20000 + (ADT_TZ2_SIZE >> 12) - 1);
}

static void expect_t8132_driven(void)
{
    assert(mcc_init() == 0);
    assert(mcc_initialized && mcc_count == 2);
    for (int i = 0; i < 2; i++) {
        assert(mcc_regs[i].plane_base == REG_BASE(7 + i));
        assert(mcc_regs[i].plane_count == 2 && mcc_regs[i].dcs_count == 4);
        assert(mcc_regs[i].tz == &t6030_tz_regs && mcc_regs[i].has_cache_control);
    }
    /* Carveouts come from the TZ registers, which agree with the ADT list. */
    assert(mcc_unmap_carveouts() == 0);
    assert(mcc_carveout_count == 2);
    assert(mcc_carveouts[0].base == ADT_TZ4_START && mcc_carveouts[0].size == TZ0_SIZE);
    assert(mcc_carveouts[1].base == ADT_TZ2_START && mcc_carveouts[1].size == ADT_TZ2_SIZE);
    /* Every plane of every AMCC gets its cache enable. */
    assert(mcc_enable_cache() == 0 && writes == 2 * 2);
}

/*
 * The MCC initializes, but its TZ windows disagree with iBoot's list: the carveouts are the ADT
 * list's, and the layout that read them is not driven any further.
 */
static void expect_adt_carveouts_from_driven_mcc(void)
{
    assert(mcc_init() == 0 && mcc_initialized);
    assert(mcc_unmap_carveouts() == 0);
    assert(mcc_carveout_count == 2);
    assert(mcc_carveouts[0].base == ADT_TZ4_START && mcc_carveouts[0].size == ADT_TZ4_SIZE);
    assert(mcc_carveouts[1].base == ADT_TZ2_START && mcc_carveouts[1].size == ADT_TZ2_SIZE);
    assert(!mcc_initialized && mcc_enable_cache() < 0 && !writes);
    u64 base, stride;
    int count;
    assert(mcc_plane_info(0, &base, &stride, &count) < 0);
}

static void tz_cross_check_cases(void)
{
    /* A layout that reads nothing enabled would leave every carveout mapped. */
    t8132("amcc_aperture_count", 4);
    nmmio = 0;
    expect_adt_carveouts_from_driven_mcc();
    /* One ADT range not inside any window. */
    t8132("amcc_aperture_count", 4);
    nmmio = 0;
    set_tz(0, 0x10000, 0x10000 + (TZ0_SIZE >> 12) - 1);
    expect_adt_carveouts_from_driven_mcc();
    /* A window that covers both ranges but runs past the end of RAM. */
    t8132("amcc_aperture_count", 4);
    nmmio = 0;
    set_tz(0, 0x10000, 0x7fffffff);
    expect_adt_carveouts_from_driven_mcc();
    /* Windows that cover both ranges, but another one outside RAM: the layout is not trusted. */
    t8132("amcc_aperture_count", 4);
    set_tz(2, 0x7fff0000, 0x7fffffff);
    expect_adt_carveouts_from_driven_mcc();
    /* With no usable ADT list there is nothing to compare: the TZ windows are used as read. */
    t8132("amcc_aperture_count", 4);
    del_prop(CARVE_NODE, "region-id-2");
    assert(mcc_init() == 0 && mcc_unmap_carveouts() == 0 && mcc_carveout_count == 2);
    assert(mcc_carveouts[0].size == TZ0_SIZE);
    t8132("amcc_aperture_count", 4);
    have_carve = false;
    assert(mcc_init() == 0 && mcc_unmap_carveouts() == 0 && mcc_carveout_count == 2);
    assert(mcc_carveouts[0].size == TZ0_SIZE);
    /* ... and a window outside RAM is left out rather than unmapped. */
    t8132("amcc_aperture_count", 4);
    have_carve = false;
    set_tz(2, 0x7fff0000, 0x7fffffff);
    assert(mcc_init() == 0 && mcc_unmap_carveouts() == 0 && mcc_carveout_count == 2);
}

/* An MCC aura cannot drive: nothing initialized, no register traffic, carveouts from the ADT. */
static void expect_undriven(void)
{
    assert(mcc_init() == MCC_UNSUPPORTED);
    assert(!mcc_initialized);
    mmio_reads = 0;
    assert(mcc_unmap_carveouts() == 0);
    assert(!mmio_reads);
    assert(mcc_carveout_count == 2);
    assert(mcc_carveouts[0].base == ADT_TZ4_START && mcc_carveouts[0].size == ADT_TZ4_SIZE);
    assert(mcc_carveouts[1].base == ADT_TZ2_START && mcc_carveouts[1].size == ADT_TZ2_SIZE);
    assert(mcc_enable_cache() < 0 && !writes);
    u64 base, stride;
    int count;
    assert(mcc_plane_info(0, &base, &stride, &count) < 0);
    /* Without TZ registers the ADT list is required where there is an MCC. */
    have_carve = false;
    assert(mcc_unmap_carveouts() < 0);
    have_carve = true;
}

static void t8132_cases(void)
{
    /* F1, F2, F4: the count at either width, under either name (F3). */
    t8132("amcc_aperture_count", 4);
    expect_t8132_driven();
    t8132("amcc_aperture_count", 8);
    expect_t8132_driven();
    t8132("amcc-count", 4);
    expect_t8132_driven();
    t8132("amcc-count", 8);
    expect_t8132_driven();

    /* F6, F7, F8: a missing count, or one of another width, cannot drive the MCC. */
    t8132(NULL, 0);
    expect_undriven();
    t8132("amcc_aperture_count", 2);
    expect_undriven();
    t8132("amcc_aperture_count", 16);
    expect_undriven();
    /* A malformed primary name is not papered over by the fallback. */
    t8132("amcc_aperture_count", 2);
    set_count(MCC_NODE, "amcc-count", 2, 4);
    expect_undriven();
    /* An 8-byte count is not truncated to its low half. */
    t8132("amcc_aperture_count", 8);
    set_count(MCC_NODE, "amcc_aperture_count", 0x100000002ULL, 8);
    expect_undriven();
    /* F10 */
    t8132("amcc_aperture_count", 4);
    set_count(MCC_NODE, "amcc_aperture_count", 0, 4);
    expect_undriven();

    /* F5: the per-AMCC counts at 8 bytes are read, not taken as 0. */
    t8132("amcc_aperture_count", 4);
    set_count(MCC_NODE, "dcs-count-per-amcc", 4, 8);
    set_count(MCC_NODE, "plane-count-per-amcc", 2, 8);
    expect_t8132_driven();
    /* ... and a missing or odd one does not "initialize" an MCC with no planes. */
    t8132("amcc_aperture_count", 4);
    del_prop(MCC_NODE, "plane-count-per-amcc");
    expect_undriven();
    t8132("amcc_aperture_count", 4);
    del_prop(MCC_NODE, "dcs-count-per-amcc");
    expect_undriven();
    t8132("amcc_aperture_count", 4);
    set_count(MCC_NODE, "plane-count-per-amcc", 2, 2);
    expect_undriven();
    t8132("amcc_aperture_count", 4);
    set_count(MCC_NODE, "plane-count-per-amcc", 0, 4);
    expect_undriven();

    /* F11 */
    t8132("amcc_aperture_count", 4);
    set_count(MCC_NODE, "amcc_aperture_count", 40, 4);
    set_regs(7 + 16);
    assert(mcc_init() == 0 && mcc_count == 16);

    /*
     * F9: a known MCC whose register windows cannot be found is not driven either. It is not
     * fatal, as before #320 on T8132; the ADT list keeps the carveouts out of the map.
     */
    t8132("amcc_aperture_count", 4);
    set_regs(7 + 1);
    expect_undriven();
}

static void m3_cases(void)
{
    /* T8122: three leading reg entries, the instances after them. */
    for (u32 len = 4; len <= 8; len += 4) {
        reset(T8122, "mcc,t8122");
        set_regs(3 + 2);
        set_count(MCC_NODE, "dcs-count-per-amcc", 4, len);
        set_count(MCC_NODE, "plane-count-per-amcc", 4, len);
        assert(mcc_init() == 0 && mcc_initialized && mcc_count == 2);
        assert(mcc_regs[1].plane_base == REG_BASE(4));
        assert(mcc_regs[0].plane_count == 4 && mcc_regs[0].dcs_count == 4);
        assert(mcc_regs[0].tz == &t8122_tz_regs);
    }
    reset(T8122, "mcc,t8122");
    set_regs(3 + 2);
    set_count(MCC_NODE, "dcs-count-per-amcc", 4, 4);
    expect_undriven();
    reset(T8122, "mcc,t8122");
    set_regs(3 + 2);
    set_count(MCC_NODE, "plane-count-per-amcc", 4, 4);
    expect_undriven();
    reset(T8122, "mcc,t8122");
    set_regs(3 + 2);
    set_count(MCC_NODE, "dcs-count-per-amcc", 4, 4);
    set_count(MCC_NODE, "plane-count-per-amcc", 0, 4);
    expect_undriven();
}

static void m5_cases(void)
{
    for (u32 len = 4; len <= 8; len += 4) {
        reset(T8142, "mcc,t8142");
        set_regs(1 + 2);
        set_count(MCC_NODE, "amcc-count", 2, len);
        set_count(MCC_NODE, "dcs-count-per-amcc", 2, len);
        assert(mcc_init() == 0 && mcc_initialized && mcc_count == 2);
        assert(mcc_regs[0].plane_count == 4 && mcc_regs[0].dcs_count == 2);
        assert(mcc_regs[0].tz == &t8142_tz_regs);
    }
    reset(T8142, "mcc,t8142");
    set_regs(1 + 2);
    set_count(MCC_NODE, "dcs-count-per-amcc", 2, 4);
    expect_undriven();
    reset(T8142, "mcc,t8142");
    set_regs(1 + 2);
    set_count(MCC_NODE, "amcc-count", 2, 4);
    expect_undriven();
}

static void m1_cases(void)
{
    for (u32 len = 4; len <= 8; len += 4) {
        reset(T8103, "mcc,t8103");
        set_regs(3);
        set_count(MCC_NODE, "dcs_num_channels", 8, len);
        assert(mcc_init() == 0 && mcc_initialized && mcc_count == 1);
        assert(mcc_regs[0].dcs_count == 8 && mcc_regs[0].tz == &t8103_tz_regs);
    }
    reset(T8103, "mcc,t8103");
    set_regs(3);
    expect_undriven();
    /* A known MCC whose register windows cannot be found is not driven. */
    reset(T8103, "mcc,t8103");
    set_regs(2);
    set_count(MCC_NODE, "dcs_num_channels", 8, 4);
    expect_undriven();
}

static void m1_pro_cases(void)
{
    /* T6000: one AMCC per 16-byte reg entry, with a fixed layout. */
    reset(T6000, "mcc,t6000");
    set_regs(2);
    assert(mcc_init() == 0 && mcc_initialized && mcc_count == 2);
    assert(mcc_regs[1].plane_base == REG_BASE(1) && mcc_regs[1].tz == &t8103_tz_regs);
    /* An instance whose register window cannot be found: the MCC is not driven. */
    reset(T6000, "mcc,t6000");
    set_regs(2);
    reg_entries = 1;
    expect_undriven();
    reset(T6000, "mcc,t6000");
    del_prop(MCC_NODE, "reg");
    expect_undriven();
}

static void other_cases(void)
{
    /* F13 */
    reset(T6050, "mcc,t6050");
    expect_undriven();
    reset(T8103, NULL);
    have_mcc = false;
    assert(mcc_init() == MCC_ABSENT && !mcc_initialized);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    t8132_cases();
    tz_cross_check_cases();
    m3_cases();
    m5_cases();
    m1_cases();
    m1_pro_cases();
    other_cases();
    puts("mcc adt counts: ok");
    return 0;
}
'''


def strip_includes(text):
    return re.sub(r'^#include "[^"]+"\n', "", text, flags=re.MULTILINE)


def test_mcc_adt_count_widths(tmp_path):
    header = strip_includes((ROOT / "src/mcc.h").read_text())
    source = strip_includes((ROOT / "src/mcc.c").read_text())
    cfile = tmp_path / "mcc_adt_counts.c"
    binary = tmp_path / "mcc_adt_counts"
    cfile.write_text(PRELUDE + header + source + CASES)
    subprocess.run(host_cc() + ["-std=gnu11", "-Wall", "-Wno-format", "-Wno-unused-function",
                                "-o", str(binary), str(cfile)], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout[-4000:] + result.stderr[-4000:]
    assert "mcc adt counts: ok" in result.stdout
