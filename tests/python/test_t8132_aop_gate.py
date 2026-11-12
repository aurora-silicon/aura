# SPDX-License-Identifier: GPL-2.0-only
"""Run the production T8132 AOP handoff gate on J713, J604 and boards it does not qualify."""
from pathlib import Path

from test_j713_dapf_instance import block
from test_siliconforest_handoff import run_c

ROOT = Path(__file__).resolve().parents[2]

# What can go wrong: an unqualified board keeps an enabled AOP that no loader handed over
# (the OS then starts it without its DAPF or its DRAM segments); the gate touches the AOP
# or its DART on a board it does not qualify; a J713 or J604 refusal leaves it enabled.
HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "libfdt.h"
typedef uint32_t u32;
typedef unsigned long u64;
typedef struct { int unused; } asc_dev_t;
#define bail(...) do { printf(__VA_ARGS__); return -1; } while (0)
static void *dt;
static asc_dev_t the_asc;
static int touched, fail;
static bool running;
static asc_dev_t *asc_init(const char *path)
{ assert(!strcmp(path, "/arm-io/aop")); touched++; return fail == 1 ? NULL : &the_asc; }
static bool asc_cpu_running(asc_dev_t *asc) { return running; }
static void asc_free(asc_dev_t *asc) {}
static int dapf_init_t8132_halted(const char *path, int index)
{ assert(!strcmp(path, "/arm-io/dart-aop") && index == 1); touched++; return fail == 2 ? -1 : 0; }
static int dt_reserve_asc_segments(const char *adt_path, const char *alt, const char *path,
                                   bool remap, u64 base, bool dram_only)
{ assert(!strcmp(adt_path, "/arm-io/aop/iop-aop-nub") && remap && dram_only); touched++;
  return fail == 3 ? -1 : 0; }
static int dt_reserve_t8132_aop_windows(const char *path) { touched++; return fail == 4 ? -1 : 0; }
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
    int chosen = fdt_add_subnode(dt, 0, "chosen");
    if (requested)
        assert(!fdt_setprop(dt, chosen, "linux-enablement-mac,dapf-handoff",
                            "/arm-io/aop\0/arm-io/pmp", sizeof("/arm-io/aop\0/arm-io/pmp")));
    int aop = fdt_add_subnode(dt, 0, "aop@390600000");
    assert(aop >= 0 && !fdt_setprop_string(dt, aop, "compatible", "apple,t8132-aop"));
    assert(!fdt_setprop_string(dt, aop, "status", "okay"));
    touched = fail = 0;
    running = false;
}
static const char *status(void)
{
    return fdt_getprop(dt, fdt_path_offset(dt, "/aop@390600000"), "status", NULL);
}
int main(void)
{
    /* J713 and J604: a requested, halted AOP is handed over; any failed step disables it. */
    static const char *const qualified[] = {"apple,j713", "apple,j604"};
    for (unsigned i = 0; i < sizeof(qualified) / sizeof(*qualified); i++) {
        tree(qualified[i], true);
        assert(!dt_set_t8132_aop("/aop@390600000") && !strcmp(status(), "okay") && touched == 4);
        for (int f = 1; f <= 4; f++) {
            tree(qualified[i], true);
            fail = f;
            assert(!dt_set_t8132_aop("/aop@390600000") && !strcmp(status(), "disabled"));
        }
        tree(qualified[i], true);
        running = true;
        assert(!dt_set_t8132_aop("/aop@390600000") && !strcmp(status(), "disabled") && touched == 1);
        tree(qualified[i], false);
        assert(!dt_set_t8132_aop("/aop@390600000") && !strcmp(status(), "disabled") && !touched);
    }
    /* Other T8132 boards: the AOP ends disabled and neither it nor its DART is touched,
     * whether or not the tree asks for the handoff. */
    static const char *const others[] = {"apple,j623", "apple,j624", "apple,j715",
                                         "apple,j773g"};
    for (unsigned i = 0; i < sizeof(others) / sizeof(*others); i++)
        for (int requested = 0; requested <= 1; requested++) {
            tree(others[i], requested);
            assert(!dt_set_t8132_aop("/aop@390600000"));
            assert(!strcmp(status(), "disabled") && !touched);
        }
    return 0;
}
'''


def production():
    kboot = (ROOT / "src/kboot.c").read_text()
    return block(kboot, "static int dt_set_t8132_aop(") + "\n"


def test_t8132_aop_gate(tmp_path):
    run_c(tmp_path, "t8132-aop-gate",
          HARNESS + production() + MAIN)
