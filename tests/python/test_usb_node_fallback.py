# SPDX-License-Identifier: GPL-2.0-only
"""USB port nodes: numbered ADT node first, unnumbered name only for a missing port 0."""

from pathlib import Path
import re
import subprocess

SOURCE = Path(__file__).resolve().parents[2] / "src/usb.c"


def test_no_chip_gate_on_node_names():
    source = SOURCE.read_text()
    assert not re.search(r'chip_id == T8140[^\n]*\n[^\n]*"/arm-io/(usb-drd|dart-usb)"', source)
    for name in ('"/arm-io/usb-drd"', '"/arm-io/dart-usb"', '"/arm-io/dart-usb/mapper-usb"'):
        for line in source.splitlines():
            if name in line:
                assert "usb_adt_node_path(" in line


def test_node_path_fallback(tmp_path):
    source = SOURCE.read_text()
    helper = source[source.index("static void usb_adt_node_path("):
                    source.index("static dart_dev_t *usb_dart_init(")]
    harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
#define FMT_DART_PATH        "/arm-io/dart-usb%u"
#define FMT_DART_MAPPER_PATH "/arm-io/dart-usb%u/mapper-usb%u"
#define FMT_DRD_PATH         "/arm-io/usb-drd%u"
static const char *const *nodes;
static void *adt;
static int adt_path_offset(void *dt, const char *path)
{
    (void)dt;
    for (int i = 0; nodes[i]; i++)
        if (!strcmp(nodes[i], path))
            return i + 1;
    return -1;
}
''' + helper + r'''
static void check(const char *fmt, const char *unindexed, u32 idx, const char *want)
{
    char path[64];
    usb_adt_node_path(path, sizeof(path), fmt, unindexed, idx);
    if (strcmp(path, want)) {
        fprintf(stderr, "got %s want %s\n", path, want);
        assert(0);
    }
}
int main(void)
{
    /* Every numbered ADT in the collection (t8103 .. t8152): unchanged. */
    static const char *const numbered[] = {"/arm-io/usb-drd0", "/arm-io/dart-usb0",
        "/arm-io/dart-usb0/mapper-usb0", "/arm-io/usb-drd1", "/arm-io/dart-usb1",
        "/arm-io/dart-usb1/mapper-usb1", NULL};
    nodes = numbered;
    check(FMT_DRD_PATH, "/arm-io/usb-drd", 0, "/arm-io/usb-drd0");
    check(FMT_DART_PATH, "/arm-io/dart-usb", 0, "/arm-io/dart-usb0");
    check(FMT_DART_MAPPER_PATH, "/arm-io/dart-usb/mapper-usb", 1, "/arm-io/dart-usb1/mapper-usb1");
    check(FMT_DRD_PATH, "/arm-io/usb-drd", 2, "/arm-io/usb-drd2");

    /* J700: one port, nodes without an index. */
    static const char *const single[] = {"/arm-io/usb-drd", "/arm-io/dart-usb",
        "/arm-io/dart-usb/mapper-usb", "/arm-io/atc-phy0", NULL};
    nodes = single;
    check(FMT_DRD_PATH, "/arm-io/usb-drd", 0, "/arm-io/usb-drd");
    check(FMT_DART_PATH, "/arm-io/dart-usb", 0, "/arm-io/dart-usb");
    check(FMT_DART_MAPPER_PATH, "/arm-io/dart-usb/mapper-usb", 0, "/arm-io/dart-usb/mapper-usb");
    /* An unnumbered node never stands for port 1. */
    check(FMT_DRD_PATH, "/arm-io/usb-drd", 1, "/arm-io/usb-drd1");

    /* Neither present: keep the numbered name so the caller reports it missing. */
    static const char *const none[] = {NULL};
    nodes = none;
    check(FMT_DRD_PATH, "/arm-io/usb-drd", 0, "/arm-io/usb-drd0");
    return 0;
}
'''
    binary = tmp_path / "usb-node-fallback"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-format-extra-args",
                    "-x", "c", "-", "-o", str(binary)], input=harness, text=True, check=True)
    subprocess.run([str(binary)], check=True)
