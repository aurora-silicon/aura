# SPDX-License-Identifier: MIT
"""Check address resolution for root devices and untranslated buses."""

from pathlib import Path
import subprocess


def test_root_address(tmp_path):
    repo = Path(__file__).resolve().parents[2]
    harness = r'''
#include <assert.h>
#include <stdint.h>
#include <libfdt.h>
#include "devicetree.h"

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
    return 0;
}
'''
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
                    str(repo / "src/devicetree.c"), *(str(p) for p in sources),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
