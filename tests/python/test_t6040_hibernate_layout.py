# SPDX-License-Identifier: MIT
"""T6040 hib=1: the fixed hibernation layout and the RAM handed to Linux with it.

kboot_set_hibernate_layout(), dt_t6040_memory_holes() and heapblock_raise_base()
are the production bodies; the boot arguments and the ADT carveout map are modeled.
"""
import re

from test_smp_timeouts import REPO, compile_and_run


def body(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}\n", start) + 3]


def test_t6040_hibernate_layout(tmp_path):
    kboot = (REPO / "src/kboot.c").read_text()
    heap = (REPO / "src/heapblock.c").read_text()
    defines = "\n".join(re.findall(r"^#define HIB_\w+ .*$", kboot, re.M))
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef unsigned long u64;
typedef uint32_t u32;
typedef uint8_t u8;
typedef u64 fdt64_t;
_Static_assert(sizeof(u64) == 8, "production u64 ABI");
#define T6040 0x6040
#define T6041 0x6041
#define SZ_16K 0x4000UL
#define SZ_1M  0x100000UL
#define SZ_32M 0x2000000UL
#define MAX_MEM_REGIONS 32
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define cpu_to_fdt64(x) __builtin_bswap64(x)
#define fdt64_to_cpu(x) __builtin_bswap64(x)
#define bail(...) do { printf(__VA_ARGS__); return -1; } while (0)
static int chip_id;
static struct { u64 phys_base, mem_size, top_of_kernel_data; } cur_boot_args;
static void *heap_base;
static uintptr_t heapblock_high_water(void) { return (uintptr_t)heap_base; }
''' + body(heap, "bool heapblock_raise_base(uintptr_t base)") + defines + r'''
static u64 hib_heap_base, hib_heap_end, hib_dt_base;
''' + body(kboot, "void kboot_set_hibernate_layout(void)") + r'''
/* The ADT carveout map: name and {base, size}. */
struct carveout { char name[32]; u64 range[2]; };
static struct carveout carveouts[8];
static int ncarveouts;
static void *adt;
static int adt_path_offset(void *tree, const char *path)
{
    (void)tree;
    assert(!strcmp(path, "/chosen/carveout-memory-map"));
    return 1;
}
struct prop_view { const char *name; u32 size; const void *value; };
#define ADT_FOREACH_PROPERTY(tree, node, prop)                                              \
    for (int _i = 0; _i < ncarveouts; _i++)                                                 \
        for (struct prop_view _v = {carveouts[_i].name, 16, carveouts[_i].range}, *prop = &_v; \
             prop; prop = NULL)
''' + body(kboot, "static int dt_t6040_memory_holes(") + r'''
#define RAM 0x10000000000UL
static void boot(u64 top, u64 heap)
{
    cur_boot_args.phys_base = RAM;
    cur_boot_args.mem_size = 48UL << 30;
    cur_boot_args.top_of_kernel_data = top;
    heap_base = (void *)heap;
    hib_heap_base = hib_heap_end = hib_dt_base = 0;
    chip_id = T6040;
}
static void carveout(const char *name, u64 base, u64 size)
{
    strcpy(carveouts[ncarveouts].name, name);
    carveouts[ncarveouts].range[0] = base;
    carveouts[ncarveouts++].range[1] = size;
}
static int ram(u64 *out)
{
    fdt64_t reg[2 * MAX_MEM_REGIONS];
    int n = 0;
    assert(!dt_t6040_memory_holes(RAM, RAM + (48UL << 30), reg, &n));
    for (int i = 0; i < 2 * n; i++)
        out[i] = fdt64_to_cpu(reg[i]);
    return n;
}
int main(void)
{
    u64 r[2 * MAX_MEM_REGIONS];
    const u64 dt = RAM + 160 * SZ_1M, end = dt + 128 * SZ_1M;

    /* The heap restarts above the device tree slot, at the same place on every boot. */
    boot(RAM + 0x6988000, RAM + 0x7000000);
    kboot_set_hibernate_layout();
    assert(hib_dt_base == dt && hib_heap_base == dt + 2 * SZ_1M && hib_heap_end == end);
    assert(heapblock_high_water() == dt + 2 * SZ_1M);
    boot(RAM + 0x6658000, RAM + 0x6800000);
    kboot_set_hibernate_layout();
    assert(hib_dt_base == dt && hib_heap_end == end && heapblock_high_water() == dt + 2 * SZ_1M);

    /* Linux gets region-id-45 from the fixed end on, the other carveouts and guards left out. */
    carveout("region-id-24", RAM, 48UL << 30);
    carveout("region-id-45", RAM, 512 * SZ_1M);
    carveout("region-id-10", RAM + (1UL << 30), 16 * SZ_1M);
    carveout("region-id-11", RAM + (1UL << 30) + 16 * SZ_1M + SZ_16K, 16 * SZ_1M);
    carveout("region-id-12", RAM + (2UL << 30), 0);
    assert(ram(r) == 3);
    assert(r[0] == RAM && r[1] == end - RAM);
    assert(r[2] == RAM + 512 * SZ_1M && r[3] == (1UL << 30) - 512 * SZ_1M);
    assert(r[4] == RAM + (1UL << 30) + 32 * SZ_1M + SZ_16K);
    assert(r[4] + r[5] == RAM + (48UL << 30));

    /* A heap that ran past the fixed end refuses the boot. */
    heap_base = (void *)(end + SZ_16K);
    fdt64_t reg[2 * MAX_MEM_REGIONS];
    int count;
    assert(dt_t6040_memory_holes(RAM, RAM + (48UL << 30), reg, &count) == -1);

    /* Without hib=1, region-id-45 is handed back from the heap high water as before. */
    boot(RAM + 0x6988000, RAM + 0x7001000);
    assert(ram(r) == 3 && r[0] == RAM && r[1] == 0x7004000 && r[2] == RAM + 512 * SZ_1M);

    /* Refused: another chip, an object above the fixed heap, a heap already past it, too
     * little RAM. Nothing is set and the heap stays where it was. */
    boot(RAM + 0x6988000, RAM + 0x7000000);
    chip_id = T6041;
    kboot_set_hibernate_layout();
    assert(!hib_heap_end && heapblock_high_water() == RAM + 0x7000000);
    boot(dt + SZ_16K, RAM + 0x7000000);
    kboot_set_hibernate_layout();
    assert(!hib_heap_end && heapblock_high_water() == RAM + 0x7000000);
    boot(RAM + 0x6988000, dt + 4 * SZ_1M);
    kboot_set_hibernate_layout();
    assert(!hib_heap_end && heapblock_high_water() == dt + 4 * SZ_1M);
    boot(RAM + 0x6988000, RAM + 0x7000000);
    cur_boot_args.mem_size = 256 * SZ_1M;
    kboot_set_hibernate_layout();
    assert(!hib_heap_end && heapblock_high_water() == RAM + 0x7000000);
    puts("T6040 hibernation layout PASS");
    return 0;
}
'''
    compile_and_run(tmp_path, "t6040-hibernate-layout", harness)
