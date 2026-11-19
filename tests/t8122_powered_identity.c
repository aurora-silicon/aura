/* SPDX-License-Identifier: MIT */
#include <assert.h>

#include "../src/kboot_gpu_t8122_power.h"
#include "../src/libfdt/libfdt.h"

struct probe_fixture {
    int enable_result;
    int read_result;
    int disable_result;
    int enable_calls;
    int read_calls;
    int disable_calls;
    u32 id;
    u32 masks[3];
    u32 counts;
    u32 counts2;
};

static int enable(void *context)
{
    struct probe_fixture *fixture = context;
    fixture->enable_calls++;
    return fixture->enable_result;
}

static int read_id(void *context, u32 *id, u32 masks[3], u32 *counts, u32 *counts2)
{
    struct probe_fixture *fixture = context;
    fixture->read_calls++;
    *id = fixture->id;
    for (int i = 0; i < 3; i++)
        masks[i] = fixture->masks[i];
    *counts = fixture->counts;
    *counts2 = fixture->counts2;
    return fixture->read_result;
}

static int disable(void *context)
{
    struct probe_fixture *fixture = context;
    fixture->disable_calls++;
    return fixture->disable_result;
}

static const struct t8122_power_probe_ops ops = {
    .enable = enable,
    .read = read_id,
    .disable = disable,
};

static void check(struct probe_fixture fixture, enum t8122_identity_result expected,
                  int expected_reads)
{
    struct t8122_powered_identity result = t8122_power_probe_run(&ops, &fixture);
    assert(result.result == expected);
    assert(fixture.enable_calls == 1);
    assert(fixture.read_calls == expected_reads);
    assert(fixture.disable_calls == 1);
    /* ID_COUNTS_1 and ID_COUNTS_2 are reported as read, and never read without power */
    assert(result.id_counts_1 == (expected_reads ? fixture.counts : 0));
    assert(result.id_counts_2 == (expected_reads ? fixture.counts2 : 0));
}

int main(void)
{
    char dt[1024];
    assert(!fdt_create_empty_tree(dt, sizeof(dt)));
    assert(!t8122_power_probe_opted_in(dt));
    int chosen = fdt_add_subnode(dt, 0, "chosen");
    assert(chosen >= 0);
    assert(!t8122_power_probe_opted_in(dt));
    assert(!fdt_setprop_string(dt, chosen, "aurora,t8122-gpu-power-diag", "0"));
    assert(!t8122_power_probe_opted_in(dt));
    assert(!fdt_setprop_string(dt, chosen, "aurora,t8122-gpu-power-diag", "1"));
    assert(t8122_power_probe_opted_in(dt));
    assert(!fdt_setprop_string(dt, chosen, "aurora,t8122-gpu-power-diag", "1x"));
    assert(!t8122_power_probe_opted_in(dt));

    const struct probe_fixture eight_cores = {
        .id = (7u << 24) | (2u << 16) | (2u << 12),
        .masks = {0xff, 0, 0},
        .counts = 0x0001010a,
        .counts2 = 0x00040404,
    };
    check(eight_cores, T8122_ID_PASS, 1);

    /* ID_COUNTS_1 and ID_COUNTS_2 do not take part in the result */
    struct probe_fixture counts = eight_cores;
    counts.counts = 0;
    counts.counts2 = 0;
    check(counts, T8122_ID_PASS, 1);
    counts.counts = 0xffffffff;
    counts.counts2 = 0xffffffff;
    check(counts, T8122_ID_PASS, 1);
    counts.id = (7u << 24) | (3u << 16);
    check(counts, T8122_ID_MISMATCH, 1);

    struct probe_fixture case_ = eight_cores;
    case_.masks[0] = 0x3ff;
    check(case_, T8122_ID_PASS, 1);

    case_ = eight_cores;
    case_.enable_result = -1;
    check(case_, T8122_ID_POWER_FAILED, 0);

    case_ = eight_cores;
    case_.read_result = -1;
    check(case_, T8122_ID_READ_FAULT, 1);

    case_ = eight_cores;
    case_.id = (7u << 24) | (3u << 16);
    check(case_, T8122_ID_MISMATCH, 1);

    case_ = eight_cores;
    case_.masks[0] = 0x400;
    check(case_, T8122_ID_MISMATCH, 1);

    case_ = eight_cores;
    case_.disable_result = -1;
    check(case_, T8122_ID_POWER_OFF_FAILED, 1);

    case_ = eight_cores;
    case_.enable_result = -1;
    case_.disable_result = -1;
    check(case_, T8122_ID_POWER_OFF_FAILED, 0);
}
