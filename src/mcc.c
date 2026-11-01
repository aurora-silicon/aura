/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#include "mcc.h"
#include "adt.h"
#include "hv.h"
#include "memory.h"
#include "string.h"
#include "utils.h"

static bool mcc_initialized = false;

#define MAX_MCC_INSTANCES 16

#define T8103_PLANES       4
#define T8103_PLANE_STRIDE 0x40000
#define T8103_DCS_STRIDE   0x40000

#define T6000_PLANES        4
#define T6000_PLANE_OFFSET  0
#define T6000_PLANE_STRIDE  0x40000
#define T6000_GLOBAL_OFFSET 0x100000
#define T6000_DCS_OFFSET    0x200000
#define T6000_DCS_STRIDE    0x200000
#define T6000_DCS_COUNT     4

#define T6031_PLANE_OFFSET  0
#define T6031_PLANE_STRIDE  0x40000
#define T6031_GLOBAL_OFFSET 0x100000
#define T6031_DCS_OFFSET    0x400000
#define T6031_DCS_STRIDE    0x200000

#define T8142_PLANES     4
#define T8142_DCS_OFFSET 0x1000000

#define PLANE_TZ_MAX_REGS 4

struct tz_regs {
    u32 count;
    u32 stride;
    u32 start;
    u32 end;
    u32 enable;
};

struct tz_regs t8103_tz_regs = {
    .count = 4,
    .stride = 0x10,
    .start = 0x6a0,
    .end = 0x6a4,
    .enable = 0x6a8,
};

struct tz_regs t8122_tz_regs = {
    .count = 4,
    .stride = 0x14,
    .start = 0x6cc,
    .end = 0x6d0,
    .enable = 0x6d8,
};

struct tz_regs t602x_tz_regs = {
    .count = 4,
    .stride = 0x14,
    .start = 0x6bc,
    .end = 0x6c0,
    .enable = 0x6c8,
};

struct tz_regs t6030_tz_regs = {
    .count = 4,
    .stride = 0x14,
    .start = 0x6dc,
    .end = 0x6e0,
    .enable = 0x6e8,
};

struct tz_regs t6031_tz_regs = {
    .count = 4,
    .stride = 0x14,
    .start = 0x6d8,
    .end = 0x6dc,
    .enable = 0x6e4,
};

struct tz_regs t8142_tz_regs = {
    .count = 4,
    .stride = 0x4,
    .start = 0xaf0,
    .end = 0xb00,
    .enable = 0xb20,
};

#define PLANE_CACHE_ENABLE 0x1c00
#define PLANE_CACHE_STATUS 0x1c04

#define T8142_PLANE_CACHE_ENABLE      0x2800
#define T8142_PLANE_CACHE_STATUS      0x2804
#define T8142_CACHE_DISABLE           0x82c
#define T8142_CACHE_STATUS_DATA_COUNT GENMASK(30, 24)
#define T8142_CACHE_STATUS_TAG_COUNT  GENMASK(14, 8)

#define T8103_CACHE_STATUS_DATA_COUNT GENMASK(14, 10)
#define T8103_CACHE_STATUS_TAG_COUNT  GENMASK(9, 5)

#define T6000_CACHE_STATUS_DATA_COUNT GENMASK(13, 9)
#define T6000_CACHE_STATUS_TAG_COUNT  GENMASK(8, 4)

#define T6000_CACHE_WAYS        12
#define T6000_CACHE_STATUS_MASK (T6000_CACHE_STATUS_DATA_COUNT | T6000_CACHE_STATUS_TAG_COUNT)
#define T6000_CACHE_STATUS_VAL                                                                     \
    (FIELD_PREP(T6000_CACHE_STATUS_DATA_COUNT, T6000_CACHE_WAYS) |                                 \
     FIELD_PREP(T6000_CACHE_STATUS_TAG_COUNT, T6000_CACHE_WAYS))

