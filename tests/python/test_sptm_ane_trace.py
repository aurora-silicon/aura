# SPDX-License-Identifier: MIT
"""The ANE trace hooks only what the ADT gives the ANE, its DART and the AIC.

ane_trace_setup() is the production body, compiled against a modeled ADT.
"""
import re

from test_smp_timeouts import REPO, compile_and_run


def setup_source():
    source = (REPO / "src/sptm_virt_gpu.c").read_text()
    start = source.index("#define ANE_TRACE_MAX_REGS")
    end = source.index("void sptm_virt_gpu_setup(void)")
    return source[start:end]


def test_no_board_chip_or_address_in_trace():
    body = setup_source()
    assert not re.search(r"0x[0-9a-f]{6,}", body, re.I)
    for token in ("chip_id", "T8122", "J613", "j613"):
        assert token not in body


def test_ane_trace_windows_from_adt(tmp_path):
    harness = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef unsigned long u64;
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define BIT(n) (1UL << (n))
#define SPTM_VIRT_ANE_TRACE BIT(11)
#define GPU_MAX_RANGES 10
static const void *adt = (const void *)1;
static u32 sptm_virt_flags = SPTM_VIRT_ANE_TRACE, sptm_virt_gpu_print_max = 20000;
static void *sptm_virt_gpu_ring;
static struct { u64 base, size; const char *what; } gpu_ranges[GPU_MAX_RANGES];
static int gpu_nranges, hooks;
struct exc_info;
static bool gpu_hook(struct exc_info *c, u64 a, u64 *v, bool w, int n)
{ (void)c; (void)a; (void)v; (void)w; (void)n; return true; }
static int hv_map_hook(u64 base, void *hook, u64 size) { (void)base; (void)hook; (void)size; hooks++; return 0; }
static jmp_buf refused;
#define panic(...) longjmp(refused, 1)

/* Modeled ADT: three nodes, each a list of (base, size). */
static const char *paths[3] = {"/arm-io/ane", "/arm-io/dart-ane", "/arm-io/aic"};
static u64 regs[3][8][2];
static int nregs[3], present[3];
static int adt_path_offset_trace(const void *t, const char *path, int *p)
{
    (void)t;
    for (int i = 0; i < 3; i++)
        if (!strcmp(path, paths[i]) && present[i]) {
            p[0] = i;
            return i;
        }
    return -1;
}
static int adt_get_reg(const void *t, int *p, const char *prop, int idx, u64 *a, u64 *s)
{
    (void)t; (void)prop;
    if (idx >= nregs[p[0]])
        return -1;
    *a = regs[p[0]][idx][0];
    *s = regs[p[0]][idx][1];
    return 0;
}
static void reset(void)
{
    static const u64 ane[][2] = {{0x5400000, 0x6c000}, {0x5050000, 0x4000}, {0x4000000, 0x2000000},
                                 {0x1700000, 0x18000}, {0x1724000, 0x4000}};
    static const u64 dart[][2] = {{0x5800000, 0x4000}, {0x5810000, 0x4000}};
    memset(regs, 0, sizeof(regs));
    memcpy(regs[0], ane, sizeof(ane));
    memcpy(regs[1], dart, sizeof(dart));
    regs[2][0][0] = 0x1000000;
    regs[2][0][1] = 0x184000;
    regs[2][1][0] = 0x9000000; /* a second AIC window is not traced */
    regs[2][1][1] = 0x4000;
    nregs[0] = 5; nregs[1] = 2; nregs[2] = 2;
    present[0] = present[1] = present[2] = 1;
    gpu_nranges = hooks = 0;
    sptm_virt_flags = SPTM_VIRT_ANE_TRACE;
}
#define REFUSED(stmt) (setjmp(refused) ? true : ((stmt), false))
''' + setup_source() + r'''
int main(void)
{
    reset();
    assert(!REFUSED(ane_trace_setup()));
    /* ane reg[2] covers ane reg[0], reg[1] and the DART; reg[3], reg[4] and aic reg[0] stand. */
    assert(gpu_nranges == 4 && hooks == 4 && sptm_virt_gpu_print_max == 0);
    assert(gpu_ranges[0].base == 0x4000000 && gpu_ranges[0].size == 0x2000000);
    assert(gpu_ranges[1].base == 0x1700000 && gpu_ranges[2].base == 0x1724000);
    assert(gpu_ranges[3].base == 0x1000000 && !strcmp(gpu_ranges[3].what, "aic"));

    reset(); present[1] = 0; assert(REFUSED(ane_trace_setup())); assert(hooks == 0);
    reset(); nregs[2] = 0; assert(REFUSED(ane_trace_setup())); assert(hooks == 0);
    reset(); regs[0][3][1] = 0; assert(REFUSED(ane_trace_setup())); assert(hooks == 0);
    reset(); sptm_virt_flags |= BIT(6); assert(REFUSED(ane_trace_setup())); assert(hooks == 0);
    reset();
    for (int i = 0; i < 8; i++) { regs[1][i][0] = 0x8000000 + i * 0x10000; regs[1][i][1] = 0x4000; }
    nregs[1] = 8;
    assert(REFUSED(ane_trace_setup())); assert(hooks == 0);
    return 0;
}
'''
    compile_and_run(tmp_path, "ane-trace", harness)
