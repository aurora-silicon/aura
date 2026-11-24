# SPDX-License-Identifier: MIT
"""mmu_map() builds an L2 block whose output range holds an armed CTRR range from L3 pages.

The page-table builder of src/memory.c (the mmu_pt_* helpers, mmu_map(), mmu_add_mapping() and
mmu_rm_mapping()) and ctxr_range() of src/ctxr_exec.c are compiled unchanged into a host program
with 16K geometry. The CTRR/CTXR registers are variables, the tables are host memory, and the
program prints the finished tables as runs of leaf descriptors. The tests compare them with what
the layouts need, and a seeded test compares them with a model of the mapping rule written here.

AURA_MMU_SOURCE names another memory.c to build (the negative controls use it).
"""
import os
from pathlib import Path
import random
import re
import subprocess

import pytest

from host_compiler import host_cc
from test_locked_dart_tables import function

REPO = Path(__file__).resolve().parents[2]
MEMORY_C = Path(os.environ.get("AURA_MMU_SOURCE", REPO / "src/memory.c"))

PAGE = 1 << 14
BLOCK = 1 << 25
MIB = 1 << 20

VALID, TYPE, ACCESS, SH_OS = 1, 2, 1 << 10, 2 << 8
AP_RO, AP_EL0, PXN, UXN = 1 << 7, 1 << 6, 1 << 53, 1 << 54
PERM_RWX, PERM_RWX_EL0, PERM_RW_EL0, PERM_RX_EL0 = 0, AP_EL0, AP_EL0 | PXN | UXN, AP_EL0 | AP_RO
REGION_RWX_EL0, REGION_RW_EL0, REGION_RX_EL1 = 0x80000000000, 0xa0000000000, 0xc0000000000

# The J873g boot of the change's description: RAM at 1 << 40, CTRR_C over 48 KiB, in the eighth
# 32 MiB block of RAM.
RAM = 0x10000000000
RAM_SIZE = 16 * BLOCK
BASE = RAM + 0x4a50000
ABOVE_BASE = RAM + 3 * BLOCK
CTRR_C = (0x1000ef9c000, 0x1000efa8000)
C_BLOCK = RAM + 7 * BLOCK


def defines(path, *names):
    text = (REPO / path).read_text()
    out = []
    for name in names:
        match = re.search(r"^#define " + re.escape(name) + r"\b.*$", text, re.M)
        assert match, name
        out.append(match.group(0))
    return "\n".join(out)


def between(text, start, end):
    begin = text.index(start)
    return text[begin:text.index(end, begin)]


HARNESS_HEAD = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* unsigned long matches the %lx the source prints with, on Linux and macOS alike. */
typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned char u8;
#define SZ_4K 0x1000UL
#define T8140 0x8140
#define PAGE_SIZE 16384UL
#define REGION_RWX_EL0 0x80000000000
#define REGION_RW_EL0  0xa0000000000
#define REGION_RX_EL1  0xc0000000000

static char console[8192];
static void console_printf(const char *fmt, ...)
{
    va_list ap;
    size_t used = strlen(console);
    va_start(ap, fmt);
    vsnprintf(console + used, sizeof(console) - used, fmt, ap);
    va_end(ap);
}
static void panic(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    abort();
}

static u64 ram_base, _base;
static u32 chip_id;
static bool mmu_on, at_el1;
static bool mmu_active(void) { return mmu_on; }
static bool is_16k(void) { return true; }
static bool in_el2(void) { return !at_el1; }
static struct { bool apple_sysregs_unlocked; } features, *cpu_features = &features;

/* The firmware RO range of the T8140. Without one the outputs are not a range: poison them. */
static int ro_status;
static u64 ro_start_v, ro_end_v;
static int memory_fw_ro_range(u64 *start, u64 *end)
{
    *start = ro_status > 0 ? ro_start_v : 0;
    *end = ro_status > 0 ? ro_end_v : ~0UL;
    return ro_status;
}

