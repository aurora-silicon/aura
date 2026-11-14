# SPDX-License-Identifier: MIT
"""J514M and J516M device trees get the T6031 handoff allowance in kboot_prepare_dt().

Both T6034 boards run the same handoff (J514M under its own chosen names). On J514M hardware the
base allowance left 2982 bytes after it. J516M runs the T6031 display and PMP handoff (PMP values, display, GPU and firmware facts), so its
output tree needs the same extra room as a T6031. Without it, copying the PMP tunables fails on
hardware and the kernel gets no PMP node: boot framebuffer, no GPU. This builds the real sizing
block and the real board helpers against libfdt, then fills a tree of that size with a T6031-sized
handoff.
"""
from pathlib import Path
from test_siliconforest_handoff import run_c
ROOT = Path(__file__).resolve().parents[2]


def pick(source, signature):
    a = source.index(signature)
    return source[a:source.index('\n}', a) + 2]


def sizing_block():
    src = (ROOT / 'src/kboot.c').read_text()
    fn = src[src.index('int kboot_prepare_dt(void *fdt)'):]
    a = fn.index('    dt_bufsize = fdt_totalsize(fdt);')
    b = fn.index('    dt = memalign(')
    return fn[a:b].replace('assert(dt_bufsize);', 'assert(dt_bufsize > 0);')


def test_t6034_boards_get_the_t6031_handoff_allowance(tmp_path):
    helper = (ROOT / 'src/kboot_t6031.c').read_text()
    code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "libfdt.h"
#define SZ_16K 0x4000
enum { T8103 = 0x8103, T6030 = 0x6030, T6031 = 0x6031, T6032 = 0x6032, T6034 = 0x6034,
       T6040 = 0x6040, T8122 = 0x8122 };
static unsigned int chip_id;
static int dt_bufsize;
''' + pick(helper, 'bool t6031_board(') + pick(helper, 'bool t6034_board(') + \
        pick(helper, 'bool j516m_board(') + r'''
static unsigned char in[4096];
static void tree(const char *board, const char *soc)
{
    assert(!fdt_create_empty_tree(in, sizeof(in)));
    char c[96];
    size_t n = strlen(board) + 1, m = strlen(soc) + 1;
    memcpy(c, board, n);
    memcpy(c + n, soc, m);
    assert(!fdt_setprop(in, 0, "compatible", c, n + m));
    assert(fdt_add_subnode(in, 0, "chosen") >= 0);
}
static int size_for(unsigned int chip, const char *board, const char *soc)
{
    void *fdt = in;
    chip_id = chip;
    tree(board, soc);
''' + sizing_block() + r'''
    return dt_bufsize;
}
/* The T6031 handoff's payload: 32K of PMP values and about 16K of display, GPU and firmware facts */
static bool takes_t6031_handoff(int size)
{
    void *dt = malloc(size);
    static char pmp[32 * 1024], facts[16 * 1024], other[6 * SZ_16K - 2048];
    assert(dt && !fdt_open_into(in, dt, size));
    int chosen = fdt_path_offset(dt, "/chosen");
    /* everything else the 96K base allowance is for, then the handoff */
    bool ok = !fdt_setprop(dt, chosen, "test,other-modifications", other, sizeof(other)) &&
              !fdt_setprop(dt, chosen, "asahi,t6031-pmp", pmp, sizeof(pmp)) &&
              !fdt_setprop(dt, chosen, "asahi,t6031-fw-uuids", facts, sizeof(facts));
    free(dt);
    return ok;
}
int main(void)
{
    int j516c = size_for(T6031, "apple,j516c", "apple,t6031");
    int j516m = size_for(T6034, "apple,j516m", "apple,t6034");
    int j514m = size_for(T6034, "apple,j514m", "apple,t6034");
    /* every tree's room: 96K for modifications and 16K for the SMBIOS description */
    int base = fdt_totalsize(in) + 7 * SZ_16K;

    assert(j516m == j516c);                 /* the same handoff, the same room */
    assert(j516m >= base + 4 * SZ_16K);
    size_for(T6034, "apple,j516m", "apple,t6034");
    assert(takes_t6031_handoff(j516m));
    assert(!takes_t6031_handoff(base));     /* the payload does not fit the base allowance */
    assert(j514m == j516c);                 /* J514M runs the same handoff */
    size_for(T6034, "apple,j514m", "apple,t6034");
    assert(takes_t6031_handoff(j514m));
    /* the allowance follows the exact board: other T6034 boards and mismatched SoCs keep the base */
    assert(size_for(T6034, "apple,j999", "apple,t6034") == base);
    assert(size_for(T6034, "apple,j516m", "apple,t6031") == base);
    assert(size_for(T6034, "apple,j514m", "apple,t6031") == base);
    return 0;
}
'''
    run_c(tmp_path, 'j516m-dt-bufsize', code)
