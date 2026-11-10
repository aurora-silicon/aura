/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#include "adt.h"
#include "string.h"
#include "tunables.h"
#include "types.h"
#include "utils.h"

struct tunable_info {
    int node_offset;
    int node_path[8];
    const u32 *tunable_raw;
    u32 tunable_len;
};

static int tunables_adt_find(const char *path, const char *prop, struct tunable_info *info,
                             u32 item_size)
{
    info->node_offset = adt_path_offset_trace(adt, path, info->node_path);
    if (info->node_offset < 0) {
        printf("tunable: unable to find ADT node %s.\n", path);
        return -1;
    }

    info->tunable_raw = adt_getprop(adt, info->node_offset, prop, &info->tunable_len);
    if (info->tunable_raw == NULL || info->tunable_len == 0) {
        printf("tunable: Error getting ADT node %s property %s .\n", path, prop);
        return -1;
    }

    if (info->tunable_len % item_size) {
        printf("tunable: tunable length needs to be a multiply of %d but is %d\n", item_size,
               info->tunable_len);
        return -1;
    }

    info->tunable_len /= item_size;

    return 0;
}

struct tunable_global {
    u32 reg_idx;
    u32 offset;
    u32 mask;
    u32 value;
} PACKED;

int tunables_apply_global(const char *path, const char *prop)
{
    struct tunable_info info;

    if (tunables_adt_find(path, prop, &info, sizeof(struct tunable_global)) < 0)
        return -1;

    const struct tunable_global *tunables = (const struct tunable_global *)info.tunable_raw;
    for (u32 i = 0; i < info.tunable_len; ++i) {
        const struct tunable_global *tunable = &tunables[i];

        u64 addr;
        if (adt_get_reg(adt, info.node_path, "reg", tunable->reg_idx, &addr, NULL) < 0) {
            printf("tunable: Error getting regs with index %d\n", tunable->reg_idx);
            return -1;
        }

        mask32(addr + tunable->offset, tunable->mask, tunable->value);
    }

    return 0;
}

struct tunable_local {
    u32 offset;
    u32 size;
    u64 mask;
    u64 value;
} PACKED;

int tunables_validate_local(const char *path, const char *prop, u64 span)
{
    int node = adt_path_offset(adt, path);
    u32 len;
    if (node < 0)
        return -1;

    const struct tunable_local *tunables = adt_getprop(adt, node, prop, &len);
    if (!tunables)
        return 1;
    if (!len || len % sizeof(*tunables))
        return -1;

    for (u32 i = 0; i < len / sizeof(*tunables); i++) {
        u32 size = tunables[i].size;
        if ((size != 1 && size != 2 && size != 4 && size != 8) || tunables[i].offset % size ||
            size > span || tunables[i].offset > span - size)
            return -1;
    }
    return 0;
}

int tunables_apply_local_addr(const char *path, const char *prop, uintptr_t base)
{
    struct tunable_info info;

    if (tunables_adt_find(path, prop, &info, sizeof(struct tunable_local)) < 0)
        return -1;

    const struct tunable_local *tunables = (const struct tunable_local *)info.tunable_raw;
    for (u32 i = 0; i < info.tunable_len; ++i) {
        const struct tunable_local *tunable = &tunables[i];

        switch (tunable->size) {
            case 1:
                mask8(base + tunable->offset, tunable->mask, tunable->value);
                break;
            case 2:
                mask16(base + tunable->offset, tunable->mask, tunable->value);
                break;
            case 4:
                mask32(base + tunable->offset, tunable->mask, tunable->value);
                break;
            case 8:
                mask64(base + tunable->offset, tunable->mask, tunable->value);
                break;
            default:
                printf("tunable: unknown tunable size 0x%08x\n", tunable->size);
                return -1;
        }
    }
    return 0;
}

int tunables_check_local_addr(const char *path, const char *prop, uintptr_t base,
                              const struct tunable_excuse *excuses)
{
    struct tunable_info info;
    int bad = 0;

    if (tunables_adt_find(path, prop, &info, sizeof(struct tunable_local)) < 0)
        return -1;

    const struct tunable_local *tunables = (const struct tunable_local *)info.tunable_raw;
    for (u32 i = 0; i < info.tunable_len; ++i) {
        const struct tunable_local *tunable = &tunables[i];
        u64 excused = 0, value;

        for (const struct tunable_excuse *e = excuses; e && e->bits; e++)
            if (e->offset == tunable->offset)
                excused = e->bits;
        switch (tunable->size) {
            case 4:
                value = read32(base + tunable->offset);
                break;
            case 8:
                value = read64(base + tunable->offset);
                break;
            default:
                printf("tunable: %s +0x%x: cannot check size %u\n", prop, tunable->offset,
                       tunable->size);
                return -1;
        }
        if ((value & tunable->mask) != (tunable->value & tunable->mask & ~excused)) {
            printf("tunable: %s +0x%x reads 0x%lx under mask 0x%lx, wrote 0x%lx\n", prop,
                   tunable->offset, value & tunable->mask, tunable->mask, tunable->value);
            bad++;
        }
    }
    return bad;
}

int tunables_validate_compact(const char *path, const char *prop, u64 span)
{
    struct tunable_info info;
    if (span < 4 || tunables_adt_find(path, prop, &info, 12) < 0)
        return -1;

    const u8 *records = (const u8 *)info.tunable_raw;
    for (u32 i = 0; i < info.tunable_len; i++) {
        u32 location;
        memcpy(&location, records + i * 12, 4);
        u32 offset = location & 0xffffff;
        u32 width = location >> 24;
        if (width != 32 || (offset & 3) || offset > span - 4)
            return -1;
    }
    return 0;
}

int tunables_apply_compact_addr(const char *path, const char *prop, uintptr_t base, u64 span)
{
    if (tunables_validate_compact(path, prop, span))
        return -1;
    struct tunable_info info;
    if (tunables_adt_find(path, prop, &info, 12) < 0)
        return -1;
    const u8 *records = (const u8 *)info.tunable_raw;
    for (u32 i = 0; i < info.tunable_len; i++) {
        u32 location, mask, value;
        memcpy(&location, records + i * 12, 4);
        memcpy(&mask, records + i * 12 + 4, 4);
        memcpy(&value, records + i * 12 + 8, 4);
        u32 offset = location & 0xffffff;
        mask32(base + offset, mask, value & mask);
    }
    return 0;
}

int tunables_apply_local(const char *path, const char *prop, u32 reg_offset)
{
    struct tunable_info info;

    if (tunables_adt_find(path, prop, &info, sizeof(struct tunable_local)) < 0)
        return -1;

    u64 base;
    if (adt_get_reg(adt, info.node_path, "reg", reg_offset, &base, NULL) < 0) {
        printf("tunable: Error getting regs\n");
        return -1;
    }

    return tunables_apply_local_addr(path, prop, base);
}
