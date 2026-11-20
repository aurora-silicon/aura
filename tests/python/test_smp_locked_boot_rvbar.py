# SPDX-License-Identifier: MIT
"""A7 to M3: never store to a core's RVBAR while its lock bit is set.

smp_point_rvbar_here() runs the production body against modeled MMIO in which a
store to a register with RVBAR_LOCK set aborts (a T8122 boot core takes an SError
there, aurora-silicon/aura#231).
"""
from test_smp_timeouts import REPO, compile_and_run
from test_locked_dart_tables import function


def test_locked_rvbar_is_never_written(tmp_path):
    source = (REPO / "src/smp.c").read_text()
    bodies = "\n".join(function(source, name) for name in (
        "smp_rvbar_reaches_here", "smp_point_rvbar_here"))
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef unsigned long u64;
typedef uint32_t u32;
_Static_assert(sizeof(u64) == 8, "production u64 ABI");
#define MAX_CPUS 8
#define RVBAR_LOCK 1UL
#define RVBAR_ADDR 0x0000fffffffff000UL
#define RVBAR_ADDR_UNLOCKED 0x00000ffffffff000UL
struct cpu_info { bool valid; u64 impl_reg; };
static struct cpu_info cpu_info[MAX_CPUS];
static int boot_cpu_idx;
static u64 installed_relay, relocated_from;
/* Inside RVBAR's 43:12 field, as on the Mac; never dereferenced here. */
#define _vectors_start ((unsigned char *)0x804ef8000UL)
static char smp_s2r_why_buf[96];
static u64 rvbars[MAX_CPUS];
static unsigned writes;
static u64 old_vectors_start(void) { return 0; }
static bool smp_is_alive(int i) { (void)i; return true; }
static u64 read64(u64 a) { assert(a < MAX_CPUS); return rvbars[a]; }
static void write64(u64 a, u64 v)
{
    assert(a < MAX_CPUS);
    assert(!(rvbars[a] & RVBAR_LOCK)); /* the SError */
    rvbars[a] = v;
    writes++;
}
static u32 read32(u64 a) { (void)a; return 0; }
''' + bodies + r'''
static void setup(int boot, u64 boot_rvbar)
{
    memset(smp_s2r_why_buf, 0, sizeof(smp_s2r_why_buf));
    writes = 0;
    boot_cpu_idx = boot;
    for (int i = 0; i < MAX_CPUS; i++) {
        cpu_info[i] = (struct cpu_info){true, (u64)i};
        /* Secondaries: smp_start_cpu() already pointed them here, unlocked. */
        rvbars[i] = (u64)_vectors_start;
    }
    rvbars[boot] = boot_rvbar;
}
int main(void)
{
    u64 here = (u64)_vectors_start;
    for (int boot = 0; boot < MAX_CPUS; boot += 4) {
        /* Unlocked boot core (M1/M2 as tested): pointed here, mailbox published. */
        setup(boot, 0x800000000UL);
        assert(smp_point_rvbar_here());
        assert(writes == 1 && rvbars[boot] == here);

        /* Locked elsewhere (the T8122 shape): no store, no mailbox, a reason. */
        setup(boot, 0x800000000UL | RVBAR_LOCK);
        assert(!smp_point_rvbar_here());
        assert(writes == 0 && rvbars[boot] == (0x800000000UL | RVBAR_LOCK));
        assert(strstr(smp_s2r_why_buf, "is locked at 0x800000000"));

        /* Locked here already: no store, mailbox published. */
        setup(boot, here | RVBAR_LOCK);
        assert(smp_point_rvbar_here());
        assert(writes == 0);
    }
    puts("locked boot RVBAR PASS");
    return 0;
}
'''
    compile_and_run(tmp_path, "smp-locked-boot-rvbar", harness)
