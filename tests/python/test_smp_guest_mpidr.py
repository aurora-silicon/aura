# SPDX-License-Identifier: MIT
"""smp_guest_mpidr() preserves ADT reg die bits on multi-die SoCs without breaking single-die."""

import re

from test_smp_timeouts import REPO, compile_and_run


def _extract_u64_fn(source, name):
    match = re.search(r"^(?:static )?u64 " + name + r"\([^;]*?\)\s*\{", source, re.M | re.S)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def test_smp_guest_mpidr_preserves_die_bits(tmp_path):
    smp = (REPO / "src/smp.c").read_text()
    guest_mpidr = _extract_u64_fn(smp, "smp_guest_mpidr")
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint8_t u8;

#define MAX_CPUS 32

struct cpu_info {
    bool valid;
    u8 die;
    u8 cluster;
    u8 core;
    u32 reg;
    u64 impl_reg;
};

static struct cpu_info cpu_info[MAX_CPUS];
static int cpu_nodes[MAX_CPUS];
static int boot_cpu_idx;
static u64 boot_cpu_mpidr;
static void *adt;
static const char *cluster_types[MAX_CPUS];

static const void *adt_getprop(void *tree, int node, const char *name, u32 *len)
{
    (void)tree;
    if (strcmp(name, "cluster-type") != 0 || node < 0 || node >= MAX_CPUS)
        return NULL;
    const char *t = cluster_types[node];
    if (!t)
        return NULL;
    if (len)
        *len = strlen(t) + 1;
    return t;
}
''' + guest_mpidr + r'''
static void set_cpu(int i, u32 reg, const char *ctype)
{
    cpu_info[i] = (struct cpu_info){
        .valid = true,
        .cluster = (reg >> 8) & 0x7,
        .core = reg & 0xff,
        .reg = reg,
    };
    cpu_nodes[i] = i;
    cluster_types[i] = ctype;
}

int main(void)
{
    /* 1. Single-die E/P SoC (e.g. M4/M5): boot E-core aff2=0, P-cluster aff2=1. */
    memset(cpu_info, 0, sizeof(cpu_info));
    boot_cpu_idx = 0;
    boot_cpu_mpidr = 0x000000;
    set_cpu(0, 0x0000, "E");
    set_cpu(1, 0x0003, "E");
    set_cpu(2, 0x0100, "P");
    set_cpu(3, 0x0202, "P");
    assert(smp_guest_mpidr(0) == 0x000000);
    assert(smp_guest_mpidr(1) == 0x000003);
    assert(smp_guest_mpidr(2) == 0x010100);
    assert(smp_guest_mpidr(3) == 0x010202);

    /* 2. M1/M2 Ultra: die 1 carries bit 11 (0x800) in ADT reg. */
    memset(cpu_info, 0, sizeof(cpu_info));
    boot_cpu_idx = 0;
    boot_cpu_mpidr = 0x000000;
    set_cpu(0, 0x0000, "E");
    set_cpu(1, 0x0100, "P");
    set_cpu(2, 0x0800, "E");
    set_cpu(3, 0x0900, "P");
    assert(smp_guest_mpidr(0) == 0x000000);
    assert(smp_guest_mpidr(1) == 0x010100);
    assert(smp_guest_mpidr(2) == 0x000800);
    assert(smp_guest_mpidr(3) == 0x010900);
    assert(smp_guest_mpidr(0) != smp_guest_mpidr(2));
    assert(smp_guest_mpidr(1) != smp_guest_mpidr(3));

    /* 3. T6050 M5 Ultra: die 1 carries bit 12 (0x1000); boot M-core has aff2=4, P-cores aff2=1. */
    memset(cpu_info, 0, sizeof(cpu_info));
    boot_cpu_idx = 0;
    boot_cpu_mpidr = 0x040000;
    /* Die 0: M cluster 0 (5 cores) + P clusters 1..2 (5 cores each) */
    for (int i = 0; i < 5; i++)
        set_cpu(i, 0x0000 | i, "M");
    for (int i = 0; i < 5; i++)
        set_cpu(5 + i, 0x0100 | i, "P");
    for (int i = 0; i < 5; i++)
        set_cpu(10 + i, 0x0200 | i, "P");
    /* Die 1: same clusters with bit 12 (0x1000) set */
    for (int i = 0; i < 5; i++)
        set_cpu(15 + i, 0x1000 | i, "M");
    for (int i = 0; i < 5; i++)
        set_cpu(20 + i, 0x1100 | i, "P");
    for (int i = 0; i < 5; i++)
        set_cpu(25 + i, 0x1200 | i, "P");

    assert(smp_guest_mpidr(0) == 0x040000);
    assert(smp_guest_mpidr(5) == 0x010100);
    assert(smp_guest_mpidr(15) == 0x041000);
    assert(smp_guest_mpidr(20) == 0x011100);
    assert(smp_guest_mpidr(29) == 0x011204);

    /* All 30 cores must map to distinct MPIDRs (no die-0/die-1 collision). */
    for (int i = 0; i < 30; i++)
        for (int j = i + 1; j < 30; j++)
            assert(smp_guest_mpidr(i) != smp_guest_mpidr(j));

    return 0;
}
'''
    compile_and_run(tmp_path, "smp-guest-mpidr", harness)
