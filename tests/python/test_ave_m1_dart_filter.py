# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""dapf_ave_filter_inherited(): which encoder CPU DART keeps the filter iBoot left.

dart-ave keeps it from the M2 on (a T8110-type DART). The M1's dart-ave is a T8020-type DART whose
filter the ADT gives as filter-data-instance-0, as the M1 Pro's dart-ave0: kboot.c programs it.
"""
from pathlib import Path
import subprocess


def block(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
static void *adt;
static const char *compat;
static bool have_filter, have_node;
#define DART 7
static int adt_path_offset(void *a, const char *p)
{
    (void)a;
    return have_node && !strcmp(p, "/arm-io/dart-ave") ? DART : -1;
}
static bool adt_is_compatible(void *a, int node, const char *c)
{
    (void)a;
    return node == DART && !strcmp(c, compat);
}
static const void *adt_getprop(void *a, int node, const char *name, u32 *len)
{
    (void)a;
    (void)len;
    return node == DART && have_filter && !strcmp(name, "filter-data-instance-0") ? "" : NULL;
}
'''

MAIN = r'''
static bool inherited(const char *c, bool filter, const char *path)
{
    compat = c;
    have_filter = filter;
    have_node = true;
    return dapf_ave_filter_inherited(path);
}
int main(void)
{
    /* M2 through M5: a T8110-type dart-ave keeps iBoot's filter, ADT filter data or not. */
    assert(inherited("dart,t8110", false, "/arm-io/dart-ave"));
    assert(inherited("dart,t8110", true, "/arm-io/dart-ave"));
    /* M1 (J313): T8020-type with its ADT filter, programmed by kboot.c. */
    assert(!inherited("dart,t8020", true, "/arm-io/dart-ave"));
    assert(!inherited("dart,t6000", true, "/arm-io/dart-ave"));
    /* A T8020-type DART without filter data has nothing to program from. */
    assert(inherited("dart,t8020", false, "/arm-io/dart-ave"));
    /* No ADT node: as before. */
    compat = "dart,t8020"; have_filter = true; have_node = false;
    assert(dapf_ave_filter_inherited("/arm-io/dart-ave"));
    /* Only dart-ave: dart-ave0 and other DARTs are never inherited here. */
    assert(!inherited("dart,t8110", true, "/arm-io/dart-ave0"));
    assert(!inherited("dart,t8020", true, "/arm-io/dart-aop"));
    puts("ok");
    return 0;
}
'''


def test_ave_m1_dart_filter(tmp_path):
    src = (Path(__file__).resolve().parents[2] / "src" / "dapf.c").read_text()
    code = PREFIX + block(src, "bool dapf_ave_filter_inherited(") + "\n" + MAIN
    c = tmp_path / "t.c"
    c.write_text(code)
    exe = tmp_path / "t"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function",
                    "-o", str(exe), str(c)], check=True)
    out = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
    assert out.stdout.strip() == "ok"
