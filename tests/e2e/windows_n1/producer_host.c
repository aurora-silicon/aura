/* Offline endpoint/ADT model. Written before the producer implementation. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define TYPES_H
#define UTILS_H
#define HV_H
#define PMGR_H
static bool hv_windows_can_prepare_device(void)
{
    return true;
}
#define __SMP_H__
static int model_cpu;
int boot_cpu_idx = 2;
static int smp_id(void)
{
    return model_cpu;
}
#define __EXCEPTION_H__
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#define BIT(x)        (1ULL << (x))
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define PACKED        __attribute__((packed))
#define UNUSED(x)     ((void)(x))
#define T8142         0x8142
enum exc_guard_t { GUARD_OFF, GUARD_SKIP, GUARD_MARK, GUARD_RETURN, GUARD_SILENT = 0x100 };
volatile enum exc_guard_t exc_guard;
volatile int exc_count;
static u64 model_DAIF, model_ISR_EL1;
#define mrs(reg) model_##reg
static bool fault_load, fault_barrier;
static void model_sysop(const char *op)
{
    if (fault_barrier && !strcmp(op, "dsb sy")) {
        assert(exc_guard == (GUARD_MARK | GUARD_SILENT));
        ++exc_count;
        fault_barrier = false;
    }
}
#define sysop(x) model_sysop(x)
u32 chip_id = 0x8142, board_id = 0x24;
void *adt;
static const char *scenario;
static unsigned trains, writes, reads;
static bool trained, held;
static bool powered, fabric_ready, validated;
static unsigned fabric_calls;
struct pmgr_gate {
    u32 entry;
    u16 id;
    u8 die;
    bool found, virt;
    const char *name;
    u64 psreg;
    u16 parent[2];
};
static const u16 gate_ids[] = {76, 74, 109, 110, 144, 152, 155, 156};
static const char *gate_names[] = {"ANS",          "APCIE_GP",     "APCIE_SYS_GP", "APCIE_ST",
                                   "APCIE_SYS_ST", "APCIE_PHY_SW", "ST0_AUS5_AON", "ST0_AUS5"};
static const u16 gate_parents[][2] = {{60, 0},   {60, 0},    {74, 0},  {76, 0},
                                      {76, 110}, {144, 109}, {152, 0}, {155, 0}};
static unsigned power_requests;
struct cell {
    u64 a;
    u32 v;
};
static struct cell cells[300];
static unsigned count;
static u32 get(u64 a)
{
    for (unsigned i = 0; i < count; i++)
        if (cells[i].a == a)
            return cells[i].v;
    return 0;
}
static void put(u64 a, u32 v)
{
    for (unsigned i = 0; i < count; i++)
        if (cells[i].a == a) {
            cells[i].v = v;
            return;
        }
    assert(count < 300);
    cells[count++] = (struct cell){a, v};
}
static bool is(const char *s)
{
    return !strcmp(scenario, s);
}
int pmgr_adt_gate(const char *path, const char *prop, u32 index, struct pmgr_gate *g)
{
    assert(!strcmp(path, "/arm-io/apcie") && !strcmp(prop, "clock-gates"));
    if (index >= 8)
        return -1;
    *g = (struct pmgr_gate){.entry = gate_ids[index],
                            .id = gate_ids[index],
                            .found = true,
                            .name = gate_names[index],
                            .psreg = 0x100000 + index * 4,
                            .parent = {gate_parents[index][0], gate_parents[index][1]}};
    if (is("cold-name") && index == 2)
        g->name = "other";
    if (is("cold-parent") && index == 2)
        g->parent[0]++;
    if (is("cold-alias") && index == 2)
        g->psreg = 0x100004;
    if (is("cold-unknown") && index == 2)
        g->found = false;
    return 0;
}
int pmgr_adt_power_enable(const char *path)
{
    assert(!strcmp(path, "/arm-io/apcie") && held && validated && !trains);
    power_requests++;
    powered = !is("power-enable") && !is("power-inactive") && !is("power-unknown");
    return is("power-enable") ? -1 : 0;
}
int pmgr_adt_is_powered(const char *path)
{
    assert(!strcmp(path, "/arm-io/apcie") && power_requests == 1);
    return is("power-unknown") ? -1 : powered;
}
static u32 read32(u64 a)
{
    if (a >= 0x100000 && a < 0x100020) {
        assert(!power_requests && !writes);
        if (a == 0x100004) {
            if (is("cold-reset"))
                return 1U << 31;
            if (is("cold-auto"))
                return 1U << 28;
            if (is("cold-target"))
                return 0xf;
        }
        return ((is("cold-active") || is("pre-bme")) && a == 0x100004) ? 0xff
               : (is("cold-clkgate") && a == 0x100004)                 ? 0x44
                                                                       : get(a);
    }
    if (a != 0x39a000300ULL) {
        assert(powered && fabric_ready);
        if (a < 0x488000000ULL || a >= 0x488020000ULL)
            assert(trained);
    }
    reads++;
    if (fault_load) {
        assert(exc_guard == (GUARD_MARK | GUARD_SILENT));
        exc_count++;
        return 0xabad1dea;
    }
    if (a == 0x1cb0100010ULL && get(a) == 0xffffffff)
        return is("bar-mask") ? 0xfffe0001 : 0xffff0004;
    if (a == 0x1cb0100014ULL && get(a) == 0xffffffff)
        return is("bar-high") ? 0xfffffffe : 0xffffffff;
    return get(a);
}
static u16 read16(u64 a)
{
    return read32(a & ~3ULL) >> ((a & 2) * 8);
}
static void write32(u64 a, u32 v)
{
    if (a != 0x39a000300ULL)
        assert(trained);
    writes++;
    if (is("bar-drop") && a == 0x1cb0100010ULL && v == 0x80000004)
        return;
    put(a, v);
    if (a == 0x39a000300ULL && (v & 15) == 2) {
        held = true;
        if (!is("clamp-fail")) {
            put(0x488028208ULL, 0);
            put(0x488028804ULL, 0);
            put(0x488028820ULL, 0);
            put(0x1cb0000728ULL, 0x14);
        }
    }
}
static void write16(u64 a, u16 v)
{
    u64 b = a & ~3ULL;
    unsigned sh = (a & 2) * 8;
    write32(b, (get(b) & ~(0xffffU << sh)) | ((u32)v << sh));
}
static void udelay(unsigned u)
{
    (void)u;
}
#include "j813_reset_provider.h"

#include "../../../src/adt.h"
static const char *paths[] = {"/",
                              "/chosen",
                              "/arm-io/apcie",
                              "/arm-io/apcie/pci-bridge0",
                              "/arm-io/dart-apcie0",
                              "/arm-io/gpio0"};
int adt_path_offset(const void *d, const char *p)
{
    (void)d;
    for (unsigned i = 0; i < ARRAY_SIZE(paths); i++)
        if (!strcmp(paths[i], p))
            return i;
    for (unsigned i = 0; i < ARRAY_SIZE(j813_reset_nodes); i++) {
        if (i == 1 && is("smc-nub-missing"))
            continue;
        if (i == 2 && (is("smc-pmu-missing") || is("smc-pmu-outside")))
            continue;
        if (!strcmp(j813_reset_nodes[i].path, p))
            return 6 + i;
    }
    if (is("smc-pmu-outside") && !strcmp(p, "/arm-io/other/smc-pmu"))
        return 99;
    return -1;
}
int adt_path_offset_trace(const void *d, const char *p, int *out)
{
    int n = adt_path_offset(d, p);
    out[0] = n;
    out[1] = -1;
    return n;
}
bool adt_is_compatible(const void *d, int n, const char *c)
{
    (void)d;
    if (n >= 6 && n <= 8) {
        if ((n == 6 && is("smc-controller-compatible")) || (n == 7 && is("smc-nub-compatible")) ||
            (n == 8 && is("smc-pmu-compatible")))
            return false;
        return !strcmp(c, j813_reset_nodes[n - 6].compatible);
    }
    return (n == 2 && !strcmp(c, "apcie,t8142")) || (n == 4 && !strcmp(c, "dart,t8110")) ||
           (n == 5 && !strcmp(c, "gpio,t8101"));
}
int adt_get_reg(const void *d, int *p, const char *prop, int i, u64 *a, u64 *z)
{
    (void)d;
    (void)prop;
    u64 b = 0, l = 0;
    if (p[0] == 2) {
        u64 bs[] = {0x1cb0000000, 0x48c000000, 0x48f000000, 0x48f020000,
                    0x48f040000,  0x48e000000, 0,           0,
                    0x488028000,  0,           0x48f010000};
        u64 ls[] = {0x10000000, 0x4000, 0x40000, 0x4000, 0x20000, 0x1000000,
                    0,          0,      0x8000,  0,      0x4000};
        if (i < 0 || i > 10)
            return -1;
        b = bs[i];
        l = ls[i];
    } else if (p[0] == 4) {
        b = 0x488000000;
        l = 0x20000;
    } else if (p[0] == 5) {
        b = 0x39a000000;
        l = 0x100000;
    } else
        return -1;
    if (is("resource") && p[0] == 4)
        b += 0x1000;
    *a = b;
    if (z)
        *z = l;
    return 0;
}
const void *adt_getprop(const void *d, int n, const char *key, u32 *len)
{
    (void)d;
    static u32 vals[16];
    memset(vals, 0, sizeof(vals));
    const void *r = vals;
    *len = 4;
    if (n == 2 && !strcmp(key, "clock-gates")) {
        *len = 32;
        for (unsigned i = 0; i < 8; i++)
            vals[i] = gate_ids[i];
    } else if (n == 0 && !strcmp(key, "model")) {
        r = is("model") ? "Mac17,4" : "Mac17,3";
        *len = 8;
    } else if (n == 1 && !strcmp(key, "chip-id"))
        vals[0] = chip_id;
    else if (n == 1 && !strcmp(key, "board-id"))
        vals[0] = board_id;
    else if (n == 5 && !strcmp(key, "AAPL,phandle"))
        vals[0] = 0xa0;
    else if (n >= 6 && n <= 8 && !strcmp(key, "AAPL,phandle"))
        vals[0] = j813_reset_nodes[n - 6].phandle;
    else if (n == 3 && !strcmp(key, "function-perst")) {
        *len = 16;
        vals[0] = 0xa0;
        vals[1] = 0x4750494f;
        vals[2] = is("perst") ? 193 : 192;
    } else if (n == 3 && !strcmp(key, "function-reset")) {
        *len = 16;
        memcpy(vals, j813_function_reset, sizeof(j813_function_reset));
        if (is("smc-controller-reset"))
            vals[0] = 0xd7;
        if (is("smc-reset-handle"))
            vals[0] = 0xff;
        if (is("smc-reset-op"))
            vals[2]++;
    } else if (n == 2 && !strcmp(key, "ranges")) {
        *len = 28;
        vals[0] = 0x02000000;
        vals[1] = 0x80000000;
        vals[3] = 0x80000000;
        vals[4] = 0xb;
        vals[5] = 0x40000000;
    } else {
        *len = 0;
        r = NULL;
    }
    return r;
}
int adt_getprop_copy(const void *d, int n, const char *k, void *out, size_t size)
{
    u32 len;
    const void *p = adt_getprop(d, n, k, &len);
    if (!p || len != size)
        return -1;
    memcpy(out, p, size);
    return len;
}
int j813_n1_validate_windows(void)
{
    validated = true;
    return is("tunables") ? -1 : 0;
}
int j813_n1_prepare_fabric_windows(void)
{
    fabric_calls++;
    assert(held && validated);
    if (pmgr_adt_power_enable("/arm-io/apcie") || pmgr_adt_is_powered("/arm-io/apcie") != 1 ||
        is("fabric"))
        return -1;
    fabric_ready = true;
    return 0;
}
int j813_n1_finish_windows(bool *ready)
{
    trains++;
    assert(fabric_ready && held);
    if (is("finish-early"))
        return -1;
    *ready = true;
    trained = true;
    held = false;
    put(0x488028208ULL, 0x1001);
    put(0x1cb0000000ULL, 0x100c106b);
    put(0x1cb0100000ULL, is("identity") ? 0x1234106b : 0x1900106b);
    put(0x1cb0101000ULL, 0xffff);
    put(0x1cb0102000ULL, 0xffff);
    put(0x1cb0100010ULL, 4);
    put(0x1cb0100004ULL, is("post-bme") ? 4 : 0);
    put(0xb80008000ULL, is("rom") ? 1 : 0);
    return is("train") || is("clamp-fail") ? -1 : 0;
}
#include "../../../src/hv/windows/windows_n1.c"
static void census_request(u64 address, u64 version, u64 reserved, bool success,
                           unsigned expected_reads)
{
    u64 regs[4] = {0x4e3152443332ULL, address, version, reserved};
    unsigned before_reads = reads, before_writes = writes, before_trains = trains;
    enum exc_guard_t before_guard = exc_guard;
    assert(hv_windows_n1_census_hvc(regs));
    assert(regs[0] == 0x4e3141434b31ULL && regs[3] == 1);
    assert(reads == before_reads + expected_reads);
    assert(writes == before_writes && trains == before_trains);
    assert(exc_guard == before_guard);
    if (success) {
        assert(regs[1] == 0 && regs[2] == get(address));
        assert(regs[2] <= UINT32_MAX);
    } else {
        assert((int64_t)regs[1] < 0 && regs[2] == 0);
    }
}

static void census_scenario(void)
{
    bool prepared = !is("census-unprepared") && !is("census-failed-prepare");
    if (!is("census-unprepared")) {
        u8 descriptor[128];
        if (is("census-failed-prepare"))
            scenario = "train";
        int result = hv_windows_n1_prepare(descriptor, sizeof(descriptor));
        if (prepared) {
            assert(result == 0 && trains == 1 && !held);
        } else {
            assert(result == -1 && held);
            for (unsigned i = 0; i < sizeof(descriptor); ++i)
                assert(descriptor[i] == 0);
            scenario = "census-failed-prepare";
        }
    }
    if (!prepared) {
        census_request(0x488028208ULL, 1, 0, false, 0);
    } else if (is("census-magic")) {
        u64 regs[4] = {0x4e3152443333ULL, 0x488028208ULL, 1, 0};
        u64 original[4];
        memcpy(original, regs, sizeof(regs));
        unsigned before_reads = reads, before_writes = writes;
        assert(!hv_windows_n1_census_hvc(regs));
        assert(!memcmp(original, regs, sizeof(regs)));
        assert(reads == before_reads && writes == before_writes);
    } else if (is("census-address")) {
        const u64 denied[] = {0,
                              0x488028204ULL,
                              0x1cb0100008ULL,
                              0x488000080ULL,
                              0xb80008004ULL,
                              0x488028209ULL,
                              0x1cb0200000ULL,
                              UINT64_MAX};
        for (unsigned i = 0; i < ARRAY_SIZE(denied); ++i)
            census_request(denied[i], 1, 0, false, 0);
    } else if (is("census-valid")) {
        const u64 allowed[] = {0x488028208ULL,  0x1cb0000000ULL, 0x1cb0100000ULL, 0x1cb0100004ULL,
                               0x1cb0100010ULL, 0x1cb0100014ULL, 0x1cb0101000ULL, 0x1cb0101004ULL,
                               0x1cb0102000ULL, 0x1cb0102004ULL, 0xb80008000ULL,  0x488000000ULL,
                               0x488000008ULL,  0x48800000cULL,  0x488000200ULL,  0x488001000ULL,
                               0x488001400ULL};
        put(0x1cb0101004ULL, 0xfedcba98); /* Must not sign-extend. */
        for (unsigned i = 0; i < ARRAY_SIZE(allowed); ++i)
            census_request(allowed[i], 1, 0, true, 1);
    } else {
        if (is("census-bsp"))
            model_cpu = 0; /* Boot CPU need not be CPU 0. */
        if (is("census-guard"))
            exc_guard = GUARD_RETURN;
        if (is("census-masked"))
            model_DAIF = 1U << 8;
        if (is("census-pending"))
            model_ISR_EL1 = 1U << 8;
        fault_load = is("census-load-fault");
        fault_barrier = is("census-barrier-fault");
        census_request(0x488028208ULL, is("census-version") ? 2 : 1, is("census-reserved") ? 1 : 0,
                       false, fault_load || fault_barrier ? 1 : 0);
    }
    printf("%s reads=%u writes=%u trains=%u PASS\n", scenario, reads, writes, trains);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    scenario = argv[1];
    model_cpu = boot_cpu_idx;
    if (is("chip"))
        chip_id = 0x8112;
    if (is("board"))
        board_id = 0x25;
    put(0x488000000ULL, 14U << 24);
    put(0x488000008ULL, (42U << 24) | (36U << 16) | 0x202);
    put(0x48800000cULL, 16);
    put(0x488000200ULL, is("locked") ? 1 : 0);
    put(0x488001000ULL, is("dart") ? 1 : 0);
    put(0x488000080ULL, is("busy") ? 0x80000000 : 0);
    put(0x39a000300ULL, 3);
    if (is("pre-bme")) {
        put(0x488028208ULL, 1);
        put(0x1cb0000000ULL, 0x100c106b);
        put(0x1cb0100000ULL, 0x1900106b);
        put(0x1cb0100004ULL, 4);
    }
    if (!strncmp(scenario, "census-", 7)) {
        census_scenario();
        return 0;
    }
    unsigned char result[128];
    memset(result, 0xaa, sizeof(result));
    int rc = hv_windows_n1_prepare(result, sizeof(result));
    bool success = is("success");
    printf("%s observed rc=%d reads=%u writes=%u trains=%u\n", scenario, rc, reads, writes, trains);
    fflush(stdout);
    if (success) {
        assert(rc == 0 && trains == 1 && !held);
        u32 magic;
        memcpy(&magic, result, 4);
        assert(magic == 0x4831574e);
        FILE *f = fopen("descriptor.bin", "wb");
        assert(f);
        assert(fwrite(result, 1, 128, f) == 128);
        fclose(f);
    } else {
        assert(rc != 0);
        for (unsigned i = 0; i < 128; i++)
            assert(!result[i]);
        if (trained)
            assert(held);
        else if (writes)
            assert(held && rc == -2);
        if (is("clamp-fail"))
            assert(rc == -2);
        if (!strncmp(scenario, "smc-", 4))
            assert(!reads && !writes && !trains);
        if (is("tunables") || !strncmp(scenario, "cold-", 5) || is("pre-bme"))
            assert(!writes && !power_requests && !fabric_calls && !trains);
    }
    printf("%s rc=%d trains=%u writes=%u held=%d PASS\n", scenario, rc, trains, writes, held);
}
