# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run the T8020 DAPF writer with the preloaded AVE __TEXT admission."""
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
typedef uint8_t u8;
typedef uint32_t u32;
typedef unsigned long u64;
#define PACKED __attribute__((packed))
#define static_assert _Static_assert
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
struct dapf_t8020_config { u64 start; u64 end; u8 unk1; u8 r0_hi; u8 r0_lo; u8 unk2; u32 r4; } PACKED;
static void *adt;
static struct dapf_t8020_config rows[17];
static u32 nrows;
static struct { u64 phys; u64 iova; u64 remap; u32 size; u32 flags; } PACKED seg[2];
static bool preloaded = true, have_seg = true;
static u64 regs[16 * 16];
#define ADT_DART 1
#define ADT_AVE 2
static int adt_path_offset(void *a, const char *p)
{
    (void)a;
    return !strcmp(p, "/arm-io/ave0") ? ADT_AVE : -1;
}
static const void *adt_getprop(void *a, int node, const char *name, u32 *len)
{
    (void)a;
    if (node == ADT_DART && !strcmp(name, "filter-data-instance-0")) {
        *len = nrows * sizeof(rows[0]);
        return rows;
    }
    if (node == ADT_AVE && !strcmp(name, "pre-loaded") && preloaded) {
        *len = 4;
        return "\1\0\0";
    }
    if (node == ADT_AVE && !strcmp(name, "segment-ranges") && have_seg) {
        *len = sizeof(seg);
        return seg;
    }
    return NULL;
}
static void write32(u64 a, u32 v) { regs[a / 4] = v; }
static void write64(u64 a, u64 v) { regs[a / 4] = v; }
static int printf_quiet(const char *f, ...) { (void)f; return 0; }
#define printf printf_quiet
'''

MAIN = r'''
static void row(u64 s, u64 e, u32 r0)
{
    rows[nrows].start = s; rows[nrows].end = e;
    rows[nrows].r0_hi = r0 >> 4; rows[nrows].r0_lo = r0 & 0xf; rows[nrows].r4 = 1;
    nrows++;
}
static void reset(void)
{
    memset(regs, 0, sizeof(regs));
    memset(rows, 0, sizeof(rows));
    nrows = 0;
    preloaded = have_seg = true;
    seg[0].phys = 0x10000b28000; seg[0].size = 0xd0000;
    seg[1].phys = 0x10001a74000; seg[1].size = 0x128000;
}
/* slot i: r0 at +0, r4 at +4, start at +8, end at +0x10 */
#define R0(i) regs[(i) * 16 + 0]
#define START(i) regs[(i) * 16 + 2]
#define END(i) regs[(i) * 16 + 4]
int main(void)
{
    /* An ADT entry that misses the preloaded TEXT: one more read-only entry. */
    reset();
    row(0x10000b54000, 0x10000c6bfff, 0x11); row(0x1f000000000, 0x1f0ffffffff, 0x33);
    row(0x506000000, 0x507c6c000, 0x31);
    assert(!dapf_init_t8020("/arm-io/dart-ave0", 0, ADT_DART));
    assert(R0(0) == 0x11 && START(0) == 0x10000b54000);
    assert(R0(3) == 0x11 && START(3) == 0x10000b28000 && END(3) == 0x10000bf7ffc);
    assert(R0(4) == 0 && START(4) == 0);

    /* iBoot's own entry already covers it: nothing added. */
    reset();
    row(0x10000b28000, 0x10000bf7ffc, 0x11); row(0x1f000000000, 0x1f0ffffffff, 0x33);
    assert(!dapf_init_t8020("/arm-io/dart-ave0", 0, ADT_DART));
    assert(START(0) == 0x10000b28000 && R0(2) == 0 && START(2) == 0);

    /* Not preloaded, no segments, or another DART: ADT entries only. */
    reset(); preloaded = false; row(0x1f000000000, 0x1f0ffffffff, 0x33);
    assert(!dapf_init_t8020("/arm-io/dart-ave0", 0, ADT_DART) && R0(1) == 0);
    reset(); have_seg = false; row(0x1f000000000, 0x1f0ffffffff, 0x33);
    assert(!dapf_init_t8020("/arm-io/dart-ave0", 0, ADT_DART) && R0(1) == 0);
    reset(); row(0x1f000000000, 0x1f0ffffffff, 0x33);
    assert(!dapf_init_t8020("/arm-io/dart-isp0", 0, ADT_DART) && R0(1) == 0);

    /* Sixteen ADT entries leave no room: refuse, do not write past the bank. */
    reset();
    for (int i = 0; i < 16; i++)
        row(0x1f000000000 + i * 0x1000, 0x1f000000fff + i * 0x1000, 0x33);
    assert(dapf_init_t8020("/arm-io/dart-ave0", 0, ADT_DART) == -1);
    assert(START(15) == 0x1f00000f000);
    puts("ok");
    return 0;
}
'''


def test_ave_preloaded_text_dapf(tmp_path):
    src = (Path(__file__).resolve().parents[2] / "src" / "dapf.c").read_text()
    code = (PREFIX + block(src, "struct dapf_segment {") + ";\n" +
            block(src, "static bool dapf_preload_text(") + "\n" +
            block(src, "static int dapf_init_t8020(") + "\n" + MAIN)
    c = tmp_path / "t.c"
    c.write_text(code)
    exe = tmp_path / "t"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function",
                    "-o", str(exe), str(c)], check=True)
    out = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
    assert out.stdout.strip() == "ok"
