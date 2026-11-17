/* SPDX-License-Identifier: MIT */
/*
 * Source-bound replay of the J813 E boot state (docs/t8142-ecpu-stepup.md).
 * The .inc files are the actual cpufreq.c setter/table code, the hv.c
 * configuration/latch/apply/hv_start prologue and the proxy.c cases, extracted
 * by tools/windows_ecpu_boot_e2e.py. Only registers, polling and the ADT are
 * modeled here.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define AURA_WINDOWS 1
typedef unsigned long u64;
typedef unsigned int u32;
#include "ecpu-defines.inc"

#define E_CMD 0x210e20020ul
#define P_CMD 0x211e20020ul
#define E_HANDOFF 0x400102ul
#define P_HANDOFF 0x40010aul
#define BUSY (1ul << 31)

static u32 chip_id = T8142, board_id = 0x24;
static int boot_cpu_idx = 6;

/* ---- modeled command registers ---- */
enum { F_NONE, F_BUSY_BEFORE, F_TIMEOUT, F_WRONG_ECHO, F_BIT, F_P_CHANGES };
static u64 cmd[2], written[32];
static unsigned reads, writes, polls, fault, fault_state;
static bool timeout_pending;

static unsigned reg_index(u64 address)
{
    assert(address == E_CMD || address == P_CMD);
    return address == P_CMD;
}
static u64 read64(u64 address)
{
    reads++;
    return cmd[reg_index(address)];
}
static void write64(u64 address, u64 value)
{
    assert(reg_index(address) == 0 && "P command register written");
    assert(writes < 32);
    written[writes++] = value;
    u32 state = value & 31;
    u64 settled = value & ~(1ul << 25);
    if (fault == F_WRONG_ECHO && state == fault_state)
        settled = (settled & ~31ul) | (state - 1);
    if (fault == F_BIT && state == fault_state)
        settled ^= 1ul << 22;
    if (fault == F_P_CHANGES && state == fault_state)
        cmd[1] ^= 1;
    cmd[0] = settled | BUSY; /* in transition until polled */
    timeout_pending = fault == F_TIMEOUT && state == fault_state;
}
static int poll64(u64 address, u64 mask, u64 target, u32 timeout)
{
    assert(reg_index(address) == 0 && mask == BUSY && target == 0 && timeout == 400);
    polls++;
    if (!(cmd[0] & BUSY)) /* before a write */
        return fault == F_BUSY_BEFORE && writes == fault_state - 3 ? -1 : 0;
    if (timeout_pending)
        return -1;
    cmd[0] &= ~BUSY;
    return 0;
}

/* ---- modeled ADT ---- */
static const char adt[] = "adt";
static u32 table[16], table_len;
static bool pmgr_missing;
static int adt_path_offset(const void *tree, const char *path)
{
    assert(tree == adt);
    return pmgr_missing || strcmp(path, "/arm-io/pmgr") ? -1 : 42;
}
static const void *adt_getprop(const void *tree, int node, const char *name, u32 *len)
{
    assert(tree == adt && node == 42);
    if (strcmp(name, "voltage-states1-sram"))
        return NULL;
    *len = table_len;
    return table;
}

#include "ecpu-cpufreq.inc"

/* ---- hypervisor state the extracted functions use ---- */
#define HV_WINDOWS_CPUFREQ BIT(5)
static bool hv_windows_initialized, hv_windows_ready, hv_windows_entered, hv_shutdown_failed;
static u64 hv_windows_flags;
static u32 hv_windows_ecpu_state, hv_windows_ecpu_khz;
static bool hv_windows_ecpu_attempted;
static unsigned quarantines;
static void hv_windows_quarantine(void)
{
    hv_windows_ready = false;
    quarantines++;
}
int hv_windows_configure_ecpu(u64 state, u64 khz);
bool hv_windows_cpufreq_owned(void);
int hv_start(void *entry, u64 regs[4]);
#include "ecpu-hv.inc"

