/* SPDX-License-Identifier: MIT */
#include "kboot.h"
#include "adt.h"
#include "firmware.h"
#include "malloc.h"
#include "soc.h"
#include "string.h"
#include "t8122_25g83.h"
#include "utils.h"

#include "libfdt/libfdt.h"
#define bail(...)                                                                                  \
    do {                                                                                           \
        printf(__VA_ARGS__);                                                                       \
        return -1;                                                                                 \
    } while (0)

/*
 * T8122 (M3) GPU firmware handoff.
 *
 * iBoot preloads the GFX ASC firmware and leaves the firmware's UAT page
 * tables, TTBAT and handoff page in RAM. Nothing else in aura reserves that
 * memory on T8122, so the OS could reuse pages the firmware still occupies.
 *
 * On the selected J613 25G83 path this reserves, no-map, the four UAT regions of
 * /arm-io/sgx and the __TEXT/__DATA segments of /arm-io/gfx-asc. When the ADT
 * description is complete and consistent and the DT has an apple,agx-t8122
 * GPU node, the six regions are also linked to that node by name (ttbs,
 * pagetables, handoff, shared-l2, fw-text, fw-data), together with the
 * segment description the M3 runtime expects: apple,m3-handoff-version,
 * apple,firmware-segment-vas and apple,firmware-segment-flags. The
 * publication runs on a copy of the FDT and is committed whole or not at all.
 *
 * The GPU node stays disabled unless the board is J613 (or, experimentally, a
 * J615 with chosen.asahi,j615-25g83-experimental=1) and the node carries the
 * exact profile and /chosen asahi,t8122-gpu=1 opt-in. Linux admits the loaded
 * firmware before starting it. This never fails the boot.
 *
 * The segment list decides the handoff layout, not the board or the release:
 * - two segments, __TEXT;__DATA with flags <1 0>: apple,m3-handoff-version 1;
 * - three segments, __TEXT;__DATA;__DATA_SHARED_RO with flags <1 0x10 0>, as
 *   iBoot loads the G15G firmware on macOS 27.2 (26B123): the read-only tail
 *   must follow __DATA directly in memory and match the sgx
 *   gfx-data-shared-ro-{base,size} pair (and gfx-data-shared-rw-size, if
 *   present, must be 0). fw-data then covers __DATA and the tail, and the
 *   description has three VAs and flags: apple,m3-handoff-version 2, which a
 *   version-1 kernel refuses before it maps anything.
 * On OS firmware 27.2 the handoff is published (reservations, links, segment
 * description, firmware-compat) with the GPU node disabled: no kernel has a
 * 27.2 G15G firmware interface yet, so there is nothing to enable. That still
 * keeps the loaded firmware and the UAT tables out of the kernel's memory.
 */
#define T8122_GPU_REGIONS  6
#define T8122_GPU_UAT      4
#define T8122_GPU_FW_VA    0xfffffc0000000000UL
#define T8122_GPU_MAX_SEG  0x200000UL
#define T8122_GPU_PA_MAX   (1UL << 42)
#define T8122_GPU_MAX_MR   32
#define T8122_GPU_OPT_IN   "asahi,t8122-gpu"
#define T8122_GPU_MAX_SEGS 3

static const char *const t8122_gpu_names[T8122_GPU_REGIONS] = {
    "ttbs", "pagetables", "handoff", "shared-l2", "fw-text", "fw-data",
};
/* Static reserved-memory nodes the M3 runtime also accepts (m3_board.rs). */
static const char *const t8122_gpu_nodes[T8122_GPU_UAT] = {
    "uat-ttbs",
    "uat-pagetables",
    "uat-handoff",
    "uat-pagetables-l2",
};
static const char *const t8122_gpu_adt_regions[T8122_GPU_UAT] = {
    "gpu-region",
    "gfx-shared-region",
    "gfx-handoff",
    "gfx-shared-l2-region",
};

struct t8122_gpu_fw {
    u64 base[T8122_GPU_REGIONS], size[T8122_GPU_REGIONS];
    u64 vas[T8122_GPU_MAX_SEGS];
    u32 flags[T8122_GPU_MAX_SEGS];
    int segments;
};

