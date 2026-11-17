/* SPDX-License-Identifier: MIT */
/* Host stand-in for src/adt.h: an in-memory ADT built by tests/t8122_display_host.c. */
#ifndef ADT_H
#define ADT_H
#include "types.h"
extern void *adt;
struct adt_property {
    char name[32];
    u32 size;
    u8 value[];
};
struct adt_segment_ranges {
    u64 phys;
    u64 iova;
    u64 remap;
    u32 size;
    u32 unk;
} PACKED;
int adt_path_offset(const void *adt, const char *path);
int adt_path_offset_trace(const void *adt, const char *path, int *offsets);
const void *adt_getprop(const void *adt, int nodeoffset, const char *name, u32 *lenp);
int adt_getprop_copy(const void *adt, int nodeoffset, const char *name, void *out, size_t len);
int adt_get_reg(const void *adt, int *path, const char *prop, int idx, u64 *addr, u64 *size);
bool adt_is_compatible(const void *adt, int nodeoffset, const char *compat);
int adt_get_property_count(const void *adt, int offset);
int host_adt_nprops(int node);
const struct adt_property *host_adt_prop(int node, int i);
#define ADT_FOREACH_PROPERTY(adt, node, prop)                                                      \
    for (int _pi = 0; _pi < host_adt_nprops(node); _pi++)                                          \
        for (const struct adt_property *prop = host_adt_prop(node, _pi); prop; prop = NULL)
#endif
