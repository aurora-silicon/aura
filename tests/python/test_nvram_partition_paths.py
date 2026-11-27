# SPDX-License-Identifier: GPL-2.0-only
# Generated-by: LLM
# Assisted-by: LLM
"""dt_set_nvram() fills the DT's nvram partition from spi1's or, failing that, qspi's ADT anvram."""

from pathlib import Path
import subprocess

import pytest

SPI1 = "/arm-io/spi1/spinor/anvram"
QSPI = "/arm-io/qspi/spinor/anvram"


@pytest.mark.parametrize("present, start, size, filled", [
    ((SPI1,), 0x700000, 0x100000, True),          # t600x/t602x/t603x: unchanged
    ((QSPI,), 0x700000, 0xc0000, True),           # T6050: the partition under the QSPI controller
    ((SPI1, QSPI), 0x700000, 0x100000, True),     # spi1 is looked up first
    ((), 0x700000, 0xc0000, False),               # neither: the DT is left alone
    ((QSPI,), 0x600000, 0xc0000, False),          # an unexpected start is still refused
    ((QSPI,), 0x700000, 0x200000, False),         # as is a partition over 1 MiB
], ids=["spi1", "qspi", "both", "none", "qspi-bad-start", "qspi-too-big"])
def test_nvram_partition_from_spi1_or_qspi(tmp_path, present, start, size, filled):
    repo = Path(__file__).resolve().parents[2]
    source = (repo / "src/kboot.c").read_text()
    def definition(start_text, terminator):
        begin = source.index(start_text)
        return source[begin:source.index(terminator, begin) + len(terminator)]

    helper = "\n".join((
        definition("#define NVRAM_START", "\n\n"),
        definition("static const char *const nvram_adt_paths[]", ";\n"),
        definition("static int dt_set_nvram(void)", "\n}"),
    ))
    paths = ", ".join(f'"{p}"' for p in present) or "NULL"
    harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef unsigned long u64;
typedef uint32_t fdt32_t;
#define SZ_1M 0x100000
#define ARRAY_SIZE(s) (sizeof(s) / sizeof((s)[0]))
static void *adt, *dt;
static const char *present[] = {''' + paths + r''', NULL};
static const char *found;
static fdt32_t set_reg[2];
static int reg_set, enabled;
static int adt_path_offset_trace(const void *tree, const char *path, int *offsets)
{
    (void)tree;
    for (int i = 0; present[i]; i++)
        if (!strcmp(path, present[i])) {
            found = present[i];
            offsets[0] = 1; offsets[1] = 2; offsets[2] = 3; offsets[3] = 4; offsets[4] = 0;
            return 4;
        }
    return -1;
}
static int adt_get_reg(const void *tree, int *path, const char *prop, int i, u64 *a, u64 *s)
{
    (void)tree; (void)i;
    assert(found && path[3] == 4 && path[4] == 0 && !strcmp(prop, "reg"));
    *a = START; *s = SIZE; return 0;
}
static int fdt_path_offset(const void *tree, const char *path)
{ (void)tree; assert(!strcmp(path, "nvram")); return 7; }
static void fdt32_st(fdt32_t *p, uint32_t v) { *p = v; }
static int fdt_setprop(void *tree, int node, const char *name, const void *val, int len)
{
    (void)tree;
    assert(node == 7 && !strcmp(name, "reg") && len == sizeof(set_reg));
    memcpy(set_reg, val, sizeof(set_reg)); reg_set = 1; return 0;
}
static int fdt_setprop_string(void *tree, int node, const char *name, const char *value)
{
    (void)tree;
    assert(node == 7 && !strcmp(name, "status") && !strcmp(value, "okay"));
    enabled = 1; return 0;
}
''' + helper + r'''
int main(void)
{
    assert(dt_set_nvram() == 0);
    if (FILLED) {
        assert(!strcmp(found, present[0]));
        assert(reg_set && enabled && set_reg[0] == START && set_reg[1] == SIZE);
    } else {
        assert(!reg_set && !enabled);
    }
    return 0;
}
'''
    binary = tmp_path / "nvram-partition-paths"
    subprocess.run(["cc", f"-DSTART={start:#x}", f"-DSIZE={size:#x}", f"-DFILLED={int(filled)}",
                    "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-variable",
                    "-x", "c", "-", "-o", str(binary)],
                   input=harness, text=True, check=True)
    subprocess.run([str(binary)], check=True)
