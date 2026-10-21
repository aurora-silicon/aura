/* SPDX-License-Identifier: MIT */

#include "kboot_gpu_t8122_power.h"
#include "libfdt/libfdt.h"

#define T8122_ID_GEN       7
#define T8122_ID_VARIANT   2
#define T8122_ID_CORE_MASK 0x3ffu

bool t8122_power_probe_opted_in(const void *dt)
{
    int chosen = fdt_path_offset(dt, "/chosen");
    int len = 0;
    const char *value = chosen < 0 ? NULL :
        fdt_getprop(dt, chosen, "asahi,t8122-gpu-power-diag", &len);
    return value && len == 2 && value[0] == '1' && value[1] == '\0';
}

struct t8122_powered_identity t8122_power_probe_run(const struct t8122_power_probe_ops *ops,
                                                     void *context)
{
    struct t8122_powered_identity identity = {.result = T8122_ID_POWER_FAILED};

    /* Disable even after a failed enable: PMGR can have made a partial vote. */
    if (!ops->enable(context)) {
        if (ops->read(context, &identity.id_version, identity.core_mask, &identity.id_counts_1,
                      &identity.id_counts_2)) {
            identity.result = T8122_ID_READ_FAULT;
        } else if ((identity.id_version >> 24) != T8122_ID_GEN ||
                   ((identity.id_version >> 16) & 0xff) != T8122_ID_VARIANT ||
                   !(identity.core_mask[0] & T8122_ID_CORE_MASK) ||
                   (identity.core_mask[0] & ~T8122_ID_CORE_MASK) ||
                   identity.core_mask[1] || identity.core_mask[2]) {
            identity.result = T8122_ID_MISMATCH;
        } else {
            identity.result = T8122_ID_PASS;
        }
    }
    if (ops->disable(context))
        identity.result = T8122_ID_POWER_OFF_FAILED;
    return identity;
}

int t8122_identity_node(void *dt)
{
    int chosen = fdt_path_offset(dt, "/chosen");
    if (chosen < 0 || fdt_subnode_offset(dt, chosen, "asahi,t8122-gpu-powered-identity") >= 0)
        return -1;
    int node = fdt_add_subnode(dt, chosen, "asahi,t8122-gpu-powered-identity");
    if (node < 0)
        return -1;
    if (fdt_setprop_u32(dt, node, "schema-version", 1) ||
        fdt_setprop_u32(dt, node, "probe-result", T8122_ID_NOT_RUN) ||
        fdt_setprop_u32(dt, node, "id-version", 0) || fdt_setprop_u32(dt, node, "core-mask-0", 0) ||
        fdt_setprop_u32(dt, node, "core-mask-1", 0) ||
        fdt_setprop_u32(dt, node, "core-mask-2", 0) ||
        fdt_setprop_u32(dt, node, "id-counts-1", 0) ||
        fdt_setprop_u32(dt, node, "id-counts-2", 0)) {
        fdt_del_node(dt, node);
        return -1;
    }
    return node;
}

int t8122_identity_set_u32(void *dt, int node, const char *name, u32 value)
{
    fdt32_t encoded = cpu_to_fdt32(value);
    return fdt_setprop_inplace(dt, node, name, &encoded, sizeof(encoded));
}

/* Full14 shares the observation schema, without the diagnostic power-off probe. */
int t8122_full_identity_prepare(void *dt)
{
    int chosen = fdt_path_offset(dt, "/chosen");
    int len = 0;
    const char *armed = chosen < 0 ? NULL : fdt_getprop(dt, chosen, "asahi,t8122-gpu", &len);
    if ((fdt_node_check_compatible(dt, 0, "apple,j613") &&
         fdt_node_check_compatible(dt, 0, "apple,j615")) ||
        fdt_node_check_compatible(dt, 0, "apple,t8122") || !armed || len != 2 || armed[0] != '1' ||
        armed[1] != '\0')
        return -1;
    return t8122_identity_node(dt);
}

static bool t8122_full_identity_valid(const struct t8122_powered_identity *identity)
{
    u32 id = identity->id_version, counts = identity->id_counts_1;
    u32 cores = __builtin_popcount(identity->core_mask[0]);
    return (id >> 24) == 7 && ((id >> 16) & 0xff) == 2 && ((id >> 8) & 0xff) == 0x20 &&
           ((counts >> 16) & 0xf) == 1 && ((counts >> 8) & 0xff) == 1 && (counts & 0xff) == 10 &&
           !(identity->core_mask[0] & ~T8122_ID_CORE_MASK) && !identity->core_mask[1] &&
           !identity->core_mask[2] && (cores == 8 || cores == 10);
}

/* Called only after full-start's successful PMGR vote and matching SGX window checks. */
int t8122_full_identity_read(t8122_identity_reader read, void *context,
                             struct t8122_powered_identity *identity)
{
    *identity = (struct t8122_powered_identity){.result = T8122_ID_NOT_RUN};
    if (read(context, &identity->id_version, identity->core_mask, &identity->id_counts_1,
             &identity->id_counts_2)) {
        identity->result = T8122_ID_READ_FAULT;
        return -1;
    }
    if (!t8122_full_identity_valid(identity)) {
        identity->result = T8122_ID_MISMATCH;
        return -1;
    }
    identity->result = T8122_ID_PASS;
    return 0;
}

int t8122_full_identity_publish(void *dt, const struct t8122_powered_identity *identity)
{
    int node = fdt_path_offset(dt, "/chosen/asahi,t8122-gpu-powered-identity");
    if (node < 0 || identity->result != T8122_ID_PASS || !t8122_full_identity_valid(identity))
        return -1;
    /* All properties were reserved before power-on. Publish PASS last. */
    if (t8122_identity_set_u32(dt, node, "id-version", identity->id_version) ||
        t8122_identity_set_u32(dt, node, "core-mask-0", identity->core_mask[0]) ||
        t8122_identity_set_u32(dt, node, "core-mask-1", identity->core_mask[1]) ||
        t8122_identity_set_u32(dt, node, "core-mask-2", identity->core_mask[2]) ||
        t8122_identity_set_u32(dt, node, "id-counts-1", identity->id_counts_1) ||
        t8122_identity_set_u32(dt, node, "id-counts-2", identity->id_counts_2) ||
        t8122_identity_set_u32(dt, node, "probe-result", identity->result))
        return -1;
    return 0;
}

void t8122_full_identity_invalidate(void *dt)
{
    int node = fdt_path_offset(dt, "/chosen/asahi,t8122-gpu-powered-identity");
    if (node >= 0)
        t8122_identity_set_u32(dt, node, "probe-result", T8122_ID_NOT_RUN);
}
