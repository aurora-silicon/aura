# SPDX-License-Identifier: MIT
"""USB4 router (ACIO) handoff by router generation.

Runs kboot.c's ACIO helpers and dt_set_acio_tunables() as they are against a
mock ADT and real FDTs:

- v2 routers ("acio,v2") keep their cio@ node: its root complex is ADT reg 1, not reg 2;
- v2 routers take the NHI table whose per-direction fabric lists are optional;
- the router AP firmware table goes only to a router whose reg names that
  window, and each row must fall inside it.
"""

from pathlib import Path
from test_siliconforest_handoff import run_c

ROOT = Path(__file__).resolve().parents[2]


def between(source, start, end):
    first = source.index(start)
    return source[first:source.index(end, first)]


def test_acio_generation_handoff(tmp_path):
    source = (ROOT / "src/kboot.c").read_text()
    assert 'dt_disable_missing_devs("acio", "cio@", 8, adt_acio_rc_regnum())' in source
    assert "dt_copy_usb4_dart_tunables" not in source
    tables = between(source, "struct adt_tunable_info {", "struct acio_tunable {")
    append = between(source, "struct acio_tunable {", "static int dt_append_dart_tunable(")
    acio = between(source, "/* The router generation is in the ADT",
                   "static const struct adt_tunable_info pciec_tunables[]")

    run_c(tmp_path, "acio-v2-handoff", r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "libfdt.h"
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define PACKED __attribute__((packed))
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define MAX_CIO_DEVS 8
#define bail(...) do { printf(__VA_ARGS__); return -1; } while (0)
#define bail_cleanup(...) do { printf(__VA_ARGS__); ret = -1; goto err; } while (0)

struct mprop { const char *name; const void *val; u32 len; };
struct mnode { const char *path; const char *compat; struct mprop props[2]; };
static const struct mnode *nodes;
static int nnodes;
static const void *adt = "adt";
static void *dt;

int adt_path_offset(const void *a, const char *path)
{
    (void)a;
    for (int i = 0; i < nnodes; i++)
        if (!strcmp(nodes[i].path, path))
            return i;
    return -1;
}

bool adt_is_compatible(const void *a, int node, const char *compat)
{
    (void)a;
    return nodes[node].compat && !strcmp(nodes[node].compat, compat);
}

const void *adt_getprop(const void *a, int node, const char *name, u32 *lenp)
{
    (void)a;
    for (int i = 0; i < 2; i++) {
        const struct mprop *p = &nodes[node].props[i];
        if (p->name && !strcmp(p->name, name)) {
            if (lenp)
                *lenp = p->len;
            return p->val;
        }
    }
    return NULL;
}

static u64 rust_usb4_router_uuid(void) { return 0x1234; }

/* What dt_set_acio_tunables() asked for, per router index. */
static const void *nhi_table[MAX_CIO_DEVS];
static size_t nhi_count[MAX_CIO_DEVS];
static int rc_calls;

''' + tables + append + r'''
static int dt_copy_acio_tunables(const char *adt_path, const char *dt_alias,
                                 const struct adt_tunable_info *tunables, size_t n_tunables)
{
    int i;
    (void)dt_alias;
    assert(sscanf(adt_path, "/arm-io/acio%d", &i) == 1);
    if (tunables == usb4_rc_tunables)
        rc_calls++;
    if (tunables == usb4_nhi_tunables || tunables == usb4_v2_nhi_tunables) {
        nhi_table[i] = tunables;
        nhi_count[i] = n_tunables;
    }
    return 0;
}

static int dt_copy_usb4_drom(const char *adt_path, const char *dt_alias, u64 uuid)
{
    (void)adt_path; (void)dt_alias; (void)uuid;
    return 0;
}

''' + acio + r'''

static const struct mnode v1[] = {
    {"/arm-io/acio0", "acio", {{0}}},
    {"/arm-io/acio1", "acio", {{0}}},
};

static const struct mnode v2[] = {
    {"/arm-io/acio0", "acio,v2", {{0}}},
    {"/arm-io/acio1", "acio,v2", {{0}}},
    {"/arm-io/acio3", "acio,v2", {{0}}},
};

int main(void)
{
    static unsigned char blob[16384];

    dt = blob;
    assert(fdt_create_empty_tree(dt, sizeof(blob)) == 0);

    /* v1 (M1/M2/M3/M4 T8132): RC is ADT reg 2, all NHI tables required. */
    nodes = v1;
    nnodes = ARRAY_SIZE(v1);
    assert(adt_acio_rc_regnum() == 2);
    assert(dt_set_acio_tunables() == 0);
    assert(rc_calls == 2);
    assert(nhi_table[0] == usb4_nhi_tunables && nhi_table[1] == usb4_nhi_tunables);
    assert(!nhi_table[2]);

    /* v2 (T8142/T6041/T6050): RC is ADT reg 1, the four fabric lists optional. */
    memset(nhi_table, 0, sizeof(nhi_table));
    rc_calls = 0;
    nodes = v2;
    nnodes = ARRAY_SIZE(v2);
    assert(adt_acio_rc_regnum() == 1);
    assert(dt_set_acio_tunables() == 0);
    assert(rc_calls == 3);
    assert(nhi_table[0] == usb4_v2_nhi_tunables && nhi_table[3] == usb4_v2_nhi_tunables);
    assert(nhi_count[3] == ARRAY_SIZE(usb4_v2_nhi_tunables));
    assert(!nhi_table[2]);
    for (size_t i = 0; i < ARRAY_SIZE(usb4_v2_nhi_tunables); i++) {
        const char *name = usb4_v2_nhi_tunables[i].adt_name;
        bool published = strstr(name, "merge") || !strcmp(name, "fw_int_ctl_management_tunables");
        assert(usb4_v2_nhi_tunables[i].required == published);
    }

    /* The AP firmware table: only into a router whose reg names its window, bounded by it. */
    static const struct acio_tunable inside[] = {{8, 4, 0xff2, 0x482}};
    static const struct acio_tunable outside[] = {{8, 4, 0xff2, 0x482}, {0x4000, 4, 1, 1}};
    static const struct mnode apfw[] = {
        {"/arm-io/acio0", "acio,v2", {{"pcie_router_ap_fw", inside, sizeof(inside)}}},
        {"/arm-io/acio1", "acio,v2", {{"pcie_router_ap_fw", outside, sizeof(outside)}}},
    };
    static const fdt32_t one[] = {0, 0x1000, 0, 0xc000};
    static const fdt32_t two[] = {0, 0x1000, 0, 0xc000, 0, 0x2000, 0, 0x4000};
    const fdt32_t two_be[] = {
        cpu_to_fdt32(two[0]), cpu_to_fdt32(two[1]), cpu_to_fdt32(two[2]), cpu_to_fdt32(two[3]),
        cpu_to_fdt32(two[4]), cpu_to_fdt32(two[5]), cpu_to_fdt32(two[6]), cpu_to_fdt32(two[7]),
    };
    const fdt32_t one_be[] = {
        cpu_to_fdt32(one[0]), cpu_to_fdt32(one[1]), cpu_to_fdt32(one[2]), cpu_to_fdt32(one[3]),
    };
    static const char names[] = "rc\0router-ap-fw";
    int soc, aliases, len;
    const fdt32_t *got;

    nodes = apfw;
    nnodes = ARRAY_SIZE(apfw);
    assert(fdt_create_empty_tree(dt, sizeof(blob)) == 0);
    soc = fdt_add_subnode(dt, 0, "soc");
    assert(soc >= 0);
    assert(!fdt_setprop_u32(dt, soc, "#address-cells", 2));
    assert(!fdt_setprop_u32(dt, soc, "#size-cells", 2));
    const char *routers[] = {"acio@0", "acio@1", "acio@2", "acio@3"};
    for (int r = 0; r < 4; r++) {
        int n = fdt_add_subnode(dt, fdt_path_offset(dt, "/soc"), routers[r]);
        assert(n >= 0);
        if (r == 2) {
            /* No router-ap-fw window */
            assert(!fdt_setprop(dt, n, "reg", one_be, sizeof(one_be)));
            assert(!fdt_setprop_string(dt, n, "reg-names", "rc"));
        } else {
            assert(!fdt_setprop(dt, n, "reg", two_be, sizeof(two_be)));
            assert(!fdt_setprop(dt, n, "reg-names", names, sizeof(names)));
        }
        if (r == 3)
            assert(!fdt_setprop_u32(dt, n, "apple,tunable-router-ap-fw", 0x1234));
    }
    aliases = fdt_add_subnode(dt, 0, "aliases");
    assert(aliases >= 0);
    for (int r = 0; r < 4; r++) {
        char alias[16], path[16];
        snprintf(alias, sizeof(alias), "usb4-%d-acio", r);
        snprintf(path, sizeof(path), "/soc/%s", routers[r]);
        assert(!fdt_setprop_string(dt, fdt_path_offset(dt, "/aliases"), alias, path));
    }

    dt_copy_router_ap_fw_tunable(0, "usb4-0-acio");
    got = fdt_getprop(dt, fdt_path_offset(dt, "/soc/acio@0"), "apple,tunable-router-ap-fw", &len);
    assert(got && len == 12 && fdt32_to_cpu(got[0]) == 8 && fdt32_to_cpu(got[1]) == 0xff2 &&
           fdt32_to_cpu(got[2]) == 0x482);
    /* A row past the 0x4000 window: nothing is left behind. */
    dt_copy_router_ap_fw_tunable(1, "usb4-1-acio");
    assert(!fdt_getprop(dt, fdt_path_offset(dt, "/soc/acio@1"), "apple,tunable-router-ap-fw", NULL));
    /* No router-ap-fw reg-name: the kernel would ignore the table, so it is not copied. */
    dt_copy_router_ap_fw_tunable(0, "usb4-2-acio");
    assert(!fdt_getprop(dt, fdt_path_offset(dt, "/soc/acio@2"), "apple,tunable-router-ap-fw", NULL));
    /* A tree's own table wins. */
    dt_copy_router_ap_fw_tunable(0, "usb4-3-acio");
    got = fdt_getprop(dt, fdt_path_offset(dt, "/soc/acio@3"), "apple,tunable-router-ap-fw", &len);
    assert(got && len == 4 && fdt32_to_cpu(got[0]) == 0x1234);

    /* No router at all (T8140): nothing to do, generation defaults to v1. */
    nnodes = 0;
    assert(adt_acio_rc_regnum() == 2);
    return 0;
}
''')