/* The two segment layouts iBoot uses for the G15G firmware (see the top of this file). */
static const struct {
    const char *names;
    u32 flags[T8122_GPU_MAX_SEGS];
} t8122_gpu_layouts[] = {
    [2] = {"__TEXT;__DATA", {1, 0}},
    [3] = {"__TEXT;__DATA;__DATA_SHARED_RO", {1, 0x10, 0}},
};

static int t8122_adt_u64(int node, const char *name, u64 *val)
{
    u32 len;
    const void *p = adt_getprop(adt, node, name, &len);
    if (!p || len != sizeof(*val))
        return -1;
    memcpy(val, p, sizeof(*val));
    return 0;
}

static bool t8122_adt_compatible(int node, const char *compat)
{
    u32 len;
    const char *p = adt_getprop(adt, node, "compatible", &len);
    for (u32 off = 0; p && off < len;) {
        u32 n = strnlen(p + off, len - off);
        if (n == strlen(compat) && !memcmp(p + off, compat, n))
            return true;
        off += n + 1;
    }
    return false;
}

static int t8122_gpu_dram(u64 *start, u64 *end)
{
    int chosen = adt_path_offset(adt, "/chosen");
    u64 size;
    if (chosen < 0 || t8122_adt_u64(chosen, "dram-base", start) ||
        t8122_adt_u64(chosen, "dram-size", &size) || !size || *start + size <= *start)
        bail("ADT: GPU: T8122 DRAM bounds missing\n");
    *end = *start + size;
    return 0;
}

static bool t8122_gpu_range_ok(u64 base, u64 size, u64 dram_start, u64 dram_end)
{
    return base && size && !((base | size) & (SZ_16K - 1)) && base + size > base &&
           base >= dram_start && base + size <= dram_end && base + size <= T8122_GPU_PA_MAX;
}

