# SPDX-License-Identifier: MIT
"""Exercise the production cooling-list parser used after CPU pruning."""

from pathlib import Path
import subprocess


def test_cpu_cooling_prune(tmp_path):
    repo = Path(__file__).resolve().parents[2]
    source = (repo / "src/kboot.c").read_text()
    start = source.index("static int dt_prune_cpu_cooling(")
    end = source.index("\nstatic int dt_set_cpus(", start)
    harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <libfdt.h>
typedef uint32_t u32;
''' + source[start:end] + r'''
int main(void)
{
    unsigned char dt[4096];
    assert(!fdt_create_empty_tree(dt, sizeof(dt)));
    assert(fdt_add_subnode(dt, 0, "thermal-zones") >= 0);
    int tz = fdt_path_offset(dt, "/thermal-zones");
    assert(fdt_add_subnode(dt, tz, "map") >= 0);
    assert(fdt_add_subnode(dt, 0, "fan") >= 0);
    int fan = fdt_path_offset(dt, "/fan");
    assert(!fdt_setprop_u32(dt, fan, "phandle", 10));
    fan = fdt_path_offset(dt, "/fan");
    assert(!fdt_setprop_u32(dt, fan, "#cooling-cells", 3));
    fdt32_t list[] = {cpu_to_fdt32(2), cpu_to_fdt32(100), cpu_to_fdt32(200),
                      cpu_to_fdt32(10), cpu_to_fdt32(1), cpu_to_fdt32(2), cpu_to_fdt32(3)};
    int map = fdt_path_offset(dt, "/thermal-zones/map");
    assert(!fdt_setprop(dt, map, "cooling-device", list, sizeof(list)));
    u32 gone[] = {2}, cells[] = {2};
    assert(!dt_prune_cpu_cooling(dt, gone, cells, 1));
    int len; map = fdt_path_offset(dt, "/thermal-zones/map");
    const fdt32_t *actual = fdt_getprop(dt, map, "cooling-device", &len);
    assert(actual && len == 4 * (int)sizeof(*actual));
    assert(!memcmp(actual, list + 3, len));
    map = fdt_path_offset(dt, "/thermal-zones/map");
    assert(!fdt_setprop(dt, map, "cooling-device", list, 3 * sizeof(*list)));
    assert(dt_prune_cpu_cooling(dt, gone, cells, 1) == -FDT_ERR_BADVALUE);
    return 0;
}
'''
    cfile = tmp_path / "cooling.c"
    cfile.write_text(harness)
    libfdt = repo / "src/libfdt"
    sources = [libfdt / name for name in (
        "fdt.c", "fdt_addresses.c", "fdt_check.c", "fdt_empty_tree.c",
        "fdt_ro.c", "fdt_rw.c", "fdt_strerror.c", "fdt_sw.c", "fdt_wip.c",
    )]
    binary = tmp_path / "cooling"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-I", str(libfdt),
                    str(cfile), *(str(p) for p in sources), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
