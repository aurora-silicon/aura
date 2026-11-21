# SPDX-License-Identifier: GPL-2.0-only
"""Run the production T8132 AOP handoff on trees of several boards: none is treated specially."""
from pathlib import Path

from test_j713_dapf_instance import block
from test_siliconforest_handoff import run_c

ROOT = Path(__file__).resolve().parents[2]

# What can go wrong: the handoff depends on the board again; a tree that does not ask for the
# handoff is changed; the DAPF (the only hardware write) is programmed before the tree edits are
# known to succeed; a failed step leaves reservations in the tree or the AOP enabled.
HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libfdt.h"
typedef uint32_t u32;
typedef unsigned long u64;
typedef struct { int unused; } asc_dev_t;
#define bail(...) do { printf(__VA_ARGS__); return -1; } while (0)
#define UNUSED(x) (void)(x)
static void *dt;
static asc_dev_t the_asc;
static int touched, fail;
static bool running;
static asc_dev_t *asc_init(const char *path)
{ assert(!strcmp(path, "/arm-io/aop")); touched++; return fail == 1 ? NULL : &the_asc; }
static bool asc_cpu_running(asc_dev_t *asc) { return running; }
static void asc_free(asc_dev_t *asc) {}
/* The one hardware write. It must come after both reservations were made in the tree. */
static int dapf_writes, reserved;
static int dapf_init_t8132_halted(const char *path, int index)
{ assert(!strcmp(path, "/arm-io/dart-aop") && index == 1 && reserved == 2); touched++;
  if (fail == 4) return -1;
  dapf_writes++; return 0; }
static int reserve(const char *name)
{
    int node = fdt_add_subnode(dt, fdt_path_offset(dt, "/reserved-memory"), name);
    assert(node >= 0);
    reserved++;
    return 0;
}
static int dt_reserve_asc_segments(const char *adt_path, const char *alt, const char *path,
                                   bool remap, u64 base, bool dram_only)
{ assert(!strcmp(adt_path, "/arm-io/aop/iop-aop-nub") && remap && dram_only && !dapf_writes);
  touched++; return fail == 2 ? -1 : reserve("asc-firmware@1"); }
static int dt_reserve_t8132_aop_windows(const char *path)
{ assert(!dapf_writes); touched++; return fail == 3 ? -1 : reserve("asc-mmio@2"); }
'''

MAIN = r'''
static unsigned char blob[4096];
static void tree(const char *board, bool requested)
{
    char compatible[64];
    int len = snprintf(compatible, sizeof(compatible), "%s%c%s", board, 0, "apple,t8132") + 1;
    dt = blob;
    assert(!fdt_create_empty_tree(dt, sizeof(blob)));
    assert(!fdt_setprop(dt, 0, "compatible", compatible, len));
    assert(fdt_add_subnode(dt, 0, "reserved-memory") >= 0);
    int chosen = fdt_add_subnode(dt, 0, "chosen");
    if (requested)
        assert(!fdt_setprop(dt, chosen, "linux-enablement-mac,dapf-handoff",
                            "/arm-io/aop\0/arm-io/pmp", sizeof("/arm-io/aop\0/arm-io/pmp")));
    int aop = fdt_add_subnode(dt, 0, "aop@390600000");
    assert(aop >= 0 && !fdt_setprop_string(dt, aop, "compatible", "apple,t8132-aop"));
    assert(!fdt_setprop_string(dt, aop, "status", "okay"));
    touched = fail = dapf_writes = reserved = 0;
    running = false;
}
static const char *status(void)
{
    return fdt_getprop(dt, fdt_path_offset(dt, "/aop@390600000"), "status", NULL);
}
int main(void)
{
    /* Any T8132 tree: a requested, halted AOP is handed over; any failed step disables it. */
    static const char *const boards[] = {"apple,j713", "apple,j604", "apple,j623",
                                         "apple,j624", "apple,j715", "apple,j773g"};
    static unsigned char before[sizeof(blob)];
    for (unsigned i = 0; i < sizeof(boards) / sizeof(*boards); i++) {
        tree(boards[i], true);
        assert(!dt_set_t8132_aop("/aop@390600000") && !strcmp(status(), "okay"));
        assert(touched == 4 && dapf_writes == 1 && reserved == 2);
        assert(fdt_path_offset(dt, "/reserved-memory/asc-firmware@1") >= 0);
        assert(fdt_path_offset(dt, "/reserved-memory/asc-mmio@2") >= 0);
        /* 1: no ASC. 2: segments. 3: windows. 4: the DAPF write itself, which comes last. */
        for (int f = 1; f <= 4; f++) {
            tree(boards[i], true);
            fail = f;
            assert(!dt_set_t8132_aop("/aop@390600000") && !strcmp(status(), "disabled"));
            assert(touched == f && !dapf_writes);
            /* No reservation of a refused handoff stays in the tree. */
            assert(fdt_path_offset(dt, "/reserved-memory/asc-firmware@1") < 0);
            assert(fdt_path_offset(dt, "/reserved-memory/asc-mmio@2") < 0);
        }
        tree(boards[i], true);
        running = true;
        assert(!dt_set_t8132_aop("/aop@390600000") && !strcmp(status(), "disabled") && touched == 1);
        /* Not requested: the tree is byte for byte as it was and no hardware is touched. */
        tree(boards[i], false);
        memcpy(before, blob, sizeof(blob));
        assert(!dt_set_t8132_aop("/aop@390600000") && !touched);
        assert(!memcmp(before, blob, sizeof(blob)) && !strcmp(status(), "okay"));
    }
    return 0;
}
'''


def production():
    kboot = (ROOT / "src/kboot.c").read_text()
    return (block(kboot, "static int dt_display_transaction(") + "\n"
            + "static const char *t8132_aop_path;\n"
            + block(kboot, "static int dt_set_t8132_aop_one(") + "\n"
            + block(kboot, "static int dt_set_t8132_aop(") + "\n")


def test_t8132_aop_gate(tmp_path):
    run_c(tmp_path, "t8132-aop-gate",
          HARNESS + production() + MAIN)