/* Validate the complete ADT description before anything touches the FDT. */
static int t8122_gpu_read_adt(struct t8122_gpu_fw *fw)
{
    int sgx = adt_path_offset(adt, "/arm-io/sgx");
    if (sgx < 0 || !t8122_adt_compatible(sgx, "gpu,t8122"))
        bail("ADT: GPU: T8122 /arm-io/sgx missing or not gpu,t8122\n");

    for (int i = 0; i < T8122_GPU_UAT; i++) {
        char prop[64];
        snprintf(prop, sizeof(prop), "%s-base", t8122_gpu_adt_regions[i]);
        if (t8122_adt_u64(sgx, prop, &fw->base[i]))
            bail("ADT: GPU: T8122 sgx %s missing\n", prop);
        snprintf(prop, sizeof(prop), "%s-size", t8122_gpu_adt_regions[i]);
        if (t8122_adt_u64(sgx, prop, &fw->size[i]))
            bail("ADT: GPU: T8122 sgx %s missing\n", prop);
    }

    int asc = adt_path_offset(adt, "/arm-io/gfx-asc");
    int nub = adt_path_offset(adt, "/arm-io/gfx-asc/iop-gfx-nub");
    if (asc < 0 || nub < 0)
        bail("ADT: GPU: T8122 gfx-asc or iop-gfx-nub missing\n");

    u32 len, nub_len, preloaded;
    const struct adt_segment_ranges *seg = adt_getprop(adt, asc, "segment-ranges", &len);
    const void *nub_seg = adt_getprop(adt, nub, "segment-ranges", &nub_len);
    if (!seg || (len != 2 * sizeof(*seg) && len != 3 * sizeof(*seg)) || !nub_seg ||
        nub_len != len || memcmp(seg, nub_seg, len))
        bail("ADT: GPU: T8122 gfx-asc segment-ranges missing or unlike iop-gfx-nub\n");
    int segments = len / sizeof(*seg);
    const char *layout = t8122_gpu_layouts[segments].names;
    const char *names = adt_getprop(adt, asc, "segment-names", &len);
    if (!names || len < strlen(layout) + 1 || memcmp(names, layout, strlen(layout) + 1))
        bail("ADT: GPU: T8122 gfx-asc segment-names is not %s\n", layout);
    const void *pre = adt_getprop(adt, nub, "pre-loaded", &len);
    if (!pre || len != sizeof(preloaded))
        bail("ADT: GPU: T8122 GPU firmware is not pre-loaded\n");
    memcpy(&preloaded, pre, sizeof(preloaded));
    if (preloaded != 1)
        bail("ADT: GPU: T8122 GPU firmware is not pre-loaded\n");

    u64 next_va = T8122_GPU_FW_VA;
    for (int i = 0; i < segments; i++) {
        struct adt_segment_ranges s;
        memcpy(&s, &seg[i], sizeof(s));
        if (s.phys != s.remap || !s.size || s.size & (SZ_16K - 1) || s.size > T8122_GPU_MAX_SEG ||
            s.unk != t8122_gpu_layouts[segments].flags[i])
            bail("ADT: GPU: T8122 gfx-asc segment %d is not a plain preloaded segment\n", i);
        if (s.iova != next_va)
            bail("ADT: GPU: T8122 firmware segment %d at unexpected VA 0x%lx (expected 0x%lx)\n", i,
                 s.iova, next_va);
        next_va += s.size;
        if (i < 2) {
            fw->base[T8122_GPU_UAT + i] = s.phys;
            fw->size[T8122_GPU_UAT + i] = s.size;
        } else if (s.phys != fw->base[T8122_GPU_UAT + 1] + fw->size[T8122_GPU_UAT + 1]) {
            bail("ADT: GPU: T8122 __DATA_SHARED_RO 0x%lx does not follow __DATA\n", s.phys);
        } else {
            /* fw-data covers __DATA and the read-only tail after it. */
            u64 ro_base, ro_size, rw_size;
            if (t8122_adt_u64(sgx, "gfx-data-shared-ro-base", &ro_base) ||
                t8122_adt_u64(sgx, "gfx-data-shared-ro-size", &ro_size) || ro_base != s.phys ||
                ro_size != s.size)
                bail("ADT: GPU: T8122 sgx gfx-data-shared-ro does not match __DATA_SHARED_RO\n");
            if (!t8122_adt_u64(sgx, "gfx-data-shared-rw-size", &rw_size) && rw_size)
                bail("ADT: GPU: T8122 sgx has a gfx-data-shared-rw region (0x%lx)\n", rw_size);
            fw->size[T8122_GPU_UAT + 1] += s.size;
        }
        fw->vas[i] = s.iova;
        fw->flags[i] = s.unk;
    }
    fw->segments = segments;

    u64 dram_start, dram_end;
    if (t8122_gpu_dram(&dram_start, &dram_end))
        return -1;
    for (int i = 0; i < T8122_GPU_REGIONS; i++) {
        if (!t8122_gpu_range_ok(fw->base[i], fw->size[i], dram_start, dram_end))
            bail("ADT: GPU: T8122 %s region 0x%lx+0x%lx is not a 16K-aligned DRAM range\n",
                 t8122_gpu_names[i], fw->base[i], fw->size[i]);
        for (int j = 0; j < i; j++)
            if (fw->base[i] < fw->base[j] + fw->size[j] && fw->base[j] < fw->base[i] + fw->size[i])
                bail("ADT: GPU: T8122 %s region overlaps %s\n", t8122_gpu_names[i],
                     t8122_gpu_names[j]);
    }
    return 0;
}

/* Run fn on a copy of the FDT and commit the copy only if fn succeeds. */
static int t8122_fdt_transaction(void *dt, int (*fn)(void *dt, void *arg), void *arg)
{
    int size = fdt_totalsize(dt), ret = -1;
    void *copy = malloc(size);
    if (copy && !fdt_open_into(dt, copy, size)) {
        ret = fn(copy, arg);
        if (!ret)
            memcpy(dt, copy, size);
    }
    free(copy);
    return ret;
}

