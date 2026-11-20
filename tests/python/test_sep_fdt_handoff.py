# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the production SEP handoff across an FDT relocation."""

from pathlib import Path
import subprocess


def test_sep_fdt_handoff(tmp_path):
    repo = Path(__file__).resolve().parents[2]
    source = (repo / "src/kboot.c").read_text()
    start = source.index("static int dt_set_sep(void)")
    end = source.index("static int dt_set_sio_fwdata(", start)
    helper = source[start:end]
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "libfdt.h"
typedef uint64_t u64;
typedef uint32_t u32;
#define T8140 1
#define T8152 2
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define bail(...) do { printf(__VA_ARGS__); return -1; } while (0)
#define ADT_GETPROP(a, n, p, v) adt_get((n), (p), (v), sizeof(*(v)))
#define ADT_GETPROP_ARRAY(a, n, p, v) adt_get((n), (p), (v), sizeof(v))
static unsigned char old_blob[8192], new_blob[8192];
static void *dt;
static int dt_bufsize = sizeof(old_blob);
static int chip_id = T8140;
static void *adt;
static u64 mem_size_actual;
static unsigned char lpol[] = {1, 2, 3, 4}, ibot[] = {5, 6, 7};
static int manifest_present, bad_lpol, bad_sepfw, reservations;
static u64 dram_base, dram_size;

static int adt_path_offset(void *a, const char *path)
{
    (void)a;
    if (!strcmp(path, "/chosen")) return 1;
    if (!strcmp(path, "/chosen/memory-map")) return 2;
    if (!strcmp(path, "/chosen/boot-object-manifests"))
        return manifest_present ? 3 : -1;
    return -1;
}

static int adt_get(int node, const char *prop, void *out, size_t len)
{
    u64 pair[2];
    if (node == 1 && !strcmp(prop, "dram-base") && len == 8) {
        memcpy(out, &dram_base, 8); return 8;
    }
    if (node == 1 && !strcmp(prop, "dram-size") && len == 8) {
        memcpy(out, &dram_size, 8); return 8;
    }
    if (node == 2 && !strcmp(prop, "SEPFW") && len == 16) {
        pair[0] = bad_sepfw ? dram_base - 1 : (u64)(uintptr_t)lpol;
        pair[1] = sizeof(lpol);
    } else if (node == 3 && !strcmp(prop, "lpol") && len == 16) {
        pair[0] = (u64)(uintptr_t)lpol;
        pair[1] = bad_lpol ? UINT64_MAX : sizeof(lpol);
    } else if (node == 3 && !strcmp(prop, "ibot") && len == 16) {
        pair[0] = (u64)(uintptr_t)ibot;
        pair[1] = sizeof(ibot);
    } else {
        return -1;
    }
    memcpy(out, pair, len);
    return len;
}

static int dt_get_or_add_reserved_mem(const char *name, const char *compat,
                                       int nomap, u64 addr, size_t size)
{
    (void)nomap;
    int parent = fdt_path_offset(dt, "/reserved-memory");
    int node = fdt_add_subnode(dt, parent, name);
    assert(node >= 0);
    assert(fdt_setprop_string(dt, node, "compatible", compat) == 0);
    node = fdt_path_offset(dt, "/reserved-memory/sep-firmware");
    assert(fdt_setprop_u32(dt, node, "phandle", 1) == 0);
    node = fdt_path_offset(dt, "/reserved-memory/sep-firmware");
    u64 reg[] = {cpu_to_fdt64(addr), cpu_to_fdt64(size)};
    assert(fdt_setprop(dt, node, "reg", reg, sizeof(reg)) == 0);
    assert(fdt_open_into(dt, new_blob, sizeof(new_blob)) == 0);
    dt = new_blob;
    memset(old_blob, 0xa5, sizeof(old_blob));
    reservations++;
    return fdt_path_offset(dt, "/reserved-memory/sep-firmware");
}

