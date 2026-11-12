# SPDX-License-Identifier: MIT
"""A USB controller that could not be stopped boots on with its DMA reserved.

Runs kboot.c's dt_reserve_retained_dma() against real FDTs, usb_dwc3_retain.c
as built, and dart.c's dart_collect_tables() against a modelled DART.

Ways this can go wrong, each checked below:
- the retained buffers or DART tables never reach the kernel's tree, or reach
  it as a node the kernel would map (no no-map) or misread (unaligned, wrong
  cells, a tree without /reserved-memory);
- a second pass duplicates nodes, or the tree is left unpacked or invalid;
- a full buffer corrupts the tree instead of failing;
- ranges beyond the record's capacity are dropped silently;
- a DART's L2 tables or roots are missed, or tables aura does not own
  (locked or kept DARTs, tables outside the heap) are claimed;
- the handoff refuses to boot instead of reserving.
"""
from pathlib import Path
import re
import subprocess

from host_compiler import host_cc
from test_siliconforest_handoff import run_c

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    match = re.search(r"^(?:static )?(?:bool|void|int) " + name + r"\([^;]*?\)\s*\{", source,
                      re.M | re.S)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end] + "\n"


def test_reserved_memory_nodes(tmp_path):
    reserve = function((ROOT / "src/kboot.c").read_text(), "dt_reserve_retained_dma")
    run_c(tmp_path, "usb-dma-reserve", r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "libfdt.h"
typedef uint64_t u64;
#define SZ_16K 0x4000
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
struct usb_dwc3_dma_range { u64 base; u64 size; };
''' + reserve + r'''
static const fdt64_t *reg_of(void *fdt, u64 start, int *len)
{
    char path[64];
    snprintf(path, sizeof(path), "/reserved-memory/aura-usb-dma@%llx", (unsigned long long)start);
    int node = fdt_path_offset(fdt, path);
    assert(node >= 0);
    assert(fdt_getprop(fdt, node, "no-map", NULL));
    return fdt_getprop(fdt, node, "reg", len);
}
static int count_nodes(void *fdt)
{
    int n = 0, node, resv = fdt_path_offset(fdt, "/reserved-memory");
    fdt_for_each_subnode(node, fdt, resv)
        n++;
    return n;
}
int main(void)
{
    static unsigned char blob[8192];
    const struct usb_dwc3_dma_range ranges[] = {
        {0x10000c000ULL, 0x4000},  /* event buffer, page aligned */
        {0x100020100ULL, 0x8000},  /* a range off a page boundary is rounded out */
        {0x10000c000ULL, 0x4000},  /* the same page again */
    };
    int len;

    /* A packed tree without /reserved-memory gets one with 2/2 cells. */
    assert(fdt_create_empty_tree(blob, sizeof(blob)) == 0);
    assert(fdt_pack(blob) == 0);
    assert(dt_reserve_retained_dma(blob, sizeof(blob), ranges, 3) == 0);
    assert(fdt_check_header(blob) == 0 && fdt_totalsize(blob) < sizeof(blob));
    int resv = fdt_path_offset(blob, "/reserved-memory");
    assert(resv >= 0);
    assert(fdt32_to_cpu(*(const fdt32_t *)fdt_getprop(blob, resv, "#address-cells", NULL)) == 2);
    assert(fdt32_to_cpu(*(const fdt32_t *)fdt_getprop(blob, resv, "#size-cells", NULL)) == 2);
    assert(fdt_getprop(blob, resv, "ranges", &len) && len == 0);
    const fdt64_t *reg = reg_of(blob, 0x10000c000ULL, &len);
    assert(len == 16 && fdt64_to_cpu(reg[0]) == 0x10000c000ULL && fdt64_to_cpu(reg[1]) == 0x4000);
    reg = reg_of(blob, 0x100020000ULL, &len);
    assert(len == 16 && fdt64_to_cpu(reg[0]) == 0x100020000ULL && fdt64_to_cpu(reg[1]) == 0xc000);
    assert(count_nodes(blob) == 2);

    /* Again (a second pass): nothing doubled, the tree stays valid. */
    assert(dt_reserve_retained_dma(blob, sizeof(blob), ranges, 3) == 0);
    assert(count_nodes(blob) == 2 && fdt_check_header(blob) == 0);

    /* An existing /reserved-memory keeps its nodes. */
    assert(fdt_create_empty_tree(blob, sizeof(blob)) == 0);
    resv = fdt_add_subnode(blob, 0, "reserved-memory");
    assert(fdt_add_subnode(blob, resv, "other@0") >= 0);
    assert(fdt_pack(blob) == 0);
    assert(dt_reserve_retained_dma(blob, sizeof(blob), ranges, 2) == 0);
    assert(count_nodes(blob) == 3 && fdt_path_offset(blob, "/reserved-memory/other@0") >= 0);

    /* No room: refused, and the tree is still the valid one it was. */
    assert(fdt_create_empty_tree(blob, sizeof(blob)) == 0);
    assert(fdt_pack(blob) == 0);
    int small = fdt_totalsize(blob) + 64;
    assert(dt_reserve_retained_dma(blob, small, ranges, 2) < 0);
    assert(fdt_check_header(blob) == 0);
    return 0;
}
''')


def test_retained_record_and_dart_tables(tmp_path):
    retain = (ROOT / "src/usb_dwc3_retain.c").read_text()
    retain = retain.replace('#include "dart.h"', "").replace('#include "usb_dwc3.h"', "")
    retain = retain.replace('#include "utils.h"', "")
    collect = function((ROOT / "src/dart.c").read_text(), "dart_collect_tables")
    header = (ROOT / "src/usb_dwc3.h").read_text()
    dma_range = re.search(r"struct usb_dwc3_dma_range \{.*?\};", header, re.S).group(0)
    capacity = re.search(r"#define USB_DWC3_RETAINED_MAX \d+", header).group(0)
    source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef uint64_t u64;
#define SZ_16K 0x4000
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define BIT(n) (1ULL << (n))
#define GENMASK(h, l) (((~0ULL) << (l)) & (~0ULL >> (63 - (h))))
#define FIELD_GET(m, v) (((v) & (m)) >> __builtin_ctzll(m))
#define DART_PTE_VALID BIT(0)
#define DART_PTE_OFFSET_SHIFT 14
struct dart_params { int ttbr_count; u64 offset_mask; };
typedef struct dart_dev {
    bool locked, keep;
    const struct dart_params *params;
    u64 *l1[4];
} dart_dev_t;
static _Alignas(SZ_16K) u64 heap[8][SZ_16K / 8];
static bool is_heap(void *p) { return (char *)p >= (char *)heap && (char *)p < (char *)heap + sizeof(heap); }
''' + dma_range + "\n" + capacity + "\n" + collect + r'''
int dart_collect_tables(dart_dev_t *dart, u64 *pages, size_t capacity, size_t *count);
''' + retain + r'''
static u64 pte(void *table, u64 mask) { return ((((u64)table) >> DART_PTE_OFFSET_SHIFT) << __builtin_ctzll(mask)) | DART_PTE_VALID; }
int main(void)
{
    const struct dart_params params = {2, GENMASK(57, 10)};
    dart_dev_t dart = {.params = &params, .l1 = {heap[0], heap[1]}};
    static _Alignas(SZ_16K) u64 outside[SZ_16K / 8];
    u64 pages[8];
    size_t n;

    /* Roots, then the L2 tables they point to; a table off the heap is not aura's. */
    heap[0][3] = pte(heap[2], params.offset_mask);
    heap[0][9] = pte(outside, params.offset_mask);
    heap[1][0] = pte(heap[3], params.offset_mask);
    assert(dart_collect_tables(&dart, pages, 8, &n) == 0 && n == 4);
    assert(pages[0] == (u64)heap[0] && pages[1] == (u64)heap[2]);
    assert(pages[2] == (u64)heap[1] && pages[3] == (u64)heap[3]);
    assert(dart_collect_tables(&dart, pages, 3, &n) < 0 && n == 0);

    /* Locked or kept DARTs: their tables are not aura's to reserve. */
    dart.locked = true;
    assert(dart_collect_tables(&dart, pages, 8, &n) == 0 && n == 0);
    dart.locked = false;

    /* The record: buffers, then the DART's tables, 16 KiB each. */
    const struct usb_dwc3_dma_range *r;
    bool overflow;
    assert(usb_dwc3_retain_dma(0x1000, 0x4000) && usb_dwc3_retain_dma(0, 0x4000));
    assert(usb_dwc3_retain_dart(&dart) && usb_dwc3_retain_dart(NULL));
    assert(usb_dwc3_retained(&r, &overflow) == 5 && !overflow);
    assert(r[0].base == 0x1000 && r[1].base == (u64)heap[0] && r[4].size == SZ_16K);

    /* Past the capacity: recorded as an overflow, never dropped silently. */
    while (usb_dwc3_retain_dma(0x2000, 0x4000))
        ;
    assert(usb_dwc3_retained(&r, &overflow) == USB_DWC3_RETAINED_MAX && overflow);
    return 0;
}
'''
    binary = tmp_path / "usb-retain"
    subprocess.run([*host_cc(), "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-x", "c", "-",
                    "-o", str(binary)], input=source, text=True, check=True)
    subprocess.run([str(binary)], check=True, timeout=5)


def test_handoff_reserves_and_boots_on():
    main = (ROOT / "src/main.c").read_text()
    shutdown = main.index("        usb_iodev_shutdown();\n")
    display = main.index("display_shutdown(", shutdown)
    step = main[shutdown:display]
    assert "kboot_reserve_usb_dma()" in step
    assert "flush_and_reboot" not in step and "panic(" not in step and "uartproxy_run" not in step
    reserve = function((ROOT / "src/kboot.c").read_text(), "kboot_reserve_usb_dma")
    # Only the tree kboot_boot() handed over (dt, or its hibernation-slot copy) is edited.
    assert "fdt == dt" in reserve and "fdt == (void *)hib_dt_base" in reserve
    assert "dt_reserve_retained_dma(fdt, size" in reserve
