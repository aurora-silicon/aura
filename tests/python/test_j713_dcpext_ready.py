# SPDX-License-Identifier: MIT
"""Exercise the J713 external DCP handoff with production kboot and PMGR code and real FDTs."""

import re
from pathlib import Path
from test_siliconforest_handoff import run_c

ROOT = Path(__file__).resolve().parents[2]


def test_j713_dcpext_ready_is_opt_in_complete_and_reads_pmgr_only(tmp_path):
    kboot = (ROOT / "src/kboot.c").read_text()
    pmgr = (ROOT / "src/pmgr.c").read_text()

    def function(source, name, kind="static int "):
        start = source.index(kind + name + "(")
        return source[start:source.index("\n}", start) + 2] + "\n"

    def define(source, name):
        return re.search(r"^#define " + name + r"\b.*$", source, re.M).group(0) + "\n"

    start = kboot.index("static const char dcpext_aliases")
    aliases = kboot[start:kboot.index("\n};", start) + 3]
    header = (ROOT / "src/adt.h").read_text()
    start = header.index("struct adt_segment_ranges {")
    layout = header[start:header.index("} PACKED;", start) + len("} PACKED;")]
    pmgr_bits = (define(pmgr, "PMGR_AUTO_ENABLE") + define(pmgr, "PMGR_PS_ACTUAL") +
                 define(pmgr, "PMGR_PS_TARGET") +
                 define(pmgr, "PMGR_FLAG_VIRTUAL") +
                 define((ROOT / "src/pmgr.h").read_text(), "PMGR_PS_ACTIVE"))
    run_c(tmp_path, "j713-dcpext-ready", r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libfdt.h"
/* Match aura's aarch64-linux-gnu target, not the host libc's uint64_t alias. */
typedef unsigned long u64;
_Static_assert(sizeof(u64) == 8, "the aura host fixture requires an LP64 host");
typedef uint32_t u32;
typedef uint8_t u8;
#define PACKED __attribute__((packed))
#define T8122 0x8122
#define T6030 0x6030
#define T8132 0x8132
#define T6000 0x6000
#define T6001 0x6001
#define T6002 0x6002
#define SZ_16K 16384
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define BIT(n) (1u << (n))
#define GENMASK(h, l) ((~0u << (l)) & (~0u >> (31 - (h))))
#define FIELD_GET(mask, v) (((v) & (mask)) >> __builtin_ctz(mask))
#define T8132_DCPEXT_READY "apple,t8132-dcpext-firmware-ready"
#define bail(...) do { return -1; } while (0)
''' + pmgr_bits + r'''
static void *dt, *adt;
static u32 chip_id = T8132;
/* fault applies to the controllers in victims (bit n = dcpextn). */
static int fault, victims, reservations, attempts[2];
static const char *expected_status[2];
static unsigned char blob[32768];
''' + layout + r'''
static struct adt_segment_ranges segments[2][2];
static u64 dram_base = 0x10000000000ULL, dram_size = 0x1000000;
static const char *uuids[2] = {"81BAF476-679D-3CC0-8C6A-E6B6FA659D5E",
                               "0F1E2D3C-4B5A-6978-8796-A5B4C3D2E1F0"};
static char bad_uuid[37];
static bool hit(int n) { return victims & (1 << n); }
static int adt_path_offset(void *tree, const char *path)
{
    if (!strcmp(path, "/chosen")) return 2;
    for (int n = 0; n < 2; n++) {
        char nub[48];
        snprintf(nub, sizeof(nub), "/arm-io/dcpext%d/iop-dcpext%d-nub", n, n);
        if (!strcmp(path, nub)) return fault == 1 && hit(n) ? -1 : 10 + n;
    }
    return -1;
}
static const void *adt_getprop(void *tree, int node, const char *name, u32 *len)
{
    int n = node - 10;
    if (n >= 0 && n < 2 && !strcmp(name, "segment-ranges")) {
        *len = fault == 2 && hit(n) ? 0 : fault == 3 && hit(n) ? sizeof(segments[n]) - 1 :
                                                               sizeof(segments[n]);
        return fault == 4 && hit(n) ? NULL : segments[n];
    }
    if (n >= 0 && n < 2 && !strcmp(name, "uuid")) {
        if (fault == 6 && hit(n)) return NULL;
        *len = fault == 7 && hit(n) ? 36 : 37;
        if (fault == 8 && hit(n)) { memset(bad_uuid, 'A', sizeof(bad_uuid)); return bad_uuid; }
        return uuids[n];
    }
    *len = fault == 5 ? 4 : 8;
    if (node == 2 && !strcmp(name, "dram-base")) return &dram_base;
    if (node == 2 && !strcmp(name, "dram-size")) return &dram_size;
    return NULL;
}
static int get_value(int node, const char *name, void *out, size_t capacity)
{
    u32 len = 0;
    const void *p = adt_getprop(adt, node, name, &len);
    if (!p || len > capacity) return -1;
    memcpy(out, p, len);
    return len;
}
#define ADT_GETPROP(a, n, k, v) get_value(n, k, v, sizeof(*(v)))
static int dt_get_or_add_reserved_mem(const char *name, const char *compat,
                                     bool nomap, u64 physical, size_t size)
{
    int n = physical >= dram_base + 0x40000;
    reservations++; attempts[n]++;
    if (hit(n) && ((fault == 9 && attempts[n] == 1) || (fault == 10 && attempts[n] == 2)))
        return -FDT_ERR_NOSPACE;
    int parent = fdt_path_offset(dt, "/reserved-memory");
    int node = fdt_add_subnode(dt, parent, name);
    assert(node >= 0);
    assert(!fdt_setprop_u32(dt, node, "phandle", 100 + reservations));
    fdt64_t reg[2] = {cpu_to_fdt64(physical), cpu_to_fdt64(size)};
    assert(!fdt_setprop(dt, node, "reg", reg, sizeof(reg)));
    assert(!fdt_setprop_empty(dt, node, "no-map"));
    return node;
}
static int dt_set_dcp_firmware(const char *alias)
{
    int node = fdt_path_offset(dt, alias);
    fdt32_t ver[3] = {cpu_to_fdt32(26), cpu_to_fdt32(6), cpu_to_fdt32(2)};
    assert(!fdt_setprop(dt, node, "apple,firmware-compat", ver, sizeof(ver)));
    return fault == 11 && hit(alias[6] - '0') ? -1 : 0;
}
static int set_u32(void *tree, int node, const char *name, uint32_t value)
{
    for (int n = 0; n < 2; n++) {
        char alias[8] = "dcpext0";
        alias[6] += n;
        if (fault == 12 && hit(n) && !strcmp(name, T8132_DCPEXT_READY) &&
            node == fdt_path_offset(tree, alias))
            return -FDT_ERR_NOSPACE;
    }
    return fdt_setprop_u32(tree, node, name, value);
}
#define fdt_setprop_u32 set_u32
/*
 * PMGR: the production readers over a fake power-state table. Only reads are
 * provided, so a PMGR write in the handoff would not even build; MMIO writes
 * are counted as well.
 */
struct pmgr_device {
    const char name[0x10];
    u8 flags;
};
static const struct pmgr_device devices[] = {
    {.name = "DISPEXT0_SYS"}, {.name = "DISPEXT0_FE"}, {.name = "DISPEXT0_CPU"},
    {.name = "DISP0_SYS"}, {.name = "DISPEXT1_SYS"}, {.name = "DISPEXT1_FE"},
    {.name = "DISPEXT1_CPU"},
};
static const struct pmgr_device *pmgr_devices = devices;
static u32 pmgr_devices_len = ARRAY_SIZE(devices);
static int pmgr_dies = 1, pmgr_reads, pmgr_writes;
static u32 ps[ARRAY_SIZE(devices)];
static uintptr_t pmgr_device_get_addr(u8 die, const struct pmgr_device *device)
{
    return 0x1000 + 8 * (device - devices);
}
static u32 read32(uintptr_t addr)
{
    size_t i = (addr - 0x1000) / 8;
    assert(addr >= 0x1000 && i < ARRAY_SIZE(devices) && !strncmp(devices[i].name, "DISPEXT", 7));
    pmgr_reads++;
    return ps[i];
}
__attribute__((unused)) static void write32(uintptr_t addr, u32 value) { pmgr_writes++; }
__attribute__((unused)) static u32 mask32(uintptr_t addr, u32 clear, u32 set)
{
    return pmgr_writes++;
}
__attribute__((unused)) static u32 set32(uintptr_t addr, u32 set) { return pmgr_writes++; }
__attribute__((unused)) static u32 clear32(uintptr_t addr, u32 clear) { return pmgr_writes++; }
''' + function(pmgr, "pmgr_find_device_by_name", "static const struct pmgr_device *") +
        function(pmgr, "pmgr_psreg_by_name", "uintptr_t ") +
        function(pmgr, "pmgr_power_is_on", "int ") +
        function(pmgr, "pmgr_power_is_requested", "int ") +
        function(kboot, "dt_device_set_reserved_mem") +
        function(kboot, "dt_device_add_mem_region") +
        function(kboot, "dt_reserve_asc_segments") + function(kboot, "dt_reserve_asc_firmware") +
        aliases + "\n" + function(kboot, "dt_display_transaction") +
        # The native26 path is T8122-only; J713 must never reach it.
        "static int dt_ext26_snapshot(const char *alias) { assert(0); return -1; }\n"
        "static int dt_ext26_reserve(const char *alias) { assert(0); return -1; }\n"
        "static void dt_ext26_step(const char *step) { assert(0); }\n"
        "static int dt_ext26_retain_cpu(const char *alias) { assert(0); return -1; }\n" +
        function(kboot, "dt_reserve_dcpext_ready") +
        function(kboot, "dt_reserve_j713_dcpext_one") +
        function(kboot, "dt_j713_dcpext_powered", "static bool ") +
        function(kboot, "dt_reserve_j713_dcpext", "static void ") + r'''
#define GATED (PMGR_AUTO_ENABLE | PMGR_PS_ACTIVE) /* targeted on, gated while idle */
#define ON ((PMGR_PS_ACTIVE << 4) | PMGR_PS_ACTIVE)
static void world(void)
{
    fault = victims = reservations = attempts[0] = attempts[1] = 0;
    expected_status[0] = expected_status[1] = "disabled";
    pmgr_reads = pmgr_writes = 0;
    chip_id = T8132; pmgr_devices_len = ARRAY_SIZE(devices);
    dram_base = 0x10000000000ULL; dram_size = 0x1000000;
    for (int n = 0; n < 2; n++) {
        u64 at = dram_base + 0x40000 * n;
        segments[n][0] = (struct adt_segment_ranges){.phys = at + 0x4000,
            .remap = 0x10000000, .size = 0x4000};
        /* The OS log segment, reserved by physical address. */
        segments[n][1] = (struct adt_segment_ranges){.phys = at + 0x10000,
            .remap = 0x20000000, .size = 0x8000, .unk = 0xa};
    }
    for (unsigned i = 0; i < ARRAY_SIZE(devices); i++)
        ps[i] = strstr(devices[i].name, "_CPU") ? GATED : ON;
    memset(blob, 0, sizeof(blob)); dt = blob;
    assert(!fdt_create_empty_tree(dt, sizeof(blob)));
    assert(!fdt_setprop_string(dt, 0, "compatible", "apple,t8132"));
    assert(fdt_add_subnode(dt, 0, "reserved-memory") >= 0);
    int soc = fdt_add_subnode(dt, 0, "soc");
    assert(soc >= 0);
    /* The internal DCP was handed over: it carries its firmware UUID. */
    int node = fdt_add_subnode(dt, soc, "dcp");
    assert(node >= 0 && !fdt_setprop_string(dt, node, "apple,firmware-uuid", uuids[0]));
    const char *names[] = {"dcpext0", "dcpext1"};
    for (unsigned i = 0; i < 2; i++) {
        node = fdt_add_subnode(dt, fdt_path_offset(dt, "/soc"), names[i]);
        assert(node >= 0 && !fdt_setprop_string(dt, node, "status", "disabled"));
        assert(!fdt_setprop_string(dt, node, "compatible", "apple,t8132-dcpext"));
    }
    node = fdt_add_subnode(dt, 0, "aliases");
    assert(node >= 0 && !fdt_setprop_string(dt, node, "dcp", "/soc/dcp"));
    assert(!fdt_setprop_string(dt, node, "dcpext0", "/soc/dcpext0"));
    assert(!fdt_setprop_string(dt, node, "dcpext1", "/soc/dcpext1"));
    /* Armed: chosen.asahi,j713-dcpext=1. */
    node = fdt_add_subnode(dt, 0, "chosen");
    assert(node >= 0 && !fdt_setprop_string(dt, node, "asahi,j713-dcpext", "1"));
}
static int ext(int n) { return fdt_path_offset(dt, n ? "dcpext1" : "dcpext0"); }
static int reserved(int n, int segment)
{
    char name[64];
    snprintf(name, sizeof(name), "asc-firmware@%lx", segments[n][segment].phys);
    return fdt_subnode_offset(dt, fdt_path_offset(dt, "/reserved-memory"), name);
}
static void status_preserved(int n)
{
    int len = 0;
    const char *status = fdt_getprop(dt, ext(n), "status", &len);
    if (expected_status[n])
        assert(status && !strcmp(status, expected_status[n]));
    else
        assert(!status && len == -FDT_ERR_NOTFOUND);
}
static void published(int n)
{
    int len = 0, node = ext(n);
    const fdt32_t *ready = fdt_getprop(dt, node, T8132_DCPEXT_READY, &len);
    assert(ready && len == 4 && fdt32_ld(ready) == 1);
    const char *uuid = fdt_getprop(dt, node, "apple,firmware-uuid", &len);
    assert(uuid && len == 37 && !strcmp(uuid, uuids[n]));
    assert(fdt_getprop(dt, node, "memory-region", &len) && len == 8);
    assert(reserved(n, 0) >= 0 && reserved(n, 1) >= 0);
    assert(fdt_getprop(dt, reserved(n, 0), "iommu-addresses", NULL));
    assert(!fdt_getprop(dt, reserved(n, 0), "apple,dcp-os-log", NULL));
    assert(fdt_getprop(dt, reserved(n, 1), "apple,dcp-os-log", NULL));
    assert(!fdt_getprop(dt, reserved(n, 1), "iommu-addresses", NULL));
    status_preserved(n);
}
static void untouched(int n)
{
    int node = ext(n);
    assert(!fdt_getprop(dt, node, T8132_DCPEXT_READY, NULL));
    assert(!fdt_getprop(dt, node, "apple,firmware-uuid", NULL));
    assert(!fdt_getprop(dt, node, "memory-region", NULL));
    assert(reserved(n, 0) < 0 && reserved(n, 1) < 0);
    status_preserved(n);
}
static void stale_markers(void)
{
    for (int n = 0; n < 2; n++)
        assert(!fdt_setprop_u32(dt, ext(n), T8132_DCPEXT_READY, 1));
}
/* Not armed: nothing is reserved, PMGR is not even read, and stale promises are withdrawn. */
static void unarmed(void)
{
    stale_markers();
    dt_reserve_j713_dcpext();
    untouched(0); untouched(1);
    assert(!reservations && !pmgr_reads && !pmgr_writes);
}
int main(void)
{
    /* Armed, both controllers left powered by iBoot: both are handed over. */
    world();
    dt_reserve_j713_dcpext();
    published(0); published(1);
    assert(reservations == 4 && pmgr_reads == 6 && !pmgr_writes);
    /* A stale marker is replaced by this boot's handoff. */
    world(); stale_markers();
    dt_reserve_j713_dcpext();
    published(0); published(1);

    /* An incomplete handoff publishes nothing for that controller, the other is unaffected. */
    for (int scenario = 1; scenario <= 12; scenario++) {
        for (int mask = 1; mask <= 3; mask++) {
            if (scenario == 5 && mask != 3) continue; /* the DRAM range is shared */
            world(); fault = scenario; victims = mask;
            unsigned char before[sizeof(blob)]; memcpy(before, blob, sizeof(blob));
            dt_reserve_j713_dcpext();
            for (int n = 0; n < 2; n++) {
                if (hit(n)) untouched(n); else published(n);
            }
            if (mask == 3) assert(!memcmp(before, blob, sizeof(blob)));
            assert(!pmgr_writes);
        }
    }
    for (int scenario = 0; scenario < 6; scenario++) {
        world();
        if (scenario == 0) segments[1][0].phys = dram_base - 0x4000;
        if (scenario == 1) segments[1][0].phys++;
        if (scenario == 2) segments[1][1].size = 0;
        if (scenario == 3) segments[1][1].remap = UINT64_MAX;
        if (scenario == 4) segments[1][1].phys = dram_base + dram_size;
        if (scenario == 5) dram_size = 0;
        dt_reserve_j713_dcpext();
        if (scenario == 5) untouched(0); else published(0);
        untouched(1);
        assert(!pmgr_writes);
    }

    /* Opt-in, and only after the internal DCP was handed over. */
    world(); assert(!fdt_delprop(dt, fdt_path_offset(dt, "/chosen"), "asahi,j713-dcpext"));
    unarmed();
    const char *values[] = {"0", "", "11", "yes"};
    for (unsigned i = 0; i < ARRAY_SIZE(values); i++) {
        world();
        assert(!fdt_setprop_string(dt, fdt_path_offset(dt, "/chosen"), "asahi,j713-dcpext",
                                   values[i]));
        unarmed();
    }
    world(); assert(!fdt_setprop(dt, fdt_path_offset(dt, "/chosen"), "asahi,j713-dcpext", "1", 1));
    unarmed(); /* "1" without its NUL */
    world(); assert(!fdt_del_node(dt, fdt_path_offset(dt, "/chosen")));
    unarmed();
    world(); assert(!fdt_delprop(dt, fdt_path_offset(dt, "/soc/dcp"), "apple,firmware-uuid"));
    unarmed();
    world(); assert(!fdt_delprop(dt, fdt_path_offset(dt, "/aliases"), "dcp"));
    unarmed();

    /* Only a controller iBoot left powered, read from PMGR without a write. */
    for (int scenario = 0; scenario < 6; scenario++) {
        world(); stale_markers();
        if (scenario == 0) ps[4] = 0;                   /* DISPEXT1_SYS off */
        if (scenario == 1) ps[5] = GATED;               /* DISPEXT1_FE not on */
        if (scenario == 2) ps[6] = 0;                   /* DISPEXT1_CPU off */
        if (scenario == 3) ps[6] = PMGR_PS_ACTIVE;      /* targeted, no hardware gating */
        if (scenario == 4) pmgr_devices_len--;          /* DISPEXT1_CPU unknown */
        if (scenario == 5) ps[6] = ON;                  /* actually on: handed over */
        dt_reserve_j713_dcpext();
        published(0);
        if (scenario == 5) published(1); else untouched(1);
        assert(!pmgr_writes);
    }

    /* Current kernel DTs leave dcpext available; older embedded DTs disabled it.
     * Reserve either shape without changing status, routes or suppliers. */
    const char *accepted[] = {NULL, "okay", "ok", "disabled"};
    for (unsigned a = 0; a < ARRAY_SIZE(accepted); a++) {
        for (unsigned b = 0; b < ARRAY_SIZE(accepted); b++) {
            for (unsigned mode = 0; mode < 3; mode++) {
                world(); stale_markers();
                expected_status[0] = accepted[a]; expected_status[1] = accepted[b];
                for (int n = 0; n < 2; n++) {
                    if (expected_status[n])
                        assert(!fdt_setprop_string(dt, ext(n), "status", expected_status[n]));
                    else
                        assert(!fdt_delprop(dt, ext(n), "status"));
                }
                if (mode == 1) { fault = 9; victims = 2; }
                if (mode == 2) {
                    assert(!fdt_delprop(dt, fdt_path_offset(dt, "/chosen"), "asahi,j713-dcpext"));
                    unarmed();
                    continue;
                }
                dt_reserve_j713_dcpext();
                published(0);
                if (mode == 1) untouched(1); else published(1);
                assert(pmgr_reads == 6 && !pmgr_writes);
            }
        }
    }
    /* Other statuses, including truncated strings and trailing bytes, stay closed. */
    const struct { const char *data; int size; } rejected[] = {
        {"reserved", 9}, {"fail", 5}, {"", 1}, {"okay", 4},
        {"okay\0extra", 11}, {"disabledjunk", 13}, {"ok", 0},
    };
    for (unsigned i = 0; i < ARRAY_SIZE(rejected); i++) {
        world(); stale_markers();
        assert(!fdt_setprop(dt, ext(1), "status", rejected[i].data, rejected[i].size));
        dt_reserve_j713_dcpext();
        published(0);
        assert(!fdt_getprop(dt, ext(1), T8132_DCPEXT_READY, NULL) && reserved(1, 0) < 0);
        int len = 0;
        const void *status = fdt_getprop(dt, ext(1), "status", &len);
        assert(status && len == rejected[i].size && !memcmp(status, rejected[i].data, len));
        assert(pmgr_reads == 3 && !pmgr_writes);
    }
    /* Other SoCs never acquire the J713 handoff. */
    world(); stale_markers();
    assert(!fdt_setprop_string(dt, ext(1), "compatible", "apple,t8122-dcpext"));
    dt_reserve_j713_dcpext();
    published(0); untouched(1);
    return 0;
}
''')
