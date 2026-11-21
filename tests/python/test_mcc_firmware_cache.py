# SPDX-License-Identifier: MIT
"""Run the production firmware-cache MCC initializer (mcc,t6041, mcc,t8140) against real Mac
ADT values and against ADTs that contradict themselves.

The whole of src/mcc.c is compiled against a fake ADT built from tests/data/mcc-firmware-cache.json
(the binned and full M4 Max, the M4 Pro and the T8140) and tests/data/j773s-mcc.json. Since
aura#320 an mcc_init() result below 0 panics aura_main() before the watchdog is armed, so every M4
Max reset about 44 ms into any b14 image (aura#406): the path admitted only the M4 Pro's four AMCCs.

The ways this initializer can fail, which the cases below cover:
 F1. A real tree is refused (returns < 0), so the Mac resets at boot: the full M4 Max (8 AMCCs),
     the binned one (8 AMCCs, 6 enabled), an M4 Max tree before iBoot adds its masks, the M4 Pro.
 F2. An AMCC that amcc-enable-mask leaves out (fused off on a binned part) is read: an SError.
 F3. An ADT that contradicts itself is admitted and its registers read: amcc-count 0, an empty
     amcc-enable-mask or one past amcc-count, more enabled channels than the enabled AMCCs hold,
     a channel mask past the channel count, a channel mask with no channel count, an enabled AMCC
     with no window or one too small for its planes and the status read, a window lock-regs does
     not list or of another size, a lock-regs list whose count disagrees with it, no planes, or a
     stride that is not word aligned. Each must fail before any register is read.
 F4. An ADT that does not describe what this needs stops the boot: a missing amcc-reg-idx, a count
     or mask of another width, more AMCCs than aura tracks, a channel mask wider than 64 channels,
     or (for the status reads) no plane stride in lock-regs. The MCC must be left alone instead,
     with no register read, and the boot goes on.
 F5. A cache status other than the M4 Pro's stops the boot. The path writes no register and the
     cache is the firmware's, so the MCC is left alone instead, after the read that showed it.
 F6. A layout that boots today changes: the M4 Pro's 4 AMCCs and 16 status reads, the T8140's one
     AMCC with no reads.
 F7. A consumer sees a masked-out AMCC: mcc_count and mcc_regs[] hold only the enabled ones, in
     order, and mcc_plane_info() refuses an index past them.
 F8. A firmware-cache MCC, driven or left alone, takes its carveouts from the ADT list, never TZ
     registers, and mcc_enable_cache() writes nothing.
"""

import json
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
#define T6041 0x6041
#define T6050 0x6050
#define T8122 0x8122
#define T8132 0x8132
#define T8140 0x8140
#define T8142 0x8142

static u32 chip_id, board_id;
static u64 ram_base = 0x10000000000ULL, mem_size_actual = 0x200000000ULL;
static u64 get_page_size(void) { return 0x4000; }
static int rm_calls;
static void mmu_rm_mapping(u64 from, u64 size) { (void)from; (void)size; rm_calls++; }

/* MMIO: every read is recorded; the cache status reads back STATUS_OK unless a test moves it. */
#define STATUS_OK 0x0c000c00
static u64 read_addr[512];
static int writes, mmio_reads;
static u64 bad_status_addr;
static u32 read32(u64 addr)
{
    assert(mmio_reads < (int)ARRAY_SIZE(read_addr));
    read_addr[mmio_reads++] = addr;
    return addr == bad_status_addr ? 0x08000800 : STATUS_OK;
}
static void write32(u64 addr, u32 val) { (void)addr; (void)val; writes++; }
static int poll32(u64 addr, u32 mask, u32 target, u32 timeout)
{
    (void)addr; (void)mask; (void)target; (void)timeout;
    return 0;
}