#define T6031_CACHE_WAYS        12
#define T6031_CACHE_STATUS_MASK (T6000_CACHE_STATUS_DATA_COUNT | T6000_CACHE_STATUS_TAG_COUNT)
#define T6031_CACHE_STATUS_VAL                                                                     \
    (FIELD_PREP(T6000_CACHE_STATUS_DATA_COUNT, T6031_CACHE_WAYS) |                                 \
     FIELD_PREP(T6000_CACHE_STATUS_TAG_COUNT, T6031_CACHE_WAYS))

#define T8103_CACHE_WAYS        16
#define T8103_CACHE_STATUS_MASK (T8103_CACHE_STATUS_DATA_COUNT | T8103_CACHE_STATUS_TAG_COUNT)
#define T8103_CACHE_STATUS_VAL                                                                     \
    (FIELD_PREP(T8103_CACHE_STATUS_DATA_COUNT, T8103_CACHE_WAYS) |                                 \
     FIELD_PREP(T8103_CACHE_STATUS_TAG_COUNT, T8103_CACHE_WAYS))

#define T8112_CACHE_DISABLE 0x424

#define T8122_CACHE_WAYS        16
#define T8122_CACHE_STATUS_MASK (T6000_CACHE_STATUS_DATA_COUNT | T6000_CACHE_STATUS_TAG_COUNT)
#define T8122_CACHE_STATUS_VAL                                                                     \
    (FIELD_PREP(T6000_CACHE_STATUS_DATA_COUNT, T8122_CACHE_WAYS) |                                 \
     FIELD_PREP(T6000_CACHE_STATUS_TAG_COUNT, T8122_CACHE_WAYS))

#define T8142_CACHE_WAYS        16
#define T8142_CACHE_STATUS_MASK (T8142_CACHE_STATUS_DATA_COUNT | T8142_CACHE_STATUS_TAG_COUNT)
#define T8142_CACHE_STATUS_VAL                                                                     \
    (FIELD_PREP(T8142_CACHE_STATUS_DATA_COUNT, T8142_CACHE_WAYS) |                                 \
     FIELD_PREP(T8142_CACHE_STATUS_TAG_COUNT, T8142_CACHE_WAYS))

#define CACHE_ENABLE_TIMEOUT 10000

#define T8103_DCC_DRAMCFG0         0xdc4
#define T8103_DCC_DRAMCFG1         0xdbc
#define T8103_DCC_DRAMCFG0_DEFAULT 0x813057f
#define T8103_DCC_DRAMCFG1_DEFAULT 0x1800180
#define T8103_DCC_DRAMCFG0_FAST    0x133
#define T8103_DCC_DRAMCFG1_FAST    0x55555340

#define T6000_DCC_DRAMCFG         0x13cc
#define T6000_DCC_DRAMCFG_DEFAULT 0x55551555
#define T6000_DCC_DRAMCFG_FAST    0xffff0000

size_t mcc_carveout_count;
struct mcc_carveout mcc_carveouts[PLANE_TZ_MAX_REGS + 1];

struct mcc_regs {
    u64 plane_base;
    u64 plane_stride;
    int plane_count;

    u64 global_base;

    u64 dcs_base;
    u64 dcs_stride;
    int dcs_count;

    /* Plane cache enable and status registers; 0 selects PLANE_CACHE_ENABLE/STATUS. */
    u32 cache_enable;
    u32 cache_status;
    u32 cache_enable_val;
    int cache_ways;
    u32 cache_status_mask;
    u32 cache_status_val;
    u32 cache_disable;
    bool has_cache_control;

    struct tz_regs *tz;
};

static int mcc_count;
static struct mcc_regs mcc_regs[MAX_MCC_INSTANCES];

static int mcc_get_count(int node, const char *name, u64 *value)
{
    u32 len;
    const void *prop = adt_getprop(adt, node, name, &len);

    if (!prop)
        return -1;
    if (len == sizeof(u32)) {
        u32 count;
        memcpy(&count, prop, sizeof(count));
        *value = count;
    } else if (len == sizeof(u64)) {
        memcpy(value, prop, sizeof(*value));
    } else {
        return -1;
    }
    return 0;
}

