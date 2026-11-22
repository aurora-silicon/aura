# SPDX-License-Identifier: MIT
"""Run the M4 internal DCP handoff, with production kboot code and real FDTs, per board.

Each board's DCP segment list is the one its ADT carries (the J713 on 26.5.1, the J604 and
the J773g on 26.6.2, the J715 on 27.0). The handed-over trees are written out as DTBs.
"""

import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def function(source, name, kind="static int "):
    start = source.index(kind + name + "(")
    return source[start:source.index("\n}", start) + 2] + "\n"


def test_m4_dcp_handoff_follows_the_firmware_and_the_panel(tmp_path):
    kboot = (ROOT / "src/kboot.c").read_text()
    firmware = (ROOT / "src/firmware.c").read_text()
    header = (ROOT / "src/adt.h").read_text()
    start = header.index("struct adt_segment_ranges {")
    layout = header[start:header.index("} PACKED;", start) + len("} PACKED;")]
    # The clock publisher is declared ahead of the handoff; take its definition.
    start = kboot.index("static int dt_set_dcp_fw27_clocks(const char *alias, const char *adt_engine)\n{")
    clocks = kboot[start:kboot.index("\n}", start) + 2] + "\n"
    start = kboot.index("struct j713_display_maps {")
    maps = kboot[start:kboot.index("\n};", start) + 3] + "\n"
    production = (
        function(kboot, "dt_get_iommu_node") + function(kboot, "dt_device_set_reserved_mem") +
        function(kboot, "dt_get_or_add_reserved_mem") +
        function(kboot, "dt_device_add_mem_region") + function(kboot, "dt_set_dcp_firmware") +
        function(kboot, "dt_reserve_asc_segments_at") + function(kboot, "dt_reserve_asc_segments") + function(kboot, "dt_reserve_asc_firmware") +
        maps + function(kboot, "j713_display_page") + function(kboot, "dt_set_j713_display_maps") +
        function(kboot, "dt_dcp_fw_clocks", "static bool ") + clocks +
        function(kboot, "dt_set_m4_dcp_handoff"))
    production = function(firmware, "firmware_set_fdt", "int ") + production
    header = (ROOT / "src/kboot.h").read_text()
    start = header.index("enum dcp_internal {")
    enum = header[start:header.index("\n};", start) + 3] + "\n"
    assert not re.search(r"\bdt_set_j713_dcp_handoff\b", kboot)

    source = r'''
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
typedef uint8_t u8;
#define PACKED __attribute__((packed))
#define T6030 0x6030
#define T8132 0x8132
#define T8122 0x8122
#define SZ_16K 0x4000
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((u64)(a) - 1))
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define bail(...) do { printf(__VA_ARGS__); return -1; } while (0)
static int dt_device_dart_va_bits(int fdt_node, int adt_node, u32 *bits) { (void)fdt_node; (void)adt_node; (void)bits; return -1; }
enum fw_version { V12_3, V12_3_1, V12_4, V13_5B4, V13_5, V13_6_1, V14_7, V26_5, V26_6, V26_6_2,
                  V27_0, V27_2 };
struct fw_version_info {
    enum fw_version version;
    const char *string;
    u32 num[3];
    size_t num_length;
    const char *iboot;
};
static const struct fw_version_info fw_versions[] = {
    [V12_3] = {V12_3, "12.3", {12, 3, 0}, 2, ""},
    [V13_5] = {V13_5, "13.5", {13, 5, 0}, 2, ""},
    [V14_7] = {V14_7, "14.7", {14, 7, 0}, 2, ""},
    [V26_5] = {V26_5, "26.5", {26, 5, 0}, 2, ""},
    [V26_6] = {V26_6, "26.6", {26, 6, 0}, 3, ""},
    [V26_6_2] = {V26_6_2, "26.6.2", {26, 6, 2}, 3, ""},
    [V27_0] = {V27_0, "27.0", {27, 0, 0}, 2, ""},
    [V27_2] = {V27_2, "27.2", {27, 2, 0}, 3, ""},
};
static struct fw_version_info os_firmware;
''' + enum + layout + r'''
static void *dt, *adt;
static u32 chip_id = T8132;

/* The ADT: /chosen DRAM bounds, the DCP nub and /arm-io/disp0. */
struct board {
    const char *name, *uuid;
    u64 dram_size;
    int external; /* /arm-io/disp0: 0 internal, 1 "external" present, -1 no node */
    bool native;  /* the DT asks for PIODMA and scanout (t8132-laptop.dtsi) */
    unsigned count;
    struct adt_segment_ranges segs[7];
};
#define FW2662 "81BAF476-679D-3CC0-8C6A-E6B6FA659D5E"
#define SEG(p, i, r, s, f) {.phys = p, .iova = i, .remap = r, .size = s, .unk = f}
#define NONE 0xffffffffffffffffUL
static const struct board j713 = {"j713", FW2662, 16UL << 30, 0, true, 7, { /* ADT 26.5.1 */
    SEG(0x10000298000, 0, 0x10005b8c000, 0x806000, 1),
    SEG(0x103e187c000, 0x806000, 0x10006394000, 0x325000, 0),
    SEG(0x10004304000, 0xb2b000, 0x10004304000, 0x24000, 10),
    SEG(0x103e5c20000, NONE, 0x10000b30000, 0x1000000, 2),
    SEG(0x103e5c1c000, NONE, 0x10001b58000, 0x4000, 2),
    SEG(0x103e5b5c000, NONE, 0x10001b5c000, 0xc0000, 2),
    SEG(0x103e1bec000, NONE, 0x10001c1c000, 0x3f70000, 2)}};
static const struct board j604 = {"j604", FW2662, 16UL << 30, 0, true, 7, { /* ADT 26.6.2 */
    SEG(0x100002a8000, 0, 0x10007770000, 0x806000, 1),
    SEG(0x103dfcdc000, 0x806000, 0x10007f78000, 0x325000, 0),
    SEG(0x10006084000, 0xb2b000, 0x10006084000, 0x24000, 10),
    SEG(0x103e5c1c000, NONE, 0x10000b70000, 0x1000000, 2),
    SEG(0x103e58b8000, NONE, 0x10001ba0000, 0x364000, 2),
    SEG(0x103e57f8000, NONE, 0x10001f04000, 0xc0000, 2),
    SEG(0x103e004c000, NONE, 0x10001fc4000, 0x57ac000, 2)}};
static const struct board j715 = {"j715", FW2662, 24UL << 30, 0, true, 7, { /* ADT 27.0 */
    SEG(0x100002a8000, 0, 0x10006c04000, 0x842000, 1),
    SEG(0x105e081c000, 0x842000, 0x10007448000, 0x32c000, 0),
    SEG(0x1000c030000, 0xb6e000, 0x1000c030000, 0x26000, 10),
    SEG(0x105e5c1c000, NONE, 0x10000b70000, 0x1000000, 2),
    SEG(0x105e5c18000, NONE, 0x10001b98000, 0x4000, 2),
    SEG(0x105e5b58000, NONE, 0x10001b9c000, 0xc0000, 2),
    SEG(0x105e0bb0000, NONE, 0x10001c5c000, 0x4fa8000, 2)}};
/* The Mac mini: disp0 is "external" (4096x2160, DP switch), and there is no panel segment. */
static const struct board j773g = {"j773g", FW2662, 16UL << 30, 1, false, 6, { /* ADT 26.6.2 */
    SEG(0x100002a8000, 0, 0x10001c4c000, 0x806000, 1),
    SEG(0x103e4f94000, 0x806000, 0x10002454000, 0x325000, 0),
    SEG(0x10003f0c000, 0xb2b000, 0x10003f0c000, 0x24000, 10),
    SEG(0x103e5c1c000, NONE, 0x10000b70000, 0x1000000, 2),
    SEG(0x103e5c18000, NONE, 0x10001b88000, 0x4000, 2),
    SEG(0x103e5b58000, NONE, 0x10001b8c000, 0xc0000, 2)}};

static struct board b;
static u64 dram_base = 0x10000000000UL;
static const u32 disp0_clock_ids[] = {364, 431};
static u32 clock_hz[200] = {[364 - 256] = 300000000, [431 - 256] = 0};
static const u32 one = 1;
static int walks;
static u64 dart_skew; /* added to every inherited page's physical address */
static int adt_path_offset(void *tree, const char *path)
{
    if (!strcmp(path, "/chosen")) return 1;
    if (!strcmp(path, "/arm-io/dcp/iop-dcp-nub")) return 2;
    if (!strcmp(path, "/arm-io/disp0")) return b.external < 0 ? -1 : 3;
    if (!strcmp(path, "/arm-io")) return 4;
    return -1;
}
static const void *adt_getprop(void *tree, int node, const char *name, u32 *len)
{
    u32 ignored;
    if (!len) len = &ignored;
    if (node == 1 && !strcmp(name, "dram-base")) { *len = 8; return &dram_base; }
    if (node == 1 && !strcmp(name, "dram-size")) { *len = 8; return &b.dram_size; }
    if (node == 2 && !strcmp(name, "segment-ranges")) {
        *len = b.count * sizeof(b.segs[0]);
        return b.segs;
    }
    if (node == 2 && !strcmp(name, "uuid")) { *len = strlen(b.uuid) + 1; return b.uuid; }
    if (node == 3 && !strcmp(name, "external") && b.external > 0) { *len = 4; return &one; }
    /* /arm-io/disp0 clock-ids index /arm-io clock-frequencies from 256 (every M4 ADT read). */
    if (node == 3 && !strcmp(name, "clock-ids")) { *len = sizeof(disp0_clock_ids); return disp0_clock_ids; }
    if (node == 4 && !strcmp(name, "clock-frequencies")) { *len = sizeof(clock_hz); return clock_hz; }
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

/*
 * The inherited display DART: SID0 maps the first and third display segments (flags 2), SID16
 * the second, page by page at their remap addresses.
 */
static int dart_walk_locked_t8110(uintptr_t base, u64 size, u8 sid, u64 dram_start,
                                  u64 dram_len, int (*visit)(u64, u64, void *), void *opaque)
{
    assert(base == 0x30a300000 && size == 0xc000 && (sid == 0 || sid == 16));
    walks++;
    for (unsigned i = 0, n = 0; i < b.count; i++) {
        if (b.segs[i].unk != 2) continue;
        bool mapped = sid == 0 ? n == 0 || n == 2 : n == 1;
        n++;
        if (!mapped) continue;
        for (u64 at = 0; at < ALIGN_UP((u64)b.segs[i].size, SZ_16K); at += SZ_16K)
            if (visit((b.segs[i].remap & ((1UL << 36) - 1)) + at, b.segs[i].phys + at + dart_skew,
                      opaque))
                return -1;
    }
    return 0;
}
''' + production + r'''
static unsigned char blob[1 << 16];
static const char *outdir;
static int phandle_of(const char *path) { return fdt_get_phandle(dt, fdt_path_offset(dt, path)); }
static void world(const struct board *board, enum fw_version fw)
{
    b = *board; walks = 0; dart_skew = 0; chip_id = T8132;
    os_firmware = fw_versions[fw];
    dt = blob;
    memset(blob, 0, sizeof(blob));
    assert(!fdt_create_empty_tree(dt, sizeof(blob)));
    char compat[64];
    int n = snprintf(compat, sizeof(compat), "apple,%s%capple,t8132%capple,arm-platform",
                     b.name, 0, 0);
    assert(!fdt_setprop(dt, 0, "compatible", compat, n + 1));
    assert(fdt_add_subnode(dt, 0, "reserved-memory") >= 0);
    int soc = fdt_add_subnode(dt, 0, "soc");
    int dcp_dart = fdt_add_subnode(dt, soc, "iommu@30a340000");
    assert(!fdt_setprop_u32(dt, dcp_dart, "phandle", 1));
    assert(!fdt_setprop_string(dt, dcp_dart, "status", "disabled"));
    int disp_dart = fdt_add_subnode(dt, fdt_path_offset(dt, "/soc"), "iommu@30a300000");
    assert(!fdt_setprop_u32(dt, disp_dart, "phandle", 2));
    assert(!fdt_setprop_string(dt, disp_dart, "status", "disabled"));
    int dcp = fdt_add_subnode(dt, fdt_path_offset(dt, "/soc"), "dcp@30ae00000");
    fdt32_t iommus[2] = {cpu_to_fdt32(1), cpu_to_fdt32(23)};
    assert(!fdt_setprop_string(dt, dcp, "compatible", "apple,t8132-dcp"));
    assert(!fdt_setprop(dt, dcp, "iommus", iommus, sizeof(iommus)));
    assert(!fdt_setprop_string(dt, dcp, "status", "disabled"));
    if (b.native) {
        assert(!fdt_setprop_empty(dt, dcp, "apple,j713-native-piodma"));
        assert(!fdt_setprop_empty(dt, dcp, "apple,j713-native-scanout"));
    }
    int piodma = fdt_add_subnode(dt, fdt_path_offset(dt, "/soc/dcp@30ae00000"), "piodma");
    iommus[0] = cpu_to_fdt32(2); iommus[1] = cpu_to_fdt32(16);
    assert(!fdt_setprop(dt, piodma, "iommus", iommus, sizeof(iommus)));
    assert(!fdt_setprop_string(dt, piodma, "status", "disabled"));
    int display = fdt_add_subnode(dt, fdt_path_offset(dt, "/soc"), "display-subsystem");
    static const char display_compat[] =
        "apple,t8132-display-subsystem\0apple,t8132-display-diagnostics";
    assert(!fdt_setprop(dt, display, "compatible", display_compat, sizeof(display_compat)));
    iommus[1] = cpu_to_fdt32(0);
    assert(!fdt_setprop(dt, display, "iommus", iommus, sizeof(iommus)));
    assert(!fdt_setprop_string(dt, display, "status", "disabled"));
    int aliases = fdt_add_subnode(dt, 0, "aliases");
    assert(!fdt_setprop_string(dt, aliases, "dcp", "/soc/dcp@30ae00000"));
    assert(!fdt_setprop_string(dt, aliases, "disp0", "/soc/display-subsystem"));
    assert(!fdt_setprop_string(dt, aliases, "disp0_piodma", "/soc/dcp@30ae00000/piodma"));
}
static void save(const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.dtb", outdir, name);
    FILE *f = fopen(path, "wb");
    assert(f && fwrite(blob, 1, fdt_totalsize(blob), f) == fdt_totalsize(blob) && !fclose(f));
}
static bool status_is(const char *path, const char *want)
{
    const char *status = fdt_getprop(dt, fdt_path_offset(dt, path), "status", NULL);
    return status && !strcmp(status, want);
}
static void version_is(const char *name, const u32 *num, size_t count)
{
    int len = 0;
    const fdt32_t *v = fdt_getprop(dt, fdt_path_offset(dt, "dcp"), name, &len);
    assert(v && len == (int)(count * 4));
    for (size_t i = 0; i < count; i++) assert(fdt32_ld(&v[i]) == num[i]);
}
/* What the kernel's iomfb_m4_init() and the reserved-memory code read. */
static void published(bool piodma)
{
    int dcp = fdt_path_offset(dt, "dcp"), len = 0;
    const char *uuid = fdt_getprop(dt, dcp, "apple,firmware-uuid", &len);
    assert(uuid && len == 37 && !strcmp(uuid, b.uuid));
    version_is("apple,firmware-version", os_firmware.num, os_firmware.num_length);
    version_is("apple,firmware-compat", os_firmware.num, os_firmware.num_length);
    const fdt32_t *regions = fdt_getprop(dt, dcp, "memory-region", &len);
    assert(regions && len == (int)(b.count * 4));
    int resv = fdt_path_offset(dt, "/reserved-memory");
    for (unsigned i = 0; i < b.count; i++) {
        char name[64];
        snprintf(name, sizeof(name), "asc-firmware@%lx", b.segs[i].phys);
        int node = fdt_subnode_offset(dt, resv, name);
        assert(node >= 0 && fdt_getprop(dt, node, "no-map", NULL));
        assert(fdt32_ld(&regions[i]) == fdt_get_phandle(dt, node));
        const fdt64_t *reg = fdt_getprop(dt, node, "reg", &len);
        assert(reg && len == 16 && fdt64_ld(&reg[0]) == b.segs[i].phys &&
               fdt64_ld(&reg[1]) == ALIGN_UP((u64)b.segs[i].size, SZ_16K));
        const fdt32_t *iova = fdt_getprop(dt, node, "iommu-addresses", &len);
        if (b.segs[i].unk == 10) {
            /* The OS log runs at its physical address, outside the DART. */
            assert(fdt_getprop(dt, node, "apple,dcp-os-log", NULL) && !iova);
            continue;
        }
        assert(iova && len >= 20 && fdt32_ld(&iova[0]) == (u32)phandle_of("/soc/dcp@30ae00000"));
        assert(((u64)fdt32_ld(&iova[1]) << 32 | fdt32_ld(&iova[2])) == b.segs[i].remap);
    }
    assert(status_is("/soc/dcp@30ae00000", "disabled")); /* the kernel DT enables it */
    bool inherited = fdt_getprop(dt, fdt_path_offset(dt, "disp0_piodma"),
                                 "apple,j713-inherited-mappings", NULL);
    assert(inherited == piodma);
    assert(status_is("/soc/dcp@30ae00000/piodma", piodma ? "okay" : "disabled"));
    assert(status_is("/soc/iommu@30a300000", piodma ? "okay" : "disabled"));
    assert(status_is("/soc/display-subsystem", piodma ? "okay" : "disabled"));
    assert(!!fdt_getprop(dt, fdt_path_offset(dt, "disp0"), "apple,j713-inherited-mappings",
                         NULL) == piodma);
    assert(!!fdt_getprop(dt, fdt_path_offset(dt, "disp0"), "memory-region", NULL) == piodma);
    /* The walk runs wherever the DT asks for PIODMA, on any firmware. */
    assert(walks == (b.native ? 2 : 0));
    /*
     * Firmware 27 is given /arm-io/disp0's clocks, only with the inherited display maps (the
     * kernel admits the DCP on apple,clock-ids); 26.x trees are not touched.
     */
    const fdt32_t *ids = fdt_getprop(dt, dcp, "apple,clock-ids", &len);
    if (os_firmware.version < V27_0 || !piodma) {
        assert(!ids && !fdt_getprop(dt, dcp, "clocks", NULL) &&
               fdt_path_offset(dt, "/clock-dcp-firmware-364") < 0);
        return;
    }
    assert(ids && len == 8 && fdt32_ld(&ids[0]) == 364 && fdt32_ld(&ids[1]) == 431);
    const char *names = fdt_getprop(dt, dcp, "clock-names", &len);
    assert(names && len == 22 && !memcmp(names, "firmware-0\0firmware-1\0", 22));
    const fdt32_t *clk = fdt_getprop(dt, dcp, "clocks", &len);
    assert(clk && len == 8);
    static const char *const clock_path[] = {"/clock-dcp-firmware-364", "/clock-dcp-firmware-431"};
    static const u32 rate[] = {300000000, 0};
    for (int i = 0; i < 2; i++) {
        int node = fdt_path_offset(dt, clock_path[i]);
        assert(node >= 0 && fdt32_ld(&clk[i]) == fdt_get_phandle(dt, node));
        assert(!fdt_node_check_compatible(dt, node, "fixed-clock"));
        const fdt32_t *hz = fdt_getprop(dt, node, "clock-frequency", &len);
        assert(hz && len == 4 && fdt32_ld(hz) == rate[i]);
    }
}
static enum dcp_internal internal;
static void handed_over(const struct board *board, enum fw_version fw, bool piodma)
{
    world(board, fw);
    assert(!dt_set_m4_dcp_handoff(&internal));
    published(piodma);
    /* Complete only with the maps (and on firmware 27 the clocks the kernel admits on). */
    assert(internal == (piodma ? DCP_INTERNAL_COMPLETE : DCP_INTERNAL_NONE));
    char name[64];
    snprintf(name, sizeof(name), "%s-%s", board->name, fw_versions[fw].string);
    save(name);
}
/* A refusal leaves the tree byte for byte as it was. */
static void refused(void)
{
    static unsigned char before[sizeof(blob)];
    memcpy(before, blob, sizeof(blob));
    assert(dt_set_m4_dcp_handoff(&internal) == -1);
    assert(!memcmp(before, blob, sizeof(blob)) && !walks);
    assert(internal == (b.external > 0 ? DCP_INTERNAL_NO_PANEL : DCP_INTERNAL_NONE));
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    outdir = argv[1];

    /*
     * Every M4 Mac whose ADT has an internal panel on disp0, on any firmware: the J713 on
     * 26.6.2 with PIODMA and scanout, and the same on older images. PIODMA follows the
     * inherited pages, not a firmware version list.
     */
    handed_over(&j713, V26_6_2, true);
    struct board old = j713;
    old.uuid = "0BC97BDD-E2FE-3A1B-96B4-8EA8FC5B8BE8"; /* 26.5.1 */
    handed_over(&old, V26_5, true);
    handed_over(&old, V27_0, true);
    handed_over(&old, V26_6, true);
    /* The 27.2 stub: the 26.6.2 path's PIODMA and scanout, plus the firmware-27 clocks. */
    struct board stub = j713;
    stub.uuid = "B3BA603C-3CA7-3E21-B393-44EB20BD2AEF";
    handed_over(&stub, V27_2, true);
    /*
     * Inherited pages that do not match the image's segments: nothing of PIODMA or scanout is
     * published (the walk stops at the first page), and neither are the firmware clocks, so
     * the kernel does not admit the DCP. It keeps its firmware.
     */
    world(&stub, V27_2);
    dart_skew = SZ_16K;
    assert(!dt_set_m4_dcp_handoff(&internal) && internal == DCP_INTERNAL_NONE);
    assert(walks == 1 && status_is("/soc/display-subsystem", "disabled") &&
           status_is("/soc/dcp@30ae00000/piodma", "disabled") &&
           status_is("/soc/iommu@30a300000", "disabled"));
    assert(!fdt_getprop(dt, fdt_path_offset(dt, "dcp"), "apple,clock-ids", NULL) &&
           !fdt_getprop(dt, fdt_path_offset(dt, "dcp"), "clocks", NULL) &&
           fdt_path_offset(dt, "/clock-dcp-firmware-364") < 0);
    assert(fdt_getprop(dt, fdt_path_offset(dt, "dcp"), "apple,firmware-uuid", NULL));
    /* The same pages on 26.6.2 are refused the same way. */
    world(&j713, V26_6_2);
    dart_skew = SZ_16K;
    assert(!dt_set_m4_dcp_handoff(&internal) && internal == DCP_INTERNAL_NONE);
    assert(walks == 1 && status_is("/soc/display-subsystem", "disabled"));

    /*
     * The other M4 Macs with a panel take the same handoff as the J713, decided by their ADT
     * and not their board name. The laptops' DT asks for PIODMA.
     */
    handed_over(&j604, V26_6_2, true);
    handed_over(&j715, V26_6_2, true);
    struct board other = j604;
    other.name = "j604-other-image";
    other.uuid = "0BC97BDD-E2FE-3A1B-96B4-8EA8FC5B8BE8"; /* any image on 26.6.2 */
    handed_over(&other, V26_6_2, true);
    struct board imac = j604;
    imac.name = "j624";
    imac.native = false;
    handed_over(&imac, V26_6_2, false);
    struct board pro = j604;
    pro.name = "j604-27.2";
    pro.uuid = stub.uuid;
    handed_over(&pro, V27_2, true);
    const enum fw_version others[] = {V26_5, V26_6, V27_0, V27_2, V14_7};
    const struct board *panels[] = {&j604, &j715, &imac};
    for (unsigned i = 0; i < ARRAY_SIZE(others); i++)
        for (unsigned k = 0; k < ARRAY_SIZE(panels); k++)
            handed_over(panels[k], others[i], panels[k]->native);

    /* No internal panel: the Mac mini on any firmware, or no disp0 at all, the J713 too. */
    world(&j773g, V26_6_2); refused(); save("j773g-26.6.2");
    world(&j773g, V26_6); refused();
    world(&j773g, V27_2); refused();
    struct board bare = j604;
    bare.external = -1;
    world(&bare, V26_6_2); refused();
    bare = j713;
    bare.external = -1;
    world(&bare, V27_2); refused();

    /* Not a T8132, or not its DCP. */
    world(&j713, V26_6_2); chip_id = T8122; refused();
    world(&j604, V26_6_2); chip_id = T8122; refused();
    world(&j715, V26_6_2);
    assert(!fdt_setprop_string(dt, fdt_path_offset(dt, "dcp"), "compatible", "apple,t8122-dcp"));
    refused();

    /* A DT with no DCP is left alone, as before. */
    world(&j604, V26_6_2);
    assert(!fdt_delprop(dt, fdt_path_offset(dt, "/aliases"), "dcp"));
    static unsigned char before[sizeof(blob)];
    memcpy(before, blob, sizeof(blob));
    assert(!dt_set_m4_dcp_handoff(&internal) && !memcmp(before, blob, sizeof(blob)));
    assert(internal == DCP_INTERNAL_NONE);
    /* The Mac mini on 27.2 too: no panel, whatever the firmware (the external processors'). */
    world(&j773g, V27_2); refused();
    return 0;
}
'''
    binary = tmp_path / "m4-dcp-handoff"
    lib = ROOT / "src/libfdt"
    subprocess.run(
        ["cc", "-std=gnu11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
         "-Wno-unused-parameter", "-Wno-unused-function", "-I", str(lib), "-x", "c", "-",
         *map(str, (lib / f for f in (
             "fdt.c", "fdt_ro.c", "fdt_rw.c", "fdt_sw.c", "fdt_wip.c",
             "fdt_empty_tree.c", "fdt_strerror.c"))), "-o", str(binary)],
        input=source, text=True, check=True,
    )
    result = subprocess.run([str(binary), str(tmp_path)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    for name in ("j713-27.2", "j713-26.6.2", "j604-26.6.2", "j715-26.6.2", "j624-26.6.2", "j773g-26.6.2",
                 "j604-27.2-27.2", "j715-27.2"):
        assert (tmp_path / f"{name}.dtb").stat().st_size > 0
    # The refusals say why.
    assert "no internal panel" in result.stdout
    # No board name decides the handoff.
    handoff = function(kboot, "dt_set_m4_dcp_handoff")
    assert not re.search(r"check_compatible\(dt, 0,", handoff)
    assert "V26_6_2" not in handoff and "V27_2" not in handoff
