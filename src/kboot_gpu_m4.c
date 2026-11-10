/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

#include "kboot.h"
#include "adt.h"
#include "firmware.h"
#include "soc.h"
#include "utils.h"

#include "libfdt/libfdt.h"

/* Resource publication only. Never start, stop, reset or power the GPU here. */
struct gpu_m4_range {
    u64 base;
    u64 size;
};

static bool valid_range(struct gpu_m4_range r)
{
    return r.base && r.size && !((r.base | r.size) & (SZ_16K - 1)) && r.base + r.size > r.base &&
           r.base + r.size <= (1ULL << 42);
}

static int read_exact(int node, const char *name, void *out, u32 size)
{
    u32 len;
    const void *value = adt_getprop(adt, node, name, &len);
    if (!value || len != size)
        return -1;
    memcpy(out, value, size);
    return 0;
}

static int read_range(int node, const char *name, struct gpu_m4_range *r)
{
    char prop[64];
    snprintf(prop, sizeof(prop), "%s-base", name);
    if (read_exact(node, prop, &r->base, sizeof(r->base)))
        return -1;
    snprintf(prop, sizeof(prop), "%s-size", name);
    if (read_exact(node, prop, &r->size, sizeof(r->size)))
        return -1;
    return valid_range(*r) ? 0 : -1;
}

static int reserve_range(void *dt, struct gpu_m4_range range)
{
    int count = fdt_num_mem_rsv(dt);
    if (count < 0)
        return count;
    for (int i = 0; i < count; i++) {
        u64 base, size;
        if (fdt_get_mem_rsv(dt, i, &base, &size))
            return -1;
        if (base + size >= base && base <= range.base && base + size >= range.base + range.size)
            return 0;
    }
    return fdt_add_mem_rsv(dt, range.base, range.size);
}