/* ---- proxy dispatch ---- */
enum { P_CPUFREQ_INIT, P_CPUFREQ_GET_CLUSTER_HZ, P_CPUFREQ_SET_CLUSTER_PSTATE,
       P_HV_WINDOWS_ECPU_BOOT_STATE };
static unsigned host_requests;
static int cpufreq_init(void) { host_requests++; return 7; }
static int cpufreq_set_cluster_pstate(unsigned c, unsigned s) { (void)c; (void)s; host_requests++; return 8; }
static u64 cpufreq_get_cluster_hz(unsigned c) { (void)c; return 9; }
struct req { unsigned opcode; u64 args[2]; };
struct rep { u64 retval; };
static void dispatch(struct req *request, struct rep *reply)
{
    switch (request->opcode) {
#include "ecpu-proxy.inc"
    }
}

static const u32 ladder[8] = {972000, 1152000, 1584000, 1992000, 2352000, 2700000, 2964000, 3048000};
static unsigned checks;
#define CHECK(x) do { assert(x); checks++; } while (0)

static void reset(void)
{
    cmd[0] = E_HANDOFF;
    cmd[1] = P_HANDOFF;
    reads = writes = polls = fault = fault_state = quarantines = host_requests = 0;
    timeout_pending = pmgr_missing = false;
    for (unsigned i = 0; i < 8; i++) {
        table[2 * i] = ladder[i];
        table[2 * i + 1] = 800 + i;
    }
    table_len = sizeof(table);
    chip_id = T8142;
    board_id = 0x24;
    hv_windows_initialized = hv_windows_ready = hv_windows_entered = hv_shutdown_failed = false;
    hv_windows_flags = 0;
    hv_windows_ecpu_state = hv_windows_ecpu_khz = 0;
    hv_windows_ecpu_attempted = false;
}

/* hv_init() has run: configuration is closed and the session is ready. */
static int start(void)
{
    u64 r[4] = {0};
    hv_windows_initialized = hv_windows_ready = true;
    return hv_start(NULL, r);
}

static void raise_ok(u32 state)
{
    reset();
    CHECK(!hv_windows_configure_ecpu(state, ladder[state - 2]) && hv_windows_cpufreq_owned());
    CHECK(!reads && !writes); /* configuration reads only the ADT */
    CHECK(start() == 0 && hv_windows_entered && !quarantines);
    CHECK(writes == state - 2);
    for (u32 i = 0; i < writes; i++)
        CHECK(written[i] == ((E_HANDOFF & ~31ul) | (1ul << 25) | (3 + i)));
    CHECK(cmd[0] == ((E_HANDOFF & ~31ul) | state) && cmd[1] == P_HANDOFF);
    unsigned before = reads + writes;
    CHECK(start() == -1 && reads + writes == before); /* one session per resident */
    printf("{\"case\":\"raise-E%u\",\"writes\":%u,\"polls\":%u}\n", state, writes, polls);
}

static void apply_fails(const char *name, unsigned expected_writes)
{
    u64 p_before = cmd[1];
    CHECK(start() == -1);
    CHECK(!hv_windows_entered && !hv_windows_ready && quarantines == 1);
    CHECK(writes == expected_writes);
    CHECK(cmd[1] == p_before || fault == F_P_CHANGES); /* write64() also asserts P is never written */
    unsigned before = reads + writes;
    CHECK(start() == -1 && reads + writes == before); /* never retried on this resident */
    printf("{\"case\":\"%s\",\"writes\":%u,\"e\":\"0x%lx\"}\n", name, writes, cmd[0]);
}

