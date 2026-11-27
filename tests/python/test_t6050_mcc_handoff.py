# SPDX-License-Identifier: MIT
"""Exercise the production single-die T6050 MCC FDT handoff (dt_set_t6050_mcc) in src/kboot.c.

Compiles the exact production block from src/kboot.c against aura's libfdt and a synthetic ADT
covering:
- Full 8-AMCC M5 Max (J716c: amcc-enable-mask 0xff, 32 planes, 8-byte amcc-count).
- 4-AMCC M5 Pro (amcc-enable-mask 0xf, 16 planes).
- Binned contiguous (0x3f, 24 planes) and non-contiguous (0x55, 16 planes; 0xfe, 28 planes)
  enable masks, verifying fused-off AMCCs are never emitted or queried.
- Lock-regs listing only enabled AMCCs vs all AMCCs, non-identity parent ranges, and nested buses.
- Already-enabled nodes (e.g., two-die M5 Ultra with 64 planes and no status property, or explicit
  status = "okay") left completely untouched.
- Fail-closed behavior leaving status = "disabled" and original reg byte-for-byte intact on:
  multi-die (die-count != 1 or die-enable-mask != 1), missing/zero/out-of-range amcc-enable-mask,
  inconsistent dcs-channel-enable-mask (channel on disabled AMCC, past channels, missing count),
  missing/malformed lock-regs (plane-count, plane-stride < 0x4000, unaligned stride, missing or
  mismatched aperture-phys-addr / aperture-count / aperture-size), inconsistent
  amcc-plane-enable-mask, total planes > 64, overlapping/duplicate AMCC bases, plane outside
  parent bus ranges, missing/malformed parent ranges, multiple apple,t6050-mcc nodes, and
  FDT out-of-space.
"""

from pathlib import Path
import subprocess

from host_compiler import host_cc

ROOT = Path(__file__).resolve().parents[2]
LIBFDT = [
    "fdt.c",
    "fdt_ro.c",
    "fdt_rw.c",
    "fdt_wip.c",
    "fdt_sw.c",
    "fdt_empty_tree.c",
    "fdt_strerror.c",
    "fdt_addresses.c",
]