static u32 plane_read32(int mcc, int plane, u64 offset)
{
    return read32(mcc_regs[mcc].plane_base + plane * mcc_regs[mcc].plane_stride + offset);
}

static void plane_write32(int mcc, int plane, u64 offset, u32 value)
{
    write32(mcc_regs[mcc].plane_base + plane * mcc_regs[mcc].plane_stride + offset, value);
}

static int plane_poll32(int mcc, int plane, u64 offset, u32 mask, u32 target, u32 timeout)
{
    return poll32(mcc_regs[mcc].plane_base + plane * mcc_regs[mcc].plane_stride + offset, mask,
                  target, timeout);
}

int mcc_enable_cache(void)
{
    int ret = 0;

    if (!mcc_initialized)
        return -1;

    /* The 6030 memory controller supports setting a waymask, but the desktop chips do not appear to
       use it */
    for (int mcc = 0; mcc < mcc_count; mcc++) {
        if (!mcc_regs[mcc].has_cache_control) {
            printf("MCC: cache control is firmware-owned on MCC %d; leaving it enabled\n", mcc);
            continue;
        }
        u32 enable = mcc_regs[mcc].cache_enable ?: PLANE_CACHE_ENABLE;
        u32 status = mcc_regs[mcc].cache_status ?: PLANE_CACHE_STATUS;
        for (int plane = 0; plane < mcc_regs[mcc].plane_count; plane++) {
            plane_write32(mcc, plane, enable, mcc_regs[mcc].cache_enable_val);
            if (plane_poll32(mcc, plane, status, mcc_regs[mcc].cache_status_mask,
                             mcc_regs[mcc].cache_status_val, CACHE_ENABLE_TIMEOUT)) {
                printf("MCC: timeout while enabling cache for MCC %d plane %d: 0x%x\n", mcc, plane,
                       plane_read32(mcc, plane, status));
                ret = -1;
            } else if (mcc_regs[mcc].cache_disable) {
                plane_write32(mcc, plane, mcc_regs[mcc].cache_disable, 0);
            }
        }
    }

    if (!ret)
        printf("MCC: System level cache enabled\n");

    return ret;
}

int mcc_unmap_carveouts(void)
{
    if (!mcc_initialized)
        return -1;

    mcc_carveout_count = 0;
    memset(mcc_carveouts, 0, sizeof mcc_carveouts);

    // All MCCs and planes should have identical configs
    // Note: For unhandled machines, the TZ regions can be found (on m1, m2, m3) by looking at
    // region-id-2 and region-id-4 on a booted macos, in the /chosen/carveout-memory-map DT node.
    // This can be used along with dumping the mcc reg space to find the correct start/end/enable
    // above.
    if (!mcc_regs[0].tz || !mcc_regs[0].tz->count) {
        static const char *const names[] = {"region-id-4", "region-id-2"};
        int node = adt_path_offset(adt, "/chosen/carveout-memory-map");

        if (node < 0)
            return -1;
        if (mem_size_actual > UINT64_MAX - ram_base)
            return -1;
        for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
            u32 len;
            const u64 *range = adt_getprop(adt, node, names[i], &len);
            u64 start, size;

            if (!range || len != 2 * sizeof(u64))
                return -1;
            memcpy(&start, range, sizeof(start));
            memcpy(&size, range + 1, sizeof(size));
            if (!size || start < ram_base || start > UINT64_MAX - size ||
                start + size > ram_base + mem_size_actual ||
                ((start | size) & (get_page_size() - 1)) ||
                mcc_carveout_count >= ARRAY_SIZE(mcc_carveouts) - 1)
                return -1;

            printf("MMU: Unmapping %s at 0x%lx..0x%lx\n", names[i], start, start + size);
            mmu_rm_mapping(start, size);
            mmu_rm_mapping(start | REGION_RWX_EL0, size);
            mmu_rm_mapping(start | REGION_RW_EL0, size);
            mmu_rm_mapping(start | REGION_RX_EL1, size);
            mcc_carveouts[mcc_carveout_count].base = start;
            mcc_carveouts[mcc_carveout_count].size = size;
            mcc_carveout_count++;
        }
        return 0;
    }

    for (u32 i = 0; i < mcc_regs[0].tz->count; i++) {
        uint64_t off = mcc_regs[0].tz->stride * i;
        uint64_t start = plane_read32(0, 0, mcc_regs[0].tz->start + off);
        uint64_t end = plane_read32(0, 0, mcc_regs[0].tz->end + off);
        bool enabled = plane_read32(0, 0, mcc_regs[0].tz->enable + off);

        if (enabled) {
            if (!start || start == end) {
                printf("MMU: TZ%d region has bad bounds 0x%lx..0x%lx (iBoot bug?)\n", i, start,
                       end);
                continue;
            }

            start = start << 12;
            end = (end + 1) << 12;
            start |= ram_base;
            end |= ram_base;
            printf("MMU: Unmapping TZ%d region at 0x%lx..0x%lx\n", i, start, end);
            mmu_rm_mapping(start, end - start);
            mmu_rm_mapping(start | REGION_RWX_EL0, end - start);
            mmu_rm_mapping(start | REGION_RW_EL0, end - start);
            mmu_rm_mapping(start | REGION_RX_EL1, end - start);
            mcc_carveouts[mcc_carveout_count].base = start;
            mcc_carveouts[mcc_carveout_count].size = end - start;
            mcc_carveout_count++;
        }
    }

    return 0;
}

