/* SPDX-License-Identifier: MIT */
#ifndef KBOOT_GPU_T8122_POWER_H
#define KBOOT_GPU_T8122_POWER_H

#include "types.h"

enum t8122_identity_result {
    T8122_ID_NOT_RUN,
    T8122_ID_PASS,
    T8122_ID_POWER_FAILED,
    T8122_ID_READ_FAULT,
    T8122_ID_MISMATCH,
    T8122_ID_POWER_OFF_FAILED,
};

struct t8122_powered_identity {
    enum t8122_identity_result result;
    u32 id_version;
    u32 core_mask[3];
    u32 id_counts_1; /* diagnostic reports it; full14 validates topology */
    u32 id_counts_2; /* reported only; not part of the result */
};

struct t8122_power_probe_ops {
    int (*enable)(void *context);
    int (*read)(void *context, u32 *id_version, u32 core_mask[3], u32 *id_counts_1,
                u32 *id_counts_2);
    int (*disable)(void *context);
};

struct t8122_powered_identity t8122_power_probe_run(const struct t8122_power_probe_ops *ops,
                                                    void *context);
bool t8122_power_probe_opted_in(const void *dt);

int t8122_identity_node(void *dt);
int t8122_identity_set_u32(void *dt, int node, const char *name, u32 value);
typedef int (*t8122_identity_reader)(void *context, u32 *id_version, u32 core_mask[3],
                                     u32 *id_counts_1, u32 *id_counts_2);
int t8122_full_identity_prepare(void *dt);
int t8122_full_identity_read(t8122_identity_reader read, void *context,
                             struct t8122_powered_identity *identity);
int t8122_full_identity_publish(void *dt, const struct t8122_powered_identity *identity);
void t8122_full_identity_invalidate(void *dt);

#endif