/* The GPU node: the gpu alias (as for the other SoCs), else the only
 * apple,agx-t8122 node. An alias to anything else is refused.
 */
static int t8122_gpu_node(void *dt)
{
    int gpu = fdt_path_offset(dt, "gpu");
    if (gpu >= 0)
        return fdt_node_check_compatible(dt, gpu, "apple,agx-t8122") ? -FDT_ERR_BADVALUE : gpu;
    gpu = fdt_node_offset_by_compatible(dt, -1, "apple,agx-t8122");
    if (gpu >= 0 && fdt_node_offset_by_compatible(dt, gpu, "apple,agx-t8122") >= 0)
        return -FDT_ERR_BADVALUE;
    return gpu;
}

static int t8122_resv_node(void *dt)
{
    int resv = fdt_path_offset(dt, "/reserved-memory");
    if (resv < 0 || fdt_address_cells(dt, resv) != 2 || fdt_size_cells(dt, resv) != 2)
        return -1;
    return resv;
}

/* The /reserved-memory node that holds region i: the static uat-* node for the
 * UAT regions, asc-firmware@<phys> (as for the other ASCs) for the segments.
 */
static void t8122_gpu_node_name(char *name, size_t len, int i, u64 base)
{
    if (i < T8122_GPU_UAT)
        snprintf(name, len, "%s", t8122_gpu_nodes[i]);
    else
        snprintf(name, len, "asc-firmware@%lx", base);
}

/*
 * Whether base+size overlaps an enabled reserved-memory node or a /memreserve/
 * entry. The node `self`, which is about to be (re)written, does not count:
 * a static uat-* node whatever its old reg, an asc-firmware@ node only if it
 * already describes exactly this range. Sets *covered when an overlapping
 * entry contains the whole range. Returns 1 on overlap, 0 if free, -1 on error.
 */
static int t8122_resv_overlap(void *dt, const char *self, bool self_any, u64 base, u64 size,
                              bool *covered)
{
    int resv = t8122_resv_node(dt), node, ret = 0;
    *covered = false;
    if (resv < 0)
        return -1;
    fdt_for_each_subnode(node, dt, resv)
    {
        const char *name = fdt_get_name(dt, node, NULL);
        const char *status = fdt_getprop(dt, node, "status", NULL);
        int len;
        const fdt64_t *reg = fdt_getprop(dt, node, "reg", &len);
        bool disabled = status && strcmp(status, "okay") && strcmp(status, "ok");
        if (disabled || !reg)
            continue;
        if (name && !strcmp(name, self) &&
            (self_any || (len == 16 && fdt64_ld(reg) == base && fdt64_ld(reg + 1) == size)))
            continue;
        for (int i = 0; i + 16 <= len; i += 16, reg += 2) {
            u64 start = fdt64_ld(reg), end = start + fdt64_ld(reg + 1);
            if (start < base + size && base < end) {
                printf("FDT: GPU: T8122 range 0x%lx+0x%lx overlaps /reserved-memory/%s\n", base,
                       size, name);
                *covered |= start <= base && base + size <= end;
                ret = 1;
            }
        }
    }
    for (int i = 0; i < fdt_num_mem_rsv(dt); i++) {
        u64 start, len;
        if (fdt_get_mem_rsv(dt, i, &start, &len) == 0 && start < base + size &&
            base < start + len) {
            printf("FDT: GPU: T8122 range 0x%lx+0x%lx overlaps /memreserve/ 0x%lx+0x%lx\n", base,
                   size, start, len);
            *covered |= start <= base && base + size <= start + len;
            ret = 1;
        }
    }
    return ret;
}