/* The registers. A read of `fault_reg` under the guard faults, as an undefined register does. */
enum exc_guard_t { GUARD_OFF = 0, GUARD_MARK = 2, GUARD_SILENT = 0x100 };
static volatile int exc_guard, exc_count;
static int register_reads;
static const u64 *fault_reg;
static u64 read_register(const u64 *reg)
{
    register_reads++;
    if (reg == fault_reg) {
        assert(exc_guard == (GUARD_MARK | GUARD_SILENT));
        exc_count++;
        return 0xacce5515abad1dea;
    }
    return *reg;
}
#define mrs(reg) read_register(&(reg))
static u64 SYS_IMP_APL_CTXR_A_LWR_EL1, SYS_IMP_APL_CTXR_A_UPR_EL1, SYS_IMP_APL_CTXR_B_LWR_EL1,
    SYS_IMP_APL_CTXR_B_UPR_EL1, SYS_IMP_APL_CTXR_C_LWR_EL1, SYS_IMP_APL_CTXR_C_UPR_EL1,
    SYS_IMP_APL_CTXR_D_LWR_EL1, SYS_IMP_APL_CTXR_D_UPR_EL1, SYS_IMP_APL_CTRR_C_LWR_EL1,
    SYS_IMP_APL_CTRR_C_UPR_EL1, SYS_IMP_APL_CTRR_D_LWR_EL1, SYS_IMP_APL_CTRR_D_UPR_EL1;

/* Tables are host memory. With the MMU off a descriptor write is a plain store. */
static int allocs, frees;
static void *table_alloc(size_t align, size_t size)
{
    void *p = NULL;
    assert(!posix_memalign(&p, align, size));
    allocs++;
    return p;
}
static void table_free(void *p)
{
    frees++;
    free(p);
}
static void memset64(void *dst, u64 value, size_t size)
{
    for (size_t i = 0; i < size / 8; i++)
        ((u64 *)dst)[i] = value;
}
static u64 *mmu_pt_L0;
static int barriers;
static void mmu_write_descriptor(u64 *entry, u64 value, u64 from)
{
    (void)from;
    *entry = value;
}
#define memalign(align, size) table_alloc(align, size)
#define free(p)               table_free(p)
#define sysop(op)             ((void)barriers++)
#define printf(...)           console_printf(__VA_ARGS__)
'''

HARNESS_TAIL = r'''
#undef printf
#undef free
#undef memalign

/* The walk knows 16K geometry by itself: 1 + 11 + 11 + 11 index bits over a 14-bit offset. */
#define OUT_MASK 0x0003ffffffffc000UL
static u64 run_va, run_size, run_out, run_attr;
static int run_level;
static void flush(void)
{
    if (run_size)
        printf("leaf 0x%lx 0x%lx %d 0x%lx 0x%lx\n", run_va, run_size, run_level, run_out, run_attr);
    run_size = 0;
}
static void leaf(u64 va, u64 size, int level, u64 desc)
{
    u64 out = desc & OUT_MASK, attr = desc & ~OUT_MASK;
    /* A block descriptor carries no output bits under its size, and is not a table. */
    assert(!(out & (size - 1)));
    assert(((desc >> 1) & 1) == (level == 3));
    if (run_size && run_level == level && run_attr == attr && run_va + run_size == va &&
        run_out + run_size == out) {
        run_size += size;
        return;
    }
    flush();
    run_va = va; run_size = size; run_level = level; run_out = out; run_attr = attr;
}
static int tables[4];
static void walk(const u64 *table, int level, u64 va)
{
    static const int shift[] = {47, 36, 25, 14};
    int entries = level ? 2048 : 2;
    tables[level]++;
    for (int i = 0; i < entries; i++) {
        u64 desc = table[i], at = va + ((u64)i << shift[level]);
        if (!(desc & 1))
            continue;
        if (level < 3 && (desc & 2))
            walk((const u64 *)(desc & OUT_MASK), level + 1, at);
        else {
            assert(level >= 2); /* no L0 or L1 blocks with 16K pages */
            leaf(at, 1UL << shift[level], level, desc);
        }
    }
}

static const u64 *reg_named(const char *name)
{
    if (!strcmp(name, "C")) return &SYS_IMP_APL_CTRR_C_LWR_EL1;
    if (!strcmp(name, "CU")) return &SYS_IMP_APL_CTRR_C_UPR_EL1;
    if (!strcmp(name, "D")) return &SYS_IMP_APL_CTRR_D_LWR_EL1;
    if (!strcmp(name, "DU")) return &SYS_IMP_APL_CTRR_D_UPR_EL1;
    assert(0);
    return NULL;
}

