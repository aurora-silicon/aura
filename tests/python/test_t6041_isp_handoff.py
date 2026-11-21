# SPDX-License-Identifier: MIT
"""Exercise property-driven ISP power-up skip, placeholder segment fill, and DAPF sync."""

import shutil
import subprocess
from pathlib import Path

import pytest
from test_siliconforest_handoff import run_c

ROOT = Path(__file__).resolve().parents[2]

TREE = r'''/dts-v1/;
/ {
    compatible = "apple,j616c", "apple,t6041";
    #address-cells = <2>; #size-cells = <2>;
    aliases { isp = &isp; };
    reserved-memory {
        #address-cells = <2>; #size-cells = <2>; ranges;
        isp_fw_text: isp-firmware-text@10000c44000 {
            reg = <0x100 0x00c44000 0x0 0xca8000>;
            iommu-addresses = <&isp 0x100 0x0 0x0 0xca8000>;
            no-map;
        };
        isp_fw_data: isp-firmware-data@10001e7c000 {
            reg = <0x100 0x01e7c000 0x0 0x1344000>;
            iommu-addresses = <&isp 0x100 0xca8000 0x0 0x1344000>;
            no-map;
        };
        isp_heap: isp-heap { iommu-addresses = <&isp 0x100 0x0 0x0 0x2000000>; };
    };
    soc {
        #address-cells = <2>; #size-cells = <2>; ranges;
        isp: isp@580000000 {
            compatible = "apple,t6041-isp", "apple,isp";
            iommus = <&isp_dart0 0>, <&isp_dart1 0>, <&isp_dart2 0>;
            memory-region = <&isp_fw_text>, <&isp_fw_data>, <&isp_heap>;
            memory-region-names = "text", "data", "heap";
            status = "okay";
        };
        isp_dart0: iommu@5828c0000 {
            #iommu-cells = <1>;
            apple,dapf-ranges = /bits/ 64 <0x502284000 0x50228402b>;
            status = "okay";
        };
        isp_dart1: iommu@5828e0000 { #iommu-cells = <1>; status = "okay"; };
        isp_dart2: iommu@582900000 { #iommu-cells = <1>; status = "okay"; };
    };
};
'''


