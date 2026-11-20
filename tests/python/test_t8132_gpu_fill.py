# SPDX-License-Identifier: MIT
"""Run the M4 (T8132) G16 GPU handoff on real FDTs with the J773g's two firmware layouts.

The accepted cases use one J773g (M4 Mac mini): on 26.6.2 (25G83) the regions aura published
from its ADT (two gfx-asc segments, __TEXT and __DATA), on 27.2 (26B123) the values of its
iBoot ADT (three gfx-asc segments, __TEXT, __DATA and __DATA_SHARED_RO). The UAT regions are the
same in both.

Failure model, written before the code (the layout is chosen by the ADT's shape, as on
T6040/T6041, through the same helper):
- Two segments named __TEXT;__DATA, flags 1 and 0, at contiguous firmware VAs from
  0xfffffc0000000000, keep today's handoff exactly: gpu-fw-text is __TEXT, gpu-fw-data is __DATA,
  two segment VAs and flags, apple,g16-handoff-version 1. Zero gfx-data-shared-* properties on
  /arm-io/sgx do not change that.
- Three segments named __TEXT;__DATA;__DATA_SHARED_RO, flags 1, 0x10 and 0 (as iBoot sets them
  on 27.2), at contiguous firmware VAs, are accepted only when /arm-io/sgx
  gfx-data-shared-ro-{base,size} describe the third (same base, identity remap, the same non-zero
  16 KiB-aligned size, right after __DATA with no gap) and gfx-data-shared-rw-{base,size} are
  absent or zero. gpu-fw-data then covers __DATA and that tail, the GPU gets three segment VAs
  and flags, and apple,g16-handoff-version 2.
- The layout follows the ADT, not the OS release: a two-segment ADT on 27.2 gets version 1.
- Other boards than the J713 take the handoff on 26.6.2 and 27.2 only; the J713 on any release.
- Refused: one or four segments; segment names that do not match the count; a third segment
  without, or disagreeing with, the sgx pair; a shared-RO tail apart from __DATA, unaligned, not
  identity-mapped or at the wrong VA; non-zero shared RW; flags of the other layout; an extended
  gpu-fw-data that overlaps a UAT region or ends above 42 bits.
- A refusal publishes nothing: the GPU is disabled, without handoff version, segment VAs or flags,
  no reserved-memory node is enabled and no memreserve is added. The framebuffer stays.
"""

from pathlib import Path
import subprocess

from host_compiler import host_cc

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2] + "\n"


HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libfdt.h"
typedef uint8_t u8;
typedef uint32_t u32;
/* Match aura's aarch64-linux-gnu target, not the host libc's uint64_t alias. */
typedef unsigned long u64;
_Static_assert(sizeof(u64) == 8, "the aura host fixture requires an LP64 host");
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define SZ_16K 0x4000
#define T8132 0x8132
enum fw_version { V_UNKNOWN, V26_6, V26_6_2, V27_0, V27_2 };
static struct { enum fw_version version; } os_firmware;
static unsigned chip_id = T8132;
static void *adt = (void *)1;
struct adt_segment_ranges { u64 phys, iova, remap; u32 size, unk; };

/* /arm-io/sgx is node 1, /arm-io/gfx-asc node 2. */
static struct adt_segment_ranges segments[4];
static u32 segment_count;
static const char *segment_names;
static u64 uat[4][2], shared_ro[2], shared_rw[2];
static bool has_shared_ro, has_shared_rw;
static u32 irqs[4] = {994, 993, 996, 995};
static const char *const uat_names[4] = {
    "gpu-region", "gfx-shared-region", "gfx-handoff", "gfx-shared-l2-region",
};