/* The fake ADT: /arm-io/mcc, /chosen/lock-regs/amcc, /chosen/carveout-memory-map. */
#define MCC_NODE   10
#define CARVE_NODE 20
#define LOCK_NODE  30
struct adt_property {
    char name[32];
    u32 size;
    u8 value[];
};
static void *adt = (void *)1;
static struct { int node; const char *name; u32 len; _Alignas(8) u8 data[256]; } props[48];
static int nprops;
static bool have_mcc, have_carve, have_lock;
static const char *mcc_compat;
/* The MCC node's reg entries, translated: reg_ok[i] says whether entry i exists. */
static u64 reg_base[64], reg_size[64];
static bool reg_ok[64];

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
    assert(nprops <= (int)ARRAY_SIZE(props) && len <= sizeof(props[i].data));
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
/* A little-endian count of len bytes. */
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
    if (!strcmp(path, "/chosen/lock-regs/amcc"))
        return have_lock ? LOCK_NODE : -1;
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
    if (idx < 0 || idx >= (int)ARRAY_SIZE(reg_ok) || !reg_ok[idx])
        return -1;
    *addr = reg_base[idx];
    if (size)
        *size = reg_size[idx];
    return 0;
}
'''

CASES = r'''
#define ADT_TZ4_START (0x10000000000ULL + 0x10000000)
#define ADT_TZ4_SIZE  0x4000000ULL
#define ADT_TZ2_START (0x10000000000ULL + 0x20000000)
#define ADT_TZ2_SIZE  0x2000000ULL
#define STATUS_REG    0x2804

static const struct profile *cur;
static long long amcc_mask_of(const struct profile *p)
{
    return p->amcc_mask != ABSENT ? p->amcc_mask : (long long)(BIT(p->count) - 1);
}

static void set_aperture_list(const u64 *list, int n)
{
    set_prop(LOCK_NODE, "aperture-phys-addr", list, (u32)(n * sizeof(u64)));
}

/* Load a profile as iBoot hands it on: counts in 8 bytes, masks and lock-regs counts in 4. */
static void load(const struct profile *p)
{
    cur = p;
    nprops = writes = rm_calls = mmio_reads = 0;
    bad_status_addr = 0;
    mcc_initialized = false;
    mcc_count = 0;
    memset(mcc_regs, 0, sizeof(mcc_regs));
    memset(reg_ok, 0, sizeof(reg_ok));
    chip_id = p->chip;
    board_id = 0;
    have_mcc = have_carve = true;
    have_lock = p->lock;
    mcc_compat = p->compat;
    u64 r4[2] = {ADT_TZ4_START, ADT_TZ4_SIZE}, r2[2] = {ADT_TZ2_START, ADT_TZ2_SIZE};
    set_prop(CARVE_NODE, "region-id-4", r4, sizeof(r4));
    set_prop(CARVE_NODE, "region-id-2", r2, sizeof(r2));

    set_count(MCC_NODE, "amcc-reg-idx", p->reg_idx, 4);
    set_count(MCC_NODE, "amcc-count", p->count, 8);
    set_count(MCC_NODE, "dcs-count-per-amcc", p->channels, 8);
    if (p->amcc_mask != ABSENT)
        set_count(MCC_NODE, "amcc-enable-mask", p->amcc_mask, 4);
    if (p->dcs_mask != ABSENT)
        set_count(MCC_NODE, "dcs-channel-enable-mask", p->dcs_mask, 4);
    if (p->plane_mask != ABSENT)
        set_count(MCC_NODE, "amcc-plane-enable-mask", p->plane_mask, 4);
    /* Entries before the AMCCs (socni, dcs and so on) are elsewhere; only their presence counts. */
    for (int i = 0; i < p->reg_idx; i++) {
        reg_ok[i] = true;
        reg_base[i] = 0x300000000ULL + (u64)i * 0x10000;
        reg_size[i] = 0x4000;
    }
    for (int i = 0; i < p->naps; i++) {
        reg_ok[p->reg_idx + i] = true;
        reg_base[p->reg_idx + i] = p->ap_base[i];
        reg_size[p->reg_idx + i] = p->ap_size[i];
    }
    if (p->lock) {
        set_count(LOCK_NODE, "aperture-count", p->lock_count, 4);
        set_count(LOCK_NODE, "aperture-size", p->lock_size, 4);
        if (p->nphys)
            set_aperture_list(p->phys, p->nphys);
        set_count(LOCK_NODE, "plane-count", p->plane_count, 4);
        set_count(LOCK_NODE, "plane-stride", p->plane_stride, 4);
    }
}

/* lock-regs as iBoot fills it on a live M4 Max (the J616c dump), for a tree that lacks it. */
static void add_m4_max_lock_regs(int listed)
{
    have_lock = true;
    set_count(LOCK_NODE, "aperture-count", listed, 4);
    set_count(LOCK_NODE, "aperture-size", 0x200000, 4);
    u64 list[8];
    for (int i = 0; i < listed; i++)
        list[i] = 0x220000000ULL + (u64)i * 0x2000000;
    set_aperture_list(list, listed);
    set_count(LOCK_NODE, "plane-count", 4, 4);
    set_count(LOCK_NODE, "plane-stride", 0x40000, 4);
}

