# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run the M4 resource publisher against real libfdt and malformed firmware data."""
from pathlib import Path
import subprocess


def test_m4_gpu_resource_admission(tmp_path):
    repo = Path(__file__).resolve().parents[2]
    production = (repo / 'src/kboot_gpu_m4.c').read_text()
    production = '\n'.join(line for line in production.splitlines() if not line.startswith('#include'))
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "libfdt.h"
typedef uint64_t u64;
typedef uint32_t u32;
#define T8132 0x8132
#define SZ_16K 0x4000
enum { V26_6, V26_6_2 };
static struct { int version; } os_firmware;
struct adt_segment_ranges { u64 phys, iova, remap; u32 size, unk; };
static void *adt;
static unsigned chip_id = T8132;
static int fault;
static struct adt_segment_ranges segs[2];
static u64 bases[4], sizes[4];
static u32 irqs[4];
static int adt_path_offset_trace(void *a, const char *p, int *trace)
{ trace[0] = strstr(p, "gfx-asc") ? 2 : 1; return trace[0]; }
static int adt_get_reg(void *a, int *trace, const char *name, unsigned i, u64 *base, u64 *size)
{
    *base = trace[0] == 1 ? 0x300000000 : 0x302600000;
    *size = trace[0] == 1 ? 0x4000000 : 0x88000;
    if (fault == 1) *base += 0x4000;
    return 0;
}
static const void *adt_getprop(void *a, int n, const char *p, u32 *len)
{
    const char *names[] = {"gpu-region", "gfx-shared-region", "gfx-handoff", "gfx-shared-l2-region"};
    if (!strcmp(p, "interrupts")) { *len = sizeof(irqs); return irqs; }
    if (!strcmp(p, "segment-names")) { *len = sizeof("__TEXT;__DATA"); return "__TEXT;__DATA"; }
    if (!strcmp(p, "segment-ranges")) { *len = sizeof(segs); return segs; }
    for (unsigned i = 0; i < 4; i++) {
        char prop[64];
        snprintf(prop, sizeof(prop), "%s-base", names[i]);
        if (!strcmp(p, prop)) { *len = fault == 8 ? 4 : 8; return &bases[i]; }
        snprintf(prop, sizeof(prop), "%s-size", names[i]);
        if (!strcmp(p, prop)) { *len = 8; return &sizes[i]; }
    }
    *len = 0; return NULL;
}
''' + production + r'''
int main(void)
{
    char blob[32768];
    const char *regions[] = {"uat-ttbs", "uat-pagetables", "uat-handoff", "uat-shared-l2", "gpu-fw-text", "gpu-fw-data"};
    /* 13: another M4 Mac on 26.6.2 publishes; 14: the J713 on 26.6 still does. */
    for (fault = 0; fault <= 14; fault++) {
        os_firmware.version = fault == 14 ? V26_6 : V26_6_2;
        assert(!fdt_create_empty_tree(blob, sizeof(blob)));
        assert(!fdt_setprop_string(blob, 0, "compatible", fault == 13 ? "apple,j604" : "apple,j713"));
        int gpu = fdt_add_subnode(blob, 0, "gpu"); assert(gpu >= 0);
        assert(!fdt_setprop_string(blob, gpu, "compatible", "apple,agx-t8132"));
        assert(!fdt_setprop_u32(blob, gpu, "apple,g16-handoff-version", 99));
        int aliases = fdt_add_subnode(blob, 0, "aliases"); assert(aliases >= 0);
        assert(!fdt_setprop_string(blob, aliases, "gpu", "/gpu"));
        int fb = fdt_add_subnode(blob, 0, "framebuffer"); assert(fb >= 0);
        assert(!fdt_setprop_string(blob, fb, "status", "okay"));
        int res = fdt_add_subnode(blob, 0, "reserved-memory"); assert(res >= 0);
        for (unsigned i = 0; i < 6; i++)
            if (fault != 7 || i != 5) assert(fdt_add_subnode(blob, res, regions[i]) >= 0);
        for (unsigned i = 0; i < 4; i++) {
            bases[i] = 0x800000000ULL + i * 0x100000;
            sizes[i] = 0x4000;
        }
        segs[0] = (struct adt_segment_ranges){0x800400000, 0xfffffc0000000000, 0x800400000, 0x4000, 1};
        segs[1] = (struct adt_segment_ranges){0x800500000, 0xfffffc0000004000, 0x800500000, 0x8000, 0};
        irqs[0] = 994; irqs[1] = 993; irqs[2] = 996; irqs[3] = 995;
        if (fault == 2) irqs[0]++;
        if (fault == 3) bases[1] = bases[0];
        if (fault == 4) sizes[0] = 0;
        if (fault == 5) segs[1].iova++;
        if (fault == 6) bases[0] = (1ULL << 42);
        if (fault == 9) sizes[0] = UINT64_MAX - 0x3fff;
        if (fault == 10) bases[0]++;
        if (fault == 11) {
            assert(!fdt_setprop_string(blob, 0, "compatible", "apple,j715"));
            os_firmware.version = V26_6;
        }
        if (fault == 12) {
            gpu = fdt_path_offset(blob, "/gpu");
            assert(!fdt_setprop_string(blob, gpu, "compatible", "apple,unsupported-gpu"));
        }
        int result = dt_set_gpu_m4(blob);
        fb = fdt_path_offset(blob, "/framebuffer");
        const char *fb_status = fdt_getprop(blob, fb, "status", NULL);
        assert(fb >= 0 && fb_status && !strcmp(fb_status, "okay"));
        gpu = fdt_path_offset(blob, "/gpu");
        int len;
        const fdt32_t *version = fdt_getprop(blob, gpu, "apple,g16-handoff-version", &len);
        if (fault && fault < 13) {
            assert(!result && !version && !fdt_num_mem_rsv(blob));
            const char *status = fdt_getprop(blob, gpu, "status", NULL);
            assert(status && !strcmp(status, "disabled"));
        } else {
            assert(!result && version && len == 4 && fdt32_to_cpu(*version) == 1);
            assert(fdt_num_mem_rsv(blob) == 6);
            for (unsigned i = 0; i < 6; i++) {
                char path[80]; snprintf(path, sizeof(path), "/reserved-memory/%s", regions[i]);
                int node = fdt_path_offset(blob, path); assert(node >= 0);
                assert(fdt_getprop(blob, node, "no-map", &len) && !len);
                const fdt64_t *reg = fdt_getprop(blob, node, "reg", &len); assert(reg && len == 16);
                assert(fdt64_to_cpu(reg[0]) == (i < 4 ? bases[i] : segs[i - 4].phys));
                assert(fdt64_to_cpu(reg[1]) == (i < 4 ? sizes[i] : segs[i - 4].size));
            }
            assert(!dt_set_gpu_m4(blob) && fdt_num_mem_rsv(blob) == 6);
        }
    }
    puts("M4 live resources, repeat publication and malformed/overlapping carveouts PASS");
    return 0;
}
'''
    source = tmp_path / 'gpu.c'
    source.write_text(harness)
    binary = tmp_path / 'gpu'
    libfdt = repo / 'src/libfdt'
    sources = [libfdt / (n + '.c') for n in ('fdt', 'fdt_ro', 'fdt_rw', 'fdt_sw', 'fdt_wip', 'fdt_empty_tree', 'fdt_check', 'fdt_strerror', 'fdt_addresses')]
    subprocess.run(['cc', '-D_GNU_SOURCE', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-format', '-I', str(libfdt), str(source), *map(str, sources), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