static int adt_path_offset(void *tree, const char *path)
{
    (void)tree;
    return !strcmp(path, "/arm-io/sgx") ? 1 : !strcmp(path, "/arm-io/gfx-asc") ? 2 : -1;
}
static int adt_path_offset_trace(void *tree, const char *path, int *trace)
{
    return trace[0] = adt_path_offset(tree, path);
}
static int adt_get_reg(void *tree, int *trace, const char *name, int index, u64 *base, u64 *size)
{
    (void)tree; (void)name; (void)index;
    *base = trace[0] == 1 ? 0x300000000UL : 0x302600000UL;
    *size = trace[0] == 1 ? 0x4000000UL : 0x88000UL;
    return 0;
}
static const void *adt_getprop(void *tree, int node, const char *name, u32 *len)
{
    u32 dummy;
    (void)tree;
    if (!len)
        len = &dummy;
    if (node == 2 && !strcmp(name, "interrupts")) { *len = sizeof(irqs); return irqs; }
    if (node == 2 && !strcmp(name, "segment-names")) {
        *len = strlen(segment_names) + 1;
        return segment_names;
    }
    if (node == 2 && !strcmp(name, "segment-ranges")) {
        *len = segment_count * sizeof(segments[0]);
        return segments;
    }
    if (node != 1)
        return NULL;
    *len = sizeof(u64);
    for (int i = 0; i < 4; i++) {
        size_t n = strlen(uat_names[i]);
        if (!strncmp(name, uat_names[i], n) && !strcmp(name + n, "-base")) return &uat[i][0];
        if (!strncmp(name, uat_names[i], n) && !strcmp(name + n, "-size")) return &uat[i][1];
    }
    if (has_shared_ro && !strcmp(name, "gfx-data-shared-ro-base")) return &shared_ro[0];
    if (has_shared_ro && !strcmp(name, "gfx-data-shared-ro-size")) return &shared_ro[1];
    if (has_shared_rw && !strcmp(name, "gfx-data-shared-rw-base")) return &shared_rw[0];
    if (has_shared_rw && !strcmp(name, "gfx-data-shared-rw-size")) return &shared_rw[1];
    *len = 0;
    return NULL;
}
static int adt_getprop_copy(void *tree, int node, const char *name, void *out, size_t size)
{
    u32 len;
    const void *value = adt_getprop(tree, node, name, &len);
    if (!value || len != size)
        return -1;
    memcpy(out, value, size);
    return len;
}
#define ADT_GETPROP(a, n, name, ptr) adt_getprop_copy(a, n, name, (ptr), sizeof(*(ptr)))
'''

MAIN = r'''
#define FW_VA 0xfffffc0000000000UL
static unsigned char blob[65536];
static void *dt = blob;
static const char *const regions[6] = {
    "uat-ttbs", "uat-pagetables", "uat-handoff", "uat-shared-l2", "gpu-fw-text", "gpu-fw-data",
};

static void tree(const char *board)
{
    memset(blob, 0, sizeof(blob));
    assert(!fdt_create_empty_tree(dt, sizeof(blob)));
    assert(!fdt_setprop_string(dt, 0, "compatible", board));
    int gpu = fdt_add_subnode(dt, 0, "gpu");
    assert(gpu >= 0 && !fdt_setprop_string(dt, gpu, "compatible", "apple,agx-t8132"));
    int aliases = fdt_add_subnode(dt, 0, "aliases");
    assert(aliases >= 0 && !fdt_setprop_string(dt, aliases, "gpu", "/gpu"));
    int fb = fdt_add_subnode(dt, 0, "framebuffer");
    assert(fb >= 0 && !fdt_setprop_string(dt, fb, "status", "okay"));
    int resv = fdt_add_subnode(dt, 0, "reserved-memory");
    assert(resv >= 0);
    for (int i = 0; i < 6; i++)
        assert(fdt_add_subnode(dt, fdt_path_offset(dt, "/reserved-memory"), regions[i]) >= 0);
}

/* The J773g's UAT regions, the same on 26.6.2 and 27.2: ttbs, pagetables, handoff, shared L2 */
static void uat_regions(void)
{
    const u64 j773g[4][2] = {
        {0x105fffb8000UL, 0x4000}, {0x105fff78000UL, 0x40000},
        {0x105fff70000UL, 0x4000}, {0x105fff74000UL, 0x4000},
    };
    memcpy(uat, j773g, sizeof(uat));
    irqs[0] = 994; irqs[1] = 993; irqs[2] = 996; irqs[3] = 995;
    chip_id = T8132;
    has_shared_ro = has_shared_rw = false;
    shared_ro[0] = shared_ro[1] = shared_rw[0] = shared_rw[1] = 0;
}

/* J773g on 26.6.2 (25G83): __TEXT 0x60000, __DATA 0x13c000 */
static void world_26_6_2(void)
{
    tree("apple,j773g");
    uat_regions();
    os_firmware.version = V26_6_2;
    segment_names = "__TEXT;__DATA";
    segments[0] = (struct adt_segment_ranges){0x10000cc0000UL, FW_VA, 0x10000cc0000UL, 0x60000, 1};
    segments[1] = (struct adt_segment_ranges){0x10001150000UL, FW_VA + 0x60000, 0x10001150000UL,
                                              0x13c000, 0};
    segment_count = 2;
}