static bool reads_status(void) { return !strcmp(cur->compat, "mcc,t6041"); }

/* F8: carveouts from the ADT list, no TZ register read, no cache write. */
static void expect_adt_carveouts(void)
{
    int reads = mmio_reads;
    assert(mcc_unmap_carveouts() == 0);
    assert(mmio_reads == reads);
    assert(mcc_carveout_count == 2);
    assert(mcc_carveouts[0].base == ADT_TZ4_START && mcc_carveouts[0].size == ADT_TZ4_SIZE);
    assert(mcc_carveouts[1].base == ADT_TZ2_START && mcc_carveouts[1].size == ADT_TZ2_SIZE);
}

/*
 * F1, F2, F6, F7: driven with the AMCCs in mask, in order, and (mcc,t6041) one status read per
 * enabled plane at its address and nowhere else.
 */
static void expect_driven(long long mask, int planes, u64 stride)
{
    int enabled = __builtin_popcountll(mask);
    int ret = mcc_init();
    if (ret != 0)
        printf("%s: mcc_init() = %d, want 0\n", cur->name, ret);
    assert(ret == 0);
    assert(mcc_initialized && mcc_count == enabled);
    int k = 0;
    for (int i = 0; i < cur->count; i++) {
        if (!(mask & BIT(i)))
            continue;
        assert(mcc_regs[k].plane_base == cur->ap_base[i]);
        assert(mcc_regs[k].plane_stride == stride && mcc_regs[k].plane_count == planes);
        assert(!mcc_regs[k].has_cache_control && !mcc_regs[k].tz);
        k++;
    }
    if (reads_status()) {
        assert(mmio_reads == enabled * planes);
        for (int r = 0; r < mmio_reads; r++) {
            bool found = false;
            for (int i = 0; i < cur->count && !found; i++)
                for (int pl = 0; pl < planes && !found; pl++)
                    found = (mask & BIT(i)) &&
                            read_addr[r] == cur->ap_base[i] + (u64)pl * stride + STATUS_REG;
            assert(found);
        }
    } else {
        assert(!mmio_reads);
    }
    u64 base, st;
    int count;
    assert(mcc_plane_info(enabled - 1, &base, &st, &count) == 0 && count == planes);
    assert(mcc_plane_info(enabled, &base, &st, &count) < 0);
    expect_adt_carveouts();
    assert(mcc_enable_cache() == 0 && !writes);
}

/* F4, F5: left alone and the boot goes on, after at most `reads` register reads. */
static void expect_left_alone(int reads)
{
    int ret = mcc_init();
    if (ret != MCC_UNSUPPORTED)
        printf("%s: mcc_init() = %d, want MCC_UNSUPPORTED\n", cur->name, ret);
    assert(ret == MCC_UNSUPPORTED);
    assert(!mcc_initialized && mmio_reads == reads);
    u64 base, stride;
    int count;
    assert(mcc_plane_info(0, &base, &stride, &count) < 0);
    expect_adt_carveouts();
    assert(mcc_enable_cache() < 0 && !writes);
}

/* F3: refused before any register is read. */
static void expect_inconsistent(void)
{
    int ret = mcc_init();
    if (ret >= 0)
        printf("%s: mcc_init() = %d, want < 0\n", cur->name, ret);
    assert(ret < 0);
    assert(!mcc_initialized && !mmio_reads);
}

