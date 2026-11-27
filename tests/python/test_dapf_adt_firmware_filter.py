# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""dapf_init_fdt() leaves a DAPF to the firmware only where the DART's ADT says
allow-dram-apf-slices-<n> = 0 for the instance it would write; every other DART is written as
before."""
from pathlib import Path
import subprocess


def test_dapf_adt_keeps_firmware_filter(tmp_path):
    repo = Path(__file__).resolve().parents[2]
    text = (repo / "src/dapf.c").read_text()
    start = text.index("static int dapf_adt_keeps_firmware_filter(")
    production = text[start:text.index("static int dapf_init_j713(", start)]
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
static void *adt;
static int present = 1, t8110 = 1, instance = 0, slices_instance = 0, slices_len = 4;
static u32 slices;
static const unsigned char entries[52];
static int adt_path_offset(void *t, const char *p) { return present ? 1 : -1; }
static bool adt_is_compatible(void *t, int n, const char *c)
{ return t8110 && !strcmp(c, "dart,t8110"); }
static const void *adt_getprop(void *t, int n, const char *p, u32 *len)
{
    char want[32];
    snprintf(want, sizeof(want), "dapf-instance-%d", instance);
    if (!strcmp(p, want)) { *len = sizeof(entries); return entries; }
    snprintf(want, sizeof(want), "allow-dram-apf-slices-%d", slices_instance);
    if (slices_len >= 0 && !strcmp(p, want)) { *len = slices_len; return &slices; }
    return NULL;
}
''' + production + r'''
int main(void)
{
    /* J716s dart-aop: dapf-instance-0 and allow-dram-apf-slices-0 = 0 */
    assert(dapf_adt_keeps_firmware_filter("/arm-io/dart-aop") == 0);
    /* Slices allowed: written as before */
    slices = 1;
    assert(dapf_adt_keeps_firmware_filter("/arm-io/dart-aop") < 0);
    slices = 0;
    /* No property (every DART but the M5-family dart-aop): written as before */
    slices_len = -1;
    assert(dapf_adt_keeps_firmware_filter("/arm-io/dart-mtp") < 0);
    /* A malformed property is not taken as 0 */
    slices_len = 8;
    assert(dapf_adt_keeps_firmware_filter("/arm-io/dart-aop") < 0);
    slices_len = 4;
    /* The property of an instance other than the one written does not count */
    instance = 1;
    assert(dapf_adt_keeps_firmware_filter("/arm-io/dart-aop") < 0);
    slices_instance = 1;
    assert(dapf_adt_keeps_firmware_filter("/arm-io/dart-aop") == 1);
    /* Not a T8110 DART, or not in the ADT */
    t8110 = 0;
    assert(dapf_adt_keeps_firmware_filter("/arm-io/dart-aop") < 0);
    t8110 = 1;
    present = 0;
    assert(dapf_adt_keeps_firmware_filter("/arm-io/dart-aop") < 0);
    return 0;
}
'''
    source = tmp_path / "dapf-keep.c"
    source.write_text(harness)
    binary = tmp_path / "dapf-keep"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
