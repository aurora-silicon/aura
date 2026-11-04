# SPDX-License-Identifier: GPL-2.0-only
"""An FDT reservation with no space fails both ISP handoff paths."""

from pathlib import Path
import subprocess


def test_isp_reservation_no_space(tmp_path):
    repo = Path(__file__).resolve().parents[2]
    source = (repo / "src/kboot.c").read_text()
    header = (repo / "src/adt.h").read_text()
    layout_start = header.index("struct adt_segment_ranges {")
    layout = header[layout_start:header.index("} PACKED;", layout_start) + len("} PACKED;")]
    def definition(signature):
        start = source.index(signature)
        return source[start:source.index("\n}", start) + 2]

    firmware = definition("static int dt_reserve_asc_segments(") + "\n" + \
        definition("static int dt_reserve_asc_firmware(")
    heap = definition("static int dt_set_isp_fwdata(void)")
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef unsigned long u64;
typedef uint32_t u32;
#define SZ_16K 0x4000
#define T6030 0x6030
#define T6041 0x6041
#define T8132 0x8132
/* Only the dram_only segment filter reads properties this way. */
#define ADT_GETPROP(a, n, k, v) ((size_t)0)
static u32 chip_id;
static unsigned int mappings, links;
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
/* Stub: the T6041 ISP handoff isn't exercised here. */
__attribute__((unused)) static int dt_set_isp_t6041(int node) { (void)node; return 0; }
#define V13_5 0
#define V13_6_1 1
#define bail(...) do { return -1; } while (0)
struct fw_version_info { int version; };
static struct fw_version_info os_firmware = {0}, fw_versions[2];
#define PACKED __attribute__((packed))
''' + layout + r'''
static struct adt_segment_ranges segment = {0x100000, 0x200000, 0, 0x4000, 0};
static void *dt, *adt;
static int fdt_path_offset(const void *tree, const char *path)
{ (void)tree; (void)path; return 1; }
static int adt_path_offset(const void *tree, const char *path)
{ (void)tree; (void)path; return 1; }
static uint32_t fdt_get_phandle(const void *tree, int node)
{ (void)tree; (void)node; return 1; }
static int fdt_generate_phandle(const void *tree, u32 *value)
{ (void)tree; *value = 1; return 0; }
static int fdt_setprop_u32(void *tree, int node, const char *name, u32 value)
{ (void)tree; (void)node; (void)name; (void)value; return 0; }
static const void *adt_getprop(const void *tree, int node, const char *name, u32 *len)
{ (void)tree; (void)node; (void)name; *len = sizeof(segment); return &segment; }
static int dt_get_or_add_reserved_mem(const char *name, const char *compat,
                                      bool no_map, u64 phys, u64 size)
{ (void)name; (void)compat; (void)no_map; (void)phys; (void)size; return -3; }
static int dt_device_set_reserved_mem(int node, const char *name, u32 handle,
                                      u64 iova, u64 size)
{ (void)node; (void)name; (void)handle; (void)iova; (void)size; mappings++; return 0; }
static int dt_device_add_mem_region(const char *path, u32 handle, const char *name)
{ (void)path; (void)handle; (void)name; links++; return 0; }
static int firmware_set_fdt(void *tree, int node, const char *name,
                            const struct fw_version_info *version)
{ (void)tree; (void)node; (void)name; (void)version; return 0; }
static int isp_get_heap(u64 *phys, u64 *iova, u64 *size)
{ *phys = 0x300000; *iova = 0x400000; *size = 0x4000; return 0; }
static const void *fdt_getprop(const void *tree, int node, const char *name, int *len)
{ (void)tree; (void)node; (void)name; (void)len; return "okay"; }
static int fdt_setprop_string(void *tree, int node, const char *name, const char *value)
{ (void)tree; (void)node; (void)name; (void)value; return 0; }
static int fdt_setprop_empty(void *tree, int node, const char *name)
{ (void)tree; (void)node; (void)name; assert(0); return -1; }
''' + firmware + heap + r'''
int main(void)
{
    assert(dt_reserve_asc_firmware("/arm-io/isp", NULL, "isp", false, 0) == -3);
    assert(!mappings && !links);
    assert(dt_set_isp_fwdata() == -1);
    assert(!mappings && !links);
    return 0;
}
'''
    binary = tmp_path / "isp-reservation-full"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-x", "c", "-",
                    "-o", str(binary)], input=harness, text=True, check=True)
    subprocess.run([str(binary)], check=True)
