/* SPDX-License-Identifier: MIT */

#include <malloc.h>

#include "adt.h"
#include "firmware.h"
#include "gfx_fw_image_info.h"
#include "kboot.h"
#include "kboot_gpu_t8122.h"

#include "libfdt/libfdt.h"

/*
 * The Air has no grounded GPU DT or hardware configuration yet. Preserve a
 * small, explicit set of iBoot's ADT metadata for Linux-side inspection before
 * anyone guesses resources from the Pro. These are raw little-endian ADT
 * properties, not Linux device bindings. Revision 4 also reads the short
 * RTKit version string at the fixed image-info header's bounded text offset.
 * Only identity fields are exported, with no GPU MMIO reads.
 *
 * No export unless chosen.asahi,t8122-gpu-diag=1. The caller selects T8122.
 * Build in a private FDT copy and publish only on success, including strings.
 * Failure is non-fatal to kboot. A pre-existing export is never overwritten.
 */
#define EXPORT_NAME "asahi,t8122-gpu-diagnostics"
#define EXPORT_PATH "/chosen/" EXPORT_NAME
#define MAX_PROPERTY_BYTES 32768u
#define MAX_TOTAL_BYTES 49152u

static const char *const nodes[] = {
    "arm-io", "sgx", "gfx-asc", "pmgr", "mapper-gfx-asc", "gfx-nub", "chosen", "root",
};
static const char *const paths[] = {
    "/arm-io", "/arm-io/sgx", "/arm-io/gfx-asc", "/arm-io/pmgr",
    "/arm-io/mapper-gfx-asc", "/arm-io/gfx-asc/iop-gfx-nub", "/chosen", "/",
};

/* Only named hardware-description fields, never arbitrary node contents. */
static const char *const properties[] = {
    "compatible", "chip-revision", "#address-cells", "#size-cells", "ranges",
    "reg", "interrupts", "interrupts-valid", "device", "devices", "ps-regs", "clock-gates",
    "AAPL,phandle", "iommu-parent", "gpu-num-perf-states", "gpu-perf-base-pstate",
    "gptbat-ready",
    "power-gates", "perf-state-count", "perf-state-table-count", "perf-states",
    "perf-states-sram", "gpu-core-count", "gpu-core-mask", "gpu-count",
    "gpu-num-mgpus", "gpu-num-cores", "gpu-num-frags",
    "gfx-handoff-base", "gfx-handoff-size", "gfx-shared-region-base",
    "gfx-shared-region-size", "gfx-shared-l2-region-base",
    "gfx-shared-l2-region-size", "gpu-region-base", "gpu-region-size",
};

/* Additive revision 2 fields. No wildcard property or firmware-memory copying. */
static const char *const segment_properties[] = {
    "segment-names", "segment-ranges", "pre-loaded",
};
static const char *const dram_properties[] = {"dram-base", "dram-size"};
static const char *const root_properties[] = {"#address-cells", "#size-cells"};
static const char *const tuning_properties[] = {
    "gpu-fast-die0-sensor-mask", "gpu-fast-die0-alarm-threshold", "gpu-fast-die0-target",
    "gpu-fast-die0-integral-gain", "gpu-fast-die0-proportional-gain", "gpu-sochot-temp",
    "gpu-se-tgt", "gpu-se-engagement-criteria", "gpu-se-inactive-threshold",
    "gpu-se-reset-criteria", "gpu-se-filter-time-constant", "gpu-se-filter-time-constant-1",
    "gpu-se-ki", "gpu-se-ki-1", "gpu-se-kp", "gpu-se-kp-1",
    "gpu-avg-power-filter-tc-ms", "gpu-avg-power-ki-only", "gpu-avg-power-kp",
    "gpu-avg-power-min-duty-cycle", "gpu-avg-power-target-filter-tc",
    "gpu-idleoff-standby-timer", "gpu-idleoff-min_conf",
    "gpu-perf-boost-ce-step", "gpu-perf-boost-min-util", "gpu-perf-filter-drop-threshold",
    "gpu-perf-filter-time-constant", "gpu-perf-filter-time-constant2",
    "gpu-perf-integral-gain", "gpu-perf-integral-gain2", "gpu-perf-integral-min-clamp",
    "gpu-perf-proportional-gain", "gpu-perf-proportional-gain2", "gpu-perf-tgt-utilization",
    "gpu-power-sample-period", "gpu-ppm-filter-time-constant-ms", "gpu-ppm-ki", "gpu-ppm-kp",
    "gpu-pwr-filter-time-constant", "gpu-pwr-integral-gain", "gpu-pwr-integral-min-clamp",
    "gpu-pwr-min-duty-cycle", "gpu-pwr-proportional-gain", "gpu-pwr-sample-period-aic-clks",
};

