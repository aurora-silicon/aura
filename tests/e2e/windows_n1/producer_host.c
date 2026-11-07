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
static bool acquiring;
static unsigned dart_writes, tlbis, unlocks, gate_writes;
static bool global_enabled;
static unsigned trains, writes, reads;
static unsigned dart_reads, endpoint_command_reads;
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
static const u64 model_pm_addresses[] = {
    0x380700300ULL, 0x3807002f0ULL, 0x380700408ULL, 0x380700410ULL, 0x380700520ULL, 0x380700560ULL,
    0x380700578ULL, 0x380700580ULL, 0x380700518ULL, 0x380700568ULL, 0x380000000ULL};
static bool model_pm_address(u64 a)
{
    for (unsigned i = 0; i < ARRAY_SIZE(model_pm_addresses); i++)
        if (a == model_pm_addresses[i])
            return true;
    return false;
}
static unsigned power_requests;
struct cell {
    u64 a;
    u32 v;
};
static struct cell cells[512];
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
    assert(count < 512);
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
                            .psreg = model_pm_addresses[index],
                            .parent = {gate_parents[index][0], gate_parents[index][1]}};
    if (is("acquire-pmgr-address") && index == 2)
        g->psreg += 4;
    if (is("cold-name") && index == 2)
        g->name = "other";
    if (is("cold-parent") && index == 2)
        g->parent[0]++;
    if (is("cold-alias") && index == 2)
        g->psreg = 0x3807002f0ULL;
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
int pmgr_device_count(void)
{
    return 15;
}
int pmgr_device_at(u32 index, struct pmgr_gate *g)
{
    static const u16 ids[] = {143, 153, 338, 339, 340, 341, 60};
    static const char *names[] = {"SYS_GP_OCLA", "SYS_ST_OCLA",    "APCIE-GP-V", "APCIE-SYS-GP-V",
                                  "APCIE-ST-V",  "APCIE-SYS-ST-V", "ROOT"};
    static const u16 parents[] = {109, 144, 74, 109, 110, 144, 0};
    if (index < 8)
        return pmgr_adt_gate("/arm-io/apcie", "clock-gates", index, g);
    if (index >= 15)
        return -1;
    unsigned j = index - 8;
    *g = (struct pmgr_gate){.entry = ids[j],
                            .id = ids[j],
                            .found = true,
                            .name = names[j],
                            .virt = j >= 2 && j <= 5,
                            .psreg = j < 2 ? model_pm_addresses[8 + j] : model_pm_addresses[10],
                            .parent = {parents[j], 0}};
    if (is("acquire-pmgr-descendant") && j == 6)
        g->parent[0] = 155;
    if (is("acquire-pmgr-virtual") && j == 5)
        g->parent[0] = 60;
    if (is("acquire-pmgr-duplicate") && j == 6)
        g->id = g->entry = 74;
    return 0;
}
int pmgr_device_gate(u8 die, u16 id, struct pmgr_gate *g)
{
    if (die)
        return -1;
    for (unsigned i = 0; i < 15; i++) {
        pmgr_device_at(i, g);
        if (g->id == id)
            return 0;
    }
    return -1;
}
int pmgr_set_mode(uintptr_t a, u8 mode)
{
    assert(acquiring && held && validated && mode == 15 && a >= 0x3807002f0ULL &&
           a <= 0x380700580ULL);
    gate_writes++;
    writes++;
    put(a, 0xff);
    if (is("acquire-protected-change"))
        put(0x380700300ULL, 0xff);
    return is("acquire-power-failure") ? -1 : 0;
}
static u32 read32(u64 a)
{
    if (model_pm_address(a)) {
        if (!acquiring)
            assert(!power_requests && !writes);
        if (a == 0x3807002f0ULL) {
            if (is("cold-reset"))
                return 1U << 31;
            if (is("cold-auto"))
                return 1U << 28;
            if (is("cold-target"))
                return 0xf;
        }
        return ((is("cold-active") || is("pre-bme")) && a == 0x3807002f0ULL) ? 0xff
               : (is("cold-clkgate") && a == 0x3807002f0ULL)                 ? 0x44
                                                                             : get(a);
    }
    if (a != 0x39a000300ULL) {
        assert(powered && fabric_ready && trained);
        if (a >= 0x488000000ULL && a < 0x488020000ULL) {
            assert(endpoint_command_reads && !held);
            if (is("acquire-version") || is("acquire-stream-count") || is("acquire-geometry"))
                assert(a == 0x488000000ULL || a == 0x488000008ULL || a == 0x48800000cULL);
            dart_reads++;
            if (is("dart-load-fault")) {
                assert(dart_reads == 1 && exc_guard == (GUARD_MARK | GUARD_SILENT));
                exc_count++;
                return 0xabad1dea;
            }
            if (is("post-dart-active") && a == 0x488001400ULL && get(0x1cb0100010ULL) == 0x80000004)
                return 1;
        }
        if (a == 0x1cb0100004ULL || a == 0x1cb0101004ULL || a == 0x1cb0102004ULL)
            endpoint_command_reads++;
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
    if (acquiring && a >= 0x488000000ULL && a < 0x488020000ULL) {
        assert(trained && endpoint_command_reads && !held);
        dart_writes++;
        if (a >= 0x488001000ULL && a < 0x48800104cULL) {
            assert(v == 0 && !global_enabled);
            if (is("acquire-disable-drop"))
                return;
        }
        if (a >= 0x488001400ULL && a < 0x48800144cULL) {
            assert(v == 0 && !global_enabled);
            if (is("acquire-root-drop"))
                return;
        }
        if (a == 0x488000c00ULL) {
            assert(v == 0xffffffffU);
            for (unsigned i = 0; i < 19; i++)
                assert(!get(0x488001000ULL + 4 * i) && !get(0x488001400ULL + 4 * i));
            global_enabled = true;
            return; /* W1S reads zero. */
        }
        if (a == 0x488004000ULL) {
            assert(v == get(a) && global_enabled);
            if (!is("acquire-exception-persistent"))
                put(a, 0);
            return;
        }
        if (a == 0x488000104ULL) {
            assert(v == 0 && global_enabled);
            if (is("acquire-mask-drop"))
                return;
        }
        if (a == 0x488000080ULL) {
            assert(global_enabled);
            if (v == 0x400) {
                unlocks++;
                put(a, is("acquire-unlock-timeout") ? 0x80000000 : 0);
                return;
            }
            assert(v == (0x8100 | tlbis) && tlbis < 19);
            assert(!get(0x488000104ULL));
            tlbis++;
            put(a, is("acquire-command-timeout") ? 0x80000000 : 0);
            return;
        }
    }
    if (acquiring && is("acquire-final-dirty") && a == 0x1cb0100010ULL && v == 0x80000004)
        put(0x488001448ULL, 1);
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
static u64 model_ticks;
static u64 timeout_calculate(u32 us)
{
    return model_ticks + us;
}
static bool timeout_expired(u64 deadline)
{
    return ++model_ticks > deadline;
}
static void udelay(unsigned u)
{
    (void)u;
}
#include "j813_reset_provider.h"

#include "../../../src/adt.h"
static int offsets[32];
static bool fixture_ready;
static u8 fixture[32768];
static u32 fixture_size;
static int node_index(int n)
{
    if (!fixture_ready)
        return n;
    for (unsigned i = 0; i < 32; i++)
        if (offsets[i] == n)
            return i;
    return -1;
}
u32 adt_get_size(void)
{
    return fixture_size;
}
static const char *paths[] = {"/",
                              "/chosen",
                              "/arm-io/apcie",
                              "/arm-io/apcie/pci-bridge0",
                              "/arm-io/dart-apcie0",
                              "/arm-io/gpio0",
                              "/arm-io/smc",
                              "/arm-io/smc/iop-smc-nub",
                              "/arm-io/smc/iop-smc-nub/smc-pmu",
                              "/arm-io",
                              "/arm-io/dart-apcie0/mapper-apcie0-control",
                              "/arm-io/dart-apcie0/mapper-apcie0-alpha",
                              "/arm-io/dart-apcie0/mapper-apcie0-beta",
                              "/arm-io/dart-apcie0/mapper-apcie0-piodma",
                              "/arm-io/apcie/pci-bridge0/centauri-control",
                              "/arm-io/apcie/pci-bridge0/centauri-alpha",
                              "/arm-io/apcie/pci-bridge0/centauri-beta",
                              "/arm-io/apcie0-piodma",
                              "/arm-io/other"};
int adt_path_offset(const void *d, const char *p)
{
    (void)d;
    for (unsigned i = 0; i < ARRAY_SIZE(paths); i++) {
        if (i >= 6 && i <= 8 && !acquiring)
            continue;
        if (acquiring && is("acquire-mapper-missing") && i == 11)
            continue;
        if (!strcmp(paths[i], p))
            return fixture_ready ? offsets[i] : (int)i;
    }
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
    n = node_index(n);
    if (acquiring && n >= 10 && n <= 13)
        return !strcmp(c, "iommu-mapper");
    if (acquiring && n == 17)
        return !strcmp(c, "pcie-apiodma,t6020");
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
    int mapped[2] = {node_index(p[0]), -1};
    p = mapped;
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
    n = node_index(n);
    static u32 vals[16];
    memset(vals, 0, sizeof(vals));
    const void *r = vals;
    *len = 4;
    if (acquiring && n >= 10 && n <= 17 && !strcmp(key, "AAPL,phandle")) {
        vals[0] = 0xa100 + n;
        if (is("acquire-duplicate-phandle") && n == 11)
            vals[0] = 0xa10a;
    } else if (acquiring && n == 4 && !strcmp(key, "AAPL,phandle"))
        vals[0] = 0xa104;
    else if (acquiring && n == 4 && !strcmp(key, "sid-count"))
        vals[0] = 19;
    else if (acquiring && n == 4 && !strcmp(key, "sid")) {
        *len = 12;
        vals[0] = 16;
        vals[1] = 17;
        vals[2] = 18;
    } else if (acquiring && n == 2 && !strcmp(key, "first-bypass-sid"))
        vals[0] = 16;
    else if (acquiring && n == 3 && !strcmp(key, "apcie-piodma"))
        vals[0] = 0xa111;
    else if (acquiring && n == 3 && !strcmp(key, "apcie-piodma-sid"))
        vals[0] = is("acquire-piodma-sid") ? 18 : 17;
    else if (acquiring && n == 3 && !strncmp(key, "function-dart_", 14)) {
        *len = 8;
        vals[0] = is("acquire-dart-function") ? 0xdead : 0xa104;
        vals[1] = !strcmp(key, "function-dart_force_active")  ? 0x46616374
                  : !strcmp(key, "function-dart_release_sid") ? 0x5352656c
                  : !strcmp(key, "function-dart_request_sid") ? 0x53526571
                                                              : 0x53656c66;
    } else if (acquiring && n >= 14 && n <= 17 && !strcmp(key, "iommu-parent"))
        vals[0] = 0xa100 + n - 4;
    else if (acquiring && n == 13 && !strcmp(key, "reg"))
        vals[0] = 17;
    else if (acquiring && n == 18 && !strcmp(key, "iommu-parent") && is("acquire-extra-consumer"))
        vals[0] = 0xa10a;
    else if (acquiring && n == 18 &&
             ((!strcmp(key, "clock-gates") && is("acquire-clock-consumer")) ||
              (!strcmp(key, "power-gates") && is("acquire-power-consumer"))))
        vals[0] = 155;
    else if (n == 2 &&
             (!strcmp(key, "clock-gates") || (acquiring && !strcmp(key, "power-gates")))) {
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
static void fixture_node(int index)
{
    static const int parents[] = {-1, 0, 9, 2, 9, 9, 9, 6, 7, 0, 4, 4, 4, 4, 3, 3, 3, 9, 9, 1, 9};
    static const char *keys[] = {"model",
                                 "chip-id",
                                 "board-id",
                                 "clock-gates",
                                 "power-gates",
                                 "AAPL,phandle",
                                 "sid-count",
                                 "sid",
                                 "first-bypass-sid",
                                 "apcie-piodma",
                                 "apcie-piodma-sid",
                                 "function-dart_force_active",
                                 "function-dart_release_sid",
                                 "function-dart_request_sid",
                                 "function-dart_self",
                                 "iommu-parent",
                                 "reg"};
    int start = fixture_size;
    offsets[index] = start;
    struct adt_node_hdr h = {0, 0};
    fixture_size += sizeof(h);
    for (unsigned k = 0; k < ARRAY_SIZE(keys); k++) {
        u32 len;
        const void *v = adt_getprop(NULL, index, keys[k], &len);
        if (!v)
            continue;
        struct adt_property *p = (void *)(fixture + fixture_size);
        memset(p, 0, sizeof(*p));
        strcpy(p->name, keys[k]);
        p->size = len;
        memcpy(p->value, v, len);
        fixture_size += sizeof(*p) + ((len + 3) & ~3U);
        h.property_count++;
    }
    bool large_adt = is("acquire-adt-large") || is("acquire-adt-large-duplicate") ||
                     is("acquire-adt-property-limit");
    if (large_adt && index >= 19) {
        unsigned properties = index == 19 ? (is("acquire-adt-property-limit") ? 257 : 226) : 152;
        for (unsigned i = 0; i < properties; i++) {
            assert(fixture_size + sizeof(struct adt_property) + 4 <= sizeof(fixture));
            struct adt_property *p = (void *)(fixture + fixture_size);
            memset(p, 0, sizeof(*p) + 4);
            unsigned key = is("acquire-adt-large-duplicate") && index == 19 && i == 225 ? 1 : i;
            snprintf(p->name, sizeof(p->name), "sample%03u", key);
            p->size = 4;
            fixture_size += sizeof(*p) + 4;
            h.property_count++;
        }
    }
    for (unsigned i = 1; i < ARRAY_SIZE(parents); i++) {
        if (i >= 19 && !large_adt)
            continue;
        int parent = parents[i];
        if (is("acquire-extra-mapper") && i == 18)
            parent = 4;
        if (parent == index) {
            h.child_count++;
            fixture_node(i);
        }
    }
    memcpy(fixture + start, &h, sizeof(h));
}
static void acquire_fixture(void)
{
    for (unsigned i = 0; i < ARRAY_SIZE(offsets); i++)
        offsets[i] = -1;
    fixture_node(0);
    adt = fixture;
    fixture_ready = true;
    if (is("acquire-adt-malformed"))
        ((struct adt_node_hdr *)adt)->property_count = UINT32_MAX;
}
int j813_n1_prepare_fabric_acquired_windows(void)
{
    assert(acquiring && gate_writes == 7 && held && validated && !power_requests);
    powered = true;
    fabric_ready = true;
    fabric_calls++;
    return 0;
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
    put(0x1cb0101000ULL, is("secondary-bme") ? 0x1902106b : 0xffff);
    put(0x1cb0102000ULL, is("tertiary-bme") ? 0x1903106b : 0xffff);
    put(0x1cb0101004ULL, is("secondary-bme") ? 4 : 0);
    put(0x1cb0102004ULL, is("tertiary-bme") ? 4 : 0);
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
    setbuf(stdout, NULL);
    assert(argc == 2);
    scenario = argv[1];
    model_cpu = boot_cpu_idx;
    acquiring = !strncmp(scenario, "acquire-", 8);
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
    if (acquiring) {
        acquire_fixture();
        if (is("acquire-captured-params"))
            put(0x488000000ULL, 0x9e313020);
        put(0x488000008ULL, 0x2a2a0204);
        put(0x48800000cULL, 0x11013);
        put(0x488000104ULL, 0xffffffffU);
        for (unsigned sid = 0; sid < 19; sid++) {
            put(0x488001000ULL + 4 * sid, 1);
            put(0x488001400ULL + 4 * sid, 0x1400e000 + sid * 4);
        }
        if (is("acquire-version"))
            put(0x488000008ULL, 0x2a2a0201);
        if (is("acquire-stream-count"))
            put(0x48800000cULL, 18);
        if (is("acquire-geometry"))
            put(0x488000000ULL, 12U << 24);
        if (is("acquire-protect"))
            put(0x488000200ULL, 1);
        if (is("acquire-busy"))
            put(0x488000080ULL, 0x80000000);
        if (is("acquire-tcr-bypass"))
            put(0x488001044ULL, 6);
        if (is("acquire-valid-root"))
            put(0x488001444ULL, 0x1400e001);
        if (is("acquire-unlock") || is("acquire-unlock-timeout"))
            put(0x488000210ULL, 2);
        if (is("acquire-stale") || is("acquire-exception-persistent"))
            put(0x488004000ULL, 0x20001);
        if (is("acquire-exception-reserved"))
            put(0x488004000ULL, 0x80000000);
        if (is("acquire-block-error"))
            put(0x488000704ULL, 0x111);
    }
    if (!strncmp(scenario, "census-", 7)) {
        census_scenario();
        return 0;
    }
    unsigned char result[128];
    memset(result, 0xaa, sizeof(result));
    if (is("acquire-cold-retry")) {
        put(0x3807002f0ULL, 0xff);
        assert(hv_windows_n1_prepare(result, sizeof(result)) < 0);
        unsigned before = writes;
        assert(hv_windows_n1_acquire(result, sizeof(result)) < 0 && writes == before && !trains);
        for (unsigned i = 0; i < sizeof(result); i++)
            assert(!result[i]);
        puts("cold failure then acquire: no retry or writes PASS");
        return 0;
    }
    int rc = acquiring ? hv_windows_n1_acquire(result, sizeof(result))
                       : hv_windows_n1_prepare(result, sizeof(result));
    bool success = is("success") || is("acquire-success") || is("acquire-captured-params") ||
                   is("acquire-adt-large") ||
                   is("acquire-invalid-roots") || is("acquire-unlock") || is("acquire-stale") ||
                   is("acquire-exception-reserved") || is("acquire-retry") ||
                   is("acquire-cold-retry");
    if (acquiring && success) {
        assert(tlbis == 19 && global_enabled && gate_writes == 7 && !power_requests);
        for (unsigned i = 0; i < 19; i++)
            assert(!get(0x488001000ULL + 4 * i) && !get(0x488001400ULL + 4 * i));
        assert(!get(0x488000104ULL) && !get(0x488004000ULL));
        if (is("acquire-unlock"))
            assert(unlocks == 1 && get(0x488000210ULL) == 2);
        census_request(0x488001000ULL, 1, 0, true, 1);
        if (is("acquire-retry") || is("acquire-cold-retry")) {
            u8 again[128];
            unsigned before = writes;
            int retry = is("acquire-retry") ? hv_windows_n1_acquire(again, sizeof(again))
                                            : hv_windows_n1_prepare(again, sizeof(again));
            assert(retry && before == writes);
            for (unsigned i = 0; i < 128; i++)
                assert(!again[i]);
        }
    }
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
        if (acquiring && (strstr(scenario, "pmgr-") || strstr(scenario, "consumer") ||
                          strstr(scenario, "mapper") || strstr(scenario, "phandle") ||
                          strstr(scenario, "piodma") || strstr(scenario, "dart-function") ||
                          strstr(scenario, "adt-")))
            assert(!writes && !trains);
        if (acquiring && (is("acquire-version") || is("acquire-stream-count") ||
                          is("acquire-geometry") || is("acquire-protect") || is("acquire-busy") ||
                          is("acquire-tcr-bypass") || is("acquire-valid-root")))
            assert(!dart_writes);
        assert(rc != 0);
        for (unsigned i = 0; i < 128; i++)
            assert(!result[i]);
        if (trained)
            assert(held);
        else if (writes)
            assert(held && rc == -2);
        if (is("clamp-fail"))
            assert(rc == -2);
        if (is("post-bme") || is("secondary-bme") || is("tertiary-bme"))
            assert(!dart_reads && get(0x1cb0100010ULL) == 4);
        if (is("dart-load-fault"))
            assert(dart_reads == 1 && get(0x1cb0100010ULL) == 4);
        if (is("post-dart-active"))
            assert(dart_reads > 7);
        if (!strncmp(scenario, "smc-", 4))
            assert(!reads && !writes && !trains);
        if (is("tunables") || !strncmp(scenario, "cold-", 5) || is("pre-bme"))
            assert(!writes && !power_requests && !fabric_calls && !trains);
    }
    printf("%s rc=%d trains=%u writes=%u held=%d PASS\n", scenario, rc, trains, writes, held);
}
