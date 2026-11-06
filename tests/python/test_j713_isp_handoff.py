# SPDX-License-Identifier: MIT
"""Exercise the J713 ISP firmware handoff with production kboot code and real FDTs."""

import shutil
import subprocess
from pathlib import Path

import pytest
from test_siliconforest_handoff import run_c

ROOT = Path(__file__).resolve().parents[2]

# The kernel's J713 camera description: the ISP and its DARTs disabled, empty
# firmware placeholders and the firmware's 20 MiB main memory window.
TREE = r'''/dts-v1/;
/ {
    compatible = "apple,j713", "apple,t8132";
    #address-cells = <2>; #size-cells = <2>;
    reserved-memory {
        #address-cells = <2>; #size-cells = <2>; ranges;
        isp_fw_text: isp-firmware-text { };
        isp_fw_data: isp-firmware-data { };
        isp_heap: isp-heap { iommu-addresses = <&isp 0x100 0x0 0x0 0x1400000>; };
    };
    soc {
        #address-cells = <2>; #size-cells = <2>; ranges;
        isp: isp@48c000000 {
            compatible = "apple,t8132-isp", "apple,isp";
            iommus = <&isp_dart0 0>, <&isp_dart1 0>, <&isp_dart2 0>;
            memory-region = <&isp_fw_text>, <&isp_fw_data>, <&isp_heap>;
            memory-region-names = "text", "data", "heap";
            status = "disabled";
        };
        isp_dart0: iommu@48e8c0000 { #iommu-cells = <1>; status = "disabled"; };
        isp_dart1: iommu@48e8e0000 { #iommu-cells = <1>; status = "disabled"; };
        isp_dart2: iommu@48e900000 { #iommu-cells = <1>; status = "disabled"; };
    };
};
'''


