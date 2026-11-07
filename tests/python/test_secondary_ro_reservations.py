# SPDX-License-Identifier: MIT
"""What kboot reserves for the ranges the secondaries can't write, at EL1 and EL2.

dt_reserve_secondary_ro() and ctxr_range() are the production bodies. Three cases
differ from main on purpose, and say so below.
"""
from test_locked_dart_tables import function
from test_smp_timeouts import REPO, compile_and_run


def bodies():
    kboot = (REPO / "src/kboot.c").read_text()
    ctxr = (REPO / "src/ctxr_exec.c").read_text()
    header = (REPO / "src/ctxr_exec.h").read_text()
    start = header.index("enum ctxr_range {")
    ranges = header[start:header.index("};", start) + 2]
    return ranges + "\n" + function(ctxr, "ctxr_range") + "\n" + function(kboot, "dt_reserve_secondary_ro")


HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef unsigned long u64;
typedef unsigned long fdt64_t;
#define SZ_4K 0x1000UL
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define cpu_to_fdt64(x) (x)
enum exc_guard_t { GUARD_OFF = 0, GUARD_MARK = 2, GUARD_SILENT = 0x100 };
static volatile int exc_guard, exc_count;
static int el2, reads, nrsv, nodes, fail_rsv, ro_status;
static u64 rsv[8][2], ro_s, ro_e;
static char log_text[512];
static struct { bool apple_sysregs_unlocked; } features, *cpu_features = &features;
static void *dt;
static u64 SYS_IMP_APL_CTXR_A_LWR_EL1, SYS_IMP_APL_CTXR_A_UPR_EL1, SYS_IMP_APL_CTXR_B_LWR_EL1,
    SYS_IMP_APL_CTXR_B_UPR_EL1, SYS_IMP_APL_CTXR_C_LWR_EL1, SYS_IMP_APL_CTXR_C_UPR_EL1,
    SYS_IMP_APL_CTXR_D_LWR_EL1, SYS_IMP_APL_CTXR_D_UPR_EL1, SYS_IMP_APL_CTRR_C_LWR_EL1,
    SYS_IMP_APL_CTRR_C_UPR_EL1, SYS_IMP_APL_CTRR_D_LWR_EL1, SYS_IMP_APL_CTRR_D_UPR_EL1;
