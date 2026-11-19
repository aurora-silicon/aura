# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run production dt_copy_atc_tunables() on four atc-phy,t8132 PHYs under several board trees.

The calibration groups are those of /arm-io/atc-phy0..3 in a J773g ADT on the macOS 27.2
(26B123) stub (tests/data/j773g-atc-usb3.json). What this can catch:

- the copy depending on the board, the chip id or the OS firmware again: no
  apple,tunable-usb3-* on a tree it should serve;
- a PHY whose ADT lacks one group still getting part of the bundle, or costing the other PHYs
  theirs;
- the bundle published for an ADT PHY that is not atc-phy,t8132, or under a tree PHY node
  that is not apple,t8132-atcphy;
- a group the copy rejects (width, alignment, window), or published values other than the ADT's.
"""
import json
from pathlib import Path
import subprocess


def test_t8132_usb3_calibration_follows_the_adt_bundle(tmp_path):
    repo = Path(__file__).resolve().parents[2]
    source = (repo / "src/kboot_atc.c").read_text()
    # Everything dt_copy_atc_tunables() is built from, as it stands in the tree.
    production = source[source.index("#define MAX_ATC_DEVS"):
                        source.index("/* T6040 uses separate register resources")]
    fixture = json.loads((repo / "tests/data/j773g-atc-usb3.json").read_text())
    assert len(fixture["ports"]) == 4

    declarations, tables, checks = [], [], []
    for port, entry in enumerate(fixture["ports"]):
        rows = []
        for i, (name, raw) in enumerate(entry["properties"].items()):
            data = bytes.fromhex(raw)
            body = ",".join(str(x) for x in data) or "0"
            declarations.append(f"static const u8 in{port}_{i}[] = {{{body}}};")
            rows.append(f'{{"{name}", in{port}_{i}, {len(data)}}}')
        tables.append(f"static const struct property props{port}[] = {{{','.join(rows)}}};")
        for i, (name, raw) in enumerate(entry["expected_be_hex"].items()):
            data = bytes.fromhex(raw)
            declarations.append(
                f'static const u8 out{port}_{i}[] = {{{",".join(str(x) for x in data)}}};')
            checks.append(f'    if (port == {port}) expect(node, "{name}", out{port}_{i}, '
                          f'sizeof(out{port}_{i}));')
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libfdt.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef int32_t s32;
/* Match aura's aarch64 target, not the host libc's uint64_t alias. */
typedef unsigned long u64;
_Static_assert(sizeof(u64) == 8, "the aura host fixture requires an LP64 host");
#define static_assert _Static_assert
#define PACKED __attribute__((packed))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define BIT(x) (1UL << (x))
#define GENMASK(h, l) (((~0UL) << (l)) & (~0UL >> (63 - (h))))
#define FIELD_PREP(mask, val) (((u64)(val) << __builtin_ctzl(mask)) & (mask))
#define T6030 0x6030
#define T8132 0x8132
#define T8140 0x8140
static u32 chip_id;
static const char *adt_phy = "atc-phy,t8132";
static const char *tree_phy = "apple,t8132-atcphy";
static int hide_port = -1;
static bool other_node;
static const char *hide_name;
static char adt_blob;
static void *adt = &adt_blob;
static u32 read32(u64 addr) { (void)addr; abort(); } /* T8132 PHYs take no fuses from here */
struct property { const char *name; const u8 *data; u32 len; };
''' + "\n".join(declarations) + "\n" + "\n".join(tables) + r'''
static const struct { const struct property *props; size_t count; } phys[] = {
    {props0, ARRAY_SIZE(props0)}, {props1, ARRAY_SIZE(props1)},
    {props2, ARRAY_SIZE(props2)}, {props3, ARRAY_SIZE(props3)},
};
/* The ADT: /arm-io/atc-phyN is node N + 1, compatible atc-phy,t8132. */
static int adt_path_offset(const void *tree, const char *path)
{
    static const char prefix[] = "/arm-io/atc-phy";
    assert(tree == adt);
    if (strncmp(path, prefix, sizeof(prefix) - 1) || strlen(path) != sizeof(prefix))
        return -1;
    int port = path[sizeof(prefix) - 1] - '0';
    return port >= 0 && port < (int)ARRAY_SIZE(phys) ? port + 1 : -1;
}
static int adt_path_offset_trace(const void *tree, const char *path, int *trace)
{
    (void)trace;
    return adt_path_offset(tree, path);
}
static int adt_get_reg(const void *tree, int *path, const char *prop, int idx, u64 *a, u64 *s)
{
    return -1;
}
static bool adt_is_compatible_at(const void *tree, int node, const char *compatible, int index)
{
    return tree == adt && node >= 1 && !index && !strcmp(compatible, adt_phy);
}
static const void *adt_getprop(const void *tree, int node, const char *name, u32 *len)
{
    assert(tree == adt && node >= 1 && node <= (int)ARRAY_SIZE(phys));
    if (node - 1 == hide_port && !strcmp(name, hide_name))
        return NULL;
    for (size_t i = 0; i < phys[node - 1].count; i++)
        if (!strcmp(phys[node - 1].props[i].name, name)) {
            if (len) *len = phys[node - 1].props[i].len;
            return phys[node - 1].props[i].data;
        }
    return NULL;
}
''' + production + r'''
static char dt[1 << 18];
static const char *const phy_path[] = {"/soc/phy@402a90000", "/soc/phy@40aa90000",
                                       "/soc/phy@412a90000", "/soc/phy@41aa90000"};
static void world(u32 chip, const char *board)
{
    chip_id = chip; adt_phy = "atc-phy,t8132"; hide_port = -1; hide_name = NULL;
    tree_phy = other_node ? "apple,t6030-atcphy-dp" : "apple,t8132-atcphy";
    assert(!fdt_create_empty_tree(dt, sizeof(dt)));
    char compat[64];
    int n = snprintf(compat, sizeof(compat), "%s%capple,t8132%capple,arm-platform", board, 0, 0);
    assert(!fdt_setprop(dt, 0, "compatible", compat, n + 1));
    assert(fdt_add_subnode(dt, 0, "soc") >= 0);
    int aliases = fdt_add_subnode(dt, 0, "aliases");
    assert(aliases >= 0);
    for (size_t port = 0; port < ARRAY_SIZE(phy_path); port++) {
        char alias[16];
        int node = fdt_add_subnode(dt, fdt_path_offset(dt, "/soc"), phy_path[port] + 5);
        assert(node >= 0 && !fdt_setprop_string(dt, node, "compatible", tree_phy));
        snprintf(alias, sizeof(alias), "atcphy%zu", port);
        assert(!fdt_setprop_string(dt, fdt_path_offset(dt, "/aliases"), alias, phy_path[port]));
    }
}
static void copy_all(void)
{
    for (size_t port = 0; port < ARRAY_SIZE(phy_path); port++) {
        char adt_path[32], alias[16];
        snprintf(adt_path, sizeof(adt_path), "/arm-io/atc-phy%zu", port);
        snprintf(alias, sizeof(alias), "atcphy%zu", port);
        dt_copy_atc_tunables(dt, adt_path, alias, port);
    }
}
/* The apple,tunable-usb3-* properties of one PHY: what the hardware check counts (seven). */
static int usb3_properties(int node)
{
    int prop, count = 0;
    fdt_for_each_property_offset(prop, dt, node) {
        const char *name;
        assert(fdt_getprop_by_offset(dt, prop, &name, NULL));
        count += !strncmp(name, "apple,tunable-usb3-", 19);
    }
    return count;
}
static void expect(int node, const char *name, const u8 *want, int count)
{
    int len;
    const void *data = fdt_getprop(dt, node, name, &len);
    assert(data && len == count && !memcmp(data, want, count));
}
static void check(int except)
{
    copy_all();
    for (size_t port = 0; port < ARRAY_SIZE(phy_path); port++) {
        int node = fdt_path_offset(dt, phy_path[port]);
        if ((int)port == except || except == -2) {
            assert(!usb3_properties(node));
            assert(!fdt_getprop(dt, node, "apple,tunable-lane0-usb", NULL));
            assert(!fdt_getprop(dt, node, "apple,tunable-lane1-usb", NULL));
            continue;
        }
        assert(usb3_properties(node) == 7);
''' + "\n".join("    " + c for c in checks) + r'''
        /* The T8132 bundle replaces the t8122-style lists, which this PHY's driver ignores. */
        assert(!fdt_getprop(dt, node, "apple,tunable-common-b", NULL));
    }
}
int main(void)
{
    /* No board, chip id or firmware is asked: the ADT PHY and the tree's PHY node decide. */
    static const char *const boards[] = {"apple,j773g", "apple,j713", "apple,j604",
                                         "apple,j623", "apple,j715"};
    for (size_t i = 0; i < ARRAY_SIZE(boards); i++) {
        world(i & 1 ? 0 : T8132, boards[i]); puts("@all"); check(-1);
    }
    /* One PHY whose ADT lacks one required group keeps USB2; the other three are served. */
    static const char *const groups[] = {"tunable_ATCAXI2AF", "tunable_ATC_FABRIC",
                                         "tunable_ACIOPHY_TOP", "tunable_LN1_TX_SHM_USB_DFLT"};
    for (size_t i = 0; i < ARRAY_SIZE(groups); i++) {
        world(T8132, "apple,j773g"); hide_port = i; hide_name = groups[i];
        puts("@one-missing"); check(i);
    }
    /* An ADT PHY that is not atc-phy,t8132 gets none of this bundle. */
    world(T8132, "apple,j773g"); adt_phy = "atc-phy,t8140"; puts("@other-phy"); check(-2);
    /* An atc-phy,t8132 ADT PHY under a tree node that is not apple,t8132-atcphy: none either. */
    other_node = true;
    world(T8132, "apple,j773g"); puts("@other-node"); check(-2);
    return 0;
}
'''
    cfile = tmp_path / "t8132-atc.c"
    binary = tmp_path / "t8132-atc"
    cfile.write_text(harness)
    libfdt = repo / "src/libfdt"
    sources = [str(libfdt / name) for name in (
        "fdt.c", "fdt_ro.c", "fdt_rw.c", "fdt_sw.c", "fdt_wip.c", "fdt_empty_tree.c",
        "fdt_strerror.c",
    )]
    subprocess.run(["cc", "-std=gnu11", "-D_GNU_SOURCE", "-Wall", "-Werror", "-Wno-unused-function",
                    "-Wno-unused-parameter", "-I", str(libfdt), str(cfile), *sources, "-o",
                    str(binary)], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    # The lines the hardware boots are judged by, once per PHY.
    copied = "FDT: Copied T8132 USB3 calibration"
    refused = "FDT: T8132 USB3 calibration incomplete; preserving USB2 fallback"
    stages = result.stdout.split("@")[1:]
    assert len(stages) == 11
    for stage in stages:
        name = stage.split("\n", 1)[0]
        want = {"all": (4, 0), "one-missing": (3, 1), "other-phy": (0, 0), "other-node": (0, 0)}[name]
        assert (stage.count(copied), stage.count(refused)) == want, stage