static void real_trees(void)
{
    /* F1, F2: the binned J616C reads its six enabled AMCCs (24 planes), never slots 6 and 7. */
    load(&j616c_binned);
    add_m4_max_lock_regs(8);
    expect_driven(0x3f, 4, 0x40000);
    /* ... whether lock-regs lists all eight windows or only the enabled six. */
    load(&j616c_binned);
    add_m4_max_lock_regs(6);
    expect_driven(0x3f, 4, 0x40000);
    /* ... and without lock-regs nothing says where the planes are: left alone, nothing read. */
    load(&j616c_binned);
    expect_left_alone(0);
    /* The full M4 Max: 8 AMCCs, 32 status reads. */
    load(&j616c);
    expect_driven(0xff, 4, 0x40000);
    /* The J614c as the IPSW has it (no masks, no lock-regs): left alone, the boot goes on. */
    load(&j614c);
    expect_left_alone(0);
    /* ... and with what iBoot adds on a live M4 Max, all 8 AMCCs. */
    load(&j614c);
    add_m4_max_lock_regs(8);
    set_count(MCC_NODE, "amcc-enable-mask", 0xff, 4);
    set_count(MCC_NODE, "dcs-channel-enable-mask", 0xffffffff, 4);
    expect_driven(0xff, 4, 0x40000);
    /* F6: the M4 Pro, unchanged: 4 AMCCs, 16 status reads. */
    load(&j773s);
    expect_driven(0xf, 4, 0x40000);
    /* F6: the T8140, unchanged: one AMCC, nothing read. */
    load(&j700);
    expect_driven(0x1, 4, 0x40000);
    /* A sparse mask: only the AMCCs it names, in order. */
    load(&j616c);
    set_count(MCC_NODE, "amcc-enable-mask", 0xb5, 4);
    set_count(MCC_NODE, "dcs-channel-enable-mask", 0xf0ff0f0f, 4);
    expect_driven(0xb5, 4, 0x40000);
    /* Masks at 8 bytes. */
    load(&j616c_binned);
    add_m4_max_lock_regs(8);
    set_count(MCC_NODE, "amcc-enable-mask", 0x3f, 8);
    set_count(MCC_NODE, "dcs-channel-enable-mask", 0xffffff, 8);
    expect_driven(0x3f, 4, 0x40000);
}

/* The J773s values from the reporter-posted IODeviceTree (tests/data/j773s-mcc.json). */
static void j773s_posted_tree(void)
{
    load(&j773s);
    del_prop(MCC_NODE, "amcc-enable-mask");
    del_prop(MCC_NODE, "dcs-channel-enable-mask");
    set_count(MCC_NODE, "amcc-reg-idx", J773S_REG_IDX, 4);
    set_count(MCC_NODE, "amcc-count", J773S_COUNT, 8);
    set_count(MCC_NODE, "dcs-count-per-amcc", J773S_CHANNELS, 8);
    set_count(LOCK_NODE, "plane-count", J773S_PLANES, 4);
    set_count(LOCK_NODE, "plane-stride", J773S_STRIDE, 4);
    for (int i = 0; i < J773S_COUNT; i++) {
        reg_base[J773S_REG_IDX + i] = j773s_base[i];
        reg_size[J773S_REG_IDX + i] = j773s_size[i];
    }
    set_aperture_list(j773s_base, J773S_COUNT);
    expect_driven(BIT(J773S_COUNT) - 1, J773S_PLANES, J773S_STRIDE);
}