/* Create or fill the no-map node for one region. Returns its phandle or 0. */
static u32 t8122_gpu_reserve(void *dt, const char *name, bool asc_mem, u64 base, u64 size)
{
    int resv = t8122_resv_node(dt);
    if (resv < 0)
        return 0;
    int node = fdt_subnode_offset(dt, resv, name);
    if (node < 0)
        node = fdt_add_subnode(dt, resv, name);
    if (node < 0)
        return 0;

    fdt64_t reg[2];
    fdt64_st(&reg[0], base);
    fdt64_st(&reg[1], size);
    if (fdt_setprop(dt, node, "reg", reg, sizeof(reg)) || fdt_setprop_empty(dt, node, "no-map") ||
        (asc_mem && fdt_setprop_string(dt, node, "compatible", "apple,asc-mem")) ||
        (fdt_getprop(dt, node, "status", NULL) && fdt_setprop_string(dt, node, "status", "okay")))
        return 0;

    u32 phandle = fdt_get_phandle(dt, node);
    if (!phandle &&
        (fdt_generate_phandle(dt, &phandle) || fdt_setprop_u32(dt, node, "phandle", phandle)))
        return 0;
    return phandle;
}

/*
 * Set memory-region/memory-region-names on the GPU node: entries the DT
 * already has under other names stay first, in order; the six handoff regions
 * replace any of the same name.
 */
static int t8122_gpu_link(void *dt, int gpu, const u32 *phandles)
{
    fdt32_t regions[T8122_GPU_MAX_MR];
    char names[512];
    int count = 0, used = 0, len = 0, names_len = 0;

    const fdt32_t *old = fdt_getprop(dt, gpu, "memory-region", &len);
    const char *old_names = fdt_getprop(dt, gpu, "memory-region-names", &names_len);
    int old_count = old ? len / 4 : 0;
    if ((old && len % 4) || (old_count && !old_names) || (!old && old_names))
        bail("FDT: GPU: T8122 GPU node memory-region lists are inconsistent\n");

    int off = 0;
    for (int i = 0; i < old_count; i++) {
        if (off >= names_len)
            bail("FDT: GPU: T8122 GPU node has fewer memory-region-names than regions\n");
        const char *name = old_names + off;
        int n = strnlen(name, names_len - off) + 1;
        if (off + n > names_len)
            bail("FDT: GPU: T8122 GPU node memory-region-names is not terminated\n");
        off += n;
        bool ours = false;
        for (int j = 0; j < T8122_GPU_REGIONS; j++)
            ours |= !strcmp(name, t8122_gpu_names[j]);
        if (ours)
            continue;
        if (count >= T8122_GPU_MAX_MR - T8122_GPU_REGIONS || used + n > (int)sizeof(names) - 64)
            bail("FDT: GPU: T8122 GPU node has too many memory regions\n");
        regions[count++] = old[i];
        memcpy(names + used, name, n);
        used += n;
    }
    if (old_names && off != names_len)
        bail("FDT: GPU: T8122 GPU node has more memory-region-names than regions\n");
    for (int j = 0; j < T8122_GPU_REGIONS; j++) {
        regions[count++] = cpu_to_fdt32(phandles[j]);
        int n = strlen(t8122_gpu_names[j]) + 1;
        memcpy(names + used, t8122_gpu_names[j], n);
        used += n;
    }

    if (fdt_setprop(dt, gpu, "memory-region", regions, count * sizeof(fdt32_t)))
        bail("FDT: GPU: T8122 cannot set memory-region\n");
    gpu = t8122_gpu_node(dt);
    if (gpu < 0 || fdt_setprop(dt, gpu, "memory-region-names", names, used))
        bail("FDT: GPU: T8122 cannot set memory-region-names\n");
    return 0;
}

static void t8122_gpu_log(const struct t8122_gpu_fw *fw, const char *state)
{
    for (int i = 0; i < T8122_GPU_REGIONS; i++)
        printf("FDT: GPU: T8122 %s: 0x%lx+0x%lx (no-map)\n", t8122_gpu_names[i], fw->base[i],
               fw->size[i]);
    if (fw->segments > 2)
        printf("FDT: GPU: T8122 firmware VAs 0x%lx/0x%lx/0x%lx; handoff version 2 published, GPU "
               "%s\n",
               fw->vas[0], fw->vas[1], fw->vas[2], state);
    else
        printf("FDT: GPU: T8122 firmware VAs 0x%lx/0x%lx; handoff published, GPU %s\n", fw->vas[0],
               fw->vas[1], state);
}

