# SPDX-License-Identifier: MIT
"""kboot_boot() starts the OS only on a tree kboot_prepare_dt() finished.

A preparation that gives up part way (a display handoff it refuses, say) leaves an incomplete tree
behind. The proxy's P_KBOOT_BOOT is a separate request from P_KBOOT_PREPARE_DT, so a host can ask
for a boot after a failed preparation: kboot_boot() must refuse it, before it touches the
hardware or publishes an entry point, and the proxy reply must say so. A later preparation that
succeeds boots normally, and a failed one after it does not boot the earlier tree.
"""

from test_locked_dart_tables import function
from test_smp_timeouts import REPO, compile_and_run


def test_boot_after_failed_preparation_is_refused(tmp_path):
    source = (REPO / "src/kboot.c").read_text()
    # Preparation: its own prologue, then either a failure part way or its own success tail
    prepare = source[source.index("int kboot_prepare_dt("):]
    prologue = prepare[:prepare.index("    if (dt) {")]
    tail_start = prepare.index("    dt_retained_live_dcp = local_dcp || inherited_dcp;\n")
    tail = prepare[tail_start:prepare.index("    return 0;\n}\n", tail_start)]
    assert "dt_ready = true;" in tail
    boot = function(source, "kboot_boot")
    proxy = (REPO / "src/proxy.c").read_text()
    case = proxy[proxy.index("        case P_KBOOT_BOOT:\n"):]
    case = case[:case.index("            break;\n") + len("            break;\n")]
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint64_t u64; typedef uint32_t u32;
#define T8140 0x8140
#define T8142 0x8142
#define T8132 0x8132
#define T8152 0x8152
#define T6040 0x6040
#define T6041 0x6041
#define T6050 0x6050
#define SZ_16K 16384
#define S_OK 0
#define S_ERROR -2
#define P_KBOOT_BOOT 0x700
#define bail(...) do { printf(__VA_ARGS__); return -1; } while (0)
static struct { void *entry; u64 args[5]; bool restore_logo; } next_stage;
static void *dt = (void *)0x1000;
static int dt_bufsize = 0x10000;
static bool dt_ready, dt_retained_live_dcp;
static int chip_id = T8132, hardware_calls, prepare_fails;
static bool smp_has_pending_calls(void) { return false; }
static bool ctxr_exec_os_may_start(void) { return true; }
static int kboot_move_to_exec_window(void **kernel) { return 0; }
static void *kboot_hibernate_handoff_dt(void) { return dt; }
static int kboot_boot_t8152(void *kernel) { abort(); }
static int display_prepare_live_handoff(void) { return 0; }
static bool display_needs_live_receiver(void) { return false; }
static int mcc_enable_cache(void) { hardware_calls++; return 0; }
static void tunables_apply_static(void) { hardware_calls++; }
static void clk_init(void) { hardware_calls++; }
static void usb_init(void) { hardware_calls++; }
static int pcie_init(void) { hardware_calls++; return 0; }
static bool pcie_radio_board_fdt(void *p) { return false; }
static int pcie_prepare_fdt(void *p) { hardware_calls++; return 0; }
static int pcie_handoff_fdt(void *p) { return 0; }
static int pcie_t6050_handoff_fdt(void *p) { abort(); }
static int pcie_apcie_tables_fdt(void *p, int capacity) { return 0; }
static void pcie_set_os_restores_apcie(bool restores) {}
static int fdt_node_check_compatible(void *p, int node, const char *compatible) { return 1; }
static int dt_disable_j713_pcie(void) { return 0; }
static int dt_disable_t8140_pcie(void) { return 0; }
static int fdt_pack(void *p) { return 0; }
static u32 fdt_totalsize(void *p) { return 0x8000; }
static int dapf_init_fdt(void *p) { return 0; }
static int dapf_init_all(void) { return 0; }
static int dt_set_ave_dapf(void) { return 0; }
static void pcie_shutdown(void) {}
static void smp_set_wfe_mode(bool value) {}
static int board_id;
static void *adt;
static bool adt_is_compatible(void *tree, int node, const char *compatible) { return false; }
static int fdt_open_into(void *from, void *to, int size) { return -1; }
static int fdt_node_offset_by_compatible(void *p, int start, const char *compatible) { return -1; }
static int fdt_setprop_string(void *p, int node, const char *name, const char *value) { return -1; }
static bool dapf_j614s_aop_ready(void) { return false; }
static bool dapf_j614s_isp_ready(void) { return false; }
static int dt_finish_j614s_aop(void *tree, bool ready) { return -1; }
static int dt_finish_j614s_isp(bool ready) { return -1; }
static int dt_t8140_recheck_display(void) { return 0; }
/* kboot_prepare_dt(): its prologue, then a refusal part way, or its own success tail */
static int prepare_rest(void)
{
    bool local_dcp = false, inherited_dcp = false;
    if (prepare_fails)
        return -1;
''' + tail + r'''    return 0;
}
''' + prologue + "    return prepare_rest();\n}\n" + boot + r'''
struct request { u64 type; u64 args[4]; };
struct reply { u64 type; int64_t status; u64 retval; };
/* The proxy's P_KBOOT_BOOT request, as proxy.c handles it */
static int proxy_boot(struct request *request, struct reply *reply)
{
    reply->status = S_OK;
    reply->retval = 0;
    switch (request->type) {
''' + case + r'''    }
    return 0;
}
int main(void)
{
    struct request req = {P_KBOOT_BOOT, {0xface}};
    struct reply rep;

    /* A preparation that gave up part way: no boot, no hardware touched, the error reported */
    prepare_fails = 1;
    assert(kboot_prepare_dt((void *)1) == -1);
    next_stage.entry = (void *)0xdead;
    assert(kboot_boot((void *)0xface) == -1 && !next_stage.entry && !hardware_calls);
    assert(proxy_boot(&req, &rep) == 0 && rep.status == S_ERROR && !next_stage.entry);
    assert(!hardware_calls);

    /* A preparation that succeeds: it boots */
    prepare_fails = 0;
    assert(kboot_prepare_dt((void *)1) == 0);
    assert(proxy_boot(&req, &rep) == 1 && next_stage.entry == (void *)0xface);
    assert(next_stage.args[0] == (u64)dt);

    /* A failed preparation after it does not boot the earlier tree */
    prepare_fails = 1;
    hardware_calls = 0;
    assert(kboot_prepare_dt((void *)1) == -1);
    next_stage.entry = (void *)0xdead;
    assert(kboot_boot((void *)0xface) == -1 && !next_stage.entry && !hardware_calls);
    return 0;
}
'''
    compile_and_run(tmp_path, "kboot-prepared-dt", harness)