static void inconsistent_trees(void)
{
    /* amcc-count 0 beside an AMCC register index. */
    load(&j616c);
    set_count(MCC_NODE, "amcc-count", 0, 8);
    expect_inconsistent();
    /* An empty amcc-enable-mask, or one that names an AMCC past amcc-count. */
    load(&j616c);
    set_count(MCC_NODE, "amcc-enable-mask", 0, 4);
    expect_inconsistent();
    load(&j616c_binned);
    add_m4_max_lock_regs(8);
    set_count(MCC_NODE, "amcc-enable-mask", 0x13f, 4);
    expect_inconsistent();
    load(&j773s);
    set_count(MCC_NODE, "amcc-enable-mask", 0x1f, 4);
    expect_inconsistent();
    /* More enabled channels than the enabled AMCCs hold: 32 channels beside 6 AMCCs. */
    load(&j616c_binned);
    add_m4_max_lock_regs(8);
    set_count(MCC_NODE, "dcs-channel-enable-mask", 0xffffffff, 4);
    expect_inconsistent();
    /* A channel past amcc-count * dcs-count-per-amcc. */
    load(&j773s);
    set_count(MCC_NODE, "dcs-channel-enable-mask", 0x1ffff, 4);
    expect_inconsistent();
    /* Enabled AMCCs with no enabled channel. */
    load(&j616c);
    set_count(MCC_NODE, "dcs-channel-enable-mask", 0, 4);
    expect_inconsistent();
    /* A channel mask with no channel count. */
    load(&j616c);
    del_prop(MCC_NODE, "dcs-count-per-amcc");
    expect_inconsistent();
    load(&j616c);
    set_count(MCC_NODE, "dcs-count-per-amcc", 0, 8);
    expect_inconsistent();
    /* A register index that does not point at the AMCC windows. */
    load(&j616c);
    set_count(MCC_NODE, "amcc-reg-idx", 11, 4);
    expect_inconsistent();
    load(&j700);
    set_count(MCC_NODE, "amcc-reg-idx", 1, 4);
    expect_inconsistent();
    /* An enabled AMCC with no register window, or one at 0. */
    load(&j616c_binned);
    add_m4_max_lock_regs(8);
    reg_ok[12 + 5] = false;
    expect_inconsistent();
    load(&j616c);
    reg_base[12 + 3] = 0;
    expect_inconsistent();
    /* ... or a window too small for its planes and the status read in the last one. */
    load(&j616c);
    reg_size[12 + 2] = 3 * 0x40000 + STATUS_REG;
    expect_inconsistent();
    load(&j700);
    reg_size[2] = 3 * 0x40000;
    expect_inconsistent();
    /* A window lock-regs does not list, or of another size than its apertures. */
    load(&j616c);
    reg_base[12 + 4] += 0x1000000;
    expect_inconsistent();
    load(&j616c);
    reg_size[12 + 7] = 0x400000;
    expect_inconsistent();
    load(&j616c_binned);
    add_m4_max_lock_regs(5);
    expect_inconsistent();
    /* A lock-regs list whose count disagrees with it, or that is not whole addresses. */
    load(&j616c);
    set_count(LOCK_NODE, "aperture-count", 7, 4);
    expect_inconsistent();
    load(&j616c);
    set_prop(LOCK_NODE, "aperture-phys-addr", j616c.phys, 7 * sizeof(u64) + 4);
    expect_inconsistent();
    load(&j616c);
    set_prop(LOCK_NODE, "aperture-phys-addr", j616c.phys, 0);
    expect_inconsistent();
    /* No planes, or a stride that is not word aligned. */
    load(&j616c);
    set_count(LOCK_NODE, "plane-count", 0, 4);
    expect_inconsistent();
    load(&j616c);
    set_count(LOCK_NODE, "plane-stride", 0x40002, 4);
    expect_inconsistent();
    load(&j700);
    set_count(MCC_NODE, "amcc-plane-enable-mask", 0, 4);
    del_prop(LOCK_NODE, "plane-count");
    expect_inconsistent();
}

static void undescribed_trees(void)
{
    /* F4: no AMCC register index. */
    load(&j616c);
    del_prop(MCC_NODE, "amcc-reg-idx");
    expect_left_alone(0);
    load(&j700);
    del_prop(MCC_NODE, "amcc-reg-idx");
    expect_left_alone(0);
    /* A count or a mask of another width. */
    load(&j616c);
    set_count(MCC_NODE, "amcc-count", 8, 2);
    expect_left_alone(0);
    load(&j616c);
    set_count(MCC_NODE, "amcc-enable-mask", 0xff, 2);
    expect_left_alone(0);
    load(&j616c);
    set_count(MCC_NODE, "dcs-channel-enable-mask", 0xffffffff, 16);
    expect_left_alone(0);
    load(&j616c);
    set_count(LOCK_NODE, "plane-stride", 0x40000, 2);
    expect_left_alone(0);
    /* More AMCCs than aura tracks. */
    load(&j616c);
    set_count(MCC_NODE, "amcc-count", 17, 8);
    del_prop(MCC_NODE, "amcc-enable-mask");
    del_prop(MCC_NODE, "dcs-channel-enable-mask");
    expect_left_alone(0);
    /* A channel mask cannot describe more than 64 channels. */
    load(&j616c);
    set_count(MCC_NODE, "dcs-count-per-amcc", 9, 8);
    expect_left_alone(0);
    /* The status reads need the plane stride from lock-regs. */
    load(&j616c);
    del_prop(LOCK_NODE, "plane-stride");
    expect_left_alone(0);
    /* ... but not the window list: the reg windows alone are used then. */
    load(&j616c);
    del_prop(LOCK_NODE, "aperture-phys-addr");
    expect_driven(0xff, 4, 0x40000);
}

