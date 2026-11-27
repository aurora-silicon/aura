/* SPDX-License-Identifier: MIT AND GPL-2.0-only */
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
 * description, firmware-compat) and the GPU is handed to Linux when this Mac
 * describes everything its start needs (see t8122_gpu_publish_27()); else the
 * GPU node stays disabled, which still keeps the loaded firmware and the UAT
 * tables out of the kernel's memory.
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
/* Keep the ADT property names together for publication and reservation fallback. */
#define UAT_REGION(name) {name, name "-base", name "-size"}
static const struct {
    const char *name, *base, *size;
} gpu26_uat[T8122_GPU_UAT] = {
    UAT_REGION("gpu-region"),
    UAT_REGION("gfx-shared-region"),
    UAT_REGION("gfx-handoff"),
    UAT_REGION("gfx-shared-l2-region"),
};
#undef UAT_REGION

/*
 * Which GPU the handoff is for: the name in the log, the /arm-io/sgx compatible to require (NULL:
 * any), the compatible of the DT GPU node, and the VA of the first firmware segment (0: take it
 * from the segment list; the others must then follow on from it).
 */
struct gpu26_target {
    const char *tag;
    const char *sgx_compat;
    const char *gpu_compat;
    u64 fw_va;
    const char *mbox_compat, *mbox_fallback;
    const char *handoff_version;
    int (*calibrate)(void *dt);
    bool fast_die;
    bool exact_regions;
    bool zero_data_flag_3seg; /* __DATA flags in the 3-segment layout (0 on G13, 0x10 on G14/G15) */
    bool sram_optional;
};

static const struct gpu26_target t8122_target = {
    .tag = "T8122",
    .sgx_compat = "gpu,t8122",
    .gpu_compat = "apple,agx-t8122",
    .fw_va = T8122_GPU_FW_VA,
    .mbox_compat = "apple,t8122-agx-asc-mailbox",
    .mbox_fallback = "apple,asc-mailbox-v4",
    .handoff_version = "apple,m3-handoff-version",
    .calibrate = dt_gpu_t8122_26_calibrate,
    .fast_die = true,
};

static const struct gpu26_target t6030_target = {
    .tag = "T6030",
    .sgx_compat = "gpu,t6030",
    .gpu_compat = "apple,agx-t6030",
    .fw_va = T8122_GPU_FW_VA,
    .mbox_compat = "apple,t6030-agx-asc-mailbox",
    .handoff_version = "apple,g16-handoff-version",
    .calibrate = dt_gpu_t6030_27_calibrate,
    .exact_regions = true,
};