def test_j713_isp_handoff_fills_placeholders_or_leaves_the_camera_off(tmp_path):
    if not shutil.which("dtc"):
        pytest.skip("needs dtc")
    kboot = (ROOT / "src/kboot.c").read_text()

    def function(name, kind="static int "):
        start = kboot.index(kind + name + "(")
        return kboot[start:kboot.index("\n}", start) + 2] + "\n"

    header = (ROOT / "src/adt.h").read_text()
    start = header.index("struct adt_segment_ranges {")
    layout = header[start:header.index("} PACKED;", start) + len("} PACKED;")]
    dtb = tmp_path / "j713.dtb"
    subprocess.run(["dtc", "-q", "-I", "dts", "-O", "dtb", "-o", str(dtb), "-"],
                   input=TREE, text=True, check=True)
    run_c(tmp_path, "j713-isp-handoff", '#define DTB_PATH "%s"\n' % dtb + r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libfdt.h"
/* Match aura's aarch64-linux-gnu target, not the host libc's uint64_t alias. */
typedef unsigned long u64;
_Static_assert(sizeof(u64) == 8, "the aura host fixture requires an LP64 host");
typedef uint32_t u32;
#define PACKED __attribute__((packed))
#define T8132 0x8132
#define SZ_16K 0x4000
#define SZ_1M 0x100000
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define UNUSED(x) (void)(x)
#define bail(...) do { return -1; } while (0)
static void *dt, *adt;
static u32 chip_id = T8132;
static int fault;
static unsigned char blob[16384], original[16384];
''' + layout + r'''
static struct adt_segment_ranges segments[2];
static u64 dram_base, dram_size;
static u32 sensor;
static const char *names;
static int adt_path_offset(void *tree, const char *path)
{
    if (!strcmp(path, "/chosen")) return 2;
    if (!strcmp(path, "/arm-io/isp")) return 3;
    return -1;
}
static const void *adt_getprop(void *tree, int node, const char *name, u32 *len)
{
    if (node == 3 && !strcmp(name, "segment-names")) {
        *len = strlen(names) + 1;
        return names;
    }
    if (node == 3 && !strcmp(name, "segment-ranges")) {
        *len = sizeof(segments);
        return segments;
    }
    if (node == 3 && !strcmp(name, "sensor-type") && fault != 2) {
        *len = 4;
        return &sensor;
    }
    *len = 8;
    if (node == 2 && !strcmp(name, "dram-base")) return &dram_base;
    if (node == 2 && !strcmp(name, "dram-size")) return &dram_size;
    return NULL;
}
static int get_value(int node, const char *name, void *out, size_t capacity)
{
    u32 len = 0;
    const void *p = adt_getprop(adt, node, name, &len);
    if (!p || len > capacity) return -1;
    memcpy(out, p, len);
    return len;
}
#define ADT_GETPROP(a, n, k, v) get_value(n, k, v, sizeof(*(v)))
''' + function("dt_device_set_reserved_mem") + function("dt_display_transaction") +
        function("dt_set_j713_isp_one") + function("dt_set_j713_isp", "static void ") + r'''
static void load(const char *path)
{
    FILE *f = fopen(path, "rb");
    unsigned char raw[8192];
    size_t n;
    assert(f && (n = fread(raw, 1, sizeof(raw), f)) > 0);
    fclose(f);
    memset(blob, 0, sizeof(blob));
    assert(!fdt_open_into(raw, blob, sizeof(blob)));
    dt = blob;
}
static const char *status(const char *path)
{
    const char *s = fdt_getprop(dt, fdt_path_offset(dt, path), "status", NULL);
    return s ? s : "okay";
}
static void world(const char *path)
{
    load(path);
    chip_id = T8132;
    names = "__TEXT;__DATA";
    sensor = 0x17;
    /* This J713's ADT isp segment-ranges. */
    dram_base = 0x10000000000ULL;
    dram_size = 0x400000000ULL;
    segments[0] = (struct adt_segment_ranges){.phys = 0x10000d30000ULL, .iova = 0,
        .remap = 0x10000000000ULL, .size = 0xca4000, .unk = 1};
    segments[1] = (struct adt_segment_ranges){.phys = 0x10001f40000ULL, .iova = 0xca4000,
        .remap = 0x10000ca4000ULL, .size = 0x748000};
}
static void check_region(const char *path, u64 phys, u64 iova, u64 size)
{
    int node = fdt_path_offset(dt, path), len;
    const fdt64_t *reg = fdt_getprop(dt, node, "reg", &len);
    assert(reg && len == 16 && fdt64_ld(reg) == phys && fdt64_ld(reg + 1) == size);
    assert(fdt_getprop(dt, node, "no-map", &len) && !len);
    assert(!strcmp(status(path), "okay"));
    const fdt32_t *map = fdt_getprop(dt, node, "iommu-addresses", &len);
    u32 isp = fdt_get_phandle(dt, fdt_path_offset(dt, "/soc/isp@48c000000"));
    assert(map && len == 20 && fdt32_ld(map) == isp);
    assert((((u64)fdt32_ld(map + 1) << 32) | fdt32_ld(map + 2)) == iova);
    assert((((u64)fdt32_ld(map + 3) << 32) | fdt32_ld(map + 4)) == size);
}
int main(void)
{
    const char *darts[] = {"/soc/iommu@48e8c0000", "/soc/iommu@48e8e0000",
                           "/soc/iommu@48e900000"};
    int len;

    /* Handed over: both segments where the firmware runs them, the platform ID, enabled. */
    world(DTB_PATH);
    dt_set_j713_isp();
    check_region("/reserved-memory/isp-firmware-text", 0x10000d30000ULL, 0x10000000000ULL,
                 0xca4000);
    check_region("/reserved-memory/isp-firmware-data", 0x10001f40000ULL, 0x10000ca4000ULL,
                 0x748000);
    const fdt32_t *id = fdt_getprop(dt, fdt_path_offset(dt, "/soc/isp@48c000000"),
                                    "apple,platform-id", &len);
    assert(id && len == 4 && fdt32_ld(id) == 0x17);
    assert(!strcmp(status("/soc/isp@48c000000"), "okay"));
    for (int i = 0; i < 3; i++)
        assert(!strcmp(status(darts[i]), "okay"));
    const fdt32_t *heap = fdt_getprop(dt, fdt_path_offset(dt, "/reserved-memory/isp-heap"),
                                      "iommu-addresses", &len);
    assert(heap && len == 20 && fdt32_ld(heap + 4) == 0x1400000);
    assert(!fdt_getprop(dt, fdt_path_offset(dt, "/reserved-memory/isp-heap"), "reg", NULL));

    /* Every refusal leaves the tree exactly as it was: the camera stays off. */
    for (fault = 1; fault <= 12; fault++) {
        world(DTB_PATH);
        if (fault == 1) names = "__TEXT";
        if (fault == 3) segments[1].remap += SZ_16K; /* DATA not right above TEXT */
        if (fault == 4) dram_base = 0x10001000000ULL; /* TEXT below DRAM */
        if (fault == 5) segments[1].size = 0x800000; /* DATA past the heap top */
        if (fault == 6) segments[1].phys = 0x10000d34000ULL; /* overlapping segments */
        if (fault == 7) {
            fdt32_t window[5] = {0};
            int heap_node = fdt_path_offset(dt, "/reserved-memory/isp-heap");
            memcpy(window, fdt_getprop(dt, heap_node, "iommu-addresses", NULL), 20);
            window[1] = cpu_to_fdt32(0x200);
            assert(!fdt_setprop(dt, heap_node, "iommu-addresses", window, sizeof(window)));
        }
        if (fault == 8) {
            fdt64_t reg[2] = {cpu_to_fdt64(0x10000d30000ULL), cpu_to_fdt64(0xca4000)};
            assert(!fdt_setprop(dt, fdt_path_offset(dt, "/reserved-memory/isp-firmware-data"),
                                "reg", reg, sizeof(reg)));
        }
        if (fault == 9)
            assert(!fdt_setprop_string(dt, fdt_path_offset(dt, "/soc/isp@48c000000"),
                                       "memory-region-names", "text"));
        if (fault == 10) segments[0].phys += 0x1000; /* not 16K aligned */
        if (fault == 11) sensor = 0;
        if (fault == 12) dram_size = 0x400000; /* DRAM smaller than TEXT */
        memcpy(original, blob, sizeof(blob));
        dt_set_j713_isp();
        assert(!memcmp(original, blob, sizeof(blob)));
        assert(!strcmp(status("/soc/isp@48c000000"), "disabled"));
    }

    /* Trees that are not handoff targets are left alone: an enabled ISP (a kernel that
     * reserves its own firmware), no placeholders, another machine. */
    for (fault = 20; fault <= 22; fault++) {
        world(DTB_PATH);
        int isp = fdt_path_offset(dt, "/soc/isp@48c000000");
        if (fault == 20) assert(!fdt_setprop_string(dt, isp, "status", "okay"));
        if (fault == 21)
            assert(!fdt_del_node(dt, fdt_path_offset(dt, "/reserved-memory/isp-firmware-text")));
        if (fault == 22) chip_id = 0x8122;
        memcpy(original, blob, sizeof(blob));
        dt_set_j713_isp();
        assert(!memcmp(original, blob, sizeof(blob)));
    }
    puts("J713 ISP handoff verified");
    return 0;
}
''')