static void status_cases(void)
{
    /* F5: an unexpected status on any enabled plane leaves the MCC alone, after that read. */
    for (int plane = 0; plane < 4; plane++) {
        load(&j616c_binned);
        add_m4_max_lock_regs(8);
        bad_status_addr = 0x22a000000ULL + (u64)plane * 0x40000 + STATUS_REG;
        expect_left_alone(5 * 4 + plane + 1);
    }
    load(&j773s);
    bad_status_addr = 0x220000000ULL + STATUS_REG;
    expect_left_alone(1);
    /* A status at a masked-out AMCC is never read, so it changes nothing. */
    load(&j616c_binned);
    add_m4_max_lock_regs(8);
    bad_status_addr = 0x22c000000ULL + STATUS_REG;
    expect_driven(0x3f, 4, 0x40000);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    real_trees();
    j773s_posted_tree();
    inconsistent_trees();
    undescribed_trees();
    status_cases();
    puts("mcc firmware cache: ok");
    return 0;
}
'''


def strip_includes(text):
    return re.sub(r'^#include "[^"]+"\n', "", text, flags=re.MULTILINE)


def c_int(value):
    return "ABSENT" if value is None else str(int(value, 16) if isinstance(value, str) else value)


def profiles():
    data = json.loads((ROOT / "tests/data/mcc-firmware-cache.json").read_text())["profiles"]
    out = [r'''
#define ABSENT (-1LL)
struct profile {
    const char *name;
    u32 chip;
    const char *compat;
    int reg_idx;
    int count;
    u64 channels;
    long long amcc_mask, dcs_mask, plane_mask;
    bool lock;
    u64 lock_count, lock_size, plane_count, plane_stride;
    int nphys;
    u64 phys[16];
    int naps;
    u64 ap_base[16], ap_size[16];
};
''']
    for name, p in data.items():
        lock = p["lock_regs"]
        phys = (lock or {}).get("aperture_phys_addr") or []
        assert len(p["apertures"]) == p["amcc_count"]
        fields = [
            f'"{name}"', p["chip_id"], f'"{p["compatible"]}"', str(p["amcc_reg_idx"]),
            str(p["amcc_count"]), str(p["dcs_count_per_amcc"]),
            c_int(p["amcc_enable_mask"]), c_int(p["dcs_channel_enable_mask"]),
            c_int(p["amcc_plane_enable_mask"]),
            "true" if lock else "false",
            c_int(lock["aperture_count"]) if lock else "0",
            c_int(lock["aperture_size"]) if lock else "0",
            c_int(lock["plane_count"]) if lock else "0",
            c_int(lock["plane_stride"]) if lock else "0",
            str(len(phys)), "{" + ", ".join(phys or ["0"]) + "}",
            str(len(p["apertures"])),
            "{" + ", ".join(a for a, _ in p["apertures"]) + "}",
            "{" + ", ".join(s for _, s in p["apertures"]) + "}",
        ]
        out.append(f"static const struct profile {name.replace('-', '_')} = {{{', '.join(fields)}}};")
    j773s = json.loads((ROOT / "tests/data/j773s-mcc.json").read_text())
    assert j773s["mcc_compatible"] == "mcc,t6041" and len(j773s["apertures"]) == j773s["amcc_count"]
    out += [
        f'#define J773S_REG_IDX {j773s["amcc_reg_idx"]}',
        f'#define J773S_COUNT {j773s["amcc_count"]}',
        f'#define J773S_CHANNELS {j773s["dcs_count_per_amcc"]}',
        f'#define J773S_PLANES {j773s["lock_regs_plane_count"]}',
        f'#define J773S_STRIDE {j773s["lock_regs_plane_stride"]}',
        "static const u64 j773s_base[] = {" + ", ".join(a["base"] for a in j773s["apertures"]) + "};",
        "static const u64 j773s_size[] = {" + ", ".join(a["size"] for a in j773s["apertures"]) + "};",
    ]
    return "\n".join(out) + "\n"


def test_mcc_firmware_cache(tmp_path):
    header = strip_includes((ROOT / "src/mcc.h").read_text())
    source = strip_includes((ROOT / "src/mcc.c").read_text())
    cfile = tmp_path / "mcc_firmware_cache.c"
    binary = tmp_path / "mcc_firmware_cache"
    cfile.write_text(PRELUDE + header + source + profiles() + CASES)
    subprocess.run(host_cc() + ["-std=gnu11", "-Wall", "-Wno-format", "-Wno-unused-function",
                                "-o", str(binary), str(cfile)], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout[-4000:] + result.stderr[-4000:]
    assert "mcc firmware cache: ok" in result.stdout