int mcc_init_t8103(int node, int *path, bool t8112)
{
    const char *chip = t8112 ? "T8112" : "T8103";

    printf("MCC: Initializing %s MCC...\n", chip);

    mcc_count = 1;
    mcc_regs[0].plane_stride = T8103_PLANE_STRIDE;
    mcc_regs[0].plane_count = T8103_PLANES;
    mcc_regs[0].dcs_stride = T8103_DCS_STRIDE;

    if (adt_get_reg(adt, path, "reg", 0, &mcc_regs[0].global_base, NULL)) {
        printf("MCC: Failed to get reg property 0!\n");
        return -1;
    }

    if (adt_get_reg(adt, path, "reg", 1, &mcc_regs[0].plane_base, NULL)) {
        printf("MCC: Failed to get reg property 1!\n");
        return -1;
    }

    if (adt_get_reg(adt, path, "reg", 2, &mcc_regs[0].dcs_base, NULL)) {
        printf("MCC: Failed to get reg property 2!\n");
        return -1;
    }

    u32 val;
    if (ADT_GETPROP(adt, node, "dcs_num_channels", &val) < 0) {
        printf("MCC: Failed to get dcs_num_channels property!\n");
        return -1;
    }

    mcc_regs[0].dcs_count = val;
    mcc_regs[0].cache_enable_val = T8103_CACHE_WAYS;
    mcc_regs[0].cache_ways = T8103_CACHE_WAYS;
    mcc_regs[0].cache_status_mask = T8103_CACHE_STATUS_MASK;
    mcc_regs[0].cache_status_val = T8103_CACHE_STATUS_VAL;
    mcc_regs[0].cache_disable = t8112 ? T8112_CACHE_DISABLE : 0;
    mcc_regs[0].has_cache_control = true;
    mcc_regs[0].tz = &t8103_tz_regs;

    printf("MCC: Initialized %s MCC (%d channels)\n", chip, val);

    mcc_initialized = true;

    return 0;
}