/*
 * OS firmware 27.2: the reservations, links and segment description are in place. Publish the
 * firmware version and leave the GPU node disabled; the mailbox is not enabled. The 25G83 profile
 * switches do not apply. Only with chosen.asahi,t8122-gpu-power-diag=1 the bounded powered
 * identity probe (the SGX ID and core-mask reads the kernel's T8122 admission checks) runs, with
 * the SGX power voted off again before Linux starts; without it nothing powers the GPU.
 */
static int t8122_gpu_publish_27(void *dt, const struct t8122_gpu_fw *fw)
{
    int gpu = t8122_gpu_node(dt);
    if (gpu < 0 || firmware_set_fdt(dt, gpu, "apple,firmware-compat", &os_firmware) ||
        firmware_set_fdt(dt, gpu, "apple,firmware-version", &os_firmware))
        bail("FDT: GPU: T8122 cannot set the 27.2 firmware version\n");
    gpu = t8122_gpu_node(dt);
    if (gpu < 0 || fdt_setprop_string(dt, gpu, "status", "disabled"))
        bail("FDT: GPU: T8122 cannot set GPU status\n");
    gpu = t8122_gpu_node(dt);
    if (gpu >= 0)
        t8122_read_powered_identity(dt, gpu);
    t8122_gpu_log(fw, "left disabled (no 27.2 G15G firmware interface in the kernel yet)");
    return 0;
}

