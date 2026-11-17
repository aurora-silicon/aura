#pragma once
#include "types.h"
extern void *adt;
int adt_path_offset(const void *adt, const char *path);
int adt_path_offset_trace(const void *adt, const char *path, int *trace);
const void *adt_getprop(const void *adt, int node, const char *name, u32 *len);
const void *adt_get_property(const void *adt, int node, const char *name);
bool adt_is_compatible(const void *adt, int node, const char *compat);
int adt_get_reg(const void *adt, int *path, const char *prop, int idx, u64 *addr, u64 *size);
#define ADT_GETPROP(adt, node, name, val) (-1)
