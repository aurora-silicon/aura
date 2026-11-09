# SPDX-License-Identifier: MIT
"""dt_set_acio_tunables() finds each USB4 node under either alias spelling.

Runs the production function against real FDTs. The tunable copies themselves are recorded,
not performed, so this checks which alias each lookup hands on.
"""

from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "libfdt.h"
typedef uint32_t u32;
typedef unsigned long u64;
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MAX_CIO_DEVS 8
#define bail(...) do { printf(__VA_ARGS__); return -1; } while (0)
struct adt_tunable_info { const char *adt_name, *fdt_name; int a, b, c; };
static const struct adt_tunable_info usb4_rc_tunables[1], usb4_pcie_adapter_tunables[1],
    usb4_nhi_tunables[1], usb4_v2_nhi_tunables[1];
/* Every /arm-io/acioN exists and is a v1 router. */
static const void *adt;
static int adt_path_offset(const void *tree, const char *path)
{
    (void)tree; (void)path;
    return 0;
}
static bool adt_acio_is_v2(int node)
{
    (void)node;
    return false;
}
static unsigned char blob[8192];
static void *dt = blob;
static char seen[64][32];
static int nseen;
static u64 rust_usb4_router_uuid(void) { return 0x1000; }
static void dt_copy_acio_tunables(const char *adt_path, const char *alias,
                                  const struct adt_tunable_info *tunables, size_t count)
{
    (void)adt_path; (void)count;
    if (tunables == usb4_pcie_adapter_tunables)
        snprintf(seen[nseen++], sizeof(seen[0]), "%s", alias);
}
static void dt_copy_usb4_drom(const char *adt_path, const char *alias, u64 uuid)
{
    (void)adt_path; (void)alias; (void)uuid;
}
'''

MAIN = r'''
static void tree(const char *const *aliases)
{
    assert(!fdt_create_empty_tree(dt, sizeof(blob)));
    int node = fdt_add_subnode(dt, 0, "aliases");
    assert(node >= 0);
    for (; *aliases; aliases++)
        assert(!fdt_setprop_string(dt, fdt_path_offset(dt, "/aliases"), *aliases, "/soc/x"));
    nseen = 0;
}

int main(void)
{
    /* Mac Studio style: only the hyphenated spelling. */
    static const char *const hyphen[] = {"usb4-0-pcie-adapter", "usb4-1-pcie-adapter", NULL};
    tree(hyphen);
    assert(dt_set_acio_tunables() == 0);
    assert(nseen == MAX_CIO_DEVS);
    assert(!strcmp(seen[0], "usb4-0-pcie-adapter") && !strcmp(seen[1], "usb4-1-pcie-adapter"));

    /* Both spellings, as the M1 Pro tree has them: the underscored one stays first. */
    static const char *const both[] = {"usb4_0_pcie_adapter", "usb4-0-pcie-adapter", NULL};
    tree(both);
    assert(dt_set_acio_tunables() == 0);
    assert(!strcmp(seen[0], "usb4_0_pcie_adapter"));

    /* Neither: the lookup keeps the hyphenated name, which finds no node, as before. */
    static const char *const none[] = {NULL};
    tree(none);
    assert(dt_set_acio_tunables() == 0);
    assert(!strcmp(seen[0], "usb4-0-pcie-adapter"));
    puts("PASS usb4 alias fallback");
    return 0;
}
'''


def test_usb4_pcie_adapter_alias_fallback(tmp_path):
    source = (ROOT / "src/kboot.c").read_text()
    start = source.index("static int dt_set_acio_tunables(void)")
    function = source[start:source.index("\n}", start) + 2] + "\n"
    cfile = tmp_path / "usb4-alias.c"
    cfile.write_text(HARNESS + function + MAIN)
    libfdt = ROOT / "src/libfdt"
    sources = [str(libfdt / name) for name in (
        "fdt.c", "fdt_ro.c", "fdt_rw.c", "fdt_sw.c", "fdt_wip.c", "fdt_empty_tree.c",
        "fdt_strerror.c",
    )]
    binary = tmp_path / "usb4-alias"
    subprocess.run(["cc", "-D_GNU_SOURCE", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-Wno-unused-const-variable", "-I", str(libfdt),
                    str(cfile), *sources, "-o", str(binary)], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "PASS usb4 alias fallback" in result.stdout
