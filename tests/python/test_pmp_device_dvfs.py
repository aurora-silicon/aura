# SPDX-License-Identifier: MIT
"""Derive PMP media request words from synthetic ADT topology and PMGR policy."""

from pathlib import Path

from test_siliconforest_handoff import run_c

ROOT = Path(__file__).resolve().parents[2]


def production_block():
    source = (ROOT / "src/kboot.c").read_text()
    start = source.index("/* PMP DEVICE DVFS START */")
    end = source.index("/* PMP DEVICE DVFS END */", start)
    return source[start:end]


def test_pmp_media_requests_are_derived_and_fail_soft(tmp_path):
    run_c(tmp_path, "pmp-device-dvfs", r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "libfdt.h"
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define PACKED __attribute__((packed))
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define bail(...) do { return -1; } while (0)

static unsigned char tree[16384];
static void *dt = tree, *adt;
static u8 ptd[32 * 3], soc[0x7c * 40], devices[48 * 12];
static u32 ptd_len, soc_len, devices_len;
static bool have_nub = true, have_pmgr = true;

static int adt_path_offset(void *unused, const char *path)
{
    (void)unused;
    if (!strcmp(path, "/arm-io/pmp/iop-pmp-nub"))
        return have_nub ? 1 : -1;
    if (!strcmp(path, "/arm-io/pmgr"))
        return have_pmgr ? 2 : -1;
    return -1;
}

static const void *adt_getprop(void *unused, int node, const char *name, u32 *len)
{
    (void)unused;
    const void *value = NULL;
    u32 bytes = 0;
    if (node == 1 && !strcmp(name, "ptd-range")) {
        value = ptd;
        bytes = ptd_len;
    } else if (node == 1 && !strcmp(name, "soc-device")) {
        value = soc;
        bytes = soc_len;
    } else if (node == 2 && !strcmp(name, "devices")) {
        value = devices;
        bytes = devices_len;
    }
    if (len)
        *len = bytes;
    return value;
}

static void put32(void *p, u32 value) { memcpy(p, &value, sizeof(value)); }

static void add_ptd(unsigned int record, u32 id, u32 base, u32 count, const char *name)
{
    u8 *p = ptd + 32 * record;
    put32(p, id); put32(p + 4, base); put32(p + 8, count);
    memcpy(p + 16, name, strlen(name) + 1);
    ptd_len = 32 * (record + 1);
}

static void add_soc(unsigned int record, u32 id, bool dvfs)
{
    u8 *p = soc + 0x7c * record;
    put32(p, id); put32(p + 0x2c, dvfs ? 2 : 0);
    soc_len = 0x7c * (record + 1);
}

static void add_perf(unsigned int record, u8 flags, u8 level, u8 domain,
                     u8 device, const char *name)
{
    u8 *p = devices + 48 * record;
    p[0] = flags; p[1] = level; p[2] = domain; p[3] = device;
    memcpy(p + 32, name, strlen(name) + 1);
    devices_len = 48 * (record + 1);
}

static int add_report(const char *unit, u32 reg, const char *label)
{
    int report = fdt_path_offset(dt, "/report");
    int node = fdt_add_subnode(dt, report, unit);
    assert(node >= 0);
    assert(!fdt_setprop_u32(dt, node, "reg", reg));
    assert(!fdt_setprop_string(dt, node, "label", label));
    assert(!fdt_setprop_u32(dt, node, "apple,dvfs-request-class", 0x25));
    assert(!fdt_setprop(dt, node, "apple,adt-dvfs", NULL, 0));
    return node;
}

static void world(void)
{
    memset(tree, 0, sizeof(tree)); memset(ptd, 0, sizeof(ptd));
    memset(soc, 0, sizeof(soc)); memset(devices, 0, sizeof(devices));
    ptd_len = soc_len = devices_len = 0; have_nub = have_pmgr = true;
    assert(!fdt_create_empty_tree(dt, sizeof(tree)));
    assert(fdt_add_subnode(dt, 0, "report") >= 0);
    add_ptd(0, 3, 8, 4, "DVFS-STATE");
    /* Synthetic numbers: slots 0x40.., DVFS owners at records 10..32 except 20. */
    add_ptd(1, 12, 0x40, 0x20, "SOC-DEV-DVFS");
    for (unsigned int i = 0; i < 40; i++)
        add_soc(i, i + 1, i >= 10 && i <= 32 && i != 20);
    add_perf(0, 0x12, 5, 0x20, 0x1d, "VENC-SOC-VMAX");
    add_perf(1, 0x12, 4, 0x21, 0x1d, "VENC-FAB0-VMAX");
    add_perf(2, 0x12, 4, 0x20, 0x1e, "AVD-SOC-VMAX");
    add_perf(3, 0x12, 6, 0x20, 0x20, "JPG-SOC-VNOM");
    add_perf(4, 0x12, 7, 0x20, 0x20, "JPG-SOC-VMAX");
    add_perf(5, 0x12, 6, 0x20, 0x20, "JPG1-SOC-VNOM");
    add_perf(6, 0x12, 2, 0x20, 0x1b, "MSR0-AVMSR-VMD2");
    add_perf(7, 0x12, 3, 0x20, 0x1b, "MSR0-AVMSR-VMAX");
    add_perf(8, 0x12, 1, 0x20, 0x1c, "SCODEC-SOC-VNOM");
    add_perf(9, 0x12, 2, 0x20, 0x1c, "SCODEC-SOC-VMAX");
}

static void cells(int node, const char *name, const u32 *expected, unsigned int count)
{
    int len;
    const fdt32_t *p = fdt_getprop(dt, node, name, &len);
    assert(p && len == (int)(count * sizeof(*p)));
    for (unsigned int i = 0; i < count; i++)
        assert(fdt32_ld(p + i) == expected[i]);
}
''' + production_block() + r'''
int main(void)
{
    world();
    int venc = add_report("report@1c", 0x1c, "pmp-venc-sys");
    int avd = add_report("report@1d", 0x1d, "pmp-avd-sys");
    int jpg = add_report("report@1f", 0x1f, "pmp-jpg");
    int msr = add_report("report@1a", 0x1a, "pmp-msr0");
    int scodec = add_report("report@1b", 0x1b, "pmp-scodec");
    /* An entry that does not ask for ADT data keeps its own properties. */
    int other = add_report("report@1e", 0x1d, "pmp-avd-sys");
    assert(!fdt_delprop(dt, other, "apple,adt-dvfs"));
    assert(!dt_set_pmp_device_dvfs());
    const u32 venc_dvfs[] = {0x51, 5}, venc_levels[] = {0x21, 5};
    const u32 avd_dvfs[] = {0x52, 0}, avd_levels[] = {0x21, 4};
    const u32 jpg_dvfs[] = {0x54, 0}, jpg_levels[] = {0x21, 6};
    /* Only the named VMAX sources: the scaler's VMD2 and the codec's VNOM are ignored. */
    const u32 msr_dvfs[] = {0x4f, 0}, msr_levels[] = {0x21, 3};
    const u32 scodec_dvfs[] = {0x50, 0}, scodec_levels[] = {0x21, 2};
    venc = fdt_path_offset(dt, "/report/report@1c");
    avd = fdt_path_offset(dt, "/report/report@1d");
    jpg = fdt_path_offset(dt, "/report/report@1f");
    msr = fdt_path_offset(dt, "/report/report@1a");
    scodec = fdt_path_offset(dt, "/report/report@1b");
    other = fdt_path_offset(dt, "/report/report@1e");
    assert(!fdt_getprop(dt, other, "apple,dvfs", NULL));
    assert(!fdt_getprop(dt, other, "apple,dvfs-request-levels", NULL));
    assert(fdt_getprop(dt, other, "apple,dvfs-request-class", NULL));
    cells(venc, "apple,dvfs", venc_dvfs, ARRAY_SIZE(venc_dvfs));
    cells(venc, "apple,dvfs-request-levels", venc_levels, ARRAY_SIZE(venc_levels));
    cells(avd, "apple,dvfs", avd_dvfs, ARRAY_SIZE(avd_dvfs));
    cells(avd, "apple,dvfs-request-levels", avd_levels, ARRAY_SIZE(avd_levels));
    cells(jpg, "apple,dvfs", jpg_dvfs, ARRAY_SIZE(jpg_dvfs));
    cells(jpg, "apple,dvfs-request-levels", jpg_levels, ARRAY_SIZE(jpg_levels));
    cells(msr, "apple,dvfs", msr_dvfs, ARRAY_SIZE(msr_dvfs));
    cells(msr, "apple,dvfs-request-levels", msr_levels, ARRAY_SIZE(msr_levels));
    cells(scodec, "apple,dvfs", scodec_dvfs, ARRAY_SIZE(scodec_dvfs));
    cells(scodec, "apple,dvfs-request-levels", scodec_levels, ARRAY_SIZE(scodec_levels));
    assert(!fdt_getprop(dt, venc, "apple,dvfs-request-class", NULL));

    /* The compact PMGR class encoding used by T8112 maps 0x52 to class 0x25. */
    world(); add_perf(0, 0x52, 9, 1, 0x1d, "VENC-AVE-VMAX");
    venc = add_report("report@1c", 0x1c, "pmp-venc-sys");
    assert(!dt_set_pmp_device_dvfs());
    const u32 compact_dvfs[] = {0x51, 0}, compact_levels[] = {0x25, 9};
    venc = fdt_path_offset(dt, "/report/report@1c");
    cells(venc, "apple,dvfs", compact_dvfs, ARRAY_SIZE(compact_dvfs));
    cells(venc, "apple,dvfs-request-levels", compact_levels, ARRAY_SIZE(compact_levels));

    /* Every absent or malformed ADT input leaves existing DT policy untouched. */
    for (unsigned int failure = 0; failure < 9; failure++) {
        world(); venc = add_report("report@1c", 0x1c, "pmp-venc-sys");
        const u32 old[] = {0xabc, 7};
        fdt32_t old_be[] = {cpu_to_fdt32(old[0]), cpu_to_fdt32(old[1])};
        assert(!fdt_setprop(dt, venc, "apple,dvfs", old_be, sizeof(old_be)));
        switch (failure) {
        case 0: have_nub = false; break;
        case 1: have_pmgr = false; break;
        case 2: ptd_len--; break;
        case 3: soc_len--; break;
        case 4: devices_len--; break;
        case 5: put32(soc + 0x7c * 28 + 0x2c, 0); break;
        case 6: devices[1] = 0x20; break;
        case 7: put32(ptd + 32 + 4, UINT32_MAX - 3); break;
        case 8: memcpy(ptd + 64, ptd + 32, 32); ptd_len = 96; break;
        }
        assert(!dt_set_pmp_device_dvfs());
        venc = fdt_path_offset(dt, "/report/report@1c");
        cells(venc, "apple,dvfs", old, ARRAY_SIZE(old));
        assert(fdt_getprop(dt, venc, "apple,dvfs-request-class", NULL));
        assert(!fdt_getprop(dt, venc, "apple,dvfs-request-levels", NULL));
    }
    puts("PASS ADT-derived PMP media DVFS entries, named levels, classes and fail-soft handoff");
    return 0;
}
''')