int mcc_init_t6000(int node, int *path, bool t602x)
{
    u32 reg_len;
    u32 reg_offset = t602x ? 2 : 0;

    if (!adt_getprop(adt, node, "reg", &reg_len)) {
        printf("MCC: Failed to get reg property!\n");
        return -1;
    }

    mcc_count = reg_len / 16 - reg_offset;

    printf("MCC: Initializing T%x MCCs (%d instances)...\n", t602x ? 0x6020 : 0x6000, mcc_count);

    if (mcc_count > MAX_MCC_INSTANCES) {
        printf("MCC: Too many instances, increase MAX_MCC_INSTANCES!\n");
        mcc_count = MAX_MCC_INSTANCES;
    }

    for (int i = 0; i < mcc_count; i++) {
        u64 base;
        if (adt_get_reg(adt, path, "reg", i + reg_offset, &base, NULL)) {
            printf("MCC: Failed to get reg index %d!\n", i + reg_offset);
            return -1;
        }

        mcc_regs[i].plane_base = base + T6000_PLANE_OFFSET;
        mcc_regs[i].plane_stride = T6000_PLANE_STRIDE;
        mcc_regs[i].plane_count = T6000_PLANES;

        mcc_regs[i].global_base = base + T6000_GLOBAL_OFFSET;

        mcc_regs[i].dcs_base = base + T6000_DCS_OFFSET;
        mcc_regs[i].dcs_stride = T6000_DCS_STRIDE;
        mcc_regs[i].dcs_count = T6000_DCS_COUNT;

        mcc_regs[i].cache_enable_val = t602x ? 1 : T6000_CACHE_WAYS;
        mcc_regs[i].cache_ways = T6000_CACHE_WAYS;
        mcc_regs[i].cache_status_mask = T6000_CACHE_STATUS_MASK;
        mcc_regs[i].cache_status_val = T6000_CACHE_STATUS_VAL;
        mcc_regs[i].cache_disable = 0;
        mcc_regs[i].has_cache_control = true;

        mcc_regs[i].tz = t602x ? &t602x_tz_regs : &t8103_tz_regs;
    }

    printf("MCC: Initialized T%x MCCs (%d instances, %d planes, %d channels)\n",
           t602x ? 0x6020 : 0x6000, mcc_count, mcc_regs[0].plane_count, mcc_regs[0].dcs_count);

    mcc_initialized = true;

    return 0;
}

int mcc_init_t6031(int *path, u32 reg_offset, u32 plane_count, u32 dcs_count)
{
    for (int i = 0; i < mcc_count; i++) {
        u64 base;
        if (adt_get_reg(adt, path, "reg", i + reg_offset, &base, NULL)) {
            printf("MCC: Failed to get reg index %d!\n", i + reg_offset);
            return -1;
        }

        mcc_regs[i].plane_base = base + T6031_PLANE_OFFSET;
        mcc_regs[i].plane_stride = T6031_PLANE_STRIDE;
        mcc_regs[i].plane_count = plane_count;

        mcc_regs[i].global_base = base + T6031_GLOBAL_OFFSET;

        mcc_regs[i].dcs_base = base + T6031_DCS_OFFSET;
        mcc_regs[i].dcs_stride = T6031_DCS_STRIDE;
        mcc_regs[i].dcs_count = dcs_count;

        mcc_regs[i].cache_enable_val = 1;
        mcc_regs[i].cache_ways = T6031_CACHE_WAYS;
        mcc_regs[i].cache_status_mask = T6031_CACHE_STATUS_MASK;
        mcc_regs[i].cache_status_val = T6031_CACHE_STATUS_VAL;
        mcc_regs[i].cache_disable = 0;
        mcc_regs[i].has_cache_control = true;

        mcc_regs[i].tz = &t6031_tz_regs;
    }

    return 0;
}

int mcc_init_t8122(int *path, u32 reg_offset, u32 plane_count, u32 dcs_count,
                   struct tz_regs *tz_regs)
{
    for (int i = 0; i < mcc_count; i++) {
        u64 base;
        if (adt_get_reg(adt, path, "reg", i + reg_offset, &base, NULL)) {
            printf("MCC: Failed to get reg index %d!\n", i + reg_offset);
            return -1;
        }

        mcc_regs[i].plane_base = base + T6031_PLANE_OFFSET;
        mcc_regs[i].plane_stride = T6031_PLANE_STRIDE;
        mcc_regs[i].plane_count = plane_count;

        mcc_regs[i].global_base = base + T6031_GLOBAL_OFFSET;

        mcc_regs[i].dcs_base = base + T6031_DCS_OFFSET;
        mcc_regs[i].dcs_stride = T6031_DCS_STRIDE;
        mcc_regs[i].dcs_count = dcs_count;

        mcc_regs[i].cache_enable_val = 1;
        mcc_regs[i].cache_ways = T8122_CACHE_WAYS;
        mcc_regs[i].cache_status_mask = T8122_CACHE_STATUS_MASK;
        mcc_regs[i].cache_status_val = T8122_CACHE_STATUS_VAL;
        mcc_regs[i].cache_disable = 0;
        mcc_regs[i].has_cache_control = true;

        mcc_regs[i].tz = tz_regs;
    }

