/* SPDX-License-Identifier: MIT */
/* Failure-first production SGI routing replay; no physical IPI or guest. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#define MAX_CPUS 32
#define BIT(n)   (1ULL << (n))
static u16 num_cpus;
static u8 redist_cpu_ids[MAX_CPUS];
static int boot_cpu_idx, sender;
static bool alive[MAX_CPUS];
static u64 mpidrs[MAX_CPUS];
static unsigned kicks[MAX_CPUS], local_sync;
struct hv_gic_cpuif {
    bool valid;
    u32 pending[32];
};
static struct hv_gic_cpuif gic_cpuif[MAX_CPUS];
static int smp_id(void)
{
    return sender;
}
static bool smp_is_alive(int cpu)
{
    assert(cpu >= 0 && cpu < MAX_CPUS);
    return alive[cpu];
}
static u64 smp_get_mpidr(int cpu)
{
    assert(cpu >= 0 && cpu < MAX_CPUS);
    return mpidrs[cpu];
}
static void smp_send_ipi(int cpu)
{
    assert(cpu >= 0 && cpu < MAX_CPUS && cpu != sender);
    kicks[cpu]++;
}
static void hv_gic_cpuif_sync_vi(void)
{
    local_sync++;
}
#include "production_sgi.inc"
static void setup(const u8 *map, unsigned count, int boot, int self)
{
    memset(alive, 0, sizeof(alive));
    memset(gic_cpuif, 0, sizeof(gic_cpuif));
    memset(kicks, 0, sizeof(kicks));
    memset(redist_cpu_ids, 0xff, sizeof(redist_cpu_ids));
    memset(mpidrs, 0, sizeof(mpidrs));
    num_cpus = count;
    boot_cpu_idx = boot;
    sender = self;
    local_sync = 0;
    for (unsigned i = 0; i < count; i++) {
        int cpu = map[i];
        redist_cpu_ids[i] = cpu;
        if (cpu >= MAX_CPUS)
            continue;
        alive[cpu] = cpu != boot;
        gic_cpuif[cpu].valid = true;
        mpidrs[cpu] = cpu < 6 ? cpu : 0x100 + cpu - 6;
    }
}
static void check(const char *name, u64 expected, u32 intid)
{
    u64 pending = 0, ipis = 0;
    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        for (unsigned word = 0; word < 32; word++) {
            u32 want = (expected & BIT(cpu)) && word == intid / 32 ? (u32)BIT(intid % 32) : 0;
            if (gic_cpuif[cpu].pending[word] != want)
                fprintf(stderr, "%s: CPU %d pending[%u]=0x%x expected 0x%x\n", name, cpu, word,
                        gic_cpuif[cpu].pending[word], want);
            assert(gic_cpuif[cpu].pending[word] == want);
        }
        if (gic_cpuif[cpu].pending[intid / 32] & BIT(intid % 32))
            pending |= BIT(cpu);
        assert(kicks[cpu] == ((expected & BIT(cpu)) && cpu != sender ? 1u : 0u));
        if (kicks[cpu])
            ipis |= BIT(cpu);
    }
    assert(local_sync == ((expected & BIT(sender)) ? 1u : 0u));
    printf("{\"case\":\"%s\",\"pending\":%llu,\"ipi\":%llu,\"local_sync\":%u}\n", name,
           (unsigned long long)pending, (unsigned long long)ipis, local_sync);
}
int main(void)
{
    const u8 dense[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    const u8 sparse[] = {0, 1, 2, 3, 4, 5, 6, 8, 9, 10};
    setup(dense, 10, 6, 6);
    assert(smp_cpu_count() == 9);
    hv_gic_cpuif_send_sgi(BIT(40));
    check("nine-secondary-flags-ten-cpus", 0x3bf, 0);
    setup(dense, 10, 6, 9);
    hv_gic_cpuif_send_sgi(BIT(16) | 1);
    check("target-bsp-with-clear-alive", BIT(6), 0);
    setup(sparse, 10, 6, 6);
    hv_gic_cpuif_send_sgi(BIT(40) | (15ULL << 24));
    check("sparse-frames-reach-physical-ten", 0x73f, 15);
    setup(dense, 10, 6, 6);
    mpidrs[9] = (3ULL << 32) | (2ULL << 16) | (1ULL << 8) | 0x21;
    mpidrs[0] = mpidrs[9] ^ BIT(32);
    mpidrs[1] = mpidrs[9] ^ BIT(16);
    mpidrs[2] = mpidrs[9] ^ BIT(8);
    mpidrs[3] = mpidrs[9] ^ BIT(4);
    mpidrs[4] = mpidrs[9] ^ BIT(0);
    hv_gic_cpuif_send_sgi((3ULL << 48) | (2ULL << 44) | (2ULL << 32) | (7ULL << 24) | BIT(16) |
                          BIT(1));
    check("all-affinities-and-range-selector", BIT(9), 7);
    setup(dense, 10, 6, 6);
    hv_gic_cpuif_send_sgi(0);
    check("empty-target-list", 0, 0);
    setup(dense, 10, 6, 9);
    hv_gic_cpuif_send_sgi(BIT(16) | BIT(3));
    check("self-target-syncs-local-line", BIT(9), 0);
    setup(dense, 10, 6, 6);
    gic_cpuif[9].valid = false;
    hv_gic_cpuif_send_sgi(BIT(16) | BIT(3));
    check("uninitialized-interface", 0, 0);
    setup(dense, 10, 6, 6);
    alive[9] = false;
    hv_gic_cpuif_send_sgi(BIT(16) | BIT(3));
    check("dead-secondary", 0, 0);
    const u8 invalid[] = {6, 255};
    setup(invalid, 2, 6, 6);
    hv_gic_cpuif_send_sgi(BIT(40));
    check("invalid-frame", 0, 0);
    return 0;
}
