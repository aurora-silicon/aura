# SPDX-License-Identifier: GPL-2.0-only
"""T8152 publishes the SEP only for a tree that names it, never fatally, on every J804 boot path."""

from pathlib import Path
import re
import subprocess


def extract(source, start, end):
    first = source.index(start)
    return source[first:source.index(end, first)]


def test_t8152_sep_call_site():
    source = (Path(__file__).resolve().parents[2] / "src/kboot.c").read_text()
    body = extract(source, "static int dt_prepare_t8152(void)\n{", "\n}\n")
    minimal = extract(body, 'if (fdt_getprop(dt, 0, "aurora,minimal-boot", NULL)) {', "\n    }\n")
    # Outside the minimal-boot branch, before /memory is written and the tree is packed.
    assert "dt_set_sep" not in minimal
    assert body.count("dt_set_sep_t8152();") == 1
    assert body.index("dt_set_sep_t8152();") < body.index("dt_set_memory()")
    assert "dt_set_sep()" not in body
    # The extra room is taken only for a T8152 tree with a sep alias.
    prepare = extract(source, "int kboot_prepare_dt(void *fdt)", "dt = memalign(")
    assert re.search(r'chip_id == T8152 && fdt_get_alias\(fdt, "sep"\)\)\s*\n\s*dt_bufsize \+= SZ_16K;',
                     prepare)


def test_t8152_sep_gate(tmp_path):
    repo = Path(__file__).resolve().parents[2]
    source = (repo / "src/kboot.c").read_text()
    gate = extract(source, "static void dt_set_sep_t8152(void)", "static int dt_prepare_t8152(void)")
    harness = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "libfdt.h"
static unsigned char blob[4096];
static void *dt = blob;
static int calls, result;
static int dt_set_sep(void) { calls++; return result; }
''' + gate + r'''
static void tree(const char *alias, int with_node)
{
    assert(fdt_create_empty_tree(dt, sizeof(blob)) == 0);
    assert(fdt_add_subnode(dt, 0, "soc") >= 0);
    if (with_node)
        assert(fdt_add_subnode(dt, fdt_path_offset(dt, "/soc"), "sep@0") >= 0);
    int aliases = fdt_add_subnode(dt, 0, "aliases");
    assert(aliases >= 0);
    assert(fdt_setprop_string(dt, aliases, "dcp", "/soc/dcp@0") == 0);
    if (alias)
        assert(fdt_setprop_string(dt, fdt_path_offset(dt, "/aliases"), "sep", alias) == 0);
    calls = 0;
}
int main(void)
{
    unsigned char before[sizeof(blob)];

    /* No alias (the installed J804 tree): untouched, no call. */
    tree(NULL, 1);
    memcpy(before, blob, sizeof(blob));
    dt_set_sep_t8152();
    assert(calls == 0 && !memcmp(before, blob, sizeof(blob)));
    /* An alias that names no node: untouched, no call, not fatal. */
    tree("/soc/sep@0", 0);
    memcpy(before, blob, sizeof(blob));
    dt_set_sep_t8152();
    assert(calls == 0 && !memcmp(before, blob, sizeof(blob)));
    /* A SEP node: dt_set_sep() runs; a refusal is logged, never fatal (the gate returns void). */
    tree("/soc/sep@0", 1);
    result = 0;
    dt_set_sep_t8152();
    assert(calls == 1);
    tree("/soc/sep@0", 1);
    result = -1;
    dt_set_sep_t8152();
    assert(calls == 1);
    return 0;
}
'''
    binary = tmp_path / "sep-t8152-gate"
    libfdt = repo / "src/libfdt"
    sources = [libfdt / name for name in (
        "fdt.c", "fdt_addresses.c", "fdt_empty_tree.c",
        "fdt_ro.c", "fdt_rw.c", "fdt_strerror.c", "fdt_sw.c", "fdt_wip.c",
    )]
    subprocess.run(
        ["cc", "-D_GNU_SOURCE", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(libfdt),
         "-x", "c", "-", *map(str, sources), "-o", str(binary)],
        input=harness, text=True, check=True,
    )
    subprocess.run([str(binary)], check=True)