    return 0;
}

int mcc_init_m3(int node, int *path)
{
    u32 reg_len;
    /* T6032 has in reg[3] the address of reg[2] with the second die's offset added. */
    u32 reg_offset = 3 + (chip_id == T6032);

    if (!adt_getprop(adt, node, "reg", &reg_len)) {
        printf("MCC: Failed to get reg property!\n");
        return -1;
    }

    mcc_count = reg_len / 16 - reg_offset;

    printf("MCC: Initializing M3* MCCs (%d instances)...\n", mcc_count);

    if (mcc_count > MAX_MCC_INSTANCES) {
        printf("MCC: Too many instances, increase MAX_MCC_INSTANCES!\n");
        mcc_count = MAX_MCC_INSTANCES;
    }

    u32 plane_count = 0;
    u32 dcs_count = 0;

    if (!ADT_GETPROP(adt, node, "dcs-count-per-amcc", &dcs_count)) {
        printf("MCC: Failed to get dcs count!\n");
        return -1;
    }

    if (!ADT_GETPROP(adt, node, "plane-count-per-amcc", &plane_count)) {
        printf("MCC: Failed to get plane count!\n");
        return -1;
    }

    int ret = -1;
    if (adt_is_compatible(adt, node, "mcc,t8122"))
        ret = mcc_init_t8122(path, reg_offset, plane_count, dcs_count, &t8122_tz_regs);
    else if (adt_is_compatible(adt, node, "mcc,t6030"))
        ret = mcc_init_t8122(path, reg_offset, plane_count, dcs_count, &t6030_tz_regs);
    else if (adt_is_compatible(adt, node, "mcc,t6031"))
        ret = mcc_init_t6031(path, reg_offset, plane_count, dcs_count);
    else
        printf("MCC: Unsupported version:%s\n", adt_get_property(adt, node, "compatible")->value);

    if (ret)
        return ret;

    printf("MCC: Initialized M3* MCCs (%d instances, %d planes, %d channels)\n", mcc_count,
           mcc_regs[0].plane_count, mcc_regs[0].dcs_count);

    mcc_initialized = true;

    return 0;
}

int mcc_init_m4(int node, int *path)
{
    u64 amcc_count;
    if (ADT_GETPROP(adt, node, "amcc_aperture_count", &amcc_count) < 0 &&
        ADT_GETPROP(adt, node, "amcc-count", &amcc_count) < 0) {
        printf("MCC: Failed to get mcc count!\n");
        return -1;
    }

    if (amcc_count > MAX_MCC_INSTANCES) {
        printf("MCC: Too many instances, increase MAX_MCC_INSTANCES!\n");
        mcc_count = MAX_MCC_INSTANCES;
    } else {
        mcc_count = amcc_count;
    }

    u32 plane_count = 0;
    u32 dcs_count = 0;

    if (!ADT_GETPROP(adt, node, "dcs-count-per-amcc", &dcs_count)) {
        printf("MCC: Failed to get dcs count!\n");
        return -1;
    }

    if (!ADT_GETPROP(adt, node, "plane-count-per-amcc", &plane_count)) {
        printf("MCC: Failed to get plane count!\n");
        return -1;
    }

    printf("MCC: Initializing T%x MCCs (%d instances)...\n", chip_id, mcc_count);

    int ret = -1;
    if (adt_is_compatible(adt, node, "mcc,t8132")) {
        int reg_offset = 7;
        ret = mcc_init_t8122(path, reg_offset, plane_count, dcs_count, &t6030_tz_regs);
    } else {
        printf("MCC: Unsupported version:%s\n", adt_get_property(adt, node, "compatible")->value);
    }

    if (ret)
        return ret;

    printf("MCC: Initialized T%x MCCs (%d instances, %d planes, %d channels)\n", chip_id, mcc_count,
           mcc_regs[0].plane_count, mcc_regs[0].dcs_count);

    mcc_initialized = true;

    return 0;
}

