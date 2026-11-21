"""The payload-load P-cluster raise and the restore of firmware's state, on every chip's table.

cpufreq_payload_boost() raises each P cluster whose table holds a state above firmware's to that
state while payloads load and puts firmware's state back after. The boost code, the ladder check
and every chip's cluster table are the production ones; the register and the ADT are modelled.
"""
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def section(source, first, last):
    start = source.index(first)
    return source[start:source.index(last, start)]


def defines(source, code, prefix):
    names = set(re.findall(r'\b' + prefix + r'[A-Z0-9_]*\b', code))
    return '\n'.join(re.search(r'^#define ' + name + r'\s+[^\n]+', source, re.M)[0]
                     for name in sorted(names) if re.search(r'^#define ' + name + r'\s', source, re.M))


def test_payload_boost(tmp_path):
    source = (ROOT / 'src/cpufreq.c').read_text()
    cluster = section(source, 'struct cluster_t {', '};') + '};'
    tables = section(source, 'static const struct cluster_t s5l8960x_clusters[]',
                     'const struct cluster_t *cpufreq_get_clusters(void)')
    ladder = section(source, 'struct cluster_ladder {', 'static bool cluster_failed[')
    boost = section(source, '/*\n * Loading payloads', 'int cpufreq_init(void)')
    code = tables + ladder + boost
    macros = defines(source, code, 'CLUSTER_') + '\n' + defines(source, code, 'PMGR_VOLTAGE_CTL_OFF_')
    program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "soc.h"
typedef uint32_t u32;
typedef uint64_t u64;
#define BIT(n) (UINT64_C(1) << (n))
#define GENMASK(h, l) ((UINT64_MAX << (l)) & (UINT64_MAX >> (63 - (h))))
#define FIELD_GET(mask, value) (((value) & (mask)) >> __builtin_ctzll(mask))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define ADT_GETPROP(adt, node, name, val) (-1)
''' + macros + '\n' + cluster + r'''
static u32 chip_id;
static void *adt;

/* The PMGR ladder: `ladder` entries, in Hz, for every cluster's property. */
static unsigned ladder = 17;
static u32 opps[64];
static int adt_path_offset(void *tree, const char *path)
{ (void)tree; return strcmp(path, "/arm-io/pmgr") ? -1 : 1; }
static const void *adt_getprop(void *tree, int node, const char *name, u32 *len)
{
    (void)tree; (void)node;
    if (!strncmp(name, "cpu-perf-state0-index", 21) || !ladder)
        return NULL;
    assert(!strncmp(name, "voltage-states", 14));
    *len = ladder * 8;
    return opps;
}

/* Each cluster's request register: the P-state firmware left, and PS2 above it. */
static u64 regs[8];
static unsigned calls, last_state, last_ps2, last_cluster;
static bool fail, busy, ps2_chip, checked_chip, adt_mismatch;
static const struct cluster_t *chip_clusters(void);
static unsigned cluster_index(const struct cluster_t *c) { return c - chip_clusters(); }
static bool has_ps2(void) { return ps2_chip; }
static bool requests_checked(void) { return checked_chip; }
static bool cluster_matches_adt(const struct cluster_t *c) { (void)c; return !adt_mismatch; }
static u32 pstate_reg_to_pstate(u64 value) { return FIELD_GET(CLUSTER_PSTATE_DESIRED1, value); }
static u64 read64(u64 addr)
{
    for (const struct cluster_t *c = chip_clusters(); c->base; c++)
        if (addr == c->base + CLUSTER_PSTATE)
            return regs[cluster_index(c)];
    assert(0);
}
static int poll64(u64 addr, u64 mask, u64 value, u32 timeout)
{ (void)addr; (void)mask; (void)value; (void)timeout; return busy ? -1 : 0; }
static int set_pstate_ps2(const struct cluster_t *c, u32 state, u32 ps2)
{
    calls++; last_state = state; last_ps2 = ps2; last_cluster = cluster_index(c);
    if (fail)
        return -1;
    regs[last_cluster] = (regs[last_cluster] & ~CLUSTER_PSTATE_DESIRED1) | state;
    return 0;
}
''' + code + r'''
static void machine(u32 chip, bool ps2, bool checked)
{
    chip_id = chip; ps2_chip = ps2; checked_chip = checked;
    calls = 0; fail = busy = adt_mismatch = false; ladder = 17;
    memset(boost, 0, sizeof(boost));
    memset(regs, 0, sizeof(regs));
}
static void inherit(unsigned cluster, u32 state, u32 ps2)
{ regs[cluster] = state | ((u64)ps2 << __builtin_ctzll(CLUSTER_PSTATE_DESIRED2)); }

