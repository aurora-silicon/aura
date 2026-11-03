# SPDX-License-Identifier: MIT
"""Run the actual RAM-map builder on four-byte-aligned FDT properties."""

import os
from pathlib import Path
import subprocess


def test_reserved_memory_reg_alignment(tmp_path):
    repo = Path(__file__).resolve().parents[2]
    source = Path(os.environ.get("AURA_MEMORY_SOURCE", repo / "src/kboot.c")).read_text()
    start = source.index("static int dt_set_memory(void)")
    function = source[start:source.index("\nstatic ", start + 1)]
    program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "libfdt.h"
/* Match aura's aarch64-linux-gnu target, not the host libc's uint64_t alias. */
typedef unsigned long u64;
_Static_assert(sizeof(u64) == 8, "the aura host fixture requires an LP64 host");
#define MAX_MEM_REGIONS 32
#define T6040 0x6040
static const unsigned chip_id = 0x8103;
#define dt_t6040_memory_holes(...) 0
#define bail(...) do { printf(__VA_ARGS__); return -1; } while (0)
static _Alignas(8) unsigned char blob[8192];
static void *dt = blob, *adt;
static u64 mem_size_actual = 0x1000000;
static struct { u64 phys_base, mem_size; } cur_boot_args = {0x100000, 0x100000};
static int adt_path_offset(void *tree, const char *name)
{
    (void)tree;
    assert(!strcmp(name, "/chosen"));
    return 1;
}
static int get_property(const char *name, u64 *value)
{
    assert(!strcmp(name, "dram-base") || !strcmp(name, "dram-size"));
    *value = !strcmp(name, "dram-size") ? 0x1000000 : 0;
    return sizeof(*value);
}
#define ADT_GETPROP(a, n, k, v) get_property(k, v)
static size_t get_page_size(void) { return 16384; }
''' + function + r'''
int main(void)
{
    const char *names[] = {"a", "aa", "aaa", "aaaa", "aaaaa", "aaaaaa", "aaaaaaa", "aaaaaaaa"};
    unsigned int aligned = 0, four_byte_aligned = 0;
    for (unsigned int n = 0; n < sizeof(names) / sizeof(names[0]); n++) {
        assert(!fdt_create_empty_tree(dt, sizeof(blob)));
        assert(fdt_add_subnode(dt, 0, "memory") >= 0);
        int parent = fdt_add_subnode(dt, 0, "reserved-memory");
        assert(parent >= 0);
        int node = fdt_add_subnode(dt, parent, names[n]);
        assert(node >= 0);
        const fdt64_t range[] = {cpu_to_fdt64(0x400000), cpu_to_fdt64(16384)};
        assert(!fdt_setprop(dt, node, "reg", range, sizeof(range)));
        node = fdt_first_subnode(dt, fdt_path_offset(dt, "/reserved-memory"));
        const void *property = fdt_getprop(dt, node, "reg", NULL);
        assert(property && !((uintptr_t)property & 3));
        if ((uintptr_t)property & 7)
            four_byte_aligned++;
        else
            aligned++;

        assert(!dt_set_memory());
        int length = 0;
        const fdt64_t *reg = fdt_getprop(dt, fdt_path_offset(dt, "/memory"), "reg", &length);
        assert(reg && length == (int)(4 * sizeof(*reg)));
        assert(fdt64_ld(&reg[0]) == cur_boot_args.phys_base);
        assert(fdt64_ld(&reg[1]) == cur_boot_args.mem_size);
        assert(fdt64_ld(&reg[2]) == 0x400000);
        assert(fdt64_ld(&reg[3]) == 16384);
    }
    assert(aligned && four_byte_aligned && aligned + four_byte_aligned == 8);
    puts("PASS: eight RAM maps include both FDT property alignments");
    return 0;
}
'''
    source_file = tmp_path / "memory-fdt-alignment.c"
    source_file.write_text(program)
    binary = tmp_path / "memory-fdt-alignment"
    libfdt = repo / "src/libfdt"
    sources = [libfdt / name for name in (
        "fdt.c", "fdt_addresses.c", "fdt_empty_tree.c", "fdt_ro.c",
        "fdt_rw.c", "fdt_strerror.c", "fdt_sw.c", "fdt_wip.c",
    )]
    subprocess.run(
        ["cc", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
         "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
         "-fno-omit-frame-pointer", "-I", str(libfdt), str(source_file),
         *map(str, sources), "-o", str(binary)],
        check=True,
    )
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "PASS: eight RAM maps include both FDT property alignments" in result.stdout
