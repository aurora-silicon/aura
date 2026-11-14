# SPDX-License-Identifier: GPL-2.0-only
"""M3 family: smp_resets_return_here() leaves every RVBAR alone and publishes no S2R mailbox.

On a J613 (T8122) the boot core's store to its own RVBAR took an SError at EL2 and the Mac
boot-looped (smp_point_rvbar_here()). M1 and M2 keep pointing RVBAR here for the mailbox.
The production bodies run against modeled MMIO.
"""
import re

from test_smp_timeouts import REPO, compile_and_run


def body(source, name):
    match = re.search(r"^(?:static )?(?:bool |const char \*)" + name + r"\([^;]*?\)\s*\{",
                      source, re.M | re.S)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef unsigned long u64;
typedef uint32_t u32;
#define MAX_CPUS 8
#define T8103 0x8103
#define T8112 0x8112
#define T8122 0x8122
#define T6030 0x6030
#define T6031 0x6031
#define T6034 0x6034
#define T8142 0x8142
#define T8152 0x8152
#define RVBAR_LOCK 1UL
#define RVBAR_ADDR 0x0000fffffffff000UL
#define RVBAR_ADDR_UNLOCKED 0x00000ffffffff000UL
/* Only its address is used: a vectors address as aura runs at on a T8112 (d1301ea7). */
#define _vectors_start ((unsigned char *)0x804ef8000UL)
static int chip_id, boot_cpu_idx = 4;
static struct { bool apple_sysregs_unlocked; } features, *cpu_features = &features;
struct cpu_info { bool valid; u64 impl_reg; };
static struct cpu_info cpu_info[MAX_CPUS];
static u64 rvbar[MAX_CPUS];
static unsigned writes;
static bool smp_is_alive(int i) { return cpu_info[i].valid; }
static u64 read64(u64 addr) { return rvbar[addr]; }
static void write64(u64 addr, u64 value) { writes++; rvbar[addr] = value; }
static bool smp_prepare_rvbar(const struct cpu_info *cpu) { (void)cpu; return false; }
static bool smp_rvbar_reaches_here(u64 locked) { (void)locked; return true; }
static char smp_s2r_why_buf[96];
'''

MAIN = r'''
static bool run(int chip, bool unlocked)
{
    chip_id = chip;
    features.apple_sysregs_unlocked = unlocked;
    writes = 0;
    smp_s2r_why_buf[0] = 0;
    for (int i = 0; i < MAX_CPUS; i++) {
        cpu_info[i] = (struct cpu_info){.valid = true, .impl_reg = (u64)i};
        rvbar[i] = unlocked ? 0 : (RVBAR_LOCK | (u64)_vectors_start);
    }
    return smp_resets_return_here();
}

int main(void)
{
    /* M3 family: no RVBAR store, no mailbox, and the reason for the OS. */
    static const int m3[] = {T8122, T6030, T6031, T6034};
    for (unsigned i = 0; i < sizeof(m3) / sizeof(m3[0]); i++) {
        assert(!run(m3[i], true));
        assert(writes == 0);
        assert(!strcmp(smp_s2r_why(), "the boot core's RVBAR store faults on M3"));
    }

    /* M1 and M2 keep the mailbox: every live core's RVBAR is pointed here. */
    static const int m1m2[] = {T8103, T8112};
    for (unsigned i = 0; i < sizeof(m1m2) / sizeof(m1m2[0]); i++) {
        assert(run(m1m2[i], true));
        assert(writes == MAX_CPUS);
        for (int c = 0; c < MAX_CPUS; c++)
            assert((rvbar[c] & RVBAR_ADDR_UNLOCKED) == (u64)_vectors_start);
    }

    /* Locked SoCs are unchanged: no store, mailbox through the locked RVBAR. */
    assert(run(0x8140, false));
    assert(writes == 0);
    return 0;
}
'''


def test_m3_rvbar_is_left_alone(tmp_path):
    source = (REPO / "src/smp.c").read_text()
    code = HARNESS + "\n".join(body(source, name) for name in (
        "smp_s2r_why", "smp_point_rvbar_here", "smp_resets_return_here")) + MAIN
    compile_and_run(tmp_path, "smp-m3-rvbar", code)