static int t8122_gpu_publish(void *dt, void *arg)
{
    const struct t8122_gpu_fw *fw = arg;
    u32 phandles[T8122_GPU_REGIONS];
    char name[64];

    int gpu = t8122_gpu_node(dt);
    if (gpu < 0)
        bail("FDT: GPU: T8122 DT has no apple,agx-t8122 GPU node (gpu alias)\n");
    if (t8122_resv_node(dt) < 0)
        bail("FDT: GPU: T8122 DT has no usable /reserved-memory\n");

    /* Nothing else may already claim any part of the handoff. */
    for (int i = 0; i < T8122_GPU_REGIONS; i++) {
        bool covered;
        t8122_gpu_node_name(name, sizeof(name), i, fw->base[i]);
        if (t8122_resv_overlap(dt, name, i < T8122_GPU_UAT, fw->base[i], fw->size[i], &covered))
            bail("FDT: GPU: T8122 %s region conflicts with an existing reservation\n",
                 t8122_gpu_names[i]);
    }
    for (int i = 0; i < T8122_GPU_REGIONS; i++) {
        t8122_gpu_node_name(name, sizeof(name), i, fw->base[i]);
        phandles[i] = t8122_gpu_reserve(dt, name, i >= T8122_GPU_UAT, fw->base[i], fw->size[i]);
        if (!phandles[i])
            bail("FDT: GPU: T8122 cannot reserve %s region\n", t8122_gpu_names[i]);
    }

    gpu = t8122_gpu_node(dt);
    if (gpu < 0 || t8122_gpu_link(dt, gpu, phandles))
        return -1;

    fdt64_t vas[T8122_GPU_MAX_SEGS];
    fdt32_t flags[T8122_GPU_MAX_SEGS];
    for (int i = 0; i < fw->segments; i++) {
        fdt64_st(&vas[i], fw->vas[i]);
        flags[i] = cpu_to_fdt32(fw->flags[i]);
    }
    gpu = t8122_gpu_node(dt);
    if (gpu < 0 || fdt_setprop_u32(dt, gpu, "apple,m3-handoff-version", fw->segments - 1) ||
        fdt_setprop(dt, gpu, "apple,firmware-segment-vas", vas, fw->segments * sizeof(vas[0])) ||
        fdt_setprop(dt, gpu, "apple,firmware-segment-flags", flags,
                    fw->segments * sizeof(flags[0])))
        bail("FDT: GPU: T8122 cannot set the firmware segment description\n");

    if (os_firmware.version == V27_2)
        return t8122_gpu_publish_27(dt, fw);

    int chosen = fdt_path_offset(dt, "/chosen"), len;
    int dcp = fdt_path_offset(dt, "dcp");
    const fdt32_t *profile =
        dcp < 0 ? NULL : fdt_getprop(dt, dcp, "apple,j613-25g83-profile", &len);
    if (!profile || len != 4 || fdt32_ld(profile) != 1)
        return -1;
    const fdt32_t *compat = fdt_getprop(dt, dcp, "apple,firmware-compat", &len);
    if (!compat || len != 12 || fdt32_ld(compat) != 26 || fdt32_ld(compat + 1) != 6 ||
        fdt32_ld(compat + 2) != 2)
        return -1;
    const char *intent = chosen < 0 ? NULL : fdt_getprop(dt, chosen, "asahi,t8122-gpu", &len);
    bool board = t8122_25g83_board(dt);
    bool enable = board && os_firmware.version == V26_6_2 && intent && len == 2 &&
                  intent[0] == '1' && intent[1] == 0;
    if (!board && !fdt_node_check_compatible(dt, 0, "apple,j615"))
        printf("FDT: GPU: J615 25G83 is experimental: no handoff without "
               "chosen." T8122_25G83_J615_OPT_IN "=1\n");
    if (os_firmware.version != V26_6_2 || !board)
        return -1;
    if (fdt_node_check_compatible(dt, 0, "apple,j615") == 0)
        printf("FDT: GPU: J615 25G83 handoff (EXPERIMENTAL, J613 firmware profile)\n");
    int mailbox = -1;
    if (enable) {
        const fdt32_t *mbox = fdt_getprop(dt, gpu, "mboxes", &len);
        mailbox = mbox && len == 4 ? fdt_node_offset_by_phandle(dt, fdt32_ld(mbox)) : -1;
        if (mailbox < 0 || fdt_node_check_compatible(dt, mailbox, "apple,asc-mailbox-v4"))
            return -1;
        /* Passive mailbox provider; kernel alone admits and starts the firmware. */
        if (fdt_setprop_string(dt, mailbox, "status", "okay"))
            return -1;
    }
    if (enable && dt_gpu_t8122_26_calibrate(dt))
        return -1;
    gpu = t8122_gpu_node(dt);
    if (gpu < 0 || firmware_set_fdt(dt, gpu, "apple,firmware-compat", &os_firmware) ||
        firmware_set_fdt(dt, gpu, "apple,firmware-version", &os_firmware) ||
        fdt_setprop_u32(dt, gpu, "apple,j613-25g83-gpu-handoff", 1))
        return -1;
    if (fdt_setprop_string(dt, gpu, "status", enable ? "okay" : "disabled"))
        bail("FDT: GPU: T8122 cannot set GPU status\n");

    t8122_gpu_log(fw, enable ? "enabled (" T8122_GPU_OPT_IN ")" : "left disabled");
    return 0;
}

struct t8122_gpu_range {
    const char *what;
    int index;
    u64 base, size;
};

static int t8122_gpu_reserve_one(void *dt, void *arg)
{
    const struct t8122_gpu_range *r = arg;
    char name[64];
    t8122_gpu_node_name(name, sizeof(name), r->index, r->base);
    return t8122_gpu_reserve(dt, name, r->index >= T8122_GPU_UAT, r->base, r->size) ? 0 : -1;
}

static void t8122_gpu_reserve_range(void *dt, const struct t8122_gpu_range *r, u64 dram_start,
                                    u64 dram_end)
{
    bool covered;
    char name[64];
    t8122_gpu_node_name(name, sizeof(name), r->index, r->base);
    if (!t8122_gpu_range_ok(r->base, r->size, dram_start, dram_end)) {
        printf("FDT: GPU: T8122 %s 0x%lx+0x%lx is unusable, not reserved\n", r->what, r->base,
               r->size);
        return;
    }
    int overlap =
        t8122_resv_overlap(dt, name, r->index < T8122_GPU_UAT, r->base, r->size, &covered);
    if (overlap) {
        printf("FDT: GPU: T8122 %s 0x%lx+0x%lx %s\n", r->what, r->base, r->size,
               overlap < 0 ? "not reserved (no /reserved-memory)"
               : covered   ? "is already reserved"
                           : "partially overlaps another reservation; not reserved");
        return;
    }
    if (t8122_fdt_transaction(dt, t8122_gpu_reserve_one, (void *)r))
        printf("FDT: GPU: T8122 failed to reserve %s 0x%lx+0x%lx\n", r->what, r->base, r->size);
    else
        printf("FDT: GPU: T8122 reserved %s 0x%lx+0x%lx (unlinked)\n", r->what, r->base, r->size);
}

