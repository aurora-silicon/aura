# SPDX-License-Identifier: MIT
"""Reserve prior-stage tables after the chainload-local record has gone away.

Any chip whose display DART is locked can have an earlier stage's tables in the
inherited roots (display_dart_tables_inherited()); the rule is not chip-specific.

The tested block and final reservation call are extracted from kboot_prepare_dt.
Real in-tree libfdt validates the outgoing reservation. Boot arguments and image
addresses are simulated; no actual chainload, firmware, or DMA is exercised.
"""
from pathlib import Path
import os
import re
import subprocess

import pytest

SOURCE = Path(os.environ.get("DART_DISPLAY_SOURCE", Path(__file__).resolve().parents[2]))


def test_inherited_reservation(tmp_path):
    text = (SOURCE / "src/kboot.c").read_text()
    start = text.index("    u64 image_end = (u64)_end;", text.index("int kboot_prepare_dt("))
    # Followed by the post-M4 retained-heap block.
    end = text.index("    if (nvme_post_m4())", start)
    block = text[start:end]
    call = re.search(
        r"    if \(dt_reserve_outside_exec_window\(\(u64\)_base, image_end - \(u64\)_base\)\)\n"
        r"        bail\([^\n]+\);", text[end:]
    )
    assert call, "missing real aura image reservation"
    code = tmp_path / "inherited.c"
    code.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "libfdt.h"
typedef uint64_t u64;
#define T8103 0x8103
#define T8112 0x8112
#define max(a, b) ((a) > (b) ? (a) : (b))
#define bail(...) do { printf(__VA_ARGS__); return -1; } while (0)
static struct { u64 phys_base, mem_size, top_of_kernel_data; } cur_boot_args;
static unsigned chip_id;
static bool tables_inherited;
static bool display_dart_tables_inherited(void) { (void)chip_id; return tables_inherited; }
static u64 image_base, image_end_address;
#define _base ((void *)(uintptr_t)image_base)
#define _end ((void *)(uintptr_t)image_end_address)
static unsigned char tree[4096], before[4096];
static void *dt = tree;
/* Without a window this is the plain call (test_ctxr_exec_window.py). */
#define dt_reserve_outside_exec_window(start, size) fdt_add_mem_rsv(dt, start, size)
static int reserve(void)
{
''' + block + call.group(0) + r'''
    return 0;
}
static void reset(void)
{
    memset(tree, 0, sizeof(tree));
    assert(!fdt_create_empty_tree(tree, sizeof(tree)));
    memcpy(before, tree, sizeof(tree));
    chip_id = T8103;
    tables_inherited = true;
    cur_boot_args.phys_base = 0x800000000UL;
    cur_boot_args.mem_size = 0x20000000;
    image_base = cur_boot_args.phys_base + 0x100000;
    image_end_address = image_base + 0x400000;
    cur_boot_args.top_of_kernel_data = image_base + 0xc00000;
}
static void range(u64 expected_end)
{
    u64 address, size;
    assert(!reserve());
    assert(fdt_num_mem_rsv(tree) == 1);
    assert(!fdt_get_mem_rsv(tree, 0, &address, &size));
    assert(address == image_base && size == expected_end - image_base);
    assert(address + size == expected_end);
}
static void refused(void)
{
    assert(reserve() == -1);
    assert(!memcmp(tree, before, sizeof(tree)));
    assert(!fdt_num_mem_rsv(tree));
}
int main(void)
{
    /* Stage 1 published an L2 at +8 MiB, then passed a +12 MiB high-water
     * mark. Its static list does not exist in this fresh stage-2 process. */
    reset();
    u64 previous_table = image_base + 0x800000;
    u64 current_heap = image_base + 0x8000000;
    range(cur_boot_args.top_of_kernel_data);
    u64 address, size;
    assert(!fdt_get_mem_rsv(tree, 0, &address, &size));
    assert(previous_table >= address && previous_table + 0x4000 <= address + size);
    assert(current_heap > address + size); /* Not the current stage's heap. */
    reset(); cur_boot_args.top_of_kernel_data = image_base + 0x200000;
    range(image_end_address);
    reset(); cur_boot_args.top_of_kernel_data = image_end_address;
    range(image_end_address);
    reset(); cur_boot_args.mem_size = 0; refused();
    reset(); cur_boot_args.phys_base = UINT64_MAX - 0x100; refused();
    reset(); image_base = cur_boot_args.phys_base - 0x4000; refused();
    /* A mark below our own base covers none of this image: image only. */
    reset(); cur_boot_args.top_of_kernel_data = image_base - 1;
    range(image_end_address);
    reset(); cur_boot_args.top_of_kernel_data =
        cur_boot_args.phys_base + cur_boot_args.mem_size + 1; refused();
    reset(); image_end_address = cur_boot_args.phys_base + cur_boot_args.mem_size + 1;
    refused();
    reset();
    fdt_set_totalsize(tree, fdt_off_dt_strings(tree) + fdt_size_dt_strings(tree));
    memcpy(before, tree, sizeof(tree));
    refused(); /* No room to add a reservation: handoff must fail. */
    /* The same rule on another chip with a locked display DART. */
    reset(); chip_id = T8112;
    range(cur_boot_args.top_of_kernel_data);
    /* No locked display DART: the image-only reservation, the mark unused. */
    reset(); chip_id = T8112; tables_inherited = false;
    range(image_end_address);
    reset(); tables_inherited = false; cur_boot_args.top_of_kernel_data = image_base - 1;
    range(image_end_address);
    reset(); tables_inherited = false; cur_boot_args.mem_size = 0;
    range(image_end_address);
    puts("PASS inherited reservation 14 cases");
    return 0;
}
''')
    lib = SOURCE / "src/libfdt"
    binary = tmp_path / "inherited"
    command = [os.environ.get("CC", "cc"), "-std=gnu11", "-O1", "-g",
               "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
               "-fno-sanitize-recover=all", "-I", str(lib), str(code)]
    command += [str(lib / name) for name in (
        "fdt.c", "fdt_ro.c", "fdt_rw.c", "fdt_wip.c", "fdt_sw.c",
        "fdt_strerror.c", "fdt_empty_tree.c", "fdt_check.c",
    )]
    command += ["-o", str(binary)]
    compiled = subprocess.run(command, capture_output=True, text=True)
    assert compiled.returncode == 0, compiled.stdout + compiled.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "PASS inherited reservation 14 cases" in result.stdout