int main(void)
{
    for (unsigned i = 0; i < ARRAY_SIZE(opps) / 2; i++)
        opps[2 * i] = 600000000 + i * 100000000;

    /* T8140: the P cluster (index 1) is held at P8; its E cluster keeps firmware's state. */
    machine(T8140, false, true);
    inherit(0, 3, 0); inherit(1, 5, 0);
    cpufreq_payload_boost(true); assert(calls == 1 && last_cluster == 1 && last_state == 8);
    cpufreq_payload_boost(true); assert(calls == 1);
    cpufreq_payload_boost(false); assert(calls == 2 && last_cluster == 1 && last_state == 5);
    cpufreq_payload_boost(false); assert(calls == 2);
    assert(pstate_reg_to_pstate(regs[0]) == 3);
    inherit(1, 3, 0); fail = true;
    cpufreq_payload_boost(true); assert(calls == 3 && last_state == 8);
    fail = false; cpufreq_payload_boost(false); assert(calls == 4 && last_state == 3);

    /* A failed restore keeps firmware's state: the next raise does not save P8 as inherited,
     * and the next restore tries the original state again. */
    inherit(1, 4, 0);
    cpufreq_payload_boost(true); assert(calls == 5 && last_state == 8);
    fail = true; cpufreq_payload_boost(false); assert(calls == 6 && last_state == 4);
    fail = false; cpufreq_payload_boost(true); assert(calls == 6);
    cpufreq_payload_boost(false); assert(calls == 7 && last_state == 4);
    cpufreq_payload_boost(false); assert(calls == 7);

    /* The hold state must be on the ADT's ladder for the cluster, and so must the inherited one. */
    inherit(1, 2, 0); ladder = 7; cpufreq_payload_boost(true); assert(calls == 7);
    ladder = 0; cpufreq_payload_boost(true); assert(calls == 7);
    ladder = 17; inherit(1, 0, 0); cpufreq_payload_boost(true); assert(calls == 7);
    inherit(1, 18, 0); cpufreq_payload_boost(true); assert(calls == 7);
    cpufreq_payload_boost(false); assert(calls == 7);
    inherit(1, 17, 0); cpufreq_payload_boost(true); assert(calls == 7); /* already above P8 */
    inherit(1, 7, 0); cpufreq_payload_boost(true); assert(calls == 8 && last_state == 8);
    cpufreq_payload_boost(false); assert(calls == 9 && last_state == 7);

    /* Not touched: a busy cluster, or one the ADT does not place where the table does. */
    busy = true; cpufreq_payload_boost(true); assert(calls == 9);
    busy = false; adt_mismatch = true; cpufreq_payload_boost(true); assert(calls == 9);

    /* T6000, by its own table (no ADT ladder named): both P clusters, PS2 left as found. */
    machine(T6000, true, false);
    inherit(0, 2, 2); inherit(1, 3, 9); inherit(2, 1, 4);
    cpufreq_payload_boost(true);
    assert(calls == 2 && last_cluster == 2 && last_state == 7 && last_ps2 == 4);
    assert(pstate_reg_to_pstate(regs[0]) == 2 && pstate_reg_to_pstate(regs[1]) == 7);
    cpufreq_payload_boost(false);
    assert(calls == 4 && last_cluster == 2 && last_state == 1 && last_ps2 == 4);
    assert(pstate_reg_to_pstate(regs[1]) == 3 && pstate_reg_to_pstate(regs[2]) == 1);

    /* T8112 and T8122 raise their P cluster to the table's hold state too. */
    machine(T8112, false, false);
    inherit(1, 1, 0); cpufreq_payload_boost(true); assert(calls == 1 && last_cluster == 1 && last_state == 6);
    cpufreq_payload_boost(false); assert(calls == 2 && last_state == 1);
    machine(T8122, false, false);
    inherit(1, 2, 0); cpufreq_payload_boost(true); assert(calls == 1 && last_state == 6);

    /* T6040/T6041 and T8152 keep firmware's state: their tables hold none. */
    const u32 kept[] = {T6040, T6041, T8152};
    for (unsigned i = 0; i < ARRAY_SIZE(kept); i++) {
        machine(kept[i], false, true);
        inherit(1, 1, 0); inherit(2, 1, 0);
        cpufreq_payload_boost(true); cpufreq_payload_boost(false); assert(!calls);
    }

    /* Before T8103 the table programs PMGR voltage control: no raise ahead of cpufreq_init(). */
    const u32 early[] = {S5L8960X, T7000, S8000, T8010, T8015};
    for (unsigned i = 0; i < ARRAY_SIZE(early); i++) {
        machine(early[i], true, false);
        cpufreq_payload_boost(true); cpufreq_payload_boost(false); assert(!calls);
    }

    /* No table: nothing. */
    machine(0x1234, false, false); cpufreq_payload_boost(true); assert(!calls);
    puts("payload boost and inherited-state restoration passed");
}
'''
    file = tmp_path / 'boost.c'
    file.write_text(program)
    binary = tmp_path / 'boost'
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                    '-Wno-missing-field-initializers', '-I', str(ROOT / 'src'), str(file), '-o',
                    str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_checked_pstate_request_waits_and_checks(tmp_path):
    source = (ROOT / 'src/cpufreq.c').read_text()
    start = source.index('static u32 pstate_reg_to_pstate(u64 val)')
    end = source.index('int cpufreq_init_cluster(', start)
    functions = source[start:end]
    names = set(re.findall(r'\bCLUSTER_[A-Z0-9_]+\b', functions))
    macros = '\n'.join(re.search(r'^#define ' + name + r'\s+[^\n]+', source, re.M)[0]
                       for name in sorted(names))
    program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "soc.h"
typedef uint32_t u32;
typedef uint64_t u64;
#define BIT(n) (UINT64_C(1) << (n))
#define GENMASK(h, l) ((UINT64_MAX << (l)) & (UINT64_MAX >> (63 - (h))))
#define FIELD_GET(mask, value) (((value) & (mask)) >> __builtin_ctzll(mask))
#define FIELD_PREP(mask, value) (((u64)(value) << __builtin_ctzll(mask)) & (mask))
struct cluster_t { const char *name; u64 base; bool pcluster; u32 apsc_pstate, default_pstate, voltage_ctl; };
''' + macros + r'''
static u32 chip_id = T8140;
static u64 reg, writes;
static bool busy_before, stuck, refuse;
static u64 read64(u64 addr) { (void)addr; return reg; }
static void write64(u64 addr, u64 value)
{
    (void)addr;
    writes++;
    /* A refused request leaves the desired state where it was. */
    reg = refuse ? (reg & ~CLUSTER_PSTATE_SET) : (value & ~CLUSTER_PSTATE_SET);
    if (stuck)
        reg |= CLUSTER_PSTATE_BUSY;
}
static int poll64(u64 addr, u64 mask, u64 target, u32 timeout)
{
    (void)addr; (void)target; (void)timeout;
    if (busy_before) return -1;
    return (reg & mask) ? -1 : 0;
}
''' + functions + r'''
int main(void)
{
    const struct cluster_t c = {"PCPU", 0x211e00000, true, 0, 0, 0};
    reg = 1;
    assert(!set_pstate(&c, 8) && writes == 1 && pstate_reg_to_pstate(reg) == 8);
    /* Busy before the request: nothing is written. */
    busy_before = true;
    assert(set_pstate(&c, 1) == -1 && writes == 1);
    busy_before = false;
    /* A request the cluster does not take is reported. */
    refuse = true;
    assert(set_pstate(&c, 2) == -1 && writes == 2 && pstate_reg_to_pstate(reg) == 8);
    refuse = false;
    stuck = true;
    assert(set_pstate(&c, 3) == -1);
    puts("T8140 P-state request protocol passed");
    return 0;
}
'''
    file = tmp_path / 'request.c'
    file.write_text(program)
    binary = tmp_path / 'request'
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-I',
                    str(ROOT / 'src'), str(file), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
