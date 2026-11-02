# SPDX-License-Identifier: GPL-2.0-only
"""Exercise payload CPU setup with failures on older chips and T8140."""

from test_smp_timeouts import REPO, compile_and_run


def test_payload_handoff_policy_and_best_effort_mitigations(tmp_path):
    smp = (REPO / "src/smp.c").read_text()
    sync = smp[smp.index("int smp_call_sync0("):smp.index("bool smp_has_pending_calls(")]
    payload = (REPO / "src/payload.c").read_text()
    setup = payload[payload.index("        cpufreq_init();"):
                    payload.index("        for (size_t i = 0; i < chosen_cnt; i++)")]
    mitigations = (REPO / "src/mitigations.c").read_text()
    mitigations = mitigations[mitigations.index("int mitigations_perform("):]
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
typedef uint64_t u64;
typedef uint32_t u32;
#define MAX_CPUS 4
#define T8140 0x8140
#define T6050 0x6050
#define SMP_TIMEOUT -2
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define smp_call0(cpu, fn) smp_call4(cpu, fn, 0, 0, 0, 0)
static int chip_id, boot_cpu_idx, start_error, fail_mitigation_cpu, fail_tso_cpu, fail_mitigation_wait, fail_tso_wait, waiting_kind;
static int mitigation_calls, tso_calls, advertised_tso;
static bool enable_tso = true;
struct mitigation { const char *name; bool vulnerable, mitigated; };
static struct mitigation mitigations[] = {{"test", true, false}};
static void apply_mitigations(void) {}
static void do_enable_tso(void) {}
static void cpufreq_init(void) {}
static bool smp_is_alive(int cpu) { return cpu > 0; }
static int smp_start_secondaries(void) { return start_error; }
static int smp_call4(int cpu, void *fn, u64 a, u64 b, u64 c, u64 d)
{
    if (fn == apply_mitigations) {
        mitigation_calls++;
        waiting_kind = 1;
        return cpu == fail_mitigation_cpu ? -1 : 0;
    }
    tso_calls++;
    waiting_kind = 2;
    return cpu == fail_tso_cpu ? -1 : 0;
}
static int smp_wait(int cpu, u64 *retval)
{ return (waiting_kind == 1 ? cpu == fail_mitigation_wait : cpu == fail_tso_wait) ? -2 : 0; }
static int smp_wait_timed(int cpu, u64 *retval, u32 timeout_ms)
{ assert(timeout_ms == 5000); return smp_wait(cpu, retval); }
static void kboot_delete_chosen(const char *key) { advertised_tso = 0; }
static void kboot_set_chosen(const char *key, const char *value)
{ advertised_tso = value != NULL; }
''' + sync + mitigations + '\nstatic int prepare_cpus(void)\n{\n' + setup + r'''
    return 0;
boot_failed:
    return -1;
}
static void reset(void)
{
    start_error = fail_mitigation_cpu = fail_tso_cpu = 0;
    fail_mitigation_wait = fail_tso_wait = waiting_kind = 0;
    mitigation_calls = tso_calls = advertised_tso = 0;
}
int main(void)
{
    int chips[] = {0x8103, 0x6000, 0x8112, 0x6020, 0x8122, 0x6030, T8140, T6050};
    for (unsigned i = 0; i < ARRAY_SIZE(chips); i++) {
        chip_id = chips[i];
        reset();
        assert(prepare_cpus() == 0 && advertised_tso == 1);
        fail_tso_cpu = 1;
        assert(prepare_cpus() == (chip_id == T8140 ? -1 : 0));
        assert(!advertised_tso);
        reset(); fail_mitigation_cpu = 1;
        assert(prepare_cpus() == (chip_id == T8140 ? -1 : 0));
        assert(mitigation_calls == 3); /* Every live CPU is still attempted. */
        reset(); fail_tso_cpu = 1;
        assert(prepare_cpus() == (chip_id == T8140 ? -1 : 0));
        assert(!advertised_tso);
        assert(tso_calls == (chip_id == T8140 ? 1 : 3));
        reset(); fail_mitigation_wait = 1;
        assert(prepare_cpus() == -1);
        assert(mitigation_calls == 1);
        reset(); fail_tso_wait = 1;
        assert(prepare_cpus() == -1);
        assert(!advertised_tso);
        assert(tso_calls == 1);
        reset(); start_error = -1;
        assert(prepare_cpus() == (chip_id == T8140 || chip_id == T6050 ? -1 : 0));
    }
    return 0;
}
'''
    compile_and_run(tmp_path, "payload-cpu-policy", harness)


def test_chosen_deletion_preserves_other_properties(tmp_path):
    source = (REPO / "src/kboot.c").read_text()
    chosen = source[source.index("int kboot_set_chosen("):
                    source.index("int kboot_set_uboot(")]
    harness = r'''
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#define MAX_CHOSEN_PARAMS 4
static char *chosen_params[MAX_CHOSEN_PARAMS][2];
static bool chosen_is_default[MAX_CHOSEN_PARAMS];
static bool chosen_lost;
void kboot_delete_chosen(const char *name);
''' + chosen + r'''
int main(void)
{
    assert(kboot_set_chosen("first", "one") == 0);
    assert(kboot_set_chosen("apple,tso", "") == 1);
    assert(kboot_set_chosen("third", "three") == 2);
    assert(kboot_set_chosen("last", "four") == 3);
    kboot_delete_chosen("apple,tso");
    assert(!strcmp(chosen_params[0][0], "first"));
    assert(!strcmp(chosen_params[1][0], "third"));
    assert(!strcmp(chosen_params[1][1], "three"));
    assert(!strcmp(chosen_params[2][0], "last"));
    assert(!chosen_params[3][0] && !chosen_params[3][1]);
    kboot_delete_chosen("absent");
    kboot_delete_chosen(NULL);
    assert(kboot_set_chosen("apple,tso", "") == 3);
    kboot_delete_chosen("apple,tso");
    kboot_delete_chosen("first");
    assert(!strcmp(chosen_params[0][0], "third"));
    assert(!strcmp(chosen_params[1][0], "last"));
    assert(!chosen_params[2][0] && !chosen_params[3][0]);
    kboot_delete_chosen("third");
    kboot_delete_chosen("last");
    assert(!chosen_params[0][0]);
    return 0;
}
'''
    compile_and_run(tmp_path, "chosen-delete", harness)