/* T8142 (M5): the T8122 plane layout with the DCS block at +16 MiB, four planes per AMCC and
 * the plane cache controls at 0x2800. */
static int mcc_init_m5(int node, int *path)
{
    u64 amcc_count, dcs_count;

    if (mcc_get_count(node, "amcc-count", &amcc_count) || !amcc_count) {
        printf("MCC: Failed to get mcc count!\n");
        return -1;
    }
    if (mcc_get_count(node, "dcs-count-per-amcc", &dcs_count) || dcs_count > INT32_MAX) {
        printf("MCC: Failed to get dcs count!\n");
        return -1;
    }

    if (amcc_count > MAX_MCC_INSTANCES) {
        printf("MCC: Too many instances, increase MAX_MCC_INSTANCES!\n");
        mcc_count = MAX_MCC_INSTANCES;
    } else {
        mcc_count = amcc_count;
    }

    printf("MCC: Initializing T%x MCCs (%d instances)...\n", chip_id, mcc_count);

    int ret = mcc_init_t8122(path, 1, T8142_PLANES, dcs_count, &t8142_tz_regs);
    if (ret)
        return ret;

    for (int i = 0; i < mcc_count; i++) {
        mcc_regs[i].dcs_base = mcc_regs[i].plane_base + T8142_DCS_OFFSET;
        mcc_regs[i].cache_enable = T8142_PLANE_CACHE_ENABLE;
        mcc_regs[i].cache_status = T8142_PLANE_CACHE_STATUS;
        mcc_regs[i].cache_enable_val = T8142_CACHE_WAYS;
        mcc_regs[i].cache_ways = T8142_CACHE_WAYS;
        mcc_regs[i].cache_status_mask = T8142_CACHE_STATUS_MASK;
        mcc_regs[i].cache_status_val = T8142_CACHE_STATUS_VAL;
        mcc_regs[i].cache_disable = T8142_CACHE_DISABLE;
    }

    printf("MCC: Initialized T%x MCCs (%d instances, %d planes, %d channels)\n", chip_id, mcc_count,
           mcc_regs[0].plane_count, mcc_regs[0].dcs_count);

    mcc_initialized = true;

    return 0;
}

