# SPDX-License-Identifier: MIT
"""Publish the firmware-27 DCP clocks for every M2 Mac, with production kboot code.

Each board's tree has the display processor aliases its Linux device tree carries, and each
/arm-io/disp0 and /arm-io/dispext<n> has the clock-ids of the board's macOS 27.2 (26B123) ADT:
two provider clocks on T8112, an empty list on every T6020/T6021/T6022 engine. A firmware-27
DCP is admitted only with apple,clock-ids, so every processor the tree names must get it,
including the dcpext<n> of the T602x machines.
"""

import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def function(source, name, kind="static int "):
    start = source.index(kind + name + "(")
    return source[start:source.index("\n}", start) + 2] + "\n"


def test_dcp_fw27_clocks_every_m2_board(tmp_path):
    kboot = (ROOT / "src/kboot.c").read_text()
    start = kboot.index("static const char dcpext_aliases[][8] = {")
    aliases = kboot[start:kboot.index("\n};", start) + 3] + "\n"
    start = kboot.index("static int dt_set_dcp_fw27_clocks(const char *alias, const char *adt_engine)\n{")
    clocks = kboot[start:kboot.index("\n}", start) + 2] + "\n"
    # The firmware-27 rule may sit in its own helper or inline in the publisher.
    rule = (function(kboot, "dt_dcp_fw_clocks", "static bool ")
            if "static bool dt_dcp_fw_clocks(" in kboot else "")
    production = (aliases + rule + clocks +
                  function(kboot, "dt_set_dcp_fw27_clocks_all", "static void "))

    source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libfdt.h"
typedef uint32_t u32;
typedef uint8_t u8;
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
enum fw_version { V13_5, V26_6_2, V27_0, V27_2 };
static struct { enum fw_version version; } os_firmware;
static unsigned char blob[16384];
static void *dt = blob;
static void *adt;

/* One board: the tree's display processor aliases and its ADT's clock-ids per engine. */
#define ENGINES 9 /* disp0, dispext0..7 */
struct board {
    const char *name;
    const char *aliases[6];   /* "dcp", "dcpext", "dcpext<n>" */
    int nodes[6];             /* tree node for each alias (two aliases may share a die's node) */
    int adt_ids[ENGINES];     /* -1: no engine; 0: empty clock-ids; 2: the T8112 pair */
    int ids_index[ENGINES];   /* which pair: 0 disp0's, 1 dispext0's */
};
static const struct board *b;
static const u32 pairs[2][2] = {{344, 415}, {350, 418}};
static u32 clock_hz[256];

static int adt_path_offset(void *tree, const char *path)
{
    (void)tree;
    if (!strcmp(path, "/arm-io")) return 1;
    if (!strcmp(path, "/arm-io/disp0")) return b->adt_ids[0] < 0 ? -1 : 10;
    unsigned n;
    if (sscanf(path, "/arm-io/dispext%u", &n) == 1 && n < 8)
        return b->adt_ids[1 + n] < 0 ? -1 : 11 + (int)n;
    return -1;
}
static const void *adt_getprop(void *tree, int node, const char *name, u32 *len)
{
    (void)tree;
    static const u32 empty;
    if (node == 1 && !strcmp(name, "clock-frequencies")) { *len = sizeof(clock_hz); return clock_hz; }
    if (node >= 10 && node < 10 + ENGINES && !strcmp(name, "clock-ids")) {
        int e = node - 10;
        if (b->adt_ids[e] == 0) { *len = 0; return &empty; }
        *len = sizeof(pairs[0]);
        return pairs[b->ids_index[e]];
    }
    return NULL;
}

''' + production + r'''

static const struct board boards[] = {
    /* T8112: dcp + dcpext; the same ADT pairs on every board. */
    {"j413", {"dcp", "dcpext"}, {0, 1}, {2, 2, -1, -1, -1, -1, -1, -1, -1}, {0, 1}},
    {"j415", {"dcp", "dcpext"}, {0, 1}, {2, 2, -1, -1, -1, -1, -1, -1, -1}, {0, 1}},
    {"j493", {"dcp", "dcpext"}, {0, 1}, {2, 2, -1, -1, -1, -1, -1, -1, -1}, {0, 1}},
    {"j473", {"dcp", "dcpext"}, {0, 1}, {2, 2, -1, -1, -1, -1, -1, -1, -1}, {0, 1}},
    /* T6020/T6021 laptops: dcp + dcpext0/1 (+2/3 on T6021); every ADT list is empty. */
    {"j414s", {"dcp", "dcpext0", "dcpext1"}, {0, 1, 2}, {0, 0, 0, -1, -1, -1, -1, -1, -1}, {0}},
    {"j416s", {"dcp", "dcpext0", "dcpext1"}, {0, 1, 2}, {0, 0, 0, -1, -1, -1, -1, -1, -1}, {0}},
    {"j414c", {"dcp", "dcpext0", "dcpext1", "dcpext2", "dcpext3"}, {0, 1, 2, 3, 4},
     {0, 0, 0, 0, 0, -1, -1, -1, -1}, {0}},
    {"j416c", {"dcp", "dcpext0", "dcpext1", "dcpext2", "dcpext3"}, {0, 1, 2, 3, 4},
     {0, 0, 0, 0, 0, -1, -1, -1, -1}, {0}},
    /* Desktops: external processors only in the tree. */
    {"j474s", {"dcpext0", "dcpext1"}, {1, 2}, {0, 0, 0, -1, -1, -1, -1, -1, -1}, {0}},
    {"j475c", {"dcpext0", "dcpext1"}, {1, 2}, {0, 0, 0, 0, 0, -1, -1, -1, -1}, {0}},
    /* T6022: die 1's first external processor is dcpext4 (/arm-io/dispext4). */
    {"j475d", {"dcpext4"}, {5}, {-1, 0, 0, 0, 0, 0, 0, 0, 0}, {0}},
    {"j180d", {"dcpext0", "dcpext4"}, {1, 5}, {-1, 0, 0, 0, 0, 0, 0, 0, 0}, {0}},
};
static const char *const node_path[6] = {"/dcp", "/dcpext-a", "/dcpext-b", "/dcpext-c",
                                         "/dcpext-d", "/die1-dcpext-a"};