/*
 * Arguments, taken in order: settings (ram= base= chip= unlocked= el1= mmu= fault=NAME c=LWR:UPR
 * d=LWR:UPR xc=LWR:UPR xd=LWR:UPR ro=START:END) and operations (map=FROM:TO:SIZE:MAIR:PERMS,
 * unmap=FROM:SIZE, raw=FROM:TO:SIZE). Every number is hex.
 */
int main(int argc, char **argv)
{
    mmu_init_pagetables();
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        u64 v[5] = {0};
        int n = 0;
        assert(eq);
        *eq++ = 0;
        if (strcmp(argv[i], "fault"))
            for (char *p = eq; n < 5; n++) {
                v[n] = strtoul(p, &p, 16);
                if (*p++ != ':') { n++; break; }
            }
        const char *key = argv[i];
        if (!strcmp(key, "ram")) ram_base = v[0];
        else if (!strcmp(key, "base")) _base = v[0];
        else if (!strcmp(key, "chip")) chip_id = v[0];
        else if (!strcmp(key, "unlocked")) features.apple_sysregs_unlocked = v[0];
        else if (!strcmp(key, "el1")) at_el1 = v[0];
        else if (!strcmp(key, "mmu")) mmu_on = v[0];
        else if (!strcmp(key, "fault")) fault_reg = reg_named(eq);
        else if (!strcmp(key, "c")) { SYS_IMP_APL_CTRR_C_LWR_EL1 = v[0]; SYS_IMP_APL_CTRR_C_UPR_EL1 = v[1]; }
        else if (!strcmp(key, "d")) { SYS_IMP_APL_CTRR_D_LWR_EL1 = v[0]; SYS_IMP_APL_CTRR_D_UPR_EL1 = v[1]; }
        else if (!strcmp(key, "xc")) { SYS_IMP_APL_CTXR_C_LWR_EL1 = v[0]; SYS_IMP_APL_CTXR_C_UPR_EL1 = v[1]; }
        else if (!strcmp(key, "xd")) { SYS_IMP_APL_CTXR_D_LWR_EL1 = v[0]; SYS_IMP_APL_CTXR_D_UPR_EL1 = v[1]; }
        else if (!strcmp(key, "ro")) { ro_status = 1; ro_start_v = v[0]; ro_end_v = v[1]; }
        else {
            int ret = 0;
            if (!strcmp(key, "map")) { assert(n == 5); mmu_add_mapping(v[0], v[1], v[2], v[3], v[4]); }
            else if (!strcmp(key, "unmap")) { assert(n == 2); mmu_rm_mapping(v[0], v[1]); }
            else if (!strcmp(key, "raw")) { assert(n == 3); ret = mmu_map(v[0], v[1], v[2]); }
            else assert(0);
            assert(exc_guard == GUARD_OFF);
            printf("op %s ret=%d reads=%d\n", key, ret, register_reads);
        }
    }
    walk(mmu_pt_L0, 0, 0);
    flush();
    printf("tables %d %d %d %d\n", tables[0], tables[1], tables[2], tables[3]);
    /* Every table but L0 is in the tree or was freed: none is lost when a block replaces it. */
    printf("lost %d\n", allocs - frees - (tables[0] + tables[1] + tables[2] + tables[3]));
    for (char *line = strtok(console, "\n"); line; line = strtok(NULL, "\n"))
        printf("log %s\n", line);
    return 0;
}
'''


def harness_source():
    memory = MEMORY_C.read_text()
    ctxr = (REPO / "src/ctxr_exec.c").read_text()
    header = (REPO / "src/ctxr_exec.h").read_text()
    return "\n".join((
        HARNESS_HEAD,
        defines("src/types.h", "BIT", "MASK", "GENMASK", "_FIELD_LSB", "FIELD_PREP", "FIELD_GET"),
        defines("src/utils.h", "ARRAY_SIZE", "ALIGN_UP", "ALIGN_DOWN", "min"),
        defines("src/memory.h", "PTE_VALID", "PTE_TYPE", "PTE_BLOCK", "PTE_TABLE", "PTE_PAGE",
                "PTE_ACCESS", "PTE_MAIR_IDX", "PTE_SH_OS"),
        between(header, "enum ctxr_range {", "};") + "};",
        function(ctxr, "ctxr_range"),
        # the table geometry and descriptor tests
        between(memory, "#define VADDR_L3_INDEX_BITS", "/*\n * aura supports 4K and 16K paging"),
        # mmu_pt_get_l1() to mmu_rm_mapping(): the table builder, the CTRR ranges and mmu_map()
        between(memory, "static u64 *mmu_pt_get_l1(u64 from)", "static void mmu_map_mmio(void)"),
        HARNESS_TAIL,
    ))


@pytest.fixture(scope="module")
def host(tmp_path_factory):
    tmp = tmp_path_factory.mktemp("mmu-ctrr")
    source = tmp / "mmu_ctrr_host.c"
    source.write_text(harness_source())
    binary = tmp / "mmu-ctrr-host"
    subprocess.run(
        [*host_cc(), "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
         "-Wno-unused-parameter", "-Wno-unused-function", "-Wno-unused-variable",
         "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
         "-fno-omit-frame-pointer", str(source), "-o", str(binary)],
        check=True)
    return binary


class Tables:
    """What the host program printed: the leaf descriptors, the log and the counters."""

    def __init__(self, text):
        self.blocks, self.page_runs, self.log, self.reads = {}, [], [], []
        for line in text.splitlines():
            kind, _, rest = line.partition(" ")
            if kind == "leaf":
                va, size, level, out, attr = (int(x, 0) for x in rest.split())
                if level == 2:
                    for off in range(0, size, BLOCK):
                        self.blocks[va + off] = (out + off, attr)
                else:
                    self.page_runs.append((va, size, out, attr))
            elif kind == "log":
                self.log.append(rest)
            elif kind == "op":
                self.reads.append(int(rest.rpartition("reads=")[2]))
            elif kind == "tables":
                self.tables = [int(x) for x in rest.split()]
            elif kind == "lost":
                self.lost = int(rest)
        assert self.lost == 0

    def pages(self):
        return {va + off: (out + off, attr)
                for va, size, out, attr in self.page_runs for off in range(0, size, PAGE)}

    def level(self, va):
        """2 for an L2 block, 3 for L3 pages, None for nothing mapped at this address."""
        if va & -BLOCK in self.blocks:
            return 2
        return 3 if any(run <= va < run + size for run, size, _, _ in self.page_runs) else None

    def split_blocks(self, start, size):
        """The 32 MiB blocks of [start, start + size) that are built from L3 pages."""
        return [va for va in range(start, start + size, BLOCK) if self.level(va) == 3]

    def translate(self, va):
        if va & -BLOCK in self.blocks:
            out, attr = self.blocks[va & -BLOCK]
            return out + (va & (BLOCK - 1)), attr
        for run, size, out, attr in self.page_runs:
            if run <= va < run + size:
                return out + (va - run), attr
        return None

    def ctrr_log(self):
        return [line for line in self.log if "CTRR" in line]


def upr(end):
    """The UPR register of a range that ends at `end`: its last 4K page."""
    return end - 0x1000


def arm(name, start, end):
    return f"{name}={start:x}:{upr(end):x}"


def mapping(frm, to, size, mair=0, perms=PERM_RWX):
    return f"map={frm:x}:{to:x}:{size:x}:{mair:x}:{perms:x}"


def unmapping(frm, size):
    return f"unmap={frm:x}:{size:x}"


def run(host, *args, ram=RAM, base=BASE):
    out = subprocess.run([str(host), f"ram={ram:x}", f"base={base:x}", *args],
                         capture_output=True, text=True)
    assert out.returncode == 0, out.stdout + out.stderr
    return Tables(out.stdout)


def attrs(perms, mair=0, level=3):
    return VALID | (TYPE if level == 3 else 0) | ACCESS | SH_OS | (mair << 2) | perms


IDENTITY = mapping(RAM, RAM, RAM_SIZE)
BELOW_BASE = [RAM, RAM + BLOCK, RAM + 2 * BLOCK]


def test_nothing_armed_keeps_l2_blocks(host):
    t = run(host, IDENTITY)
    # below aura's base everything is L3 pages, as before; above it, L2 blocks
    assert t.split_blocks(RAM, RAM_SIZE) == BELOW_BASE
    assert t.ctrr_log() == []
    assert t.translate(RAM + 9 * BLOCK + 0x12340) == (RAM + 9 * BLOCK + 0x12340, attrs(0, level=2))


def test_armed_ctrr_c_block_is_l3_pages(host):
    t = run(host, arm("c", *CTRR_C), IDENTITY)
    assert t.split_blocks(RAM, RAM_SIZE) == [*BELOW_BASE, C_BLOCK]
    assert t.log == ["MMU: L3 pages over CTRR_C [0x1000ef9c000, 0x1000efa8000)"]
    # all 2048 pages of the block, each onto itself with the block's attributes
    pages = t.pages()
    for va in range(C_BLOCK, C_BLOCK + BLOCK, PAGE):
        assert pages[va] == (va, attrs(0))
    # the framebuffer write that faulted on the J873g goes through L3 now
    assert t.level(0x1000fa12010) == 3
    assert t.translate(0x1000fa12010) == (0x1000fa12010, attrs(0))
    # one L3 table for each split block, and no more
    assert t.tables == [1, 1, 1, 4]


def test_every_alias_of_the_ram_is_split_by_output_address(host):
    aliases = [(REGION_RWX_EL0, PERM_RWX_EL0), (REGION_RW_EL0, PERM_RW_EL0),
               (REGION_RX_EL1, PERM_RX_EL0)]
    t = run(host, arm("c", *CTRR_C), IDENTITY,
            *(mapping(RAM | region, RAM, RAM_SIZE, perms=perms) for region, perms in aliases))
    for region, perms in aliases:
        # the aliases lie above aura's base, so only the CTRR block is split there
        assert t.split_blocks(RAM | region, RAM_SIZE) == [C_BLOCK | region]
        assert t.translate(CTRR_C[0] | region) == (CTRR_C[0], attrs(perms))
        assert t.translate((C_BLOCK - PAGE) | region) == (C_BLOCK - PAGE, attrs(perms, level=2))
    assert len(t.ctrr_log()) == 1


def test_split_follows_the_output_not_the_virtual_address(host):
    # RAM seen five blocks higher at an unrelated address: the range sits in VA block 12 there
    va = 0x20000000000 + 5 * BLOCK
    t = run(host, arm("c", *CTRR_C), mapping(va, RAM, RAM_SIZE, perms=PERM_RW_EL0))
    assert t.split_blocks(va, RAM_SIZE) == [va + 7 * BLOCK]
    assert t.translate(va + (CTRR_C[0] - RAM)) == (CTRR_C[0], attrs(PERM_RW_EL0))
    # a mapping of other memory at the range's own virtual address stays L2 blocks
    t = run(host, arm("c", *CTRR_C), mapping(RAM, RAM + 64 * BLOCK, RAM_SIZE))
    assert t.split_blocks(RAM, RAM_SIZE) == BELOW_BASE


@pytest.mark.parametrize("start,end,split", [
    # one 4K page at the start of a block, and at the end of one
    (8 * BLOCK, 8 * BLOCK + 0x1000, [8]),
    (9 * BLOCK - 0x1000, 9 * BLOCK, [8]),
    # ending exactly where the next block starts: the next block is left alone
    (8 * BLOCK - 3 * PAGE, 8 * BLOCK, [7]),
    # starting exactly where a block starts: the block before is left alone
    (8 * BLOCK, 8 * BLOCK + 3 * PAGE, [8]),
    # across a block boundary: both
    (8 * BLOCK - PAGE, 8 * BLOCK + PAGE, [7, 8]),
    # several whole blocks
    (9 * BLOCK, 13 * BLOCK, [9, 10, 11, 12]),
    (9 * BLOCK - 0x1000, 13 * BLOCK + 0x1000, [8, 9, 10, 11, 12, 13]),
    # the first block above aura's base, and the last block of the mapping
    (3 * BLOCK, 3 * BLOCK + PAGE, [3]),
    (16 * BLOCK - PAGE, 16 * BLOCK, [15]),
    # below aura's base (already L3) and outside the mapping: nothing more
    (BLOCK, BLOCK + PAGE, []),
    (16 * BLOCK, 16 * BLOCK + PAGE, []),
    (64 * BLOCK, 65 * BLOCK, []),
])
@pytest.mark.parametrize("name", ["c", "d"])
def test_exactly_the_blocks_holding_the_range(host, name, start, end, split):
    t = run(host, arm(name, RAM + start, RAM + end), IDENTITY)
    assert t.split_blocks(RAM, RAM_SIZE) == [*BELOW_BASE, *(RAM + n * BLOCK for n in split)]
    assert t.log == [f"MMU: L3 pages over CTRR_{name.upper()} [0x{RAM + start:x}, 0x{RAM + end:x})"]
    for va in range(RAM, RAM + RAM_SIZE, 37 * PAGE):
        assert t.translate(va)[0] == va


def test_ctrr_c_and_ctrr_d(host):
    c, d = (RAM + 5 * BLOCK + PAGE, RAM + 5 * BLOCK + 4 * PAGE), (RAM + 11 * BLOCK, RAM + 12 * BLOCK)
    t = run(host, arm("c", *c), arm("d", *d), IDENTITY)
    assert t.split_blocks(RAM, RAM_SIZE) == [*BELOW_BASE, RAM + 5 * BLOCK, RAM + 11 * BLOCK]
    assert t.log == [f"MMU: L3 pages over CTRR_C [0x{c[0]:x}, 0x{c[1]:x})",
                     f"MMU: L3 pages over CTRR_D [0x{d[0]:x}, 0x{d[1]:x})"]
    # the other order of addresses, and CTRR_D alone
    t = run(host, arm("c", *d), arm("d", *c), IDENTITY)
    assert t.split_blocks(RAM, RAM_SIZE) == [*BELOW_BASE, RAM + 5 * BLOCK, RAM + 11 * BLOCK]
    t = run(host, arm("d", *c), IDENTITY)
    assert t.split_blocks(RAM, RAM_SIZE) == [*BELOW_BASE, RAM + 5 * BLOCK]
    assert t.log == [f"MMU: L3 pages over CTRR_D [0x{c[0]:x}, 0x{c[1]:x})"]


def test_ctxr_windows_do_not_split(host):
    # CTXR_C and CTXR_D are other registers: the execute windows are not what faults here
    t = run(host, arm("xc", *CTRR_C), arm("xd", RAM + 11 * BLOCK, RAM + 12 * BLOCK), IDENTITY)
    assert t.split_blocks(RAM, RAM_SIZE) == BELOW_BASE
    assert t.log == []


@pytest.mark.parametrize("setting,reads", [
    # unlocked Apple system registers (M1 to M3): the registers are not read at all
    (["unlocked=1"], 0),
    # not at EL2: ctxr_range() cannot see them
    (["el1=1"], 0),
    # LWR reads 0: not armed, whatever UPR says
    ([f"c=0:{upr(CTRR_C[1]):x}"], 4),
    # UPR below LWR: empty
    ([f"c={CTRR_C[1]:x}:{CTRR_C[0]:x}"], 4),
    # a UPR whose end wraps is no range
    ([f"c={CTRR_C[0]:x}:fffffffffffff000"], 4),
    # a chip without the registers: the read faults under the guard
    (["fault=C"], 4),
    (["fault=CU"], 4),
])
def test_unarmed_or_unreadable_changes_nothing(host, setting, reads):
    plain = run(host, IDENTITY)
    t = run(host, arm("c", *CTRR_C), *setting, IDENTITY)
    assert t.split_blocks(RAM, RAM_SIZE) == BELOW_BASE
    assert (t.blocks, t.page_runs, t.tables, t.log) == (plain.blocks, plain.page_runs, plain.tables, [])
    assert t.reads == [reads]


def test_a_faulting_ctrr_c_does_not_hide_ctrr_d(host):
    d = (RAM + 11 * BLOCK, RAM + 11 * BLOCK + PAGE)
    t = run(host, arm("c", *CTRR_C), "fault=C", arm("d", *d), IDENTITY)
    assert t.split_blocks(RAM, RAM_SIZE) == [*BELOW_BASE, RAM + 11 * BLOCK]
    assert t.log == [f"MMU: L3 pages over CTRR_D [0x{d[0]:x}, 0x{d[1]:x})"]
    t = run(host, arm("c", *CTRR_C), "fault=D", arm("d", *d), IDENTITY)
    assert t.split_blocks(RAM, RAM_SIZE) == [*BELOW_BASE, C_BLOCK]


def test_ranges_are_read_and_logged_once(host):
    t = run(host, arm("c", *CTRR_C), arm("d", RAM + 11 * BLOCK, RAM + 12 * BLOCK), IDENTITY,
            mapping(RAM | REGION_RW_EL0, RAM, RAM_SIZE, perms=PERM_RW_EL0),
            mapping(RAM, RAM, RAM_SIZE, perms=PERM_RW_EL0))
    # two registers for each range on the first L2 mapping, none after
    assert t.reads == [4, 4, 4]
    assert len(t.log) == 2
    # the later mappings were still split
    assert t.split_blocks(RAM | REGION_RW_EL0, RAM_SIZE) == [C_BLOCK | REGION_RW_EL0,
                                                           (RAM + 11 * BLOCK) | REGION_RW_EL0]


def test_remap_keeps_the_split_and_its_table(host):
    once = run(host, arm("c", *CTRR_C), IDENTITY)
    t = run(host, arm("c", *CTRR_C), IDENTITY, mapping(RAM, RAM, RAM_SIZE, perms=PERM_RW_EL0))
    assert t.split_blocks(RAM, RAM_SIZE) == [*BELOW_BASE, C_BLOCK]
    assert t.tables == once.tables
    pages = t.pages()
    for va in range(C_BLOCK, C_BLOCK + BLOCK, PAGE):
        assert pages[va] == (va, attrs(PERM_RW_EL0))


def test_unmapping_reads_no_register_and_leaves_no_pages(host):
    # mmu_rm_mapping() maps to 0 without PTE_VALID: nothing to split, so nothing to read
    t = run(host, arm("c", *CTRR_C), unmapping(RAM + 4 * BLOCK, 8 * BLOCK))
    assert t.reads == [0] and t.log == []
    assert (t.blocks, t.page_runs) == ({}, [])
    assert t.tables[3] == 0
    # unmapping the split block frees its L3 table (Tables checks that none is lost)
    t = run(host, arm("c", *CTRR_C), IDENTITY, unmapping(RAM + 4 * BLOCK, 8 * BLOCK))
    assert [t.level(RAM + n * BLOCK) for n in range(3, 13)] == [2] + [None] * 8 + [2]
    assert t.tables[3] == 3
    # an armed range at the low addresses an unmapping "targets" does not make L3 tables either
    t = run(host, arm("c", 5 * BLOCK, 5 * BLOCK + PAGE), IDENTITY,
            unmapping(RAM + 4 * BLOCK, 8 * BLOCK))
    assert t.tables[3] == 3


def test_firmware_ro_range_and_ctrr_split_together(host):
    ro = (RAM + 5 * BLOCK + 0x6c0000, RAM + 5 * BLOCK + 0x6cc000)
    d = (RAM + 9 * BLOCK, RAM + 9 * BLOCK + PAGE)
    t = run(host, f"ro={ro[0]:x}:{ro[1]:x}", arm("d", *d), IDENTITY)
    assert t.split_blocks(RAM, RAM_SIZE) == [*BELOW_BASE, RAM + 5 * BLOCK, RAM + 9 * BLOCK]
    assert t.log == [f"MMU: L3 pages over CTRR_D [0x{d[0]:x}, 0x{d[1]:x})",
                     f"MMU: splitting firmware RO block at 0x{RAM + 5 * BLOCK:x}"]
    # CTRR_C over the firmware RO range itself, as on the T8140: one split, said once
    t = run(host, f"ro={ro[0]:x}:{ro[1]:x}", arm("c", *ro), IDENTITY)
    assert t.split_blocks(RAM, RAM_SIZE) == [*BELOW_BASE, RAM + 5 * BLOCK]
    assert t.log == [f"MMU: L3 pages over CTRR_C [0x{ro[0]:x}, 0x{ro[1]:x})",
                     f"MMU: splitting firmware RO block at 0x{RAM + 5 * BLOCK:x}"]
    # the firmware RO range alone, with no CTRR register armed: only its block
    t = run(host, f"ro={ro[0]:x}:{ro[1]:x}", IDENTITY)
    assert t.split_blocks(RAM, RAM_SIZE) == [*BELOW_BASE, RAM + 5 * BLOCK]


def test_mmu_on_maps_pages_and_reads_nothing(host):
    # with the MMU on mmu_map() makes no L2 block at all, so there is nothing to decide
    t = run(host, arm("c", *CTRR_C), "mmu=1", mapping(RAM + 6 * BLOCK, RAM + 6 * BLOCK, 3 * BLOCK))
    assert t.blocks == {} and t.reads == [0] and t.log == []


def model_map(blocks, pages, armed, base, frm, to, size, attr):
    """mmu_map()'s rule with 16K pages and the MMU off, written from its description."""
    def l3(va, out, length):
        for off in range(0, length, PAGE):
            slot = (va + off) & -BLOCK
            if slot in blocks:
                block_out, block_attr = blocks.pop(slot)
                for page in range(0, BLOCK, PAGE):
                    pages[slot + page] = (block_out + page, block_attr | TYPE)
            if attr & VALID:
                pages[va + off] = (out + off, attr | TYPE)
            else:
                pages.pop(va + off, None)

    def l2(va, out):
        for page in range(0, BLOCK, PAGE):
            pages.pop(va + page, None)
        blocks.pop(va, None)
        if attr & VALID:
            blocks[va] = (out, attr)

    boundary = (frm + BLOCK - 1) & -BLOCK
    if RAM <= boundary < base:
        boundary = (base + BLOCK - 1) & -BLOCK
    head = min(size, boundary - frm)
    l3(frm, to, head)
    frm, to, size = frm + head, to + head, size - head
    middle = size & -BLOCK
    if middle and not to & (BLOCK - 1):
        for off in range(0, middle, BLOCK):
            out = to + off
            if attr & VALID and any(out < end and out + BLOCK > start for start, end in armed):
                l3(frm + off, out, BLOCK)
            else:
                l2(frm + off, out)
        frm, to, size = frm + middle, to + middle, size - middle
    l3(frm, to, size)


@pytest.mark.parametrize("seed", range(24))
def test_seeded_layouts_match_the_model(host, seed):
    rng = random.Random(0x411 + seed)
    ram_size = 12 * BLOCK
    base = RAM + rng.randrange(0, 3 * BLOCK, PAGE)
    armed, args = [], []
    for name in rng.sample(["c", "d"], rng.choice([1, 2, 2])):
        start = RAM + rng.randrange(0, ram_size, 0x1000)
        if rng.random() < 0.3:
            start &= -BLOCK
        end = start + rng.choice([0x1000, 3 * PAGE, BLOCK, rng.randrange(0x1000, 3 * BLOCK, 0x1000)])
        if rng.random() < 0.3:
            end = (end + BLOCK - 1) & -BLOCK
        armed.append((start, end))
        args.append(arm(name, start, end))
    blocks, pages = {}, {}
    perms = [PERM_RWX, PERM_RWX_EL0, PERM_RW_EL0, PERM_RX_EL0]
    for _ in range(rng.randrange(2, 7)):
        region = rng.choice([0, 0, REGION_RWX_EL0, REGION_RW_EL0, 0x20000000000])
        shift = rng.choice([0, 0, 0, BLOCK, 5 * BLOCK, 3 * PAGE])
        frm = RAM + rng.randrange(0, ram_size // 2, PAGE)
        if rng.random() < 0.6:
            frm &= -BLOCK
        size = min(rng.randrange(PAGE, ram_size, PAGE), ram_size - (frm - RAM))
        if rng.random() < 0.5:
            size = max(size & -BLOCK, BLOCK)
        if rng.random() < 0.2:
            args.append(unmapping(frm | region, size))
            model_map(blocks, pages, armed, base, frm | region, 0, size, 0)
            continue
        perm, mair = rng.choice(perms), rng.choice([0, 1])
        args.append(mapping(frm | region, frm + shift, size, mair, perm))
        model_map(blocks, pages, armed, base, frm | region, frm + shift, size,
                  attrs(perm, mair, level=2))
    t = run(host, *args, base=base)
    assert t.blocks == blocks
    assert t.pages() == pages
    # what the change is for: no L2 block's output holds a byte of an armed range
    for out, _ in t.blocks.values():
        assert not any(out < end and out + BLOCK > start for start, end in armed)