static u64 read_register(u64 value) { reads++; return value; }
#define mrs(reg) read_register(reg)
#define printf(...) snprintf(log_text + strlen(log_text), sizeof(log_text) - strlen(log_text), __VA_ARGS__)
#define bail(...) do { printf(__VA_ARGS__); return -1; } while (0)
static bool in_el2(void) { return el2; }
static int memory_fw_ro_range(u64 *s, u64 *e)
{
    if (s) *s = ro_s;
    if (e) *e = ro_e;
    return ro_status;
}
static int fdt_add_mem_rsv(void *p, u64 s, u64 size)
{
    if (fail_rsv) return -1;
    rsv[nrsv][0] = s; rsv[nrsv][1] = size; nrsv++;
    return 0;
}
static int fdt_path_offset(void *p, const char *path) { return 1; }
static int fdt_subnode_offset(void *p, int parent, const char *name) { return -1; }
static int fdt_add_subnode(void *p, int parent, const char *name)
{ assert(!strcmp(name, "aura-secondary-ro@10018014000")); nodes++; return 2; }
static int fdt_setprop(void *p, int node, const char *name, const void *v, int len) { return 0; }
static int fdt_setprop_empty(void *p, int node, const char *name) { return 0; }
''' + "%s" + r'''
#undef printf
#define RO 0x10018014000UL
#define C 0x10000100000UL
#define D 0x10000200000UL
enum chip { T8140, OTHER_M4, M1_TO_M3 };
static void boot(enum chip chip, int at_el2)
{
    el2 = at_el2; reads = nrsv = nodes = fail_rsv = 0;
    memset(rsv, 0, sizeof(rsv)); log_text[0] = 0;
    features.apple_sysregs_unlocked = chip == M1_TO_M3;
    /* memory_fw_ro_range() answers only on T8140, with its 48 KiB range. */
    ro_status = chip == T8140; ro_s = RO; ro_e = RO + 0xc000;
    /* CTRR_C is the firmware RO range on T8140, 48 KiB elsewhere; CTRR_D is armed. */
    SYS_IMP_APL_CTRR_C_LWR_EL1 = chip == T8140 ? RO : C;
    SYS_IMP_APL_CTRR_C_UPR_EL1 = SYS_IMP_APL_CTRR_C_LWR_EL1 + 0xb000;
    SYS_IMP_APL_CTRR_D_LWR_EL1 = D; SYS_IMP_APL_CTRR_D_UPR_EL1 = D + 0xfc000;
}
static bool has(int i, u64 start, u64 size) { return rsv[i][0] == start && rsv[i][1] == size; }
int main(void)
{
    /* T8140 at EL2: the firmware RO range with its no-map node, and CTRR_D.
     * Differs from main, which never reserved CTRR_D on T8140. */
    boot(T8140, 1);
    assert(!dt_reserve_secondary_ro() && nrsv == 2 && nodes == 1);
    assert(has(0, RO, 0xc000) && has(1, D, 0xfd000));
    /* With CTRR_D not armed, exactly what main reserves. */
    boot(T8140, 1);
    SYS_IMP_APL_CTRR_D_LWR_EL1 = SYS_IMP_APL_CTRR_D_UPR_EL1 = 0;
    assert(!dt_reserve_secondary_ro() && nrsv == 1 && has(0, RO, 0xc000) && nodes == 1);
    /* T8140 at EL1, a guest under the hypervisor: the firmware RO range as on main, no CTRR read. */
    boot(T8140, 0);
    assert(!dt_reserve_secondary_ro() && nrsv == 1 && has(0, RO, 0xc000) && nodes == 1 && !reads);
    assert(strstr(log_text, "leaving CTRR_D to the host") && !strstr(log_text, "CTRR_C and"));
    /* An invalid range stops the boot, at either EL. */
    for (int at_el2 = 0; at_el2 <= 1; at_el2++) {
        boot(T8140, at_el2);
        ro_status = -1;
        assert(dt_reserve_secondary_ro() == -1 && !nrsv && !nodes);
    }

    /* Another M4-or-later chip at EL2: CTRR_C and CTRR_D, each to UPR + 4K.
     * Differs from main, whose CTRR_C ended at UPR (0xb000 here, not 0xc000). */
    boot(OTHER_M4, 1);
    assert(!dt_reserve_secondary_ro() && nrsv == 2 && !nodes);
    assert(has(0, C, 0xc000) && has(1, D, 0xfd000));
    /* Differs from main, which added a zero-size entry for an empty CTRR_C and a wrapped
     * one for UPR below LWR. Both are left out now. */
    boot(OTHER_M4, 1);
    SYS_IMP_APL_CTRR_C_LWR_EL1 = SYS_IMP_APL_CTRR_C_UPR_EL1 = 0;
    assert(!dt_reserve_secondary_ro() && nrsv == 1 && has(0, D, 0xfd000));
    boot(OTHER_M4, 1);
    SYS_IMP_APL_CTRR_C_UPR_EL1 = C - SZ_4K;
    assert(!dt_reserve_secondary_ro() && nrsv == 1 && has(0, D, 0xfd000));
    /* At EL1 it leaves both to the host and reads neither, as main did. */
    boot(OTHER_M4, 0);
    assert(!dt_reserve_secondary_ro() && !nrsv && !nodes && !reads);
    assert(strstr(log_text, "leaving CTRR_C and CTRR_D to the host"));
    /* A failed reservation stops the boot. */
    boot(OTHER_M4, 1);
    fail_rsv = 1;
    assert(dt_reserve_secondary_ro() == -1);

    /* M1 to M3: nothing to reserve and no register read, at either EL. */
    for (int at_el2 = 0; at_el2 <= 1; at_el2++) {
        boot(M1_TO_M3, at_el2);
        assert(!dt_reserve_secondary_ro() && !nrsv && !nodes && !reads && !log_text[0]);
    }
    puts("secondary RO reservations OK");
    return 0;
}
'''


def test_secondary_ro_reservations(tmp_path):
    compile_and_run(tmp_path, "secondary-ro", HARNESS % bodies())


def test_kboot_reserves_them_in_one_place():
    source = (REPO / "src/kboot.c").read_text()
    assert source.count("dt_reserve_secondary_ro()") == 1
    assert "if (dt_reserve_secondary_ro())\n        return -1;" in source