static int publish_gpu_m4(void *dt)
{
    int gpu = fdt_path_offset(dt, "gpu");
    if (gpu < 0)
        return 0; /* A framebuffer-only DT needs no GPU handoff. */
    /* The G16 handoff follows the 26.6.2 firmware, not the board; J713 as before. */
    if (chip_id != T8132 ||
        (fdt_node_check_compatible(dt, 0, "apple,j713") && os_firmware.version != V26_6_2) ||
        fdt_node_check_compatible(dt, gpu, "apple,agx-t8132"))
        return -1;

    /* A failed refresh must never leave a stale completed-handoff marker. */
    int ret = fdt_delprop(dt, gpu, "apple,g16-handoff-version");
    if (ret && ret != -FDT_ERR_NOTFOUND)
        return ret;

    int sgx_path[8], asc_path[8];
    int sgx = adt_path_offset_trace(adt, "/arm-io/sgx", sgx_path);
    int asc = adt_path_offset_trace(adt, "/arm-io/gfx-asc", asc_path);
    if (sgx < 0 || asc < 0)
        return -1;
    u64 base, size;
    if (adt_get_reg(adt, sgx_path, "reg", 0, &base, &size) < 0 || base != 0x300000000 ||
        size != 0x4000000 || adt_get_reg(adt, asc_path, "reg", 0, &base, &size) < 0 ||
        base != 0x302600000 || size != 0x88000)
        return -1;

    u32 irq[4];
    if (read_exact(asc, "interrupts", irq, sizeof(irq)) || irq[0] != 994 || irq[1] != 993 ||
        irq[2] != 996 || irq[3] != 995)
        return -1;

    const char *properties[] = {
        "gpu-region",
        "gfx-shared-region",
        "gfx-handoff",
        "gfx-shared-l2-region",
    };
    const char *paths[] = {
        "/reserved-memory/uat-ttbs",    "/reserved-memory/uat-pagetables",
        "/reserved-memory/uat-handoff", "/reserved-memory/uat-shared-l2",
        "/reserved-memory/gpu-fw-text", "/reserved-memory/gpu-fw-data",
    };
    struct gpu_m4_range ranges[6];
    for (unsigned i = 0; i < 4; i++)
        if (read_range(sgx, properties[i], &ranges[i]))
            return -1;

    u32 len;
    const char *names = adt_getprop(adt, asc, "segment-names", &len);
    if (!names || len != sizeof("__TEXT;__DATA") || memcmp(names, "__TEXT;__DATA", len))
        return -1;
    const struct adt_segment_ranges *segments = adt_getprop(adt, asc, "segment-ranges", &len);
    if (!segments || len != 2 * sizeof(*segments))
        return -1;

    fdt64_t vas[2];
    fdt32_t flags[2];
    for (unsigned i = 0; i < 2; i++) {
        ranges[4 + i] = (struct gpu_m4_range){segments[i].phys, segments[i].size};
        if (!valid_range(ranges[4 + i]) || segments[i].remap != segments[i].phys ||
            segments[i].unk != (i == 0 ? 1 : 0))
            return -1;
        fdt64_st(&vas[i], segments[i].iova);
        flags[i] = cpu_to_fdt32(segments[i].unk);
    }
    if (segments[0].iova != 0xfffffc0000000000 ||
        segments[1].iova != segments[0].iova + segments[0].size ||
        segments[1].iova + segments[1].size <= segments[1].iova)
        return -1;

    /* Boot addresses are observed, never copied from one successful boot. */
    for (unsigned i = 0; i < 6; i++) {
        for (unsigned j = 0; j < i; j++)
            if (ranges[i].base < ranges[j].base + ranges[j].size &&
                ranges[j].base < ranges[i].base + ranges[i].size)
                return -1;
        if (fdt_path_offset(dt, paths[i]) < 0)
            return -1;
    }

    for (unsigned i = 0; i < 6; i++) {
        /* Protect the firmware through intermediate loaders as well as Linux. */
        if (reserve_range(dt, ranges[i]))
            return -1;
        int node = fdt_path_offset(dt, paths[i]);
        fdt64_t reg[2];
        fdt64_st(&reg[0], ranges[i].base);
        fdt64_st(&reg[1], ranges[i].size);
        if (fdt_setprop(dt, node, "reg", reg, sizeof(reg)) ||
            fdt_setprop_empty(dt, node, "no-map") || fdt_setprop_string(dt, node, "status", "okay"))
            return -1;
    }
    gpu = fdt_path_offset(dt, "gpu");
    if (fdt_setprop(dt, gpu, "apple,firmware-segment-vas", vas, sizeof(vas)) ||
        fdt_setprop(dt, gpu, "apple,firmware-segment-flags", flags, sizeof(flags)) ||
        fdt_setprop_u32(dt, gpu, "apple,g16-handoff-version", 1))
        return -1;
    printf("FDT: GPU: published M4 G16 memory resources without starting the GPU\n");
    return 0;
}

int dt_set_gpu_m4(void *dt)
{
    int gpu = fdt_path_offset(dt, "gpu");
    if (chip_id != T8132 || gpu < 0)
        return 0;

    if (!publish_gpu_m4(dt))
        return 0;

    /* Native graphics is optional. Never admit a partially refreshed epoch,
     * but keep the firmware framebuffer and unrelated boot devices usable.
     * Reservations already published are conservative and may remain. */
    const char *properties[] = {"apple,g16-handoff-version", "apple,firmware-segment-vas",
                                "apple,firmware-segment-flags"};
    gpu = fdt_path_offset(dt, "gpu");
    for (unsigned i = 0; i < sizeof(properties) / sizeof(properties[0]); i++) {
        int ret = fdt_delprop(dt, gpu, properties[i]);
        if (ret && ret != -FDT_ERR_NOTFOUND)
            return ret;
    }
    if (fdt_setprop_string(dt, gpu, "status", "disabled") && fdt_nop_node(dt, gpu))
        return -1; /* A corrupt DT cannot safely describe the failed device. */
    printf("FDT: GPU: unqualified M4 handoff, disabling native GPU\n");
    return 0;
}
