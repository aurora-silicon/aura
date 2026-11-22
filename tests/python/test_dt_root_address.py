# SPDX-License-Identifier: MIT
"""Check address resolution for root devices, untranslated buses and nested ranges."""

from pathlib import Path
import subprocess


def test_root_address(tmp_path):
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <libfdt.h>
/* Compile the actual implementation with host types, without importing
 * firmware-only uintptr_t/ssize_t typedefs into the macOS SDK headers. */
#define TYPES_H
typedef uint64_t u64;
/* Count the logged ranges misses instead of printing them. */
#define UTILS_H
static int misses;
#define printf(...) (misses++)
#include "devicetree.c"

static void put_reg(void *fdt, const char *path, const fdt32_t *reg, int len)
{
    assert(!fdt_setprop(fdt, fdt_path_offset(fdt, path), "reg", reg, len));
}

int main(void)
{
    unsigned char fdt[4096];
    assert(!fdt_create_empty_tree(fdt, sizeof(fdt)));
    assert(!fdt_setprop_u32(fdt, 0, "#address-cells", 2));
    assert(!fdt_setprop_u32(fdt, 0, "#size-cells", 2));
    assert(fdt_add_subnode(fdt, 0, "iommu@411000000") >= 0);
    const fdt32_t reg[] = {cpu_to_fdt32(4), cpu_to_fdt32(0x11000000),
                           cpu_to_fdt32(0), cpu_to_fdt32(0x20000)};
    put_reg(fdt, "/iommu@411000000", reg, sizeof(reg));
    int node = fdt_path_offset(fdt, "/iommu@411000000");
    assert(dt_get_address(fdt, node) == UINT64_C(0x411000000));
    put_reg(fdt, "/iommu@411000000", reg, sizeof(reg) - sizeof(*reg));
    node = fdt_path_offset(fdt, "/iommu@411000000");
    assert(!dt_get_address(fdt, node));
    assert(fdt_add_subnode(fdt, 0, "bus") >= 0);
    assert(fdt_add_subnode(fdt, fdt_path_offset(fdt, "/bus"), "device@0") >= 0);
    put_reg(fdt, "/bus/device@0", reg, sizeof(reg));
    assert(!dt_get_address(fdt, fdt_path_offset(fdt, "/bus/device@0")));
    assert(!fdt_setprop_u32(fdt, 0, "#address-cells", 1));
    assert(!fdt_setprop_u32(fdt, 0, "#size-cells", 1));
    const fdt32_t reg32[] = {cpu_to_fdt32(0x12340000), cpu_to_fdt32(0x4000)};
    put_reg(fdt, "/iommu@411000000", reg32, sizeof(reg32));
    assert(dt_get_address(fdt, fdt_path_offset(fdt, "/iommu@411000000")) == 0x12340000);

    /* A USB4 router DART: one address cell under the router, which maps its
     * window into the soc's two cells, which maps one to one. */
    unsigned char t[4096];
    assert(!fdt_create_empty_tree(t, sizeof(t)));
    assert(!fdt_setprop_u32(t, 0, "#address-cells", 2));
    assert(!fdt_setprop_u32(t, 0, "#size-cells", 2));
    int soc = fdt_add_subnode(t, 0, "soc");
    assert(soc >= 0);
    assert(!fdt_setprop_u32(t, soc, "#address-cells", 2));
    assert(!fdt_setprop_u32(t, soc, "#size-cells", 2));
    assert(!fdt_setprop(t, soc, "ranges", NULL, 0));
    int acio = fdt_add_subnode(t, soc, "acio@401ac0000");
    assert(acio >= 0);
    assert(!fdt_setprop_u32(t, acio, "#address-cells", 1));
    assert(!fdt_setprop_u32(t, acio, "#size-cells", 1));
    const fdt32_t window[] = {cpu_to_fdt32(0), cpu_to_fdt32(4), cpu_to_fdt32(0x01000000),
                              cpu_to_fdt32(0x1000000)};
    assert(!fdt_setprop(t, acio, "ranges", window, sizeof(window)));
    int dart = fdt_add_subnode(t, acio, "iommu@a80000");
    assert(dart >= 0);
    const fdt32_t dart_reg[] = {cpu_to_fdt32(0xa80000), cpu_to_fdt32(0xc000)};
    assert(!fdt_setprop(t, dart, "reg", dart_reg, sizeof(dart_reg)));
    dart = fdt_path_offset(t, "/soc/acio@401ac0000/iommu@a80000");
    assert(dt_get_address(t, dart) == UINT64_C(0x401a80000));
    assert(!misses);

    /* An address outside the router's window passes through, and is logged. */
    const fdt32_t outside[] = {cpu_to_fdt32(0x2000000), cpu_to_fdt32(0xc000)};
    assert(!fdt_setprop(t, fdt_path_offset(t, "/soc/acio@401ac0000/iommu@a80000"), "reg",
                        outside, sizeof(outside)));
    dart = fdt_path_offset(t, "/soc/acio@401ac0000/iommu@a80000");
    assert(dt_get_address(t, dart) == 0x2000000);
    assert(misses == 1);

    /* A ranges or a reg that is not a whole number of entries is refused. */
    assert(!fdt_setprop(t, fdt_path_offset(t, "/soc/acio@401ac0000"), "ranges", window,
                        sizeof(window) - sizeof(*window)));
    assert(!dt_get_address(t, fdt_path_offset(t, "/soc/acio@401ac0000/iommu@a80000")));
    assert(!fdt_setprop(t, fdt_path_offset(t, "/soc/acio@401ac0000"), "ranges", window,
                        sizeof(window)));
    const fdt32_t ragged[] = {cpu_to_fdt32(0xa80000), cpu_to_fdt32(0xc000), cpu_to_fdt32(0)};
    assert(!fdt_setprop(t, fdt_path_offset(t, "/soc/acio@401ac0000/iommu@a80000"), "reg",
                        ragged, sizeof(ragged)));
    assert(!dt_get_address(t, fdt_path_offset(t, "/soc/acio@401ac0000/iommu@a80000")));
    return 0;
}
'''
    run(tmp_path, harness)


BUS_HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <libfdt.h>
#define TYPES_H
typedef uint64_t u64;
#define UTILS_H
#define printf(...) ((void)0)
#include "devicetree.c"

/* A /soc bus with one ranges entry and one device on it, at the bus's own cell widths */
static int device(void *fdt, int cells, const fdt32_t *ranges, int rlen, const fdt32_t *reg,
                  int len)
{
    assert(!fdt_create_empty_tree(fdt, 4096));
    assert(!fdt_setprop_u32(fdt, 0, "#address-cells", 2));
    assert(!fdt_setprop_u32(fdt, 0, "#size-cells", 2));
    int soc = fdt_add_subnode(fdt, 0, "soc");
    assert(soc >= 0);
    assert(!fdt_setprop_u32(fdt, soc, "#address-cells", cells));
    assert(!fdt_setprop_u32(fdt, soc, "#size-cells", cells));
    assert(!fdt_setprop(fdt, soc, "ranges", ranges, rlen));
    int dev = fdt_add_subnode(fdt, soc, "dev");
    assert(dev >= 0);
    assert(!fdt_setprop(fdt, dev, "reg", reg, len));
    /* Whatever follows the reg value in the tree must not be read as part of it */
    assert(!fdt_setprop_u32(fdt, dev, "status", 0xffffffff));
    return fdt_path_offset(fdt, "/soc/dev");
}

/* The address of a device on /soc, the way dt_disable_missing_devs() works it out */
static bool soc_address(void *fdt, int node, u64 *addr)
{
    int soc = fdt_path_offset(fdt, "/soc");
    struct dt_ranges_tbl ranges[DT_MAX_RANGES] = {0};

    dt_parse_ranges(fdt, soc, ranges);
    if (!dt_reg_address(fdt, soc, node, addr))
        return false;
    *addr = dt_translate_address(ranges, *addr);
    return true;
}

#define C(x) cpu_to_fdt32(x)

int main(void)
{
    unsigned char fdt[4096];
    u64 addr;

    /* A bus with one address cell: reg holds a 32-bit address, then a 32-bit size */
    const fdt32_t ranges1[] = {C(0x0), C(0x2), C(0x0), C(0x10000)};
    const fdt32_t reg1[] = {C(0x100), C(0x10)};
    int node = device(fdt, 1, ranges1, sizeof(ranges1), reg1, sizeof(reg1));
    assert(soc_address(fdt, node, &addr) && addr == UINT64_C(0x200000100));
    assert(dt_get_address(fdt, node) == addr);

    /* A reg shorter than one entry has no address */
    const fdt32_t ranges2[] = {C(0x0), C(0x0), C(0x2), C(0x0), C(0x0), C(0x10000)};
    const fdt32_t reg2[] = {C(0x100)};
    node = device(fdt, 2, ranges2, sizeof(ranges2), reg2, sizeof(reg2));
    assert(!soc_address(fdt, node, &addr));
    assert(dt_get_address(fdt, node) == 0);

    /* A window that ends at 2^64 still holds the addresses inside it */
    const fdt32_t ranges3[] = {C(0xffffffff), C(0xfffff000), C(0x0), C(0x1000), C(0x0), C(0x1000)};
    const fdt32_t reg3[] = {C(0xffffffff), C(0xfffff800), C(0x0), C(0x10)};
    node = device(fdt, 2, ranges3, sizeof(ranges3), reg3, sizeof(reg3));
    assert(soc_address(fdt, node, &addr) && addr == UINT64_C(0x1800));
    assert(dt_get_address(fdt, node) == addr);

    /* And one just outside a window is left alone */
    const fdt32_t reg4[] = {C(0x0), C(0x20000), C(0x0), C(0x10)};
    node = device(fdt, 2, ranges2, sizeof(ranges2), reg4, sizeof(reg4));
    assert(soc_address(fdt, node, &addr) && addr == UINT64_C(0x20000));
    assert(dt_get_address(fdt, node) == addr);
    return 0;
}
'''


def test_bus_address(tmp_path):
    run(tmp_path, BUS_HARNESS)


def run(tmp_path, harness):
    repo = Path(__file__).resolve().parents[2]
    cfile = tmp_path / "dt-root.c"
    cfile.write_text(harness)
    libfdt = repo / "src/libfdt"
    sources = [libfdt / name for name in (
        "fdt.c", "fdt_addresses.c", "fdt_check.c", "fdt_empty_tree.c",
        "fdt_ro.c", "fdt_rw.c", "fdt_strerror.c", "fdt_sw.c", "fdt_wip.c",
    )]
    binary = tmp_path / "dt-root"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(repo / "src"), "-I", str(libfdt), str(cfile),
                    *(str(p) for p in sources),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
