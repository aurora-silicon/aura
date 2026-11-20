# SPDX-License-Identifier: MIT
"""OS_LOG accepts full memreserve containment and refuses conflicting owners."""

import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}\n", start) + 3]


def test_oslog_memreserve_contract(tmp_path):
    source = (ROOT / "src/kboot_display_handoff.c").read_text()
    helper = function(source, "static int dcp6_oslog_conflict(")
    assert "dcp6_oslog_conflict(dt, logs[i].phys, logs[i].size)" in source

    c = tmp_path / "oslog.c"
    exe = tmp_path / "oslog"
    c.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint64_t u64;
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
static int dcp6_resv_node_conflict(void *p, u64 base, u64 size, const char *what)
{
    (void)base; (void)size; (void)what;
    return ((struct fake_dt *)p)->reserved_conflict ? -1 : 0;
}
''' + helper + r'''
int main(void)
{
    struct fake_dt d = {.n = 1, .a = {0x100000}, .s = {0x410000}};
    assert(dcp6_oslog_conflict(&d, 0x120000, 0x24000) == 0);
    assert(dcp6_oslog_conflict(&d, 0x0f0000, 0x24000) == -1);
    assert(dcp6_oslog_conflict(&d, 0x50f000, 0x24000) == -1);
    assert(dcp6_oslog_conflict(&d, UINT64_MAX - 0x1000, 0x24000) == -1);
    d.reserved_conflict = true;
    assert(dcp6_oslog_conflict(&d, 0x120000, 0x24000) == -1);
    d.reserved_conflict = false;
    assert(dcp6_oslog_conflict(&d, 0x600000, 0x24000) == 0);
    d.n = 2;
    d.a[1] = 0x140000;
    d.s[1] = 0x10000;
    assert(dcp6_oslog_conflict(&d, 0x120000, 0x24000) == -1);
    d.a[1] = UINT64_MAX - 0x1000;
    d.s[1] = 0x2000;
    assert(dcp6_oslog_conflict(&d, UINT64_MAX - 0x2000, 0x1800) == -1);
    assert(dcp6_oslog_conflict(&d, 0x120000, 0) == -1);
    puts("ok");
}
''')
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)],
                   check=True)
    out = subprocess.check_output([str(exe)], text=True)
    assert out == "ok\n"
