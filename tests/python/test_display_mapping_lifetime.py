# SPDX-License-Identifier: MIT
"""Display framebuffer mappings are released only after both DARTs confirm.

The production display_map_fb(), display_unmap_fb() and dart_unmap_checked()
run against modelled DART domains; the modeset call sites are checked to
reuse an IOVA only behind a confirmed unmap.
"""
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", source.index("{", start)) + 2] + "\n"


def test_framebuffer_mappings_survive_dart_failure(tmp_path):
    source = (ROOT / "src/display.c").read_text()
    dart = (ROOT / "src/dart.c").read_text()
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef unsigned long u64;
typedef long s64;
#define SZ_16K 0x4000
#define DART_PTR_ERR BIT(63)
#define BIT(n) (1UL << (n))
#define DART_IS_ERR(val) ((val) & DART_PTR_ERR)
typedef struct dart_dev dart_dev_t;
struct params { void (*tlb_invalidate)(dart_dev_t *); };
struct dart_dev { bool failed, locked, legacy_mutable; int id; const struct params *params; };
static dart_dev_t disp, domain;
static struct { dart_dev_t *dart_disp, *dart_dcp; } device;
static __typeof__(device) *dcp = &device;
static unsigned page_ops[2], tlbis[2], maps[2];
static int tlbi_failure, map_failure;
static bool uncertain_map;
static bool dart_locked_pt_writable(dart_dev_t *d) { (void)d; return false; }
static void invalidate(dart_dev_t *d)
{
    tlbis[d->id]++;
    if (tlbi_failure == d->id + 1)
        d->failed = true;
}
static const struct params params = {.tlb_invalidate = invalidate};
static bool dart_has_failed(dart_dev_t *d) { return !d || d->failed; }
static void dart_unmap_page(dart_dev_t *d, unsigned long iova) { (void)iova; page_ops[d->id]++; }
static int dart_map(dart_dev_t *d, unsigned long iova, void *pa, size_t size)
{
    (void)iova; (void)pa; (void)size;
    assert(!d->failed);
    maps[d->id]++;
    if (map_failure == d->id + 1) {
        d->failed = uncertain_map;
        return -1;
    }
    return 0;
}
static u64 dart_find_iova(dart_dev_t *d, s64 start, size_t size) { (void)d; (void)size; return start; }
static u64 dart_vm_base(dart_dev_t *d) { (void)d; return 0; }
static void reset(void)
{
    disp = (dart_dev_t){.id = 0, .params = &params};
    domain = (dart_dev_t){.id = 1, .params = &params};
    device.dart_disp = &disp;
    device.dart_dcp = &domain;
    memset(page_ops, 0, sizeof(page_ops));
    memset(tlbis, 0, sizeof(tlbis));
    memset(maps, 0, sizeof(maps));
    tlbi_failure = map_failure = 0;
    uncertain_map = false;
}
'''
    harness += function(dart, "bool dart_unmap_checked(")
    harness += function(source, "static bool display_unmap_fb(")
    harness += function(source, "static uintptr_t display_map_fb(")
    harness += r'''
int main(void)
{
    reset();
    assert(display_map_fb(0x8000, 0x100000, SZ_16K) == 0x8000);
    assert(display_unmap_fb(0x8000, SZ_16K));
    assert(page_ops[0] == 1 && page_ops[1] == 1);

    /* The second DART refuses the map: the first one's mapping is withdrawn. */
    reset();
    map_failure = 2;
    assert(DART_IS_ERR(display_map_fb(0x8000, 0x100000, SZ_16K)));
    assert(page_ops[0] == 1 && tlbis[0] == 1 && !page_ops[1]);

    /* A map that left the second DART failed: nothing is unmapped or reused. */
    reset();
    map_failure = 2;
    uncertain_map = true;
    assert(DART_IS_ERR(display_map_fb(0x8000, 0x100000, SZ_16K)));
    assert(page_ops[0] == 1);
    assert(DART_IS_ERR(display_map_fb(0x8000, 0x100000, SZ_16K)) && maps[0] == 1);
    assert(!display_unmap_fb(0x8000, SZ_16K) && page_ops[0] == 2 && !page_ops[1]);

    /* A failed invalidation on either DART reports the range as still live. */
    for (int which = 1; which <= 2; which++) {
        reset();
        tlbi_failure = which;
        assert(!display_unmap_fb(0x8000, SZ_16K));
        assert(page_ops[1] == (which == 2));
        assert(DART_IS_ERR(display_map_fb(0x8000, 0x100000, SZ_16K)));
        assert(!maps[0] && !maps[1]);
    }
    return 0;
}
'''
    binary = tmp_path / "display-mapping"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                    "-x", "c", "-", "-o", str(binary)], input=harness, text=True, check=True)
    subprocess.run([str(binary)], check=True, timeout=5)

    configure = function(source, "int display_configure(")
    assert "dart_unmap(" not in configure
    # Every IOVA release in the modeset sits behind a confirmed unmap.
    assert re.search(r"if \(display_unmap_fb\(tmp_dva, size\)\)\n\s+iova_free\(dcp->iovad_dcp, "
                     r"tmp_dva, size\);", configure)
    old = configure.index("if (!display_unmap_fb(fb_dva, fb_size))")
    assert old < configure.index("iova_free(dcp->iovad_dcp, fb_dva, fb_size)")