def test_t6050_mcc_handoff(tmp_path):
    source = (ROOT / "src/kboot.c").read_text()
    start = source.index("#define T6050_MCC_PLANE_SIZE")
    end = source.index("\n#endif /* ESP_STAGE2 */", start)
    prod = source[start:end]

    # Verify kboot_prepare_dt() invokes dt_set_t6050_mcc().
    prep_start = source.index("int kboot_prepare_dt(")
    prep_end = source.index("\nstatic int dt_disable_j713_pcie(", prep_start)
    assert "dt_set_t6050_mcc();" in source[prep_start:prep_end]

    harness = (
        r'''
#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libfdt.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define ARRAY_SIZE(s) (sizeof(s) / sizeof((s)[0]))

static void *adt = (void *)1;
static void *dt;

#define ARMIO_NODE 10
#define MCC_NODE   20
#define LOCK_NODE  30

static struct {
    int node;
    const char *name;
    u32 len;
    _Alignas(8) u8 data[256];
} props[48];
static int nprops;
static bool have_armio, have_mcc, have_lock;
static const char *mcc_compat;
static u64 reg_base[64], reg_size[64];
static bool reg_ok[64];
static u64 reg_queried_mask;

static int find_prop(int node, const char *name)
{
    for (int i = 0; i < nprops; i++)
        if (props[i].node == node && !strcmp(props[i].name, name))
            return i;
    return -1;
}

static void set_prop(int node, const char *name, const void *data, u32 len)
{
    int i = find_prop(node, name);
    if (i < 0) {
        assert(nprops < (int)ARRAY_SIZE(props));
        i = nprops++;
    }
    assert(len <= sizeof(props[i].data));
    props[i].node = node;
    props[i].name = name;
    props[i].len = len;
    memset(props[i].data, 0xa5, sizeof(props[i].data));
    memcpy(props[i].data, data, len);
}

static void del_prop(int node, const char *name)
{
    int i = find_prop(node, name);
    if (i >= 0)
        props[i] = props[--nprops];
}

static void set_u32(int node, const char *name, u32 val)
{
    set_prop(node, name, &val, sizeof(val));
}

static void set_u64(int node, const char *name, u64 val)
{
    set_prop(node, name, &val, sizeof(val));
}

static const void *adt_getprop(const void *a, int node, const char *name, u32 *len)
{
    (void)a;
    int i = find_prop(node, name);
    if (i < 0)
        return NULL;
    if (len)
        *len = props[i].len;
    return props[i].data;
}

static int adt_path_offset(const void *a, const char *path)
{
    (void)a;
    if (!strcmp(path, "/arm-io"))
        return have_armio ? ARMIO_NODE : -1;
    if (!strcmp(path, "/arm-io/mcc"))
        return have_mcc ? MCC_NODE : -1;
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

static int adt_get_reg(const void *a, int *path, const char *prop, int idx, u64 *addr, u64 *size)
{
    (void)a;
    (void)path;
    (void)prop;
    if (idx < 0 || idx >= 64)
        return -1;
    reg_queried_mask |= 1ULL << idx;
    if (!reg_ok[idx])
        return -1;
    *addr = reg_base[idx];
    if (size)
        *size = reg_size[idx];
    return 0;
}
'''
        + prod
        + r'''

static _Alignas(8) unsigned char fdt_buf[8192];

static void setup_j716c_adt(void)
{
    nprops = 0;
    have_armio = have_mcc = have_lock = true;
    mcc_compat = "mcc,t6050";
    memset(reg_ok, 0, sizeof(reg_ok));
    memset(reg_base, 0, sizeof(reg_base));
    memset(reg_size, 0, sizeof(reg_size));
    reg_queried_mask = 0;

    set_u32(ARMIO_NODE, "die-count", 1);
    set_u32(MCC_NODE, "die-enable-mask", 1);
    set_u32(MCC_NODE, "amcc-reg-idx", 14);
    set_u64(MCC_NODE, "amcc-count", 8);
    set_u32(MCC_NODE, "amcc-enable-mask", 0xff);
    set_u32(MCC_NODE, "dcs-count-per-amcc", 4);
    set_u32(MCC_NODE, "dcs-channel-enable-mask", 0xffffffffU);

    u64 aps[8];
    for (int i = 0; i < 8; i++) {
        u64 b = 0x2220000000ULL + (u64)i * 0x2000000ULL;
        reg_ok[14 + i] = true;
        reg_base[14 + i] = b;
        reg_size[14 + i] = 0x140000ULL;
        aps[i] = b;
    }
    set_u32(LOCK_NODE, "aperture-count", 8);
    set_u32(LOCK_NODE, "aperture-size", 0x140000);
    set_prop(LOCK_NODE, "aperture-phys-addr", aps, sizeof(aps));
    set_u32(LOCK_NODE, "plane-count", 4);
    set_u32(LOCK_NODE, "plane-stride", 0x40000);
}

static void setup_fdt(size_t bufsize, const char *status, const fdt32_t *ranges, int ranges_len,
                      bool with_ranges, bool second_node)
{
    assert(bufsize <= sizeof(fdt_buf));
    memset(fdt_buf, 0, sizeof(fdt_buf));
    dt = fdt_buf;
    assert(!fdt_create_empty_tree(dt, (int)bufsize));
    assert(!fdt_setprop_u32(dt, 0, "#address-cells", 2));
    assert(!fdt_setprop_u32(dt, 0, "#size-cells", 2));

    int soc = fdt_add_subnode(dt, 0, "soc@2220000000");
    assert(soc >= 0);
    assert(!fdt_setprop_u32(dt, soc, "#address-cells", 2));
    assert(!fdt_setprop_u32(dt, soc, "#size-cells", 2));
    if (with_ranges)
        assert(!fdt_setprop(dt, soc, "ranges", ranges, ranges_len));

    int mcc = fdt_add_subnode(dt, soc, "memory-controller@2220000000");
    assert(mcc >= 0);
    assert(!fdt_setprop_string(dt, mcc, "compatible", "apple,t6050-mcc"));
    fdt64_t initial_reg[2] = {cpu_to_fdt64(0x2220000000ULL), cpu_to_fdt64(0x4000ULL)};
    assert(!fdt_setprop(dt, mcc, "reg", initial_reg, sizeof(initial_reg)));
    if (status)
        assert(!fdt_setprop_string(dt, mcc, "status", status));

    if (second_node) {
        soc = fdt_path_offset(dt, "/soc@2220000000");
        int mcc2 = fdt_add_subnode(dt, soc, "memory-controller@2222000000");
        assert(mcc2 >= 0);
        assert(!fdt_setprop_string(dt, mcc2, "compatible", "apple,t6050-mcc"));
        assert(!fdt_setprop_string(dt, mcc2, "status", "disabled"));
    }
}

static void assert_still_disabled(void)
{
    int mcc = fdt_path_offset(dt, "/soc@2220000000/memory-controller@2220000000");
    assert(mcc >= 0);
    int len = 0;
    const char *st = fdt_getprop(dt, mcc, "status", &len);
    assert(st && len == (int)sizeof("disabled") && !strcmp(st, "disabled"));
    const fdt64_t *reg = fdt_getprop(dt, mcc, "reg", &len);
    assert(reg && len == 16);
    assert(fdt64_ld(&reg[0]) == 0x2220000000ULL);
    assert(fdt64_ld(&reg[1]) == 0x4000ULL);
}

static void assert_enabled_mask(u64 mask, u64 addr_offset)
{
    int mcc = fdt_path_offset(dt, "/soc@2220000000/memory-controller@2220000000");
    assert(mcc >= 0);
    int len = 0;
    const char *st = fdt_getprop(dt, mcc, "status", &len);
    assert(st && len == (int)sizeof("okay") && !strcmp(st, "okay"));

    int expected_planes = __builtin_popcountll(mask) * 4;
    const fdt64_t *reg = fdt_getprop(dt, mcc, "reg", &len);
    assert(reg && len == expected_planes * 16);

    int idx = 0;
    for (int i = 0; i < 8; i++) {
        if (!(mask & (1ULL << i)))
            continue;
        for (int p = 0; p < 4; p++) {
            u64 expect = 0x2220000000ULL + (u64)i * 0x2000000ULL + (u64)p * 0x40000ULL - addr_offset;
            assert(fdt64_ld(&reg[idx * 2]) == expect);
            assert(fdt64_ld(&reg[idx * 2 + 1]) == 0x4000ULL);
            idx++;
        }
    }
    assert(idx == expected_planes);
}

int main(void)
{
    const fdt32_t default_ranges[] = {
        cpu_to_fdt32(0x22), cpu_to_fdt32(0x20000000),
        cpu_to_fdt32(0x22), cpu_to_fdt32(0x20000000),
        cpu_to_fdt32(0x0),  cpu_to_fdt32(0x10000000),
    };

    /* 1. Full 8-AMCC J716c (32 planes, 8-byte amcc-count). */
    setup_j716c_adt();
    setup_fdt(sizeof(fdt_buf), "disabled", default_ranges, sizeof(default_ranges), true, false);
    dt_set_t6050_mcc();
    assert_enabled_mask(0xff, 0);

    /* 2. M5 Pro (4 AMCCs: mask 0xf, 16 planes), verifying disabled AMCCs 4..7 never queried. */
    setup_j716c_adt();
    set_u32(MCC_NODE, "amcc-enable-mask", 0xf);
    set_u32(MCC_NODE, "dcs-channel-enable-mask", 0xffff);
    for (int i = 4; i < 8; i++)
        reg_ok[14 + i] = false;
    setup_fdt(sizeof(fdt_buf), "disabled", default_ranges, sizeof(default_ranges), true, false);
    dt_set_t6050_mcc();
    assert_enabled_mask(0xf, 0);
    assert((reg_queried_mask & (0xfULL << 18)) == 0);

    /* 3. Binned 6-AMCC M5 Max (0x3f, 24 planes), lock-regs listing only the 6 enabled AMCCs. */
    setup_j716c_adt();
    set_u32(MCC_NODE, "amcc-enable-mask", 0x3f);
    set_u32(MCC_NODE, "dcs-channel-enable-mask", 0xffffff);
    u64 six_aps[6];
    for (int i = 0; i < 6; i++)
        six_aps[i] = 0x2220000000ULL + (u64)i * 0x2000000ULL;
    set_u32(LOCK_NODE, "aperture-count", 6);
    set_prop(LOCK_NODE, "aperture-phys-addr", six_aps, sizeof(six_aps));
    setup_fdt(sizeof(fdt_buf), "disabled", default_ranges, sizeof(default_ranges), true, false);
    dt_set_t6050_mcc();
    assert_enabled_mask(0x3f, 0);

    /* 4. Non-contiguous binned mask (0x55: AMCCs 0, 2, 4, 6) and mask without AMCC 0 (0xfe). */
    setup_j716c_adt();
    set_u32(MCC_NODE, "amcc-enable-mask", 0x55);
    set_u32(MCC_NODE, "dcs-channel-enable-mask", 0x0f0f0f0fU);
    setup_fdt(sizeof(fdt_buf), "disabled", default_ranges, sizeof(default_ranges), true, false);
    dt_set_t6050_mcc();
    assert_enabled_mask(0x55, 0);

    setup_j716c_adt();
    set_u32(MCC_NODE, "amcc-enable-mask", 0xfe);
    set_u32(MCC_NODE, "dcs-channel-enable-mask", 0xfffffff0U);
    setup_fdt(sizeof(fdt_buf), "disabled", default_ranges, sizeof(default_ranges), true, false);
    dt_set_t6050_mcc();
    assert_enabled_mask(0xfe, 0);

    /* 5. Non-identity ranges: child 0x0 maps to parent 0x2220000000. */
    const fdt32_t shifted_ranges[] = {
        cpu_to_fdt32(0x0),  cpu_to_fdt32(0x0),
        cpu_to_fdt32(0x22), cpu_to_fdt32(0x20000000),
        cpu_to_fdt32(0x0),  cpu_to_fdt32(0x10000000),
    };
    setup_j716c_adt();
    setup_fdt(sizeof(fdt_buf), "disabled", shifted_ranges, sizeof(shifted_ranges), true, false);
    dt_set_t6050_mcc();
    assert_enabled_mask(0xff, 0x2220000000ULL);

    /* 6. Already-enabled nodes (no status property like t6050-ultra.dtsi, or explicit "okay")
     *    must be left untouched. */
    setup_j716c_adt();
    setup_fdt(sizeof(fdt_buf), NULL, default_ranges, sizeof(default_ranges), true, false);
    dt_set_t6050_mcc();
    {
        int mcc = fdt_path_offset(dt, "/soc@2220000000/memory-controller@2220000000");
        int len = 0;
        assert(!fdt_getprop(dt, mcc, "status", &len));
        const fdt64_t *reg = fdt_getprop(dt, mcc, "reg", &len);
        assert(reg && len == 16 && fdt64_ld(&reg[0]) == 0x2220000000ULL);
    }

    setup_j716c_adt();
    setup_fdt(sizeof(fdt_buf), "okay", default_ranges, sizeof(default_ranges), true, false);
    dt_set_t6050_mcc();
    {
        int mcc = fdt_path_offset(dt, "/soc@2220000000/memory-controller@2220000000");
        int len = 0;
        const fdt64_t *reg = fdt_getprop(dt, mcc, "reg", &len);
        assert(reg && len == 16 && fdt64_ld(&reg[0]) == 0x2220000000ULL);
    }

    /* 7. Multi-die (die-count = 2 or die-enable-mask = 3): stays disabled. */
    setup_j716c_adt();
    set_u32(ARMIO_NODE, "die-count", 2);
    setup_fdt(sizeof(fdt_buf), "disabled", default_ranges, sizeof(default_ranges), true, false);
    dt_set_t6050_mcc();
    assert_still_disabled();

    setup_j716c_adt();
    set_u32(MCC_NODE, "die-enable-mask", 3);
    setup_fdt(sizeof(fdt_buf), "disabled", default_ranges, sizeof(default_ranges), true, false);
    dt_set_t6050_mcc();
    assert_still_disabled();

    /* 8. Missing, zero, or out-of-range amcc-enable-mask: stays disabled. */
    setup_j716c_adt();
    del_prop(MCC_NODE, "amcc-enable-mask");
    setup_fdt(sizeof(fdt_buf), "disabled", default_ranges, sizeof(default_ranges), true, false);
    dt_set_t6050_mcc();
    assert_still_disabled();

    setup_j716c_adt();
    set_u32(MCC_NODE, "amcc-enable-mask", 0);
    setup_fdt(sizeof(fdt_buf), "disabled", default_ranges, sizeof(default_ranges), true, false);
    dt_set_t6050_mcc();
    assert_still_disabled();

    setup_j716c_adt();
    set_u32(MCC_NODE, "amcc-enable-mask", 0x1ff);
    setup_fdt(sizeof(fdt_buf), "disabled", default_ranges, sizeof(default_ranges), true, false);
    dt_set_t6050_mcc();
    assert_still_disabled();

    /* 9. Contradictory dcs-channel-enable-mask (channel on disabled AMCC): stays disabled. */
    setup_j716c_adt();
    set_u32(MCC_NODE, "amcc-enable-mask", 0x3f);
    set_u32(MCC_NODE, "dcs-channel-enable-mask", 0x01ffffffU);
    setup_fdt(sizeof(fdt_buf), "disabled", default_ranges, sizeof(default_ranges), true, false);
    dt_set_t6050_mcc();
    assert_still_disabled();

    /* 10. Lock-regs failures: missing node, stride < 0x4000, unaligned stride, missing aperture,
     *     mismatched aperture-count, mismatched aperture-size. */
    setup_j716c_adt();
    have_lock = false;
    setup_fdt(sizeof(fdt_buf), "disabled", default_ranges, sizeof(default_ranges), true, false);
    dt_set_t6050_mcc();
    assert_still_disabled();

    setup_j716c_adt();
    set_u32(LOCK_NODE, "plane-stride", 0x2000);
    setup_fdt(sizeof(fdt_buf), "disabled", default_ranges, sizeof(default_ranges), true, false);
    dt_set_t6050_mcc();
    assert_still_disabled();

    setup_j716c_adt();
    set_u32(LOCK_NODE, "plane-stride", 0x4002);
    setup_fdt(sizeof(fdt_buf), "disabled", default_ranges, sizeof(default_ranges), true, false);
    dt_set_t6050_mcc();
    assert_still_disabled();

    setup_j716c_adt();
    set_u32(LOCK_NODE, "aperture-count", 7);
    setup_fdt(sizeof(fdt_buf), "disabled", default_ranges, sizeof(default_ranges), true, false);
    dt_set_t6050_mcc();
    assert_still_disabled();

    setup_j716c_adt();
    set_u32(LOCK_NODE, "aperture-size", 0x200000);
    setup_fdt(sizeof(fdt_buf), "disabled", default_ranges, sizeof(default_ranges), true, false);
    dt_set_t6050_mcc();
    assert_still_disabled();

    setup_j716c_adt();
    u64 missing_one[7];
    for (int i = 0; i < 7; i++)
        missing_one[i] = 0x2220000000ULL + (u64)i * 0x2000000ULL;
    set_u32(LOCK_NODE, "aperture-count", 7);
    set_prop(LOCK_NODE, "aperture-phys-addr", missing_one, sizeof(missing_one));
    setup_fdt(sizeof(fdt_buf), "disabled", default_ranges, sizeof(default_ranges), true, false);
    dt_set_t6050_mcc();
    assert_still_disabled();

    /* 11. Inconsistent amcc-plane-enable-mask: stays disabled. */
    setup_j716c_adt();
    set_u32(MCC_NODE, "amcc-plane-enable-mask", 0x7);
    setup_fdt(sizeof(fdt_buf), "disabled", default_ranges, sizeof(default_ranges), true, false);
    dt_set_t6050_mcc();
    assert_still_disabled();

    /* 12. >64 total planes (8 AMCCs * 9 planes = 72): stays disabled. */
    setup_j716c_adt();
    set_u32(LOCK_NODE, "plane-count", 9);
    set_u32(LOCK_NODE, "aperture-size", 0x400000);
    for (int i = 0; i < 8; i++)
        reg_size[14 + i] = 0x400000ULL;
    setup_fdt(sizeof(fdt_buf), "disabled", default_ranges, sizeof(default_ranges), true, false);
    dt_set_t6050_mcc();
    assert_still_disabled();

    /* 13. Overlapping/duplicate AMCC bases: stays disabled. */
    setup_j716c_adt();
    reg_base[15] = reg_base[14];
    setup_fdt(sizeof(fdt_buf), "disabled", default_ranges, sizeof(default_ranges), true, false);
    dt_set_t6050_mcc();
    assert_still_disabled();

    /* 14. Plane falls outside parent bus ranges, or parent lacks ranges: stays disabled. */
    const fdt32_t narrow_ranges[] = {
        cpu_to_fdt32(0x22), cpu_to_fdt32(0x20000000),
        cpu_to_fdt32(0x22), cpu_to_fdt32(0x20000000),
        cpu_to_fdt32(0x0),  cpu_to_fdt32(0x08000000),
    };
    setup_j716c_adt();
    setup_fdt(sizeof(fdt_buf), "disabled", narrow_ranges, sizeof(narrow_ranges), true, false);
    dt_set_t6050_mcc();
    assert_still_disabled();

    setup_j716c_adt();
    setup_fdt(sizeof(fdt_buf), "disabled", NULL, 0, false, false);
    dt_set_t6050_mcc();
    assert_still_disabled();

    /* 15. Multiple apple,t6050-mcc nodes: left disabled. */
    setup_j716c_adt();
    setup_fdt(sizeof(fdt_buf), "disabled", default_ranges, sizeof(default_ranges), true, true);
    dt_set_t6050_mcc();
    assert_still_disabled();

    /* 16. FDT out-of-space when growing reg from 16 to 512 bytes: stays disabled, reg intact. */
    setup_j716c_adt();
    setup_fdt(sizeof(fdt_buf), "disabled", default_ranges, sizeof(default_ranges), true, false);
    assert(!fdt_pack(dt));
    int tight = (int)fdt_totalsize(dt) + 64;
    assert(!fdt_open_into(dt, dt, tight));
    dt_set_t6050_mcc();
    assert_still_disabled();

    /* 17. Incompatible ADT /arm-io/mcc: stays disabled. */
    setup_j716c_adt();
    mcc_compat = "mcc,t6041";
    setup_fdt(sizeof(fdt_buf), "disabled", default_ranges, sizeof(default_ranges), true, false);
    dt_set_t6050_mcc();
    assert_still_disabled();

    /* 18. Overlapping child windows in parent ranges (Linux forward walk would pick entry 0 ->
     *     0x3000000000 instead of 0x2220000000): stays disabled. */
    const fdt32_t overlapping_ranges[] = {
        cpu_to_fdt32(0x22), cpu_to_fdt32(0x20000000),
        cpu_to_fdt32(0x30), cpu_to_fdt32(0x0),
        cpu_to_fdt32(0x0),  cpu_to_fdt32(0x10000000),
        cpu_to_fdt32(0x22), cpu_to_fdt32(0x20000000),
        cpu_to_fdt32(0x22), cpu_to_fdt32(0x20000000),
        cpu_to_fdt32(0x0),  cpu_to_fdt32(0x10000000),
    };
    setup_j716c_adt();
    setup_fdt(sizeof(fdt_buf), "disabled", overlapping_ranges, sizeof(overlapping_ranges), true,
              false);
    dt_set_t6050_mcc();
    assert_still_disabled();

    /* 19. Two-level nested buses with non-identity outer bus: translates cleanly. */
    setup_j716c_adt();
    memset(fdt_buf, 0, sizeof(fdt_buf));
    dt = fdt_buf;
    assert(!fdt_create_empty_tree(dt, (int)sizeof(fdt_buf)));
    assert(!fdt_setprop_u32(dt, 0, "#address-cells", 2));
    assert(!fdt_setprop_u32(dt, 0, "#size-cells", 2));
    {
        int outer = fdt_add_subnode(dt, 0, "outer@2200000000");
        assert(outer >= 0);
        assert(!fdt_setprop_u32(dt, outer, "#address-cells", 2));
        assert(!fdt_setprop_u32(dt, outer, "#size-cells", 2));
        const fdt32_t outer_ranges[] = {
            cpu_to_fdt32(0x0),  cpu_to_fdt32(0x0),
            cpu_to_fdt32(0x22), cpu_to_fdt32(0x0),
            cpu_to_fdt32(0x0),  cpu_to_fdt32(0x40000000),
        };
        assert(!fdt_setprop(dt, outer, "ranges", outer_ranges, sizeof(outer_ranges)));
        int inner = fdt_add_subnode(dt, outer, "inner@20000000");
        assert(inner >= 0);
        assert(!fdt_setprop_u32(dt, inner, "#address-cells", 2));
        assert(!fdt_setprop_u32(dt, inner, "#size-cells", 2));
        const fdt32_t inner_ranges[] = {
            cpu_to_fdt32(0x0), cpu_to_fdt32(0x0),
            cpu_to_fdt32(0x0), cpu_to_fdt32(0x20000000),
            cpu_to_fdt32(0x0), cpu_to_fdt32(0x10000000),
        };
        assert(!fdt_setprop(dt, inner, "ranges", inner_ranges, sizeof(inner_ranges)));
        int mcc = fdt_add_subnode(dt, inner, "memory-controller@0");
        assert(mcc >= 0);
        assert(!fdt_setprop_string(dt, mcc, "compatible", "apple,t6050-mcc"));
        fdt64_t initial_reg[2] = {cpu_to_fdt64(0), cpu_to_fdt64(0x4000ULL)};
        assert(!fdt_setprop(dt, mcc, "reg", initial_reg, sizeof(initial_reg)));
        assert(!fdt_setprop_string(dt, mcc, "status", "disabled"));
    }
    dt_set_t6050_mcc();
    {
        int mcc = fdt_path_offset(dt, "/outer@2200000000/inner@20000000/memory-controller@0");
        assert(mcc >= 0);
        int len = 0;
        const char *st = fdt_getprop(dt, mcc, "status", &len);
        assert(st && !strcmp(st, "okay"));
        const fdt64_t *reg = fdt_getprop(dt, mcc, "reg", &len);
        assert(reg && len == 32 * 16);
        assert(fdt64_ld(&reg[0]) == 0x0ULL && fdt64_ld(&reg[1]) == 0x4000ULL);
        assert(fdt64_ld(&reg[62]) == 0x0e0c0000ULL && fdt64_ld(&reg[63]) == 0x4000ULL);
    }

    /* 20. Missing explicit #size-cells on intermediate bus or #address-cells on root: stays disabled. */
    setup_j716c_adt();
    setup_fdt(sizeof(fdt_buf), "disabled", default_ranges, sizeof(default_ranges), true, false);
    assert(!fdt_delprop(dt, 0, "#address-cells"));
    dt_set_t6050_mcc();
    assert_still_disabled();

    setup_j716c_adt();
    setup_fdt(sizeof(fdt_buf), "disabled", default_ranges, sizeof(default_ranges), true, false);
    assert(!fdt_delprop(dt, fdt_path_offset(dt, "/soc@2220000000"), "#size-cells"));
    dt_set_t6050_mcc();
    assert_still_disabled();

    /* 21. Untranslated ADT reg address (0x2020000000) vs real lock-regs (0x2220000000): stays disabled. */
    setup_j716c_adt();
    for (int i = 0; i < 8; i++)
        reg_base[14 + i] = 0x2020000000ULL + (u64)i * 0x2000000ULL;
    setup_fdt(sizeof(fdt_buf), "disabled", default_ranges, sizeof(default_ranges), true, false);
    dt_set_t6050_mcc();
    assert_still_disabled();

    /* 22. Missing both die-count and die-enable-mask: stays disabled; either one alone = 1 works. */
    setup_j716c_adt();
    del_prop(ARMIO_NODE, "die-count");
    del_prop(MCC_NODE, "die-enable-mask");
    setup_fdt(sizeof(fdt_buf), "disabled", default_ranges, sizeof(default_ranges), true, false);
    dt_set_t6050_mcc();
    assert_still_disabled();

    setup_j716c_adt();
    del_prop(ARMIO_NODE, "die-count");
    setup_fdt(sizeof(fdt_buf), "disabled", default_ranges, sizeof(default_ranges), true, false);
    dt_set_t6050_mcc();
    assert_enabled_mask(0xff, 0);

    setup_j716c_adt();
    del_prop(MCC_NODE, "die-enable-mask");
    setup_fdt(sizeof(fdt_buf), "disabled", default_ranges, sizeof(default_ranges), true, false);
    dt_set_t6050_mcc();
    assert_enabled_mask(0xff, 0);

    /* 23. Wrong-width property (12-byte amcc-enable-mask) stays disabled; 4-byte amcc-count works. */
    setup_j716c_adt();
    u8 bad_mask[12] = {0xff};
    set_prop(MCC_NODE, "amcc-enable-mask", bad_mask, sizeof(bad_mask));
    setup_fdt(sizeof(fdt_buf), "disabled", default_ranges, sizeof(default_ranges), true, false);
    dt_set_t6050_mcc();
    assert_still_disabled();

    setup_j716c_adt();
    set_u32(MCC_NODE, "amcc-count", 8);
    setup_fdt(sizeof(fdt_buf), "disabled", default_ranges, sizeof(default_ranges), true, false);
    dt_set_t6050_mcc();
    assert_enabled_mask(0xff, 0);

    /* 24. Non-16-KiB-aligned stride (0x5000) or base (0x2220001000): stays disabled. */
    setup_j716c_adt();
    set_u32(LOCK_NODE, "plane-stride", 0x5000);
    setup_fdt(sizeof(fdt_buf), "disabled", default_ranges, sizeof(default_ranges), true, false);
    dt_set_t6050_mcc();
    assert_still_disabled();

    setup_j716c_adt();
    reg_base[14] = 0x2220001000ULL;
    u64 unaligned_aps[8];
    for (int i = 0; i < 8; i++)
        unaligned_aps[i] = reg_base[14 + i];
    set_prop(LOCK_NODE, "aperture-phys-addr", unaligned_aps, sizeof(unaligned_aps));
    setup_fdt(sizeof(fdt_buf), "disabled", default_ranges, sizeof(default_ranges), true, false);
    dt_set_t6050_mcc();
    assert_still_disabled();

    return 0;
}
'''
    )

    cfile = tmp_path / "t6050_mcc_handoff.c"
    binary = tmp_path / "t6050_mcc_handoff"
    cfile.write_text(harness)

    cmd = [
        *host_cc(),
        "-std=gnu11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I",
        str(ROOT / "src/libfdt"),
        str(cfile),
        *(str(ROOT / "src/libfdt" / f) for f in LIBFDT),
        "-o",
        str(binary),
    ]
    subprocess.run(cmd, check=True)
    subprocess.run([str(binary)], check=True, capture_output=True)