static int copy_properties(void *dt, int child, int in, const char *const *props,
                           size_t count, u32 *total)
{
    for (size_t j = 0; j < count; j++) {
        u32 size = 0;
        const void *value = adt_getprop(adt, in, props[j], &size);
        if (!value)
            continue;
        if (size > MAX_PROPERTY_BYTES || size > MAX_TOTAL_BYTES - *total)
            return -1;
        *total += size;
        char name[64] = "adt,";
        size_t n = strlen(props[j]);
        if (n >= sizeof(name) - 4)
            return -1;
        memcpy(name + 4, props[j], n + 1);
        if (fdt_setprop(dt, child, name, value, size))
            return -1;
    }
    return 0;
}
#define COPY_PROPS(props) \
    copy_properties(dt, child, in, props, sizeof(props) / sizeof((props)[0]), &total)

static int export_metadata(void *dt, int chosen)
{
    int out = fdt_add_subnode(dt, chosen, EXPORT_NAME);
    if (out < 0)
        return -1;
    if (fdt_setprop_u32(dt, out, "schema-version", 1) ||
        fdt_setprop_string(dt, out, "encoding", "raw-adt-little-endian") ||
        fdt_setprop_u32(dt, out, "exporter-revision", 4))
        goto fail;

    /* Unlike adt,* properties this is an ordinary big-endian FDT pair. */
    fdt64_t usable[] = {cpu_to_fdt64(kboot_ram_base()), cpu_to_fdt64(kboot_ram_size())};
    if (fdt_setprop(dt, out, "usable-memory", usable, sizeof(usable)))
        goto fail;

    u32 total = 0;
    for (unsigned int i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
        int in = adt_path_offset(adt, paths[i]);
        /* Missing optional nodes remain absent; sgx and gfx-asc are required. */
        if (in < 0) {
            if (i == 1 || i == 2)
                goto fail;
            continue;
        }
        int child = fdt_add_subnode(dt, out, nodes[i]);
        if (child < 0 || fdt_setprop_string(dt, child, "adt-path", paths[i]))
            goto fail;
        if (i < 5 && COPY_PROPS(properties))
            goto fail;
        if (i == 1 && COPY_PROPS(tuning_properties))
            goto fail;
        if ((i == 2 || i == 5) && COPY_PROPS(segment_properties))
            goto fail;
        if (i == 6 && COPY_PROPS(dram_properties))
            goto fail;
        if (i == 7 && COPY_PROPS(root_properties))
            goto fail;
    }
    struct gfx_fw_image_info image;
    if (firmware_gfx_image_info(&image)) {
        if (fdt_setprop(dt, out, "image-uuid", image.uuid, sizeof(image.uuid)) ||
            fdt_setprop_u32(dt, out, "image-patchbay-offset", image.patchbay_offset) ||
            fdt_setprop_u32(dt, out, "image-patchbay-size", image.patchbay_size) ||
            fdt_setprop_u32(dt, out, "image-text-size", image.text_size))
            goto fail;
        char version[GFX_FW_VERSION_MAX];
        if (firmware_gfx_rtkit_version(version) &&
            fdt_setprop_string(dt, out, "image-rtkit-version", version))
            goto fail;
    }
    if (fdt_setprop(dt, out, "complete", NULL, 0))
        goto fail;
    return 0;

fail:
    return -1;
}

int dt_gpu_t8122_diagnostics(void *dt)
{
    int chosen = fdt_path_offset(dt, "/chosen");
    int len = 0;
    const char *opt = chosen < 0 ? NULL :
        fdt_getprop(dt, chosen, "asahi,t8122-gpu-diag", &len);
    if (!opt || len != 2 || opt[0] != '1' || opt[1] != '\0')
        return 0;
    if (fdt_path_offset(dt, EXPORT_PATH) >= 0)
        return 0;

    size_t size = fdt_totalsize(dt);
    void *copy = malloc(size);
    if (!copy)
        return 0;
    memcpy(copy, dt, size);
    if (!export_metadata(copy, chosen))
        memcpy(dt, copy, size);
    free(copy);
    return 0;
}
