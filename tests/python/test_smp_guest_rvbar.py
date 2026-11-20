# SPDX-License-Identifier: MIT
"""Only the stock-SPTM guest's reset path publishes an unlocked RVBAR.

The ordinary boot path (smp_prepare_rvbar, smp_start_cpu) is unchanged; the
guest helper is the production body with RVBAR MMIO modeled.
"""
from test_smp_timeouts import REPO, compile_and_run
from test_locked_dart_tables import function


def test_guest_rvbar_write_is_guest_only():
    source = (REPO / "src/smp.c").read_text()
    prepare = function(source, "smp_prepare_rvbar")
    # Normal boots: A7 to M3 leave the write to smp_start_cpu(), as before.
    assert "if (locked == target || cpu_features->apple_sysregs_unlocked)\n        return 0;" in prepare
    assert "write64" not in prepare
    guest = function(source, "smp_prepare_guest_resets")
    assert "smp_prepare_guest_rvbar(cpu)" in guest
    callers = [line for line in source.splitlines() if "smp_prepare_guest_rvbar(" in line]
    assert len(callers) == 2  # the definition and smp_prepare_guest_resets()


def test_guest_rvbar_publish(tmp_path):
    source = (REPO / "src/smp.c").read_text()
    body = function(source, "smp_prepare_guest_rvbar")
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef unsigned long u64;
#define RVBAR_LOCK 1UL
/* Userspace test symbols may sit above the hardware's 44-bit RVBAR range. */
#define RVBAR_ADDR_UNLOCKED 0x0000fffffffff000UL
#define sysop(x) do { } while (0)
struct cpu_info { u64 impl_reg; };
struct cpu_features { bool apple_sysregs_unlocked; };
static struct cpu_features features, *cpu_features = &features;
static unsigned char _vectors_start[4096] __attribute__((aligned(4096)));
static u64 rvbar, sticky;
static int writes, prepares;
static u64 read64(u64 a) { assert(a == 8); return rvbar; }
static void write64(u64 a, u64 v) { assert(a == 8); writes++; rvbar = sticky ? sticky : v; }
static int smp_prepare_rvbar(const struct cpu_info *cpu) { (void)cpu; prepares++; return 0; }
''' + body + r'''
int main(void)
{
    const struct cpu_info cpu = {.impl_reg = 8};
    u64 target = (u64)_vectors_start;

    /* A7 to M3, unlocked: publish and verify. */
    features.apple_sysregs_unlocked = true;
    rvbar = 0x800000000UL;
    assert(smp_prepare_guest_rvbar(&cpu) == 0 && writes == 1 && prepares == 0);
    assert(rvbar == target);
    /* Already published: no second write. */
    assert(smp_prepare_guest_rvbar(&cpu) == 0 && writes == 1 && prepares == 1);
    /* A lock bit is never written through. */
    rvbar = 0x800000000UL | RVBAR_LOCK;
    assert(smp_prepare_guest_rvbar(&cpu) == 0 && writes == 1 && prepares == 2);
    /* M4 on: the normal check/relay. */
    features.apple_sysregs_unlocked = false;
    rvbar = 0x800000000UL;
    assert(smp_prepare_guest_rvbar(&cpu) == 0 && writes == 1 && prepares == 3);
    /* A write that does not stick is refused. */
    features.apple_sysregs_unlocked = true;
    sticky = 0x900000000UL;
    assert(smp_prepare_guest_rvbar(&cpu) == -1 && writes == 2);
    return 0;
}
'''
    compile_and_run(tmp_path, "smp-guest-rvbar", harness)
