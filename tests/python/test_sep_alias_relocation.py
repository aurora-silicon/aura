"""SEP firmware and manifests stay on the aliased node as the FDT grows."""

from pathlib import Path
import subprocess
import os


def extract(source, start, end):
    first = source.index(start)
    return source[first:source.index(end, first)]


def test_sep_alias_relocation(tmp_path):
    repo = Path(__file__).resolve().parents[2]
    source = Path(os.environ.get("SEP_ALIAS_SOURCE", repo / "src/kboot.c")).read_text()
    add_mem_region = extract(source, "static int dt_device_add_mem_region(",
                             "\nstatic ")
    set_sep = extract(source, "static int dt_set_sep(void)", "static int dt_set_sio_fwdata(")
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "libfdt.h"
typedef uint64_t u64;
typedef uint32_t u32;
#define bail(...) do { printf(__VA_ARGS__); return -1; } while (0)
#define ADT_GETPROP_ARRAY(a, n, p, v) adt_get((n), (p), (v), sizeof(v))
#define ADT_GETPROP(a, n, p, v)       adt_get((n), (p), (v), sizeof(*(v)))
#define ARRAY_SIZE(a)                 (sizeof(a) / sizeof((a)[0]))
#define T8140                         0x8140
static unsigned char blob[16384];
static void *dt = blob;
static size_t dt_bufsize = sizeof(blob);
/* No /chosen in this ADT: the SEP bounds fall back to the DRAM aura maps, here all of it. */
static u64 ram_base;
static u64 mem_size_actual = 1ULL << 48;
static void *adt;
static int scenario, reservations;
static unsigned char lpol[] = {1, 2, 3, 4}, ibot[] = {5, 6, 7};

static int adt_path_offset(void *a, const char *path)
{
    (void)a;
    if (!strcmp(path, "/chosen/memory-map")) return 1;
    if (!strcmp(path, "/chosen/boot-object-manifests")) return 2;
    return -1;
}

static int adt_get(int node, const char *prop, void *out, size_t len)
{
    u64 pair[2];
    if (node == 1 && !strcmp(prop, "SEPFW")) {
        if (scenario == 8) return -1;
        pair[0] = 0x10000000000; pair[1] = 0x400000;
    } else if (node == 2 && !strcmp(prop, "lpol")) {
        pair[0] = (u64)(uintptr_t)lpol; pair[1] = sizeof(lpol);
    } else if (node == 2 && !strcmp(prop, "ibot")) {
        pair[0] = (u64)(uintptr_t)ibot; pair[1] = sizeof(ibot);
    } else {
        return -1;
    }
    memcpy(out, pair, len);
    return len;
}

