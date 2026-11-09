# SPDX-License-Identifier: MIT
"""The Aura/J613 OS_LOG exception accepts containment, never overlap or another owner."""

import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}\n", start) + 3]


def test_j613_oslog_memreserve_contract(tmp_path):
    source = (ROOT / "src/kboot_display_handoff.c").read_text()
    helper = function(source, "static int dcp6_oslog_conflict(")
    j613 = source[source.index("static const struct dcp_soc dcp_soc_j613_25g83"):
                   source.index("static bool dcp25_selected")]
    assert j613.count(".oslog_in_rsv = true") == 1
    assert "dcp6_oslog_conflict(dt, logs[i].phys, logs[i].size)" in source

    c = tmp_path / "oslog.c"
    exe = tmp_path / "oslog"
    c.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint64_t u64;
struct dcp_soc { bool oslog_in_rsv; };
static struct { const struct dcp_soc *soc; } dcp6;
struct fake_dt { int n; u64 a[4], s[4]; bool reserved_conflict; };
#define bail(...) do { return -1; } while (0)
#define dlog(...) do {} while (0)
#define dlog_debug(...) do {} while (0)
static bool dcp6_overlaps(u64 a, u64 as, u64 b, u64 bs)
{
    u64 ae = a + as < a ? UINT64_MAX : a + as;
    u64 be = b + bs < b ? UINT64_MAX : b + bs;
    return as && bs && a < be && b < ae;
}
static int fdt_num_mem_rsv(void *p) { return ((struct fake_dt *)p)->n; }
static int fdt_get_mem_rsv(void *p, int i, u64 *a, u64 *s)
{
    struct fake_dt *d = p; if (i < 0 || i >= d->n) return -1;
    *a = d->a[i]; *s = d->s[i]; return 0;
}
static int fdt_del_mem_rsv(void *p, int i)
{
    struct fake_dt *d = p; assert(i == 0 && d->n > 0);
    memmove(d->a, d->a + 1, sizeof(d->a) - sizeof(d->a[0]));
    memmove(d->s, d->s + 1, sizeof(d->s) - sizeof(d->s[0]));
    d->n--; return 0;
}
static int dcp6_resv_conflict(void *p, u64 base, u64 size, const char *what)
{
    struct fake_dt *d = p; (void)what;
    if (d->reserved_conflict) return -1;
    for (int i = 0; i < d->n; i++) if (dcp6_overlaps(base, size, d->a[i], d->s[i])) return -1;
    return 0;
}
static void *dcp6_scratch(void *p);
''' + helper + r'''
static void *dcp6_scratch(void *p)
{
    struct fake_dt *copy = malloc(sizeof(*copy)); assert(copy); *copy = *(struct fake_dt *)p;
    return copy;
}
int main(void)
{
    const struct dcp_soc strict = {false}, contained = {true};
    struct fake_dt d = {.n = 1, .a = {0x100000}, .s = {0x410000}};
    dcp6.soc = &strict;
    assert(dcp6_oslog_conflict(&d, 0x120000, 0x24000) == -1);
    dcp6.soc = &contained;
    assert(dcp6_oslog_conflict(&d, 0x120000, 0x24000) == 0);
    assert(dcp6_oslog_conflict(&d, 0x0f0000, 0x24000) == -1);
    assert(dcp6_oslog_conflict(&d, 0x50f000, 0x24000) == -1);
    assert(dcp6_oslog_conflict(&d, UINT64_MAX - 0x1000, 0x24000) == -1);
    d.reserved_conflict = true;
    assert(dcp6_oslog_conflict(&d, 0x120000, 0x24000) == -1);
    d.reserved_conflict = false;
    assert(dcp6_oslog_conflict(&d, 0x600000, 0x24000) == 0);
    puts("ok");
}
''')
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)],
                   check=True)
    out = subprocess.check_output([str(exe)], text=True)
    assert out == "ok\n"
