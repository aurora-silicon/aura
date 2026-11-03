# SPDX-License-Identifier: MIT
"""T6040: forward a reset vector locked at a Mach-O boot object's load base.

MMIO is modeled; smp_prepare_rvbar() is the production body.
"""
from test_smp_timeouts import compile_and_run, REPO
from test_locked_dart_tables import function


def test_t6040_load_base_rvbar_is_forwarded_once(tmp_path):
    current = function((REPO / "src/smp.c").read_text(), "smp_prepare_rvbar")
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef unsigned long u64;
typedef uint32_t u32;
_Static_assert(sizeof(u64) == 8, "production u64 ABI");
#define T8140 0x8140
#define T6040 0x6040
#define SZ_16K 0x4000UL
#define RVBAR_LOCK 1
#define RVBAR_ADDR 0x0000fffffffff000UL
#define MAIR_IDX_NORMAL 0
#define PERM_RW 0
#define PERM_RX 1
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#define sysop(op) ((void)(op))
struct cpu_info { u64 impl_reg; };
static struct { bool apple_sysregs_unlocked; } features, *cpu_features = &features;
static unsigned char _vectors_start[4096] __attribute__((aligned(4096)));
static int chip_id = T6040;
static u64 installed_relay, relocated_from, rvbar, locked;
static u32 word;
static int writes, mappings[2], nmap;
static u64 read64(u64 address) { assert(address == 8); return rvbar; }
static u32 read32(u64 address) { assert(address == locked); return word; }
static void write32(u64 address, u32 value) { assert(address == locked); word = value; writes++; }
static u64 old_vectors_start(void) { return 0; }
static bool smp_old_vector_owned(u64 address) { (void)address; return false; }
static bool mmu_active(void) { return true; }
static u64 get_page_size(void) { return 4096; }
static void mmu_add_mapping(u64 a, u64 b, u64 size, int attr, int perm)
{
    assert(a == locked && b == a && size == SZ_16K && attr == MAIR_IDX_NORMAL && nmap < 2);
    mappings[nmap++] = perm;
}
static void dc_cvac_range(void *address, u64 size) { assert((u64)address == locked && size == 4); }
static void ic_ivau_range(void *address, u64 size) { assert((u64)address == locked && size == 4); }
''' + current + r'''
int main(void)
{
    struct cpu_info cpu = {8};
    const u32 hop = 0x14000000U | (0x10000 / 4);

    /* Locked at the load base, which still holds the Mach-O header: forward once. */
    locked = (u64)_vectors_start - 0x10000;
    rvbar = locked | RVBAR_LOCK;
    word = 0xfeedfacf;
    assert(smp_prepare_rvbar(&cpu) == 0);
    assert(word == hop && writes == 1 && nmap == 2);
    assert(mappings[0] == PERM_RW && mappings[1] == PERM_RX);

    /* The next secondary finds the branch and writes nothing. */
    assert(smp_prepare_rvbar(&cpu) == 0 && writes == 1 && nmap == 2);

    /* Anything else at the load base is not ours to overwrite. */
    word = 0;
    nmap = 0;
    assert(smp_prepare_rvbar(&cpu) == -1 && writes == 1 && !nmap);

    /* Another distance, or another chip, is refused as before. */
    locked = (u64)_vectors_start - 0x20000;
    rvbar = locked | RVBAR_LOCK;
    word = 0xfeedfacf;
    assert(smp_prepare_rvbar(&cpu) == -1 && writes == 1);
    chip_id = 0x6041;
    locked = (u64)_vectors_start - 0x10000;
    rvbar = locked | RVBAR_LOCK;
    assert(smp_prepare_rvbar(&cpu) == -1 && writes == 1);
    puts("T6040 load-base RVBAR PASS");
    return 0;
}
'''
    compile_and_run(tmp_path, "smp-t6040-load-base", harness)
