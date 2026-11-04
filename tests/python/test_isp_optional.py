# SPDX-License-Identifier: MIT
"""The ISP is optional: an unusable firmware description disables it, not the boot.

Runs kboot.c's dt_set_isp(), dt_reserve_asc_firmware() and dt_reserve_asc_segments() as they
are, on real FDTs.
"""

from pathlib import Path
from test_siliconforest_handoff import run_c

ROOT = Path(__file__).resolve().parents[2]


def test_isp_without_usable_firmware_is_disabled_and_boot_continues(tmp_path):
    source = (ROOT / "src/kboot.c").read_text()

    def function(name):
        start = source.index("static int " + name + "(")
        return source[start:source.index("\n}", start) + 2] + "\n"

    start = source.index("/*\n * The camera is optional.")
    set_isp = source[start:source.index("\n}", start) + 2] + "\n"
    header = (ROOT / "src/adt.h").read_text()
    start = header.index("struct adt_segment_ranges {")
    layout = header[start:header.index("} PACKED;", start) + len("} PACKED;")]
    run_c(tmp_path, "isp-optional", r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libfdt.h"
typedef unsigned long u64;
typedef uint32_t u32;
#define PACKED __attribute__((packed))
#define T6030 0x6030
#define T6040 0x6040
#define T8140 0x8140
#define T8132 0x8132
#define SZ_16K 16384
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define bail(...) do { printf(__VA_ARGS__); return -1; } while (0)
/* Only the dram_only segment filter reads properties this way; ISP reservations do not. */
#define ADT_GETPROP(a, n, k, v) ((size_t)0)
static void *dt, *adt;
static u32 chip_id = 0x8122;
static u32 board_id;
static bool adt_is_compatible(const void *tree, int node, const char *compat)
{ (void)tree; (void)node; (void)compat; return false; }
static unsigned char blob[32768];
''' + layout + r'''
static struct adt_segment_ranges segments[2];
/* 0: valid; 1: no ISP node; 2: no segment list; 3: empty; 4: partial entry; 5: FDT full */
static int fault, reservations, fwdata_calls;
static u64 isp_iova_base(void) { return 0x100000000ULL; }
static bool isp_up = true;
static int isp_get_heap(u64 *phys, u64 *iova, u64 *size)
{
    if (!isp_up) return -1;
    *phys = 0x100000000ULL; *iova = 0x21ec000; *size = 0x14000;
    return 0;
}
static int adt_path_offset(void *tree, const char *path)
{
    (void)tree;
    return fault != 1 && !strcmp(path, "/arm-io/isp") ? 1 : -1;
}
static const void *adt_getprop(void *tree, int node, const char *name, u32 *len)
{
    (void)tree; (void)node;
    assert(!strcmp(name, "segment-ranges"));
    *len = fault == 3 ? 0 : fault == 4 ? sizeof(segments) - 1 : sizeof(segments);
    return fault == 2 ? NULL : segments;
}
static int dt_get_or_add_reserved_mem(const char *name, const char *compat,
                                     bool nomap, u64 physical, size_t size)
{
    (void)compat; (void)nomap;
    if (fault == 5 && ++reservations == 2)
        return -FDT_ERR_NOSPACE;
    int node = fdt_add_subnode(dt, fdt_path_offset(dt, "/reserved-memory"), name);
    assert(node >= 0);
    assert(!fdt_setprop_u32(dt, node, "phandle", 100 + node));
    fdt64_t reg[2] = {cpu_to_fdt64(physical), cpu_to_fdt64(size)};
    assert(!fdt_setprop(dt, node, "reg", reg, sizeof(reg)));
    return node;
}
static int fwdata_fault;
static int dt_set_isp_fwdata(void)
{
    fwdata_calls++;
    return fwdata_fault ? -1 : 0;
}
''' + function("dt_device_set_reserved_mem") + function("dt_device_add_mem_region") +
        function("dt_reserve_asc_segments") + function("dt_reserve_asc_firmware") + set_isp + r'''
static void world(int which)
{
    fault = which;
    reservations = fwdata_calls = fwdata_fault = 0;
    chip_id = 0x8122;
    isp_up = true;
    segments[0] = (struct adt_segment_ranges){.phys = 0x10000004000ULL, .iova = 0x4000,
        .size = 0x4000};
    segments[1] = (struct adt_segment_ranges){.phys = 0x10000010000ULL, .iova = 0x10000,
        .size = 0x8000};
    memset(blob, 0, sizeof(blob));
    dt = blob;
    assert(!fdt_create_empty_tree(dt, sizeof(blob)));
    assert(fdt_add_subnode(dt, 0, "reserved-memory") >= 0);
    int soc = fdt_add_subnode(dt, 0, "soc");
    int isp = fdt_add_subnode(dt, soc, "isp");
    assert(isp >= 0 && !fdt_setprop_string(dt, isp, "status", "okay"));
    int aliases = fdt_add_subnode(dt, 0, "aliases");
    assert(aliases >= 0 && !fdt_setprop_string(dt, aliases, "isp", "/soc/isp"));
}
static const char *isp_status(void)
{
    return fdt_getprop(dt, fdt_path_offset(dt, "isp"), "status", NULL);
}
int main(void)
{
    world(0);
    assert(dt_set_isp() == 0);
    assert(!strcmp(isp_status(), "okay") && fwdata_calls == 1);
    int len = 0;
    assert(fdt_getprop(dt, fdt_path_offset(dt, "isp"), "memory-region", &len) && len == 8);

    for (int which = 1; which <= 5; which++) {
        world(which);
        assert(dt_set_isp() == 0);
        assert(!strcmp(isp_status(), "disabled"));
        assert(fwdata_calls == 0);
    }

    /* A heap or firmware data failure after the reservation disables the ISP too. */
    world(0);
    fwdata_fault = 1;
    assert(dt_set_isp() == 0);
    assert(!strcmp(isp_status(), "disabled") && fwdata_calls == 1);

    /* Without an ISP node in the kernel's tree there is nothing to reserve or disable. */
    world(2);
    assert(!fdt_del_node(dt, fdt_path_offset(dt, "/soc/isp")));
    assert(!fdt_delprop(dt, fdt_path_offset(dt, "/aliases"), "isp"));
    assert(dt_set_isp() == 0 && fwdata_calls == 1);
    /* A T8140 ISP that isp_init() did not bring up has no firmware to reserve. */
    world(2); chip_id = T8140; isp_up = false;
    assert(dt_set_isp() == 0 && fwdata_calls == 1 && !strcmp(isp_status(), "okay"));
    int found = 0;
    assert(!fdt_getprop(dt, fdt_path_offset(dt, "isp"), "memory-region", &found));
    /* One it brought up is reserved like any other. */
    world(0); chip_id = T8140;
    assert(dt_set_isp() == 0 && fwdata_calls == 1);
    assert(fdt_getprop(dt, fdt_path_offset(dt, "isp"), "memory-region", &found) && found == 8);
    /* If its firmware data then fails, it is disabled too. */
    world(2); chip_id = T8140; isp_up = false; fwdata_fault = 1;
    assert(dt_set_isp() == 0 && !strcmp(isp_status(), "disabled"));
    puts("PASS isp optional");
    return 0;
}
''')