struct t8122_gpu_fw {
    const struct gpu26_target *t;
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
    return ADT_GETPROP(adt, node, name, val) == sizeof(*val) ? 0 : -1;
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

static int gpu26_dram(const char *tag, u64 *start, u64 *end)
{
    int chosen = adt_path_offset(adt, "/chosen");
    u64 size;
    if (chosen < 0 || t8122_adt_u64(chosen, "dram-base", start) ||
        t8122_adt_u64(chosen, "dram-size", &size) || !size || *start + size <= *start)
        bail("ADT: GPU: %s DRAM bounds missing\n", tag);
    *end = *start + size;
    return 0;
}

static bool t8122_gpu_range_ok(u64 base, u64 size, u64 dram_start, u64 dram_end)
{
    return base && size && !((base | size) & (SZ_16K - 1)) && base + size > base &&
           base >= dram_start && base + size <= dram_end && base + size <= T8122_GPU_PA_MAX;
}

/* Validate the complete ADT description before anything touches the FDT. */
static int gpu26_read_adt(const struct gpu26_target *t, struct t8122_gpu_fw *fw)
{
    fw->t = t;
    int sgx = adt_path_offset(adt, "/arm-io/sgx");
    if (sgx < 0 || (t->sgx_compat && !t8122_adt_compatible(sgx, t->sgx_compat)))
        bail("ADT: GPU: %s /arm-io/sgx missing or not %s\n", t->tag,
             t->sgx_compat ? t->sgx_compat : "present");

    for (int i = 0; i < T8122_GPU_UAT; i++) {
        if (t8122_adt_u64(sgx, gpu26_uat[i].base, &fw->base[i]))
            bail("ADT: GPU: %s sgx %s missing\n", t->tag, gpu26_uat[i].base);
        if (t8122_adt_u64(sgx, gpu26_uat[i].size, &fw->size[i]))
            bail("ADT: GPU: %s sgx %s missing\n", t->tag, gpu26_uat[i].size);
    }

    int asc = adt_path_offset(adt, "/arm-io/gfx-asc");
    int nub = adt_path_offset(adt, "/arm-io/gfx-asc/iop-gfx-nub");
    if (asc < 0 || nub < 0)
        bail("ADT: GPU: %s gfx-asc or iop-gfx-nub missing\n", t->tag);

    u32 len, nub_len, preloaded;
    const struct adt_segment_ranges *seg = adt_getprop(adt, asc, "segment-ranges", &len);
    const void *nub_seg = adt_getprop(adt, nub, "segment-ranges", &nub_len);
    if (!seg || (len != 2 * sizeof(*seg) && len != 3 * sizeof(*seg)) || !nub_seg ||
        nub_len != len || memcmp(seg, nub_seg, len))
        bail("ADT: GPU: %s gfx-asc segment-ranges missing or unlike iop-gfx-nub\n", t->tag);
    int segments = len / sizeof(*seg);
    const char *layout = t8122_gpu_layouts[segments].names;
    const char *names = adt_getprop(adt, asc, "segment-names", &len);
    if (!names || len < strlen(layout) + 1 || memcmp(names, layout, strlen(layout) + 1))
        bail("ADT: GPU: %s gfx-asc segment-names is not %s\n", t->tag, layout);
    if (ADT_GETPROP(adt, nub, "pre-loaded", &preloaded) != sizeof(preloaded) || preloaded != 1)
        bail("ADT: GPU: %s GPU firmware is not pre-loaded\n", t->tag);

    u64 next_va = t->fw_va;
    for (int i = 0; i < segments; i++) {
        struct adt_segment_ranges s;
        memcpy(&s, &seg[i], sizeof(s));
        /* Without a fixed base, the segments only have to run on from the first one. */
        if (!i && !next_va && s.iova && !(s.iova & (SZ_16K - 1)))
            next_va = s.iova;
        u32 expected_flag = (segments == 3 && i == 1 && t->zero_data_flag_3seg)
                                ? 0
                                : t8122_gpu_layouts[segments].flags[i];
        if (s.phys != s.remap || !s.size || s.size & (SZ_16K - 1) || s.size > T8122_GPU_MAX_SEG ||
            s.unk != expected_flag)
            bail("ADT: GPU: %s gfx-asc segment %d is not a plain preloaded segment\n", t->tag, i);
        if (s.iova != next_va)
            bail("ADT: GPU: %s firmware segment %d at unexpected VA 0x%lx (expected 0x%lx)\n",
                 t->tag, i, s.iova, next_va);
        next_va += s.size;
        if (i < 2) {
            fw->base[T8122_GPU_UAT + i] = s.phys;
            fw->size[T8122_GPU_UAT + i] = s.size;
        } else if (s.phys != fw->base[T8122_GPU_UAT + 1] + fw->size[T8122_GPU_UAT + 1]) {
            bail("ADT: GPU: %s __DATA_SHARED_RO 0x%lx does not follow __DATA\n", t->tag, s.phys);
        } else {
            /* fw-data covers __DATA and the read-only tail after it. */
            u64 ro_base, ro_size, rw_size;
            if (t8122_adt_u64(sgx, "gfx-data-shared-ro-base", &ro_base) ||
                t8122_adt_u64(sgx, "gfx-data-shared-ro-size", &ro_size) || ro_base != s.phys ||
                ro_size != s.size)
                bail("ADT: GPU: %s sgx gfx-data-shared-ro does not match __DATA_SHARED_RO\n",
                     t->tag);
            if (!t8122_adt_u64(sgx, "gfx-data-shared-rw-size", &rw_size) && rw_size)
                bail("ADT: GPU: %s sgx has a gfx-data-shared-rw region (0x%lx)\n", t->tag, rw_size);
            fw->size[T8122_GPU_UAT + 1] += s.size;
        }
        fw->vas[i] = s.iova;
        fw->flags[i] = s.unk;
    }
    fw->segments = segments;

    u64 dram_start, dram_end;
    if (gpu26_dram(t->tag, &dram_start, &dram_end))
        return -1;
    for (int i = 0; i < T8122_GPU_REGIONS; i++) {
        if (!t8122_gpu_range_ok(fw->base[i], fw->size[i], dram_start, dram_end))
            bail("ADT: GPU: %s %s region 0x%lx+0x%lx is not a 16K-aligned DRAM range\n", t->tag,
                 t8122_gpu_names[i], fw->base[i], fw->size[i]);
        for (int j = 0; j < i; j++)
            if (fw->base[i] < fw->base[j] + fw->size[j] && fw->base[j] < fw->base[i] + fw->size[i])
                bail("ADT: GPU: %s %s region overlaps %s\n", t->tag, t8122_gpu_names[i],
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
 * node with the target's compatible. An alias to anything else is refused.
 */
static int gpu26_node(void *dt, const struct gpu26_target *t)
{
    int gpu = fdt_path_offset(dt, "gpu");
    if (gpu >= 0)
        return fdt_node_check_compatible(dt, gpu, t->gpu_compat) ? -FDT_ERR_BADVALUE : gpu;
    gpu = fdt_node_offset_by_compatible(dt, -1, t->gpu_compat);
    if (gpu >= 0 && fdt_node_offset_by_compatible(dt, gpu, t->gpu_compat) >= 0)
        return -FDT_ERR_BADVALUE;
    return gpu;
}

static int t8122_gpu_node(void *dt)
{
    return gpu26_node(dt, &t8122_target);
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
static int t8122_resv_overlap(void *dt, const char *tag, const char *self, bool self_any, u64 base,
                              u64 size, bool *covered)
{
    int resv = t8122_resv_node(dt), node, ret = 0;
    *covered = false;
    if (resv < 0)
        return -1;
    int self_node = fdt_subnode_offset(dt, resv, self);
    fdt_for_each_subnode(node, dt, resv)
    {
        const char *name = fdt_get_name(dt, node, NULL);
        const char *status = fdt_getprop(dt, node, "status", NULL);
        int len;
        const fdt64_t *reg = fdt_getprop(dt, node, "reg", &len);
        bool disabled = status && strcmp(status, "okay") && strcmp(status, "ok");
        if (disabled || !reg)
            continue;
        if (node == self_node &&
            (self_any || (len == 16 && fdt64_ld(reg) == base && fdt64_ld(reg + 1) == size)))
            continue;
        for (int i = 0; i + 16 <= len; i += 16, reg += 2) {
            u64 start = fdt64_ld(reg), end = start + fdt64_ld(reg + 1);
            if (start < base + size && base < end) {
                printf("FDT: GPU: %s range 0x%lx+0x%lx overlaps /reserved-memory/%s\n", tag, base,
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
            printf("FDT: GPU: %s range 0x%lx+0x%lx overlaps /memreserve/ 0x%lx+0x%lx\n", tag, base,
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
 * replace any of the same name. The G16 ABI takes exactly the six handoff
 * regions, so drop the disabled G15 initdata placeholders on that path.
 */
static int gpu26_link(void *dt, const struct gpu26_target *t, int gpu, const u32 *phandles)
{
    fdt32_t regions[T8122_GPU_MAX_MR];
    char names[512];
    int count = 0, used = 0, len = 0, names_len = 0;

    const fdt32_t *old = fdt_getprop(dt, gpu, "memory-region", &len);
    const char *old_names = fdt_getprop(dt, gpu, "memory-region-names", &names_len);
    int old_count = old ? len / 4 : 0;
    if ((old && len % 4) || (old_count && !old_names) || (!old && old_names))
        bail("FDT: GPU: %s GPU node memory-region lists are inconsistent\n", t->tag);

    int off = 0;
    for (int i = 0; i < old_count; i++) {
        if (off >= names_len)
            bail("FDT: GPU: %s GPU node has fewer memory-region-names than regions\n", t->tag);
        const char *name = old_names + off;
        int n = strnlen(name, names_len - off) + 1;
        if (off + n > names_len)
            bail("FDT: GPU: %s GPU node memory-region-names is not terminated\n", t->tag);
        off += n;
        bool ours = false;
        for (int j = 0; j < T8122_GPU_REGIONS; j++)
            ours |= !strcmp(name, t8122_gpu_names[j]);
        if (ours)
            continue;
        if (t->exact_regions) {
            int node = fdt_node_offset_by_phandle(dt, fdt32_ld(old + i));
            const char *status = node < 0 ? NULL : fdt_getprop(dt, node, "status", NULL);
            if (status && !strcmp(status, "disabled") &&
                (!strcmp(name, "hw-cal-a") || !strcmp(name, "hw-cal-b") ||
                 !strcmp(name, "globals")))
                continue;
            bail("FDT: GPU: %s unexpected GPU memory region %s\n", t->tag, name);
        }
        if (count >= T8122_GPU_MAX_MR - T8122_GPU_REGIONS || used + n > (int)sizeof(names) - 64)
            bail("FDT: GPU: %s GPU node has too many memory regions\n", t->tag);
        regions[count++] = old[i];
        memcpy(names + used, name, n);
        used += n;
    }
    if (old_names && off != names_len)
        bail("FDT: GPU: %s GPU node has more memory-region-names than regions\n", t->tag);
    for (int j = 0; j < T8122_GPU_REGIONS; j++) {
        regions[count++] = cpu_to_fdt32(phandles[j]);
        int n = strlen(t8122_gpu_names[j]) + 1;
        memcpy(names + used, t8122_gpu_names[j], n);
        used += n;
    }

    if (fdt_setprop(dt, gpu, "memory-region", regions, count * sizeof(fdt32_t)))
        bail("FDT: GPU: %s cannot set memory-region\n", t->tag);
    gpu = gpu26_node(dt, t);
    if (gpu < 0 || fdt_setprop(dt, gpu, "memory-region-names", names, used))
        bail("FDT: GPU: %s cannot set memory-region-names\n", t->tag);
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

/* The node a single-phandle property of the GPU node points at, or -1 */
static int gpu26_phandle_node(void *dt, const struct gpu26_target *t, const char *prop)
{
    int gpu = gpu26_node(dt, t), len = 0;
    const fdt32_t *ref = gpu < 0 ? NULL : fdt_getprop(dt, gpu, prop, &len);
    if (!ref || len != sizeof(*ref))
        return -1;
    int node = fdt_node_offset_by_phandle(dt, fdt32_ld(ref));
    return node < 0 ? -1 : node;
}

static int gpu26_publish_version(void *dt, const struct gpu26_target *t,
                                 const struct fw_version_info *image)
{
    int gpu = gpu26_node(dt, t);
    if (gpu < 0 || firmware_set_fdt(dt, gpu, "apple,firmware-compat", image) ||
        firmware_set_fdt(dt, gpu, "apple,firmware-version", image))
        bail("FDT: GPU: %s cannot set the 27.2 firmware version\n", t->tag);
    return 0;
}

/* The ADT cells Linux needs for its 27.2 start. FastDie is only supplied on T8122. */
static const struct {
    const char *path, *adt, *dt;
} gpu26_values[] = {
    {"/arm-io/sgx", "gpu-fast-die0-target", "apple,fast-die0-target"},
    {"/arm-io/gfx-asc/iop-gfx-nub", "coredump-copy-buffer", "apple,coredump-copy-buffer"},
};

/*
 * Enable only after the mailbox, power domain and start values are validated
 * and any bounded calibration has released its power vote. The caller owns the
 * FDT transaction.
 */
static int gpu26_enable(void *dt, void *arg)
{
    const struct gpu26_target *t = arg;
    int mailbox = gpu26_phandle_node(dt, t, "mboxes");
    if (mailbox < 0 ||
        (fdt_node_check_compatible(dt, mailbox, t->mbox_compat) &&
         (!t->mbox_fallback || fdt_node_check_compatible(dt, mailbox, t->mbox_fallback))))
        bail("FDT: GPU: %s 27.2: GPU mailbox is not an AGX ASC mailbox\n", t->tag);
    if (gpu26_phandle_node(dt, t, "power-domains") < 0)
        bail("FDT: GPU: %s 27.2: GPU node has no single power domain\n", t->tag);

    u32 values[ARRAY_SIZE(gpu26_values)];
    size_t first = t->fast_die ? 0 : 1;
    for (size_t i = first; i < ARRAY_SIZE(gpu26_values); i++) {
        int node = adt_path_offset(adt, gpu26_values[i].path);
        u32 len = 0;
        const void *value = node < 0 ? NULL : adt_getprop(adt, node, gpu26_values[i].adt, &len);
        if (!value)
            bail("ADT: GPU: %s 27.2: %s absent, GPU left disabled\n", t->tag, gpu26_values[i].adt);
        values[i] = 0;
        if (len == sizeof(values[i]))
            memcpy(&values[i], value, sizeof(values[i]));
        if (!values[i])
            bail("ADT: GPU: %s malformed %s (len %u)\n", t->tag, gpu26_values[i].adt, len);
    }
    if (t->calibrate) {
        int cal_ret = t->calibrate(dt);
        if (cal_ret) {
            printf("FDT: GPU: %s 27.2: calibration from this Mac's ADT failed\n", t->tag);
            return cal_ret == GPU26_ERR_FATAL ? GPU26_ERR_FATAL : -1;
        }
    }
    for (size_t i = first; i < ARRAY_SIZE(gpu26_values); i++) {
        int gpu = gpu26_node(dt, t);
        if (gpu < 0 || fdt_setprop_u32(dt, gpu, gpu26_values[i].dt, values[i]))
            bail("FDT: GPU: %s 27.2: cannot set %s\n", t->tag, gpu26_values[i].dt);
    }
    if (t->fast_die)
        printf("FDT: GPU: %s 27.2 machine values: fast-die0 target %u C, coredump copy buffer "
               "0x%x bytes\n",
               t->tag, values[0], values[1]);

    mailbox = gpu26_phandle_node(dt, t, "mboxes");
    if (mailbox < 0 || fdt_setprop_string(dt, mailbox, "status", "okay"))
        bail("FDT: GPU: %s 27.2: cannot enable the GPU mailbox\n", t->tag);
    /* Firmware idles GFX itself. Do not let pmgr auto-gate it under the firmware. */
    int domain = gpu26_phandle_node(dt, t, "power-domains");
    if (domain < 0 || fdt_setprop_empty(dt, domain, "apple,no-auto-pm"))
        bail("FDT: GPU: %s 27.2: cannot mark the GPU power domain apple,no-auto-pm\n", t->tag);
    int gpu = gpu26_node(dt, t);
    if (gpu < 0 || fdt_setprop_string(dt, gpu, "status", "okay"))
        bail("FDT: GPU: %s 27.2: cannot enable the GPU\n", t->tag);
    return 0;
}

/*
 * OS firmware 27.2: the reservations, links and segment description are in place; publish the
 * firmware version. The 25G83 profile switches do not apply. The GPU is handed to Linux when
 * this Mac describes everything its start needs: the GPU's mailbox is an AGX ASC mailbox, the GPU
 * has a single power domain, and the 26.6.2 path's calibration (OPPs and SRAM
 * voltages from this Mac's ADT, the bounded powered identity the kernel's admission checks, every
 * power vote released before Linux starts) succeeds under the M3 Air's chosen.asahi,t8122-gpu
 * switch, and this Mac's ADT has both 27.2 start values. Then the GPU node gets them
 * through gpu26_enable(), the mailbox is enabled, the GPU's power domain gets
 * apple,no-auto-pm (the 27.2 start takes no PMP link), and the GPU node is enabled; the kernel
 * admits and starts the loaded firmware. A failed power restore aborts DT preparation. Other
 * failures leave the GPU node and the mailbox disabled,
 * as before; then only with chosen.asahi,t8122-gpu-power-diag=1 the bounded powered identity probe
 * runs, with the SGX power voted off again before Linux starts; without it nothing more powers
 * the GPU.
 */
static int t8122_gpu_publish_27(void *dt, const struct t8122_gpu_fw *fw)
{
    if (gpu26_publish_version(dt, fw->t, &os_firmware))
        return -1;
    int gpu = t8122_gpu_node(dt);
    if (gpu < 0 || fdt_setprop_string(dt, gpu, "status", "disabled"))
        bail("FDT: GPU: T8122 cannot set GPU status\n");
    int ret = t8122_fdt_transaction(dt, gpu26_enable, (void *)fw->t);
    if (ret == GPU26_ERR_FATAL)
        return ret;
    if (!ret) {
        t8122_gpu_log(fw, "enabled (27.2 firmware, calibrated from this Mac's ADT)");
        return 0;
    }
    gpu = t8122_gpu_node(dt);
    if (gpu >= 0) {
        ret = t8122_read_powered_identity(dt, gpu);
        if (ret)
            return ret;
    }
    t8122_gpu_log(fw, "left disabled (27.2 firmware: the GPU start is not fully described)");
    return 0;
}

/*
 * Reserve the six handoff regions no-map and link them to the GPU node by name. Fails, before
 * writing anything, if another reservation claims any part of them.
 */
static int gpu26_publish_regions(void *dt, const struct t8122_gpu_fw *fw)
{
    const struct gpu26_target *t = fw->t;
    u32 phandles[T8122_GPU_REGIONS];
    char name[64];

    int gpu = gpu26_node(dt, t);
    if (gpu < 0)
        bail("FDT: GPU: %s DT has no %s GPU node (gpu alias)\n", t->tag, t->gpu_compat);
    if (t8122_resv_node(dt) < 0)
        bail("FDT: GPU: %s DT has no usable /reserved-memory\n", t->tag);

    /* Nothing else may already claim any part of the handoff. */
    for (int i = 0; i < T8122_GPU_REGIONS; i++) {
        bool covered;
        t8122_gpu_node_name(name, sizeof(name), i, fw->base[i]);
        if (t8122_resv_overlap(dt, t->tag, name, i < T8122_GPU_UAT, fw->base[i], fw->size[i],
                               &covered))
            bail("FDT: GPU: %s %s region conflicts with an existing reservation\n", t->tag,
                 t8122_gpu_names[i]);
    }
    for (int i = 0; i < T8122_GPU_REGIONS; i++) {
        t8122_gpu_node_name(name, sizeof(name), i, fw->base[i]);
        phandles[i] = t8122_gpu_reserve(dt, name, i >= T8122_GPU_UAT, fw->base[i], fw->size[i]);
        if (!phandles[i])
            bail("FDT: GPU: %s cannot reserve %s region\n", t->tag, t8122_gpu_names[i]);
    }

    gpu = gpu26_node(dt, t);
    if (gpu < 0 || gpu26_link(dt, t, gpu, phandles))
        return -1;
    return 0;
}

static int gpu26_publish_segments(void *dt, const struct t8122_gpu_fw *fw)
{
    const struct gpu26_target *t = fw->t;
    fdt64_t vas[T8122_GPU_MAX_SEGS];
    fdt32_t flags[T8122_GPU_MAX_SEGS];
    for (int i = 0; i < fw->segments; i++) {
        fdt64_st(&vas[i], fw->vas[i]);
        flags[i] = cpu_to_fdt32(fw->flags[i]);
    }
    int gpu = gpu26_node(dt, t);
    if (gpu < 0 || fdt_setprop_u32(dt, gpu, t->handoff_version, fw->segments - 1) ||
        fdt_setprop(dt, gpu, "apple,firmware-segment-vas", vas, fw->segments * sizeof(vas[0])) ||
        fdt_setprop(dt, gpu, "apple,firmware-segment-flags", flags,
                    fw->segments * sizeof(flags[0])))
        bail("FDT: GPU: %s cannot set the firmware segment description\n", t->tag);
    return 0;
}

static int t8122_gpu_publish(void *dt, void *arg)
{
    const struct t8122_gpu_fw *fw = arg;

    if (gpu26_publish_regions(dt, fw) || gpu26_publish_segments(dt, fw))
        return -1;

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
    int gpu = t8122_gpu_node(dt);
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

static int gpu26_reserve_range(void *dt, const struct gpu26_target *t,
                               const struct t8122_gpu_range *r, u64 dram_start, u64 dram_end)
{
    bool covered;
    char name[64];
    t8122_gpu_node_name(name, sizeof(name), r->index, r->base);
    if (!t8122_gpu_range_ok(r->base, r->size, dram_start, dram_end)) {
        printf("FDT: GPU: %s %s 0x%lx+0x%lx is unusable, not reserved\n", t->tag, r->what, r->base,
               r->size);
        return -1;
    }
    int overlap =
        t8122_resv_overlap(dt, t->tag, name, r->index < T8122_GPU_UAT, r->base, r->size, &covered);
    if (overlap) {
        printf("FDT: GPU: %s %s 0x%lx+0x%lx %s\n", t->tag, r->what, r->base, r->size,
               overlap < 0 ? "not reserved (no /reserved-memory)"
               : covered   ? "is already reserved"
                           : "partially overlaps another reservation; not reserved");
        return overlap > 0 && covered ? 0 : -1;
    }
    if (t8122_fdt_transaction(dt, t8122_gpu_reserve_one, (void *)r)) {
        printf("FDT: GPU: %s failed to reserve %s 0x%lx+0x%lx\n", t->tag, r->what, r->base,
               r->size);
        return -1;
    }
    printf("FDT: GPU: %s reserved %s 0x%lx+0x%lx (unlinked)\n", t->tag, r->what, r->base, r->size);
    return 0;
}

/* Fallback: reserve whatever the ADT describes, one range at a time. */
static int gpu26_reserve_all(void *dt, const struct gpu26_target *t)
{
    u64 dram_start = 0, dram_end = T8122_GPU_PA_MAX;
    int err = 0;
    if (gpu26_dram(t->tag, &dram_start, &dram_end)) {
        printf("ADT: GPU: %s reserving without DRAM bounds\n", t->tag);
        err = -1;
    }

    int sgx = adt_path_offset(adt, "/arm-io/sgx");
    if (sgx < 0)
        err = -1;
    for (int i = 0; sgx >= 0 && i < T8122_GPU_UAT; i++) {
        struct t8122_gpu_range r = {t8122_gpu_names[i], i, 0, 0};
        int missing = t8122_adt_u64(sgx, gpu26_uat[i].base, &r.base);
        missing |= t8122_adt_u64(sgx, gpu26_uat[i].size, &r.size);
        if (missing) {
            printf("ADT: GPU: %s sgx has no %s region\n", t->tag, gpu26_uat[i].name);
            err = -1;
        } else if (gpu26_reserve_range(dt, t, &r, dram_start, dram_end)) {
            err = -1;
        }
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
            printf("ADT: GPU: %s %s has no usable segment-ranges\n", t->tag, paths[p]);
            err = -1;
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
            if (gpu26_reserve_range(dt, t, &r, dram_start, dram_end))
                err = -1;
        }
    }
    return err;
}

static int gpu26_disable(void *dt, void *arg)
{
    const struct gpu26_target *t = arg;
    int gpu = gpu26_node(dt, t);
    return gpu < 0 ? 0 : fdt_setprop_string(dt, gpu, "status", "disabled");
}

int dt_set_gpu_t8122_26(void *dt)
{
    struct t8122_gpu_fw fw;

    printf("FDT: GPU: T8122 firmware handoff\n");
    if (!gpu26_read_adt(&t8122_target, &fw)) {
        int ret = t8122_fdt_transaction(dt, t8122_gpu_publish, &fw);
        if (!ret || ret == GPU26_ERR_FATAL)
            return ret;
    }

    printf("FDT: GPU: T8122 handoff not published; reserving the GPU firmware memory only\n");
    if (t8122_fdt_transaction(dt, gpu26_disable, (void *)&t8122_target))
        printf("FDT: GPU: T8122 cannot mark the GPU node disabled\n");
    return gpu26_reserve_all(dt, &t8122_target);
}

struct t6030_handoff {
    struct t8122_gpu_fw fw;
    const struct fw_version_info *image;
};

static int t6030_gpu_publish_27(void *dt, void *arg)
{
    const struct t6030_handoff *h = arg;
    const struct gpu26_target *t = h->fw.t;
    if (gpu26_publish_regions(dt, &h->fw) || gpu26_publish_segments(dt, &h->fw) ||
        gpu26_publish_version(dt, t, h->image))
        return -1;

    /* G16 reads its performance and CLPC data from kboot's original-ADT phram export. */
    int gpu = gpu26_node(dt, t);
    int ret = fdt_delprop(dt, gpu, "apple,pmp");
    if (ret && ret != -FDT_ERR_NOTFOUND)
        return ret;
    return gpu26_enable(dt, (void *)t);
}

int dt_set_gpu_t6030_27(void *dt, const struct fw_version_info *image)
{
    struct t6030_handoff h = {.image = image};
    int pub_ret = -1;
    if (image && image->version == V27_2 && !gpu26_read_adt(&t6030_target, &h.fw) &&
        h.fw.segments == 3) {
        pub_ret = t8122_fdt_transaction(dt, t6030_gpu_publish_27, &h);
        if (!pub_ret) {
            printf("FDT: GPU: T6030 firmware %s handoff published, GPU enabled\n", image->string);
            return 0;
        }
    }

    printf("FDT: GPU: T6030 handoff not published; reserving the GPU firmware memory only\n");
    int dis_ret = t8122_fdt_transaction(dt, gpu26_disable, (void *)&t6030_target);
    if (dis_ret)
        printf("FDT: GPU: T6030 cannot mark the GPU node disabled\n");
    int resv_ret = gpu26_reserve_all(dt, &t6030_target);
    if (pub_ret == GPU26_ERR_FATAL || dis_ret || resv_ret) {
        printf("FDT: GPU: T6030 handoff fallback failed (power=%d disable=%d reserve=%d)\n",
               pub_ret == GPU26_ERR_FATAL ? -1 : 0, dis_ret, resv_ret);
        return GPU26_ERR_FATAL;
    }
    return 0;
}

/*
 * G13G (T8103) and G14G (T8112) on firmware whose GPU interface the kernel starts itself (on 27.2,
 * Linux's g14_v27_2 backend). The caller has identified the image iBoot loaded
 * (firmware_gfx_version()) and has no initdata layout for it, and has already published the
 * operating points. The ADT must describe the same handoff as on T8122: four UAT regions in
 * /arm-io/sgx and the preloaded gfx-asc segments, the __DATA_SHARED_RO tail matching the sgx
 * gfx-data-shared-ro pair. Their firmware VAs are not fixed here: they must follow on from the
 * first segment's.
 */
static const struct gpu26_target g13g_target = {
    .tag = "G13G",
    .sgx_compat = "gpu,t8103",
    .gpu_compat = "apple,agx-g13g",
    .mbox_compat = "apple,asc-mailbox-v4",
    .zero_data_flag_3seg = true,
    .sram_optional = true,
};

static const struct gpu26_target g14g_target = {
    .tag = "G14G",
    .sgx_compat = "gpu,t8112",
    .gpu_compat = "apple,agx-g14g",
    .mbox_compat = "apple,asc-mailbox-v4",
};

struct g14g_handoff {
    struct t8122_gpu_fw fw;
    const struct fw_version_info *image;
    const void *sram;
    u32 sram_len;
};

static int g14g_gpu_publish(void *dt, void *arg)
{
    const struct g14g_handoff *h = arg;
    const char *tag = h->fw.t->tag;

    if (gpu26_publish_regions(dt, &h->fw))
        return -1;

    /*
     * On SoCs whose ADT carries perf-states-sram (T8112), the kernel's 27.2 backend reads each
     * operating point's SRAM voltage from /chosen/aurora-gpu-resources/sgx adt-perf-states-sram.
     * T8103 has no perf-states-sram in its ADT and uses apple,min-sram-microvolt instead.
     */
    if (h->sram && h->sram_len) {
        int chosen = fdt_path_offset(dt, "/chosen");
        int res = chosen < 0 ? chosen : fdt_subnode_offset(dt, chosen, "aurora-gpu-resources");
        if (chosen >= 0 && res == -FDT_ERR_NOTFOUND)
            res = fdt_add_subnode(dt, chosen, "aurora-gpu-resources");
        int sgx = res < 0 ? res : fdt_subnode_offset(dt, res, "sgx");
        if (res >= 0 && sgx == -FDT_ERR_NOTFOUND)
            sgx = fdt_add_subnode(dt, res, "sgx");
        if (sgx < 0 || fdt_setprop(dt, sgx, "adt-perf-states-sram", h->sram, h->sram_len))
            bail("FDT: GPU: %s cannot publish /chosen/aurora-gpu-resources/sgx\n", tag);
    }

    if (gpu26_publish_version(dt, h->fw.t, h->image) || gpu26_enable(dt, (void *)h->fw.t))
        return -1;

    /*
     * The firmware keeps running through suspend-to-idle, so pmgr must retain the GPU domain
     * across system sleep instead of gating it under the live ASC.
     */
    int domain = gpu26_phandle_node(dt, h->fw.t, "power-domains");
    if (domain < 0 || fdt_setprop_empty(dt, domain, "apple,sleep-retained"))
        bail("FDT: GPU: %s cannot mark the GPU power domain apple,sleep-retained\n", tag);

    for (int i = 0; i < T8122_GPU_REGIONS; i++)
        printf("FDT: GPU: %s %s: 0x%lx+0x%lx (no-map)\n", tag, t8122_gpu_names[i], h->fw.base[i],
               h->fw.size[i]);
    printf("FDT: GPU: %s firmware %s handoff published, GPU enabled\n", tag, h->image->string);
    return 0;
}

int dt_set_gpu_g14g_handoff(void *dt, const struct fw_version_info *image)
{
    int sgx = adt_path_offset(adt, "/arm-io/sgx");
    if (sgx < 0)
        return -1;

    const struct gpu26_target *target;
    if (adt_is_compatible(adt, sgx, g13g_target.sgx_compat))
        target = &g13g_target;
    else if (adt_is_compatible(adt, sgx, g14g_target.sgx_compat))
        target = &g14g_target;
    else
        return -1;

    struct g14g_handoff h = {.image = image};

    if (gpu26_read_adt(target, &h.fw))
        return -1;
    h.sram = adt_getprop(adt, sgx, "perf-states-sram", &h.sram_len);
    if (!target->sram_optional && (!h.sram || !h.sram_len))
        bail("ADT: GPU: %s perf-states-sram missing; handoff refused\n", target->tag);
    return t8122_fdt_transaction(dt, g14g_gpu_publish, &h);
}