static void world(const struct board *board, enum fw_version fw)
{
    b = board;
    os_firmware.version = fw;
    assert(!fdt_create_empty_tree(blob, sizeof(blob)));
    int clk = fdt_add_subnode(dt, 0, "clock-disp");
    assert(clk >= 0 && !fdt_setprop_u32(dt, clk, "phandle", 1));
    int aliases = fdt_add_subnode(dt, 0, "aliases");
    assert(aliases >= 0);
    for (int i = 0; i < 6 && b->aliases[i]; i++) {
        const char *path = node_path[b->nodes[i]];
        if (fdt_path_offset(dt, path) < 0) {
            int node = fdt_add_subnode(dt, 0, path + 1);
            assert(node >= 0 && !fdt_setprop_u32(dt, node, "clocks", 1));
        }
        aliases = fdt_path_offset(dt, "/aliases");
        assert(!fdt_setprop_string(dt, aliases, b->aliases[i], path));
    }
}

static int engine_of(const char *alias)
{
    if (!strcmp(alias, "dcp")) return 0;
    if (!strcmp(alias, "dcpext")) return 1;
    return 1 + atoi(alias + strlen("dcpext"));
}

int main(int argc, char **argv)
{
    (void)argc;
    for (unsigned i = 0; i < ARRAY_SIZE(clock_hz); i++)
        clock_hz[i] = 1000000u * (i + 1);
    for (size_t i = 0; i < ARRAY_SIZE(boards); i++) {
        world(&boards[i], V27_2);
        dt_set_dcp_fw27_clocks_all();
        for (int a = 0; a < 6 && b->aliases[a]; a++) {
            int node = fdt_path_offset(dt, node_path[b->nodes[a]]);
            int len = -1, clen = 0;
            const fdt32_t *ids = fdt_getprop(dt, node, "apple,clock-ids", &len);
            const fdt32_t *clocks = fdt_getprop(dt, node, "clocks", &clen);
            int e = engine_of(b->aliases[a]);
            printf("%s %s: clock-ids %d bytes, clocks %d\n", b->name, b->aliases[a], len, clen);
            assert(ids);
            if (b->adt_ids[e] == 0) {
                /* Empty in the ADT: published empty; the display clock stays alone. */
                assert(len == 0 && clen == 4 && fdt32_ld(clocks) == 1);
                assert(!fdt_getprop(dt, node, "clock-names", NULL));
            } else {
                /* The T8112 pair: display + firmware-0/1, at the ADT's rates. */
                const u32 *want = pairs[b->ids_index[e]];
                assert(len == 8 && fdt32_ld(ids) == want[0] && fdt32_ld(ids + 1) == want[1]);
                assert(clen == 12 && fdt32_ld(clocks) == 1);
                for (int k = 0; k < 2; k++) {
                    int c = fdt_node_offset_by_phandle(dt, fdt32_ld(clocks + 1 + k));
                    const fdt32_t *hz = fdt_getprop(dt, c, "clock-frequency", NULL);
                    assert(c >= 0 && hz && fdt32_ld(hz) == clock_hz[want[k] - 256]);
                }
            }
        }
        char path[512];
        snprintf(path, sizeof(path), "%s/%s-27.2.dtb", argv[1], b->name);
        FILE *f = fopen(path, "wb");
        assert(f && fwrite(blob, fdt_totalsize(blob), 1, f) == 1 && !fclose(f));

        /* Before firmware 27 nothing is published. */
        world(&boards[i], V13_5);
        static unsigned char before[sizeof(blob)];
        memcpy(before, blob, sizeof(blob));
        dt_set_dcp_fw27_clocks_all();
        assert(!memcmp(before, blob, sizeof(blob)));

        /* A second pass changes nothing (apple,clock-ids already there). */
        world(&boards[i], V27_2);
        dt_set_dcp_fw27_clocks_all();
        memcpy(before, blob, sizeof(blob));
        dt_set_dcp_fw27_clocks_all();
        assert(!memcmp(before, blob, sizeof(blob)));
    }
    return 0;
}
'''
    binary = tmp_path / "dcp-fw27-clocks"
    lib = ROOT / "src/libfdt"
    subprocess.run(
        ["cc", "-std=gnu11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
         "-Wno-unused-parameter", "-Wno-unused-function", "-I", str(lib), "-x", "c", "-",
         *map(str, (lib / f for f in (
             "fdt.c", "fdt_ro.c", "fdt_rw.c", "fdt_sw.c", "fdt_wip.c",
             "fdt_empty_tree.c", "fdt_strerror.c"))), "-o", str(binary)],
        input=source, text=True, check=True,
    )
    result = subprocess.run([str(binary), str(tmp_path)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    for name in ("j413", "j415", "j493", "j473", "j414s", "j416s", "j474s", "j414c", "j416c",
                 "j475c", "j475d", "j180d"):
        assert (tmp_path / f"{name}-27.2.dtb").stat().st_size > 0
    # Every external processor alias is walked, and no chip or board decides it.
    walk = function(kboot, "dt_set_dcp_fw27_clocks_all", "static void ")
    assert "dcpext_aliases" in walk
    assert not re.search(r"check_compatible|chip_id|V27_2", walk + clocks)
