# SPDX-License-Identifier: MIT
"""Real FDT transactions: alias fallback, provider scope and rollback on failure."""
from pathlib import Path
import subprocess


def test_t604x_handoff(tmp_path):
    root = Path(__file__).resolve().parents[2]
    source = (root / "src/kboot.c").read_text()
    def body(name):
        start = source.index("static int " + name + "(")
        return source[start:source.index("\n}", start) + 2]
    helper = body("dt_t8140_power_paths") + "\n" + body("dt_setup_t604x_sio")
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libfdt.h"
typedef uint32_t u32;
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define bail(...) do { return -1; } while (0)
static unsigned char blob[16384];
static void *dt = blob;
static size_t dt_bufsize = sizeof(blob);
static int fault, reserved, tables;
static int dt_reserve_asc_firmware(const char *adt, const char *alt, const char *path,
                                   bool nomap, uint64_t off)
{
    (void)alt; (void)off;
    assert(!strcmp(adt, "/arm-io/sio") && nomap && !strcmp(path, "/sio-node"));
    reserved++;
    assert(!fdt_setprop_string(dt, fdt_path_offset(dt, path), "test-reserved", "yes"));
    return fault == 1 ? -1 : 0;
}
static int dt_set_sio_fwdata(const char *adt, const char *path)
{
    assert(!strcmp(adt, "/arm-io/sio") && !strcmp(path, "/sio-node"));
    tables++;
    int n = fdt_path_offset(dt, path);
    assert(!fdt_setprop_u32(dt, n, "apple,sio-firmware-params", 1));
    return fault == 2 ? -1 : 0;
}
''' + helper + r'''
static int node(const char *name, const char *compat, u32 phandle)
{
    int n = fdt_add_subnode(dt, 0, name); assert(n >= 0);
    if (compat) assert(!fdt_setprop_string(dt, n, "compatible", compat));
    if (phandle) assert(!fdt_setprop_u32(dt, n, "phandle", phandle));
    assert(!fdt_setprop_string(dt, n, "status", "disabled"));
    return n;
}
static void setup(bool alias)
{
    reserved = tables = 0;
    assert(!fdt_create_empty_tree(dt, sizeof(blob)));
    int n = node("power", NULL, 4);
    assert(!fdt_setprop_u32(dt, n, "#power-domain-cells", 0));
    node("dart", "apple,t6041-dart", 3);
    node("mailbox", "apple,asc-mailbox-v4", 2);
    n = node("sio-node", "apple,t6041-sio", 1);
    assert(!fdt_setprop_u32(dt, n, "mboxes", 2));
    const fdt32_t map[] = {cpu_to_fdt32(3), 0};
    assert(!fdt_setprop(dt, n, "iommus", map, sizeof(map)));
    assert(!fdt_setprop_u32(dt, n, "power-domains", 4));
    assert(!fdt_setprop_string(dt, n, "status", "okay"));
    if (alias) {
        n = fdt_add_subnode(dt, 0, "aliases"); assert(n >= 0);
        assert(!fdt_setprop_string(dt, n, "sio", "/sio-node"));
    }
}
static bool enabled(const char *path)
{
    const char *s = fdt_getprop(dt, fdt_path_offset(dt, path), "status", NULL);
    return s && !strcmp(s, "okay");
}
int main(void)
{
    for (int alias = 0; alias < 2; alias++) {
        setup(alias); fault = 0; assert(!dt_setup_t604x_sio());
        assert(reserved == 1 && tables == 1);
        assert(enabled("/sio-node") && enabled("/dart") && enabled("/mailbox") && enabled("/power"));
        for (fault = 1; fault <= 2; fault++) {
            setup(alias); assert(!dt_setup_t604x_sio());
            assert(!enabled("/sio-node") && !enabled("/dart") && !enabled("/mailbox") && !enabled("/power"));
            assert(!fdt_getprop(dt, fdt_path_offset(dt, "/sio-node"), "test-reserved", NULL));
            assert(!fdt_getprop(dt, fdt_path_offset(dt, "/sio-node"), "apple,sio-firmware-params", NULL));
        }
        setup(alias); fault = 0;
        assert(!fdt_setprop_u32(dt, fdt_path_offset(dt, "/sio-node"), "mboxes", 99));
        assert(!dt_setup_t604x_sio() && !reserved && !tables && !enabled("/sio-node"));
        setup(alias);
        assert(!fdt_setprop_u32(dt, fdt_path_offset(dt, "/sio-node"), "iommus", 3));
        assert(!dt_setup_t604x_sio() && !reserved && !enabled("/sio-node"));
        setup(alias);
        assert(!fdt_setprop_u32(dt, fdt_path_offset(dt, "/power"), "power-domains", 4));
        assert(!dt_setup_t604x_sio() && !reserved && !enabled("/sio-node"));
    }
    puts("PASS T604x alias, fallback, scoped providers and transaction rollback");
    return 0;
}
'''
    fixture = tmp_path / "handoff.c"
    fixture.write_text(harness)
    lib = root / "src/libfdt"
    sources = [lib / name for name in ("fdt.c", "fdt_ro.c", "fdt_rw.c", "fdt_sw.c", "fdt_wip.c",
                                     "fdt_empty_tree.c", "fdt_strerror.c")]
    binary = tmp_path / "handoff"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-I" + str(lib),
                    str(fixture), *map(str, sources), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
