# SPDX-License-Identifier: MIT
"""Pending callbacks cannot alter the boot DT or publish a Linux entry point."""

from test_smp_timeouts import REPO, compile_and_run
from test_locked_dart_tables import function


def test_pending_calls_block_dt_preparation_and_kernel_publication(tmp_path):
    smp = (REPO / "src/smp.c").read_text()
    table = smp[smp.index("struct spin_table {"):smp.index("void *_reset_stack")]
    pending = function(smp, "smp_has_pending_calls")
    source = (REPO / "src/kboot.c").read_text()
    # The guard prefix is tested before the first DT free/edit. The rest of
    # prepare_dt is covered by the DT fixtures and cross-build, not this model.
    prepare = source[source.index("int kboot_prepare_dt("):]
    prepare = prepare[:prepare.index("    if (dt) {")] + "    return 123;\n}\n"
    boot = function(source, "kboot_boot")
    main = (REPO / "src/main.c").read_text()
    final_guard = main[main.index("next_stage:\n") + len("next_stage:\n"):]
    final_guard = final_guard[:final_guard.index('    printf("Preparing to run next stage')]
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
typedef uint64_t u64; typedef uint32_t u32;
#define MAX_CPUS 4
#define T8140 0x8140
#define T8142 0x8142
#define T8132 0x8132
#define T8152 0x8152
#define T6040 0x6040
#define T6041 0x6041
#define SZ_16K 16384
#define sysop(op) ((void)0)
#define bail(...) do { printf(__VA_ARGS__); return -1; } while (0)
''' + table + r'''
static struct spin_table spin_table[MAX_CPUS];
static struct { void *entry; u64 args[5]; bool restore_logo; bool monitor; } next_stage;
static void *dt = (void *)0x1000;
static int dt_bufsize = 0x10000;
static int chip_id, mode, hardware_calls;
static bool monitor_required(void) { return chip_id == T8140; }
static jmp_buf refused;
static _Noreturn void panic(const char *format, ...) { longjmp(refused, 1); }
/* The T8152 handoff has its own path and is not modelled here. */
static int kboot_boot_t8152(void *kernel) { return -1; }
static int mcc_enable_cache(void) { hardware_calls++; return 0; }
static void tunables_apply_static(void) {}
static void clk_init(void) { if (mode == 2) spin_table[1].target = 0x1234; }
static void usb_init(void) {}
static int pcie_init(void) { return 0; }
static int pcie_prepare_fdt(void *p) { return 0; }
static int pcie_handoff_fdt(void *p) { return 0; }
static int fdt_node_check_compatible(void *p, int node, const char *compatible) { return 1; }
static int dt_disable_j713_pcie(void) { return 0; }
static int dt_disable_t8140_pcie(void) { return 0; }
static int fdt_pack(void *p) { return 0; }
static u32 fdt_totalsize(void *p) { return 0x8000; }
static int dapf_init_fdt(void *p) { return 0; }
static int dapf_init_all(void) { return 0; }
static void pcie_shutdown(void) {}
static void smp_set_wfe_mode(bool value) {}
static int dt_t8140_recheck_display(void)
{ if (mode == 3) spin_table[1].target = 0x1234; return 0; }
''' + pending + prepare + boot + '\nstatic int main_stage_guard(void)\n{\n' + final_guard + r'''
    return 0;
}
int main(void)
{
    for (int i = 0; i < MAX_CPUS; i++) {
        memset(spin_table, 0, sizeof(spin_table));
        spin_table[i].target = 0x1234;
        spin_table[i].args[0] = 0x5678;
        struct spin_table saved[MAX_CPUS];
        memcpy(saved, spin_table, sizeof(saved));
        assert(smp_has_pending_calls());
        assert(!memcmp(saved, spin_table, sizeof(saved)));
        next_stage.entry = (void *)0xdead;
        assert(kboot_prepare_dt((void *)1) == -1 && !next_stage.entry);
        assert(!memcmp(saved, spin_table, sizeof(saved)));
        next_stage.entry = (void *)0xdead;
        if (!setjmp(refused)) {
            main_stage_guard();
            assert(!"pending callback passed the final main-stage guard");
        }
        assert(!next_stage.entry && !memcmp(saved, spin_table, sizeof(saved)));
    }
    int chips[] = {0x8103, 0x8112, 0x8122, 0x6030, T8140};
    for (unsigned i = 0; i < sizeof(chips) / sizeof(chips[0]); i++) {
        chip_id = chips[i];
        for (mode = 0; mode <= (chip_id == T8140 ? 3 : 2); mode++) {
            memset(spin_table, 0, sizeof(spin_table));
            hardware_calls = 0;
            next_stage.entry = (void *)0xdead;
            if (mode == 1) spin_table[2].target = 0x1234;
            int result = kboot_boot((void *)0xface);
            if (!mode) {
                assert(!result && next_stage.entry == (void *)0xface);
                assert(next_stage.args[0] == (u64)dt && !smp_has_pending_calls());
                assert(next_stage.monitor == monitor_required());
            } else {
                assert(result == -1 && !next_stage.entry && smp_has_pending_calls());
                assert(hardware_calls == (mode != 1));
            }
        }
    }
    memset(spin_table, 0, sizeof(spin_table));
    assert(!smp_has_pending_calls());
    assert(kboot_prepare_dt((void *)1) == 123);
    next_stage.entry = (void *)0xface;
    assert(main_stage_guard() == 0 && next_stage.entry == (void *)0xface);
    return 0;
}
'''
    compile_and_run(tmp_path, "smp-handoff-gate", harness)
