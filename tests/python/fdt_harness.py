# SPDX-License-Identifier: MIT
"""Build a C harness from functions extracted from src/ against the in-tree libfdt and run it."""
from pathlib import Path
import subprocess

from host_compiler import host_cc

REPO = Path(__file__).resolve().parents[2]
LIBFDT = REPO / "src/libfdt"
LIBFDT_SOURCES = ("fdt.c", "fdt_ro.c", "fdt_rw.c", "fdt_wip.c", "fdt_sw.c", "fdt_strerror.c",
                  "fdt_empty_tree.c", "fdt_addresses.c")


def definition(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}\n", start) + 3]


def run(tmp_path, name, code, expect):
    path = tmp_path / f"{name}.c"
    path.write_text(code)
    binary = tmp_path / name
    command = host_cc() + ["-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                           "-Wno-unused-function", "-fsanitize=undefined",
                           "-fno-sanitize-recover=all", "-I", str(LIBFDT), str(path)]
    command += [str(LIBFDT / f) for f in LIBFDT_SOURCES] + ["-o", str(binary)]
    built = subprocess.run(command, capture_output=True, text=True)
    assert built.returncode == 0, built.stdout + built.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stdout + result.stderr
    assert expect in result.stdout, result.stdout


PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "libfdt.h"
/* Match aura's aarch64-linux-gnu target, not the host libc's uint64_t alias. */
typedef unsigned long u64;
_Static_assert(sizeof(u64) == 8, "the aura host fixture requires an LP64 host");
/* Code under test passes u64 * to libfdt's uint64_t *; the two can differ on the host. */
static inline int harness_get_mem_rsv(const void *fdt, int n, u64 *address, u64 *size)
{
    uint64_t a = 0, s = 0;
    int ret = fdt_get_mem_rsv(fdt, n, &a, &s);
    *address = a;
    *size = s;
    return ret;
}
#define fdt_get_mem_rsv harness_get_mem_rsv
typedef uint32_t u32;
typedef uint8_t u8;
#define bail(...) do { printf(__VA_ARGS__); return -1; } while (0)
static unsigned char tree[16384];
__attribute__((unused)) static void *dt = tree;
static int node_at(const char *parent, const char *name)
{
    int p = fdt_path_offset(tree, parent);
    assert(p >= 0);
    int n = fdt_add_subnode(tree, p, name);
    assert(n >= 0);
    return n;
}
static void reg(int node, u64 base, u64 size)
{
    fdt64_t r[2] = {cpu_to_fdt64(base), cpu_to_fdt64(size)};
    assert(!fdt_setprop(tree, node, "reg", r, sizeof(r)));
}
static void fresh(void)
{
    memset(tree, 0, sizeof(tree));
    assert(!fdt_create_empty_tree(tree, sizeof(tree)));
}
'''