/* Fallback: reserve whatever the ADT describes, one range at a time. */
static void t8122_gpu_reserve_all(void *dt)
{
    u64 dram_start = 0, dram_end = T8122_GPU_PA_MAX;
    if (t8122_gpu_dram(&dram_start, &dram_end))
        printf("ADT: GPU: T8122 reserving without DRAM bounds\n");

    int sgx = adt_path_offset(adt, "/arm-io/sgx");
    for (int i = 0; sgx >= 0 && i < T8122_GPU_UAT; i++) {
        char prop[64];
        struct t8122_gpu_range r = {t8122_gpu_names[i], i, 0, 0};
        snprintf(prop, sizeof(prop), "%s-base", t8122_gpu_adt_regions[i]);
        int missing = t8122_adt_u64(sgx, prop, &r.base);
        snprintf(prop, sizeof(prop), "%s-size", t8122_gpu_adt_regions[i]);
        missing |= t8122_adt_u64(sgx, prop, &r.size);
        if (missing)
            printf("ADT: GPU: T8122 sgx has no %s region\n", t8122_gpu_adt_regions[i]);
        else
            t8122_gpu_reserve_range(dt, &r, dram_start, dram_end);
    }

    /* Both copies of the segment list, in case they disagree. */
    const char *paths[] = {"/arm-io/gfx-asc", "/arm-io/gfx-asc/iop-gfx-nub"};
    const void *first = NULL;
    u32 first_len = 0;
    for (int p = 0; p < 2; p++) {
        int node = adt_path_offset(adt, paths[p]);
        u32 len = 0;
        const struct adt_segment_ranges *seg =
            node < 0 ? NULL : adt_getprop(adt, node, "segment-ranges", &len);
        if (!seg || !len || len % sizeof(*seg) || len > 8 * sizeof(*seg)) {
            printf("ADT: GPU: T8122 %s has no usable segment-ranges\n", paths[p]);
            continue;
        }
        if (first && len == first_len && !memcmp(seg, first, len))
            continue;
        first = seg;
        first_len = len;
        for (u32 i = 0; i < len / sizeof(*seg); i++) {
            struct adt_segment_ranges s;
            memcpy(&s, &seg[i], sizeof(s));
            struct t8122_gpu_range r = {"firmware segment", T8122_GPU_UAT + (i ? 1 : 0), s.phys,
                                        ALIGN_UP((u64)s.size, SZ_16K)};
            t8122_gpu_reserve_range(dt, &r, dram_start, dram_end);
        }
    }
}

static int t8122_gpu_disable(void *dt, void *arg)
{
    UNUSED(arg);
    int gpu = t8122_gpu_node(dt);
    return gpu < 0 ? 0 : fdt_setprop_string(dt, gpu, "status", "disabled");
}

int dt_set_gpu_t8122_26(void *dt)
{
    struct t8122_gpu_fw fw;

    printf("FDT: GPU: T8122 firmware handoff\n");
    if (!t8122_gpu_read_adt(&fw) && !t8122_fdt_transaction(dt, t8122_gpu_publish, &fw))
        return 0;

    printf("FDT: GPU: T8122 handoff not published; reserving the GPU firmware memory only\n");
    if (t8122_fdt_transaction(dt, t8122_gpu_disable, NULL))
        printf("FDT: GPU: T8122 cannot mark the GPU node disabled\n");
    t8122_gpu_reserve_all(dt);
    return 0;
}