static int mcc_init_firmware_cache(int node, int *path)
{
    u32 reg_idx;
    u64 count = 1, channels = 0, planes = 4, stride = T8103_PLANE_STRIDE;
    int lock_node = adt_path_offset(adt, "/chosen/lock-regs/amcc");
    bool t6041 = adt_is_compatible(adt, node, "mcc,t6041");

    if (ADT_GETPROP(adt, node, "amcc-reg-idx", &reg_idx) < 0)
        return -1;
    if (adt_getprop(adt, node, "amcc-count", NULL) && mcc_get_count(node, "amcc-count", &count))
        return -1;
    if (adt_getprop(adt, node, "dcs-count-per-amcc", NULL) &&
        mcc_get_count(node, "dcs-count-per-amcc", &channels))
        return -1;
    if (!count || count > MAX_MCC_INSTANCES || channels > INT32_MAX)
        return -1;

    if (lock_node >= 0 && adt_getprop(adt, lock_node, "plane-count", NULL)) {
        if (mcc_get_count(lock_node, "plane-count", &planes))
            return -1;
    } else if (adt_getprop(adt, node, "amcc-plane-enable-mask", NULL)) {
        u64 mask;
        if (mcc_get_count(node, "amcc-plane-enable-mask", &mask))
            return -1;
        planes = __builtin_popcountll(mask);
    }
    if (lock_node >= 0 && adt_getprop(adt, lock_node, "plane-stride", NULL) &&
        mcc_get_count(lock_node, "plane-stride", &stride))
        return -1;
    if (!planes || planes > INT32_MAX || !stride || (stride & 3))
        return -1;

    /* Restrict this path to the observed J616s and J614s layouts. */
    if (t6041 && (chip_id != T6040 ||
                  !(board_id == 6 || (board_id == 4 && adt_is_compatible(adt, 0, "J614sAP"))) ||
                  reg_idx != 12 || count != 4 ||
                  channels != 4 || planes != 4 || stride != 0x40000))
        return -1;

    mcc_count = count;
    for (int i = 0; i < mcc_count; i++) {
        u64 base, size;
        if (reg_idx > (u32)(INT32_MAX - i) ||
            adt_get_reg(adt, path, "reg", reg_idx + i, &base, &size) || !base ||
            (planes - 1) > (UINT64_MAX - 4) / stride || (planes - 1) * stride + 4 > size ||
            (t6041 && (base != 0x220000000 + (u64)i * 0x2000000 || size != 0x200000)))
            return -1;
        mcc_regs[i].plane_base = base;
        mcc_regs[i].plane_stride = stride;
        mcc_regs[i].plane_count = planes;
        mcc_regs[i].dcs_count = channels;
        mcc_regs[i].tz = NULL;
        mcc_regs[i].has_cache_control = false;
    }

    if (t6041) {
        for (int i = 0; i < mcc_count; i++) {
            for (int plane = 0; plane < mcc_regs[i].plane_count; plane++) {
                u32 status = plane_read32(i, plane, 0x2804);
                if ((status & 0x1f001f00) != 0x0c000c00) {
                    printf("MCC: Unexpected cache status on MCC %d plane %d: 0x%x\n", i, plane,
                           status);
                    return -1;
                }
            }
        }
    }

    printf("MCC: Initialized T%x MCCs (%d instances, %d planes, %d channels)\n", chip_id, mcc_count,
           mcc_regs[0].plane_count, mcc_regs[0].dcs_count);
    mcc_initialized = true;
    return 0;
}

int mcc_init(void)
{
    int path[8];
    int node = adt_path_offset_trace(adt, "/arm-io/mcc", path);

    if (node < 0) {
        printf("MCC: MCC node not found!\n");
        return -1;
    }

    if (adt_is_compatible(adt, node, "mcc,t8103")) {
        return mcc_init_t8103(node, path, false);
    } else if (adt_is_compatible(adt, node, "mcc,t8112")) {
        return mcc_init_t8103(node, path, true);
    } else if (adt_is_compatible(adt, node, "mcc,t6000")) {
        return mcc_init_t6000(node, path, false);
    } else if (adt_is_compatible(adt, node, "mcc,t6020")) {
        return mcc_init_t6000(node, path, true);
    } else if (adt_is_compatible(adt, node, "mcc,t8122")) {
        return mcc_init_m3(node, path);
    } else if (adt_is_compatible(adt, node, "mcc,t8132")) {
        return mcc_init_m4(node, path);
    } else if (adt_is_compatible(adt, node, "mcc,t8140")) {
        return mcc_init_firmware_cache(node, path);
    } else if (adt_is_compatible(adt, node, "mcc,t6041")) {
        return mcc_init_firmware_cache(node, path);
    } else if (adt_is_compatible(adt, node, "mcc,t8142")) {
        return mcc_init_m5(node, path);
    } else if (adt_is_compatible(adt, node, "mcc,t6030")) {
        return mcc_init_m3(node, path);
    } else if (adt_is_compatible(adt, node, "mcc,t6031")) {
        return mcc_init_m3(node, path);
    } else {
        printf("MCC: Unsupported version:%s\n", adt_get_property(adt, node, "compatible")->value);
        return -1;
    }
}
