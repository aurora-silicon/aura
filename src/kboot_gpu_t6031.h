/* SPDX-License-Identifier: MIT */
#ifndef KBOOT_GPU_T6031_H
#define KBOOT_GPU_T6031_H

#include "firmware.h"
#include "kboot_gpu_t8122_power.h"
#include "types.h"

/*
 * T6031 (M3 Max, G15C) GPU handoff helpers for kboot_gpu.c: the bounded powered identity read,
 * the exact restore of the GPU power closure around it, and the loaded firmware image's facts.
 */
#define T6031_GPU_IDENTITY      "asahi,t6031-gpu-powered-identity"
#define T6031_GPU_IMAGE         "asahi,t6031-gpu-image"
#define T6031_GPU_CLUSTERS      4
#define T6031_CORES_PER_CLUSTER 10
#define T6031_CORE_SLOTS        (T6031_GPU_CLUSTERS * T6031_CORES_PER_CLUSTER)

/*
 * The real PMGR devices pmgr_adt_power_enable(path) can switch: those its clock-gates name and
 * all their parents, a virtual device by its parents. Sorted so that every device comes before
 * its parents. `before` is each power state register as found, `after` as left.
 */
#define T6031_PS_MAX 8
struct t6031_ps {
    u64 addr;
    u32 before, after;
    u32 depth;
    const char *name; /* up to 16 characters, not always NUL-terminated: print with %.16s */
};

struct t6031_closure {
    int n;
    struct t6031_ps ps[T6031_PS_MAX];
};

/* ADT and PMGR table, then one guarded read of each register. 0, or -1 with nothing touched. */
int t6031_closure_get(const char *path, struct t6031_closure *c);
/* 0 if every device of the closure is now actually on (guarded reads) */
int t6031_closure_active(const struct t6031_closure *c);
/*
 * Put every register back as it was found, children first: the power state target, and the
 * automatic power management bit pmgr_set_mode() clears. 0 if each is back (`after` is filled).
 */
int t6031_closure_restore(struct t6031_closure *c);

/* The identity probe, with the same steps as t8122_power_probe_run() */
struct t6031_probe_ops {
    int (*enable)(void *ctx);
    int (*active)(void *ctx);
    int (*read)(void *ctx, u32 *id_version, u32 core_mask[3], u32 *id_counts_1, u32 *id_counts_2);
    int (*restore)(void *ctx);
};
struct t8122_powered_identity t6031_probe_run(const struct t6031_probe_ops *ops, void *ctx);

/* G15C: family 7, variant 4, one die of 4 clusters, 1 to 40 cores in bits 0-39 of the masks */
bool t6031_identity_valid(const struct t8122_powered_identity *id);
/* Active cores per cluster (10-bit fields, as drm/asahi unpacks them); returns the total */
u32 t6031_core_counts(const struct t8122_powered_identity *id, u32 counts[T6031_GPU_CLUSTERS]);

/*
 * /chosen/asahi,t6031-gpu-powered-identity, the T8122 layout plus the power closure's registers:
 * reserved with probe-result 0 (not run) before any power-on, so that publishing never grows the
 * device tree, then filled with probe-result written last.
 */
int t6031_identity_node(void *dt, int nps);
int t6031_identity_publish(void *dt, const struct t8122_powered_identity *id,
                           const struct t6031_closure *c);

/*
 * /chosen/asahi,t6031-gpu-image: the loaded GPU image's header facts, DRAM reads only. Returns the
 * 14.8.3 firmware version if the image is of the 14.8.3 family (see the .c file), else NULL.
 */
const struct fw_version_info *dt_t6031_gpu_image(void *dt);

/* Shared G15C measurement schema, with a SoC-specific chosen node name. */
int t6031_identity_node_named(void *dt, int nps, const char *node_name);
int t6031_identity_publish_named(void *dt, const struct t8122_powered_identity *id,
                                 const struct t6031_closure *c, const char *node_name);
const struct fw_version_info *dt_t6031_gpu_image_named(void *dt, const char *node_name);

#endif