/* In place, like kboot.c: the blob grows inside its buffer. */
static int dt_get_or_add_reserved_mem(const char *name, const char *compat, bool nomap,
                                      u64 addr, size_t size)
{
    (void)nomap;
    reservations++;
    int node = fdt_add_subnode(dt, fdt_path_offset(dt, "/reserved-memory"), name);
    assert(node >= 0);
    assert(fdt_setprop_string(dt, node, "compatible", compat) == 0);
    u64 reg[] = {cpu_to_fdt64(addr), cpu_to_fdt64(size)};
    assert(fdt_setprop(dt, node, "reg", reg, sizeof(reg)) == 0);
    assert(fdt_setprop_u32(dt, node, "phandle", 0x42) == 0);
    return node;
}
static int append_u32(void *fdt, int node, const char *name, uint32_t value)
{
    if (scenario == 9 && !strcmp(name, "memory-region")) return -FDT_ERR_NOSPACE;
    return fdt_appendprop_u32(fdt, node, name, value);
}
static int setprop(void *fdt, int node, const char *name, const void *value, int len)
{
    if ((scenario == 10 && !strcmp(name, "local-policy-manifest")) ||
        (scenario == 11 && !strcmp(name, "iboot-manifest"))) return -FDT_ERR_NOSPACE;
    return fdt_setprop(fdt, node, name, value, len);
}
#define fdt_appendprop_u32 append_u32
#define fdt_setprop setprop
''' + add_mem_region + "\n" + set_sep + r'''
int main(int argc, char **argv)
{
    assert(argc == 2);
    scenario = argv[1][0] - '0';
    if (argv[1][1]) scenario = 10 + argv[1][1] - '0';
    /* fdt_add_subnode() inserts before existing siblings: build back to front
     * so the blob holds /reserved-memory, /soc, /aliases in that order. */
    assert(fdt_create_empty_tree(dt, sizeof(blob)) == 0);
    int aliases = fdt_add_subnode(dt, 0, "aliases");
    assert(aliases >= 0);
    assert(fdt_setprop_string(dt, aliases, "serial0", "/soc/serial@30812c000") == 0);
    aliases = fdt_path_offset(dt, "/aliases");
    assert(fdt_setprop_string(dt, aliases, "sep", "/soc/sep@0") == 0);
    int soc = fdt_add_subnode(dt, 0, "soc");
    assert(soc >= 0);
    assert(fdt_add_subnode(dt, soc, "sep@0") >= 0);
    assert(fdt_add_subnode(dt, fdt_path_offset(dt, "/soc"), "serial@30812c000") >= 0);
    assert(fdt_add_subnode(dt, 0, "reserved-memory") >= 0);
    assert(fdt_path_offset(dt, "/reserved-memory") < fdt_path_offset(dt, "/soc"));
    assert(fdt_path_offset(dt, "/soc") < fdt_path_offset(dt, "/aliases"));

    aliases = fdt_path_offset(dt, "/aliases");
    if (scenario == 1) assert(fdt_delprop(dt, aliases, "sep") == 0);
    if (scenario == 2) assert(fdt_setprop(dt, aliases, "sep", "soc/sep@0", 10) == 0);
    if (scenario == 3) assert(fdt_setprop(dt, aliases, "sep", "/soc/sep@0", 10) == 0);
    if (scenario == 4) assert(fdt_setprop(dt, aliases, "sep", "/soc/sep@0\0tail", 15) == 0);
    if (scenario == 5) assert(fdt_setprop(dt, aliases, "sep", "", 1) == 0);
    if (scenario == 6) {
        char long_alias[257]; memset(long_alias, 'a', sizeof(long_alias));
        long_alias[0] = '/'; long_alias[256] = 0;
        assert(fdt_setprop(dt, aliases, "sep", long_alias, sizeof(long_alias)) == 0);
    }
    if (scenario == 7) assert(fdt_setprop(dt, aliases, "sep", "", 0) == 0);
    int status = dt_set_sep();
    if (scenario == 1) { assert(status == 0 && reservations == 0); return 0; }
    if (scenario >= 2 && scenario <= 8) {
        assert(status < 0 && reservations == 0); return 0;
    }
    if (scenario == 9) {
        assert(status < 0);
        assert(!fdt_getprop(dt, fdt_path_offset(dt, "/soc/sep@0"), "local-policy-manifest", NULL));
        return 0;
    }
    assert(status == 0);

    int len, sep = fdt_path_offset(dt, "/soc/sep@0");
    /* A manifest that cannot be published is logged and left out. */
    if (scenario == 10 || scenario == 11) {
        bool lpol_present = fdt_getprop(dt, sep, "local-policy-manifest", NULL) != NULL;
        bool ibot_present = fdt_getprop(dt, sep, "iboot-manifest", NULL) != NULL;
        assert(lpol_present == (scenario == 11) && ibot_present == (scenario == 10));
        return 0;
    }
    const fdt32_t *region = fdt_getprop(dt, sep, "memory-region", &len);
    assert(region && len == 4 && fdt32_to_cpu(*region) == 0x42);
    const char *name = fdt_getprop(dt, sep, "memory-region-names", &len);
    assert(name && !strcmp(name, "sepfw"));
    const unsigned char *prop = fdt_getprop(dt, sep, "local-policy-manifest", &len);
    assert(prop && len == sizeof(lpol) && !memcmp(prop, lpol, len));
    prop = fdt_getprop(dt, sep, "iboot-manifest", &len);
    assert(prop && len == sizeof(ibot) && !memcmp(prop, ibot, len));
    assert(!fdt_getprop(dt, fdt_path_offset(dt, "/soc/serial@30812c000"), "memory-region", NULL));
    assert(fdt_check_header(dt) == 0);
    return 0;
}
'''
    binary = tmp_path / "sep-alias-relocation"
    libfdt = repo / "src/libfdt"
    sources = [libfdt / name for name in (
        "fdt.c", "fdt_addresses.c", "fdt_empty_tree.c",
        "fdt_ro.c", "fdt_rw.c", "fdt_strerror.c", "fdt_sw.c", "fdt_wip.c",
    )]
    subprocess.run(
        ["cc", "-D_GNU_SOURCE", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-type-limits", "-I", str(libfdt),
         "-x", "c", "-", *map(str, sources), "-o", str(binary)],
        input=harness, text=True, check=True,
    )
    for scenario in range(12):
        subprocess.run([str(binary), str(scenario)], check=True)