def test_isp_placeholder_handoff_dapf_sync_and_fdt_power_gate(tmp_path):
    if not shutil.which("dtc"):
        pytest.skip("needs dtc")
    kboot = (ROOT / "src/kboot.c").read_text()
    isp_src = (ROOT / "src/isp.c").read_text()
    dapf_src = (ROOT / "src/dapf.c").read_text()
    dapf_hdr = (ROOT / "src/dapf.h").read_text()

    def kboot_fn(sig):
        start = kboot.index(sig)
        return kboot[start:kboot.index("\n}", start) + 2] + "\n"

    needs_init = isp_src[isp_src.index("bool isp_fdt_needs_init("):isp_src.index("#define ISP_ASC_VERSION")]
    t8110_cfg = dapf_src[dapf_src.index("struct dapf_t8110_config {"):dapf_src.index("struct dapf_t8110b_config {")]
    dapf_slices = dapf_src[dapf_src.index("static const struct dapf_t8110_config *dapf_t8110_slices("):
                           dapf_src.index("/*\n * Program a genuine T8132")]
    dapf_ranges = dapf_src[dapf_src.index("int dapf_t8110_ranges("):
                           dapf_src.index("static bool dapf_is_j614s(")]
    window_def = dapf_hdr[dapf_hdr.index("struct dapf_window {"):dapf_hdr.index("#endif")]

    header = (ROOT / "src/adt.h").read_text()
    start = header.index("struct adt_segment_ranges {")
    layout = header[start:header.index("} PACKED;", start) + len("} PACKED;")]
    dtb = tmp_path / "j616c.dtb"
    subprocess.run(["dtc", "-q", "-I", "dts", "-O", "dtb", "-o", str(dtb), "-"],
                   input=TREE, text=True, check=True)
    run_c(tmp_path, "isp-placeholder-handoff", '#define DTB_PATH "%s"\n' % dtb + r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libfdt.h"
typedef unsigned long u64;
typedef uint32_t u32;
typedef uint8_t u8;
#define PACKED __attribute__((packed))
#define static_assert _Static_assert
#define T6030 0x6030
#define T6040 0x6040
#define T6041 0x6041
#define T6050 0x6050
#define T8132 0x8132
#define T8140 0x8140
#define T8142 0x8142
#define V13_5 0
#define V13_6_1 1
#define V26_6_2 2
#define SZ_16K 0x4000
#define SZ_1M 0x100000
#define ISP_FW_WINDOW_MAX (64 * SZ_1M)
#define T8132_DAPF_SLICES 16
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
static char log_buf[8192];
#define printf(...) snprintf(log_buf + strlen(log_buf), sizeof(log_buf) - strlen(log_buf), __VA_ARGS__)
#define bail(...) do { printf(__VA_ARGS__); return -1; } while (0)
struct fw_version_info { int version; };
static struct fw_version_info os_firmware = {V26_6_2}, fw_versions[3];
static int firmware_os_components(void) { return os_firmware.version; }
static void *dt, *adt;
static u32 chip_id = T6041, board_id = 6;
static unsigned char blob[32768];
''' + window_def + layout + t8110_cfg + r'''
static struct adt_segment_ranges segments[2];
static const char *seg_names;
static u64 dram_base, dram_size, dart_vm_base;
static struct dapf_t8110_config dapf_records[17];
static u32 dapf_len;
static bool adt_is_compatible(const void *tree, int node, const char *compat)
{ (void)tree; (void)node; (void)compat; return false; }
static int firmware_set_fdt(void *tree, int node, const char *name,
                            const struct fw_version_info *version)
{ (void)version; return fdt_setprop_u32(tree, node, name, 27); }
static u64 isp_iova_base(void) { return 0x10000000000ULL; }
static int isp_get_heap(u64 *p, u64 *i, u64 *s) { (void)p; (void)i; (void)s; return -1; }
static int dt_t6050_isp_transaction(void) { abort(); }
static int dt_reserve_asc_firmware(const char *a, const char *b, const char *c, bool d, u64 e)
{ (void)a; (void)b; (void)c; (void)d; (void)e; abort(); }
static int dt_get_or_add_reserved_mem(const char *a, const char *b, bool c, u64 d, size_t e)
{ (void)a; (void)b; (void)c; (void)d; (void)e; abort(); }
static int dt_device_set_reserved_mem(int a, const char *b, u32 c, u64 d, size_t e)
{ (void)a; (void)b; (void)c; (void)d; (void)e; abort(); }
static int adt_path_offset(const void *tree, const char *path)
{
    (void)tree;
    if (!strcmp(path, "/chosen")) return 2;
    if (!strcmp(path, "/arm-io/isp0")) return 3;
    if (!strcmp(path, "/arm-io/dart-isp0")) return 4;
    return -1;
}
static const void *adt_getprop(const void *tree, int node, const char *name, u32 *len)
{
    (void)tree;
    if (node == 2 && !strcmp(name, "dram-base")) { *len = 8; return &dram_base; }
    if (node == 2 && !strcmp(name, "dram-size")) { *len = 8; return &dram_size; }
    if (node == 3 && !strcmp(name, "segment-names")) { *len = strlen(seg_names) + 1; return seg_names; }
    if (node == 3 && !strcmp(name, "segment-ranges")) { *len = sizeof(segments); return segments; }
    if (node == 4 && !strcmp(name, "vm-base")) { *len = 8; return &dart_vm_base; }
    if (node == 4 && !strcmp(name, "dapf-instance-0")) { *len = dapf_len; return dapf_records; }
    return NULL;
}
static int get_value(int node, const char *name, void *out, size_t cap)
{
    u32 len = 0;
    const void *p = adt_getprop(adt, node, name, &len);
    if (!p || len > cap) return -1;
    memcpy(out, p, len);
    return len;
}
#define ADT_GETPROP(a, n, k, v) get_value(n, k, v, sizeof(*(v)))
/* As in src/isp.c: an ISP17a is an ADT isp node compatible with isp,isp-generic-ex */
static bool isp_is_isp17a(void)
{
    int node = adt_path_offset(adt, "/arm-io/isp");
    if (node < 0)
        node = adt_path_offset(adt, "/arm-io/isp0");
    return node >= 0 && adt_is_compatible(adt, node, "isp,isp-generic-ex");
}
''' + needs_init + dapf_slices + dapf_ranges + kboot_fn("static bool dt_isp_has_placeholders(") +
        kboot_fn("static int dt_fill_isp_placeholders(") + kboot_fn("static int dt_set_isp_fwdata(") +
        kboot_fn("static int dt_set_isp(") + kboot_fn("static int dt_finish_j614s_isp(") + r'''
static void setup(unsigned int amccs)
{
    FILE *f = fopen(DTB_PATH, "rb");
    unsigned char raw[8192];
    size_t n = fread(raw, 1, sizeof(raw), f);
    fclose(f);
    assert(n > 0);
    memset(blob, 0, sizeof(blob));
    assert(!fdt_open_into(raw, blob, sizeof(blob)));
    dt = blob;
    log_buf[0] = 0;
    chip_id = T6041;
    dram_base = 0x10000000000ULL;
    dram_size = 0x1000000000ULL;
    dart_vm_base = 0x10000000000ULL;
    seg_names = "__TEXT;__DATA";
    segments[0] = (struct adt_segment_ranges){
        .phys = 0x10000dd4000ULL, .iova = 0, .remap = 0x10000000000ULL, .size = 0xbf0000, .unk = 1
    };
    segments[1] = (struct adt_segment_ranges){
        .phys = 0x10002020000ULL, .iova = 0xbf0000, .remap = 0x10000bf0000ULL, .size = 0x13fc000
    };
    u32 count = 7 + amccs;
    memset(dapf_records, 0, sizeof(dapf_records));
    for (unsigned int i = 0; i < count; i++) {
        u64 s = i < 7 ? (0x502284000ULL + i * 0x4000) : (0x220004000ULL + (i - 7) * 0x2000000ULL);
        u64 e = i < 7 ? (s + 0x2b) : (s + 0x1d22fcULL);
        dapf_records[i].start = s;
        dapf_records[i].end = e;
    }
    dapf_len = count * sizeof(struct dapf_t8110_config);
}
int main(void)
{
    /* 1. FDT property-driven isp_init() gate: skips kernel-managed/placeholder trees on T6040/T6041/T6050. */
    setup(8);
    assert(!isp_fdt_needs_init(dt));
    int resv = fdt_path_offset(dt, "/reserved-memory");
    assert(!fdt_del_node(dt, fdt_subnode_offset(dt, resv, "isp-firmware-text")));
    resv = fdt_path_offset(dt, "/reserved-memory");
    assert(!fdt_del_node(dt, fdt_subnode_offset(dt, resv, "isp-firmware-data")));
    /* Even without placeholders, apple,t6041-isp / apple,t6050-isp / apple,t8132-isp skip isp_init(). */
    assert(!isp_fdt_needs_init(dt));
    assert(!fdt_setprop_string(dt, fdt_path_offset(dt, "isp"), "compatible", "apple,t6050-isp"));
    assert(!isp_fdt_needs_init(dt));
    /* A legacy/H17 enabled ISP without placeholders DOES run isp_init(), unless disabled. */
    assert(!fdt_setprop_string(dt, fdt_path_offset(dt, "isp"), "compatible", "apple,t8140-isp"));
    assert(isp_fdt_needs_init(dt));
    assert(!fdt_setprop_string(dt, fdt_path_offset(dt, "isp"), "status", "disabled"));
    assert(!isp_fdt_needs_init(dt));

    /* 2. Full 8-AMCC T6041: 3 memory-regions (no asc-firmware duplicates), 15 DAPF pairs. */
    setup(8);
    assert(dt_set_isp() == 0);
    int isp = fdt_path_offset(dt, "isp"), len = 0;
    assert(fdt_getprop(dt, isp, "memory-region", &len) && len == 3 * 4);
    int dart = fdt_path_offset(dt, "/soc/iommu@5828c0000");
    const fdt64_t *ranges = fdt_getprop(dt, dart, "apple,dapf-ranges", &len);
    assert(ranges && len == 15 * 16);
    assert(fdt64_ld(&ranges[28]) == 0x22e004000ULL && fdt64_ld(&ranges[29]) == 0x22e1d62fcULL);

    resv = fdt_path_offset(dt, "/reserved-memory");
    int tnode = fdt_subnode_offset(dt, resv, "isp-firmware-text");
    const fdt64_t *reg = fdt_getprop(dt, tnode, "reg", &len);
    assert(reg && len == 16 && fdt64_ld(reg) == 0x10000dd4000ULL && fdt64_ld(reg + 1) == 0xbf0000);

    /* 3. T6040 (e.g. J616s) takes the exact same placeholder path without chip_id == T6041 gating. */
    setup(6);
    chip_id = T6040;
    assert(dt_set_isp() == 0);
    dart = fdt_path_offset(dt, "/soc/iommu@5828c0000");
    assert(fdt_getprop(dt, dart, "apple,dapf-ranges", &len) && len == 13 * 16);
    assert(!strcmp(fdt_getprop(dt, fdt_path_offset(dt, "isp"), "status", NULL), "okay"));

    /* 4. J614s disabled-at-boot flow shares dt_fill_isp_placeholders() and enables ISP + 3 DARTs. */
    setup(6);
    chip_id = T6040;
    assert(!fdt_setprop_string(dt, fdt_path_offset(dt, "isp"), "status", "disabled"));
    assert(!fdt_setprop_string(dt, fdt_path_offset(dt, "/soc/iommu@5828c0000"), "status", "disabled"));
    assert(dt_finish_j614s_isp(true) == 0);
    assert(!strcmp(fdt_getprop(dt, fdt_path_offset(dt, "isp"), "status", NULL), "okay"));
    assert(!strcmp(fdt_getprop(dt, fdt_path_offset(dt, "/soc/iommu@5828c0000"), "status", NULL), "okay"));

    /* 5. Malformed dapf-instance-0 logs an error and fails closed (disables ISP + placeholders). */
    setup(8);
    dapf_len = sizeof(struct dapf_t8110_config) - 2;
    assert(dt_set_isp() == 0);
    assert(strstr(log_buf, "dapf: Invalid dapf-instance-0") != NULL);
    assert(strstr(log_buf, "refusing stale apple,dapf-ranges") != NULL);
    assert(!strcmp(fdt_getprop(dt, fdt_path_offset(dt, "isp"), "status", NULL), "disabled"));
    resv = fdt_path_offset(dt, "/reserved-memory");
    assert(!strcmp(fdt_getprop(dt, fdt_subnode_offset(dt, resv, "isp-firmware-text"), "status", NULL), "disabled"));

    /* 6. Overlapping or out-of-window segments disable ISP and both placeholders. */
    setup(8);
    segments[1].phys = segments[0].phys + 0x4000;
    assert(dt_set_isp() == 0);
    assert(!strcmp(fdt_getprop(dt, fdt_path_offset(dt, "isp"), "status", NULL), "disabled"));

    setup(8);
    segments[1].remap = dart_vm_base + 0x1ff0000ULL;
    assert(dt_set_isp() == 0);
    assert(!strcmp(fdt_getprop(dt, fdt_path_offset(dt, "isp"), "status", NULL), "disabled"));

    puts("PASS ISP placeholder handoff");
    return 0;
}
''')
