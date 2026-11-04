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
#include <stdio.h>
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
    assert(!dt_prune_cpu_cooling(dt, gone, cells, 1));
    map = fdt_path_offset(dt, "/thermal-zones/map");
    assert(map >= 0);
    assert(!fdt_getprop(dt, map, "cooling-device", &len));
    assert(len == -FDT_ERR_NOTFOUND);

    /* Deleting an earlier empty property must not skip the following map
     * or alter the zone's critical trip. */
    tz = fdt_path_offset(dt, "/thermal-zones");
    int empty = fdt_add_subnode(dt, tz, "empty-map");
    assert(empty >= 0);
    assert(!fdt_setprop(dt, empty, "cooling-device", list, 3 * sizeof(*list)));
    tz = fdt_path_offset(dt, "/thermal-zones");
    int trips = fdt_add_subnode(dt, tz, "trips");
    assert(trips >= 0);
    int trip = fdt_add_subnode(dt, trips, "critical");
    assert(trip >= 0);
    assert(!fdt_setprop_u32(dt, trip, "temperature", 105000));
    assert(!fdt_setprop_string(dt, trip, "type", "critical"));
    map = fdt_path_offset(dt, "/thermal-zones/map");
    assert(!fdt_setprop(dt, map, "cooling-device", list, sizeof(list)));
    assert(!dt_prune_cpu_cooling(dt, gone, cells, 1));
    empty = fdt_path_offset(dt, "/thermal-zones/empty-map");
    assert(empty >= 0 && !fdt_getprop(dt, empty, "cooling-device", &len));
    assert(len == -FDT_ERR_NOTFOUND);
    map = fdt_path_offset(dt, "/thermal-zones/map");
    actual = fdt_getprop(dt, map, "cooling-device", &len);
    assert(actual && len == 4 * (int)sizeof(*actual));
    assert(!memcmp(actual, list + 3, len));
    trip = fdt_path_offset(dt, "/thermal-zones/trips/critical");
    actual = fdt_getprop(dt, trip, "temperature", &len);
    assert(actual && len == sizeof(*actual) && fdt32_ld(actual) == 105000);
    const char *type = fdt_getprop(dt, trip, "type", &len);
    assert(type && !strcmp(type, "critical"));

    /* A malformed earlier map must remain byte-identical while the later
     * valid map is pruned, including after a property shrinks or disappears. */
    for (int kind = 0; kind < 3; kind++) {
        map = fdt_path_offset(dt, "/thermal-zones/map");
        assert(!fdt_setprop(dt, map, "cooling-device", list, sizeof(list)));
        tz = fdt_path_offset(dt, "/thermal-zones");
        int bad = fdt_add_subnode(dt, tz, "bad-map");
        assert(bad >= 0);
        fdt32_t malformed[] = {cpu_to_fdt32(2), cpu_to_fdt32(100), cpu_to_fdt32(200),
                               cpu_to_fdt32(99)};
        int bytes = kind == 0 ? 1 : sizeof(malformed);
        u32 unknown_cells[] = {UINT32_MAX};
        assert(!fdt_setprop(dt, bad, "cooling-device", malformed, bytes));
        assert(!dt_prune_cpu_cooling(dt, gone, kind == 2 ? unknown_cells : cells, 1));
        bad = fdt_path_offset(dt, "/thermal-zones/bad-map");
        actual = fdt_getprop(dt, bad, "cooling-device", &len);
        assert(actual && len == bytes && !memcmp(actual, malformed, bytes));
        map = fdt_path_offset(dt, "/thermal-zones/map");
        actual = fdt_getprop(dt, map, "cooling-device", &len);
        if (kind != 2) {
            assert(actual && len == 4 * (int)sizeof(*actual));
            assert(!memcmp(actual, list + 3, len));
        } else {
            assert(actual && len == sizeof(list) && !memcmp(actual, list, len));
        }
        assert(!fdt_del_node(dt, bad));
    }
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