static int dt_device_add_mem_region(const char *path, u32 phandle, const char *name)
{
    int node = fdt_path_offset(dt, path);
    if (node < 0) return -1;
    if (fdt_appendprop_u32(dt, node, "memory-region", phandle)) return -1;
    node = fdt_path_offset(dt, path);
    return fdt_appendprop_string(dt, node, "memory-region-names", name);
}
''' + helper + r'''
static void setup(void)
{
    memset(old_blob, 0, sizeof(old_blob));
    memset(new_blob, 0, sizeof(new_blob));
    dt = old_blob;
    reservations = 0;
    assert(fdt_create_empty_tree(dt, sizeof(old_blob)) == 0);
    assert(fdt_add_subnode(dt, 0, "aliases") >= 0);
    assert(fdt_add_subnode(dt, 0, "reserved-memory") >= 0);
    assert(fdt_add_subnode(dt, 0, "soc") >= 0);
    assert(fdt_add_subnode(dt, fdt_path_offset(dt, "/soc"), "sep@0") >= 0);
    int aliases = fdt_path_offset(dt, "/aliases");
    assert(fdt_setprop_string(dt, aliases, "sep", "/soc/sep@0") == 0);
    uintptr_t lo = (uintptr_t)lpol < (uintptr_t)ibot ? (uintptr_t)lpol : (uintptr_t)ibot;
    uintptr_t hi = (uintptr_t)lpol > (uintptr_t)ibot ?
                   (uintptr_t)lpol + sizeof(lpol) : (uintptr_t)ibot + sizeof(ibot);
    dram_base = lo - 4096;
    dram_size = hi - dram_base + 4096;
}

static void expect_region(void)
{
    int node = fdt_path_offset(dt, "/soc/sep@0"), len;
    assert(node >= 0);
    const u32 *region = fdt_getprop(dt, node, "memory-region", &len);
    assert(region && len == 4 && fdt32_to_cpu(*region) == 1);
    const char *name = fdt_getprop(dt, node, "memory-region-names", &len);
    assert(name && len == sizeof("sepfw") && !strcmp(name, "sepfw"));
    assert(reservations == 1);
}

int main(void)
{
    setup();
    manifest_present = 1;
    assert(dt_set_sep() == 0);
    expect_region();
    int node = fdt_path_offset(dt, "/soc/sep@0"), len;
    const unsigned char *prop = fdt_getprop(dt, node, "local-policy-manifest", &len);
    assert(prop && len == sizeof(lpol) && !memcmp(prop, lpol, len));
    prop = fdt_getprop(dt, node, "iboot-manifest", &len);
    assert(prop && len == sizeof(ibot) && !memcmp(prop, ibot, len));
    assert(fdt_check_header(dt) == 0);

    setup();
    manifest_present = 0;
    assert(dt_set_sep() == 0);
    expect_region();
    assert(fdt_getprop(dt, fdt_path_offset(dt, "/soc/sep@0"),
                       "local-policy-manifest", NULL) == NULL);

    setup();
    manifest_present = 1;
    bad_lpol = 1;
    assert(dt_set_sep() == 0);
    expect_region();
    node = fdt_path_offset(dt, "/soc/sep@0");
    assert(fdt_getprop(dt, node, "local-policy-manifest", NULL) == NULL);
    assert(fdt_getprop(dt, node, "iboot-manifest", &len) && len == sizeof(ibot));

    setup();
    bad_sepfw = 1;
    assert(dt_set_sep() == -1 && reservations == 0);

    /* T8152 takes the same DRAM bounds as T8140. */
    chip_id = T8152;
    bad_sepfw = 0;
    bad_lpol = 0;
    setup();
    manifest_present = 1;
    assert(dt_set_sep() == 0);
    expect_region();
    node = fdt_path_offset(dt, "/soc/sep@0");
    prop = fdt_getprop(dt, node, "local-policy-manifest", &len);
    assert(prop && len == sizeof(lpol) && !memcmp(prop, lpol, len));
    prop = fdt_getprop(dt, node, "iboot-manifest", &len);
    assert(prop && len == sizeof(ibot) && !memcmp(prop, ibot, len));

    setup();
    bad_sepfw = 1;
    assert(dt_set_sep() == -1 && reservations == 0);
    return 0;
}
'''
    binary = tmp_path / "sep-fdt-handoff"
    libfdt = repo / "src/libfdt"
    sources = [libfdt / name for name in (
        "fdt.c", "fdt_addresses.c", "fdt_check.c", "fdt_empty_tree.c",
        "fdt_ro.c", "fdt_rw.c", "fdt_strerror.c", "fdt_sw.c", "fdt_wip.c",
    )]
    subprocess.run(
        ["cc", "-D_GNU_SOURCE", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(libfdt),
         "-x", "c", "-", *map(str, sources), "-o", str(binary)],
        input=harness, text=True, check=True,
    )
    subprocess.run([str(binary)], check=True)