int main(void)
{
    struct req q = {0};
    struct rep r = {0};

    /* Key absent: no configuration, no register access, guest entered. */
    reset();
    CHECK(start() == 0 && hv_windows_entered && !reads && !writes && !hv_windows_cpufreq_owned());
    q.opcode = P_CPUFREQ_INIT;
    dispatch(&q, &r);
    CHECK(r.retval == 7 && host_requests == 1);

    for (u32 state = 2; state <= 9; state++)
        raise_ok(state);

    /* The proxy request reaches the real configuration; host INIT/SET are then closed. */
    reset();
    q.opcode = P_HV_WINDOWS_ECPU_BOOT_STATE;
    q.args[0] = 5;
    q.args[1] = 2352000;
    dispatch(&q, &r);
    CHECK(r.retval == (u64)-1 && !hv_windows_ecpu_state);
    q.args[1] = 1992000;
    dispatch(&q, &r);
    CHECK(r.retval == 0 && hv_windows_ecpu_state == 5);
    q.opcode = P_CPUFREQ_INIT;
    dispatch(&q, &r);
    CHECK(r.retval == (u64)-1 && !host_requests);
    q.opcode = P_CPUFREQ_SET_CLUSTER_PSTATE;
    dispatch(&q, &r);
    CHECK(r.retval == (u64)-1 && !host_requests);

    /* Configuration rejections: nothing recorded, nothing read from the registers. */
    const u64 bad_states[] = {0, 1, 10, 31, 32, (1ul << 32) + 5};
    for (unsigned i = 0; i < sizeof(bad_states) / sizeof(bad_states[0]); i++) {
        reset();
        CHECK(hv_windows_configure_ecpu(bad_states[i], 1992000) == -1);
        CHECK(hv_windows_configure_ecpu(bad_states[i], 0) == -1 && !hv_windows_ecpu_state);
    }
    for (unsigned c = 0; c < 9; c++) {
        reset();
        if (c == 0) hv_windows_initialized = true;
        if (c == 1) CHECK(!hv_windows_configure_ecpu(3, 1152000));
        if (c == 2) chip_id = 0x8152;
        if (c == 3) board_id = 0x25;
        if (c == 4) table_len = 60;
        if (c == 5) { table[6] = table[4]; }
        if (c == 6) table[0] = 0;
        if (c == 7) pmgr_missing = true;
        if (c == 8) table_len = 31 * 8;
        u32 kept = hv_windows_ecpu_state;
        CHECK(hv_windows_configure_ecpu(5, 1992000) == -1 && hv_windows_ecpu_state == kept);
        CHECK(!reads && !writes);
    }

    /* Apply failures: guest entry refused, quarantined, never retried. */
    reset();
    CHECK(!hv_windows_configure_ecpu(5, 1992000));
    cmd[0] = E_HANDOFF + 1;
    apply_fails("e-not-handoff", 0);
    reset();
    CHECK(!hv_windows_configure_ecpu(5, 1992000));
    cmd[1] = P_HANDOFF + 1;
    apply_fails("p-not-handoff", 0);
    reset();
    CHECK(!hv_windows_configure_ecpu(5, 1992000));
    table[6] += 1; /* ADT entry changed after configuration */
    apply_fails("table-changed", 0);
    CHECK(!reads);
    struct { const char *name; unsigned fault, state, writes; } faults[] = {
        {"busy-before-E3", F_BUSY_BEFORE, 3, 0}, {"busy-before-E5", F_BUSY_BEFORE, 5, 2},
        {"timeout-after-E4", F_TIMEOUT, 4, 2}, {"wrong-echo-E3", F_WRONG_ECHO, 3, 1},
        {"unrelated-bit-E4", F_BIT, 4, 3}, {"p-changes-E3", F_P_CHANGES, 3, 3},
    };
    for (unsigned i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
        reset();
        CHECK(!hv_windows_configure_ecpu(5, 1992000));
        fault = faults[i].fault;
        fault_state = faults[i].state;
        apply_fails(faults[i].name, faults[i].writes);
    }

    /* Not ready: the readiness check precedes the apply, which is not consumed. */
    reset();
    CHECK(!hv_windows_configure_ecpu(5, 1992000));
    hv_windows_initialized = true;
    u64 r4[4] = {0};
    CHECK(hv_start(NULL, r4) == -1 && !reads && !writes && !hv_windows_ecpu_attempted);

    printf("{\"result\":\"PASS\",\"checks\":%u,\"hardware_qualified\":false}\n", checks);
    return 0;
}