/* J773g on 27.2 (26B123), iBoot's ADT: __TEXT 0x64000, __DATA 0xd0000, __DATA_SHARED_RO 0x8000 */
static void world_27_2(void)
{
    tree("apple,j773g");
    uat_regions();
    os_firmware.version = V27_2;
    segment_names = "__TEXT;__DATA;__DATA_SHARED_RO";
    segments[0] = (struct adt_segment_ranges){0x10000d04000UL, FW_VA, 0x10000d04000UL, 0x64000, 1};
    segments[1] = (struct adt_segment_ranges){0x100011a4000UL, FW_VA + 0x64000, 0x100011a4000UL,
                                              0xd0000, 0x10};
    segments[2] = (struct adt_segment_ranges){0x10001274000UL, FW_VA + 0x134000, 0x10001274000UL,
                                              0x8000, 0};
    segment_count = 3;
    has_shared_ro = has_shared_rw = true;
    shared_ro[0] = 0x10001274000UL;
    shared_ro[1] = 0x8000;
}

/* The tail moved: the sgx pair and the third segment together */
static void move_shared_ro(u64 base)
{
    shared_ro[0] = segments[2].phys = segments[2].remap = base;
}

static const char *status(int node)
{
    const char *s = fdt_getprop(dt, node, "status", NULL);
    return s ? s : "(none)";
}

static void expect_published(const char *what, u32 version)
{
    for (int pass = 0; pass < 2; pass++) {
        assert(dt_set_gpu_m4(dt) == 0);
        int gpu = fdt_path_offset(dt, "/gpu"), len;
        const fdt32_t *v = fdt_getprop(dt, gpu, "apple,g16-handoff-version", &len);
        if (!v || len != 4 || fdt32_ld(v) != version) {
            fprintf(stderr, "%s: no handoff version %u\n", what, version);
            exit(1);
        }
        assert(strcmp(status(gpu), "disabled"));
        const fdt64_t *vas = fdt_getprop(dt, gpu, "apple,firmware-segment-vas", &len);
        assert(vas && len == (int)(segment_count * sizeof(*vas)));
        const fdt32_t *flags = fdt_getprop(dt, gpu, "apple,firmware-segment-flags", &len);
        assert(flags && len == (int)(segment_count * sizeof(*flags)));
        for (u32 i = 0; i < segment_count; i++)
            assert(fdt64_ld(&vas[i]) == segments[i].iova && fdt32_ld(&flags[i]) == segments[i].unk);
        /* The UAT regions, __TEXT, then __DATA and the shared-RO tail when there is one */
        u64 tail = segment_count == 3 ? segments[2].size : 0;
        const u64 want[6][2] = {
            {uat[0][0], uat[0][1]}, {uat[1][0], uat[1][1]},
            {uat[2][0], uat[2][1]}, {uat[3][0], uat[3][1]},
            {segments[0].phys, segments[0].size}, {segments[1].phys, segments[1].size + tail},
        };
        assert(fdt_num_mem_rsv(dt) == 6);
        for (int i = 0; i < 6; i++) {
            char path[64];
            snprintf(path, sizeof(path), "/reserved-memory/%s", regions[i]);
            int node = fdt_path_offset(dt, path);
            const fdt64_t *reg = fdt_getprop(dt, node, "reg", &len);
            assert(reg && len == 16 && fdt64_ld(&reg[0]) == want[i][0] &&
                   fdt64_ld(&reg[1]) == want[i][1]);
            assert(fdt_getprop(dt, node, "no-map", &len) && !len);
            assert(!strcmp(status(node), "okay"));
            u64 base, size;
            assert(!fdt_get_mem_rsv(dt, i, &base, &size) && base == want[i][0] &&
                   size == want[i][1]);
        }
    }
    printf("PASS published: %s\n", what);
}

static void expect_refused(const char *what)
{
    assert(dt_set_gpu_m4(dt) == 0);
    int gpu = fdt_path_offset(dt, "/gpu");
    if (strcmp(status(gpu), "disabled") ||
        fdt_getprop(dt, gpu, "apple,g16-handoff-version", NULL)) {
        fprintf(stderr, "%s: the GPU was handed off\n", what);
        exit(1);
    }
    assert(!fdt_getprop(dt, gpu, "apple,firmware-segment-vas", NULL));
    assert(!fdt_getprop(dt, gpu, "apple,firmware-segment-flags", NULL));
    assert(!fdt_num_mem_rsv(dt));
    for (int i = 0; i < 6; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/reserved-memory/%s", regions[i]);
        int node = fdt_path_offset(dt, path);
        assert(node >= 0 && !fdt_getprop(dt, node, "reg", NULL) && strcmp(status(node), "okay"));
    }
    assert(!strcmp(status(fdt_path_offset(dt, "/framebuffer")), "okay"));
    printf("PASS refused: %s\n", what);
}

int main(void)
{
    world_26_6_2();
    expect_published("26.6.2 J773g, two segments", 1);
    world_26_6_2(); has_shared_ro = has_shared_rw = true;
    expect_published("26.6.2 ADT with zero gfx-data-shared-ro and -rw", 1);
    world_27_2();
    expect_published("27.2 J773g, three segments", 2);
    world_27_2(); has_shared_rw = false;
    expect_published("27.2 ADT without gfx-data-shared-rw", 2);
    world_26_6_2(); os_firmware.version = V27_2;
    expect_published("two segments on 27.2: the ADT picks the layout", 1);
    world_27_2(); tree("apple,j713"); os_firmware.version = V27_0;
    expect_published("J713 on 27.0, three segments", 2);

    world_27_2(); os_firmware.version = V27_0;
    expect_refused("27.0 on the J773g");
    world_26_6_2(); os_firmware.version = V_UNKNOWN;
    expect_refused("unknown release on the J773g");
    world_27_2(); segment_count = 1;
    expect_refused("one segment");
    world_27_2(); segment_count = 4;
    segments[3] = (struct adt_segment_ranges){0x1000127c000UL, FW_VA + 0x13c000,
                                              0x1000127c000UL, 0x4000, 0};
    expect_refused("four segments");
    world_27_2(); segment_names = "__TEXT;__DATA";
    expect_refused("three segments named as two");
    world_26_6_2(); segment_names = "__TEXT;__DATA;__DATA_SHARED_RO";
    expect_refused("two segments named as three");
    world_27_2(); has_shared_ro = false;
    expect_refused("third segment without gfx-data-shared-ro");
    world_27_2(); shared_ro[0] += 0x4000;
    expect_refused("gfx-data-shared-ro base disagrees with the segment");
    world_27_2(); shared_ro[1] = 0x4000;
    expect_refused("gfx-data-shared-ro size disagrees with the segment");
    world_27_2(); segments[2].remap += 0x4000;
    expect_refused("__DATA_SHARED_RO not identity-mapped");
    world_27_2(); move_shared_ro(segments[2].phys + 0x4000);
    expect_refused("__DATA_SHARED_RO apart from __DATA");
    world_27_2(); shared_ro[1] = segments[2].size = 0x2000;
    expect_refused("unaligned __DATA_SHARED_RO");
    world_27_2(); shared_rw[1] = 0x4000;
    expect_refused("non-zero gfx-data-shared-rw");
    world_27_2(); segments[2].iova += 0x4000;
    expect_refused("__DATA_SHARED_RO VA not right after __DATA's");
    world_27_2(); segments[1].unk = 0;
    expect_refused("three segments with 26.6.2's __DATA flags");
    world_26_6_2(); segments[1].unk = 0x10;
    expect_refused("two segments with 27.2's __DATA flags");
    world_27_2(); segments[2].unk = 1;
    expect_refused("__DATA_SHARED_RO flags");
    world_27_2(); segments[1].remap += 0x4000;
    expect_refused("__DATA not identity-mapped");
    world_27_2(); uat[2][0] = segments[2].phys;
    expect_refused("UAT handoff on the __DATA_SHARED_RO tail");
    world_27_2();
    segments[1].phys = segments[1].remap = (1UL << 42) - segments[1].size;
    move_shared_ro(1UL << 42);
    expect_refused("extended gpu-fw-data above 42 bits");
    return 0;
}
'''


def test_t8132_gpu_fill(tmp_path):
    source = (ROOT / "src/kboot_gpu.c").read_text()
    header = (ROOT / "src/kboot.h").read_text()
    layout = header[header.index("struct gpu_fw_layout {"):]
    layout = layout[:layout.index("};\n") + 3]
    m4 = (ROOT / "src/kboot_gpu_m4.c").read_text()
    m4 = "\n".join(line for line in m4.splitlines() if not line.startswith("#include"))
    code = HARNESS + layout + function(source, "int gpu_fw_layout(") + m4 + MAIN
    cfile = tmp_path / "t8132-gpu-fill.c"
    cfile.write_text(code)
    libfdt = ROOT / "src/libfdt"
    sources = [str(libfdt / name) for name in (
        "fdt.c", "fdt_ro.c", "fdt_rw.c", "fdt_sw.c", "fdt_wip.c", "fdt_empty_tree.c",
        "fdt_strerror.c",
    )]
    binary = tmp_path / "t8132-gpu-fill"
    subprocess.run([*host_cc(), "-D_GNU_SOURCE", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-function", "-Wno-unused-parameter", "-Wno-format",
                    "-I", str(libfdt), str(cfile), *sources, "-o", str(binary)], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.count("PASS") == 26, result.stdout
