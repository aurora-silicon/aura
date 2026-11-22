/* SPDX-License-Identifier: MIT */

#ifndef DEVICETREE_H
#define DEVICETREE_H

#include "types.h"

#include "libfdt/libfdt.h"

#define DT_MAX_RANGES 8

struct dt_ranges_tbl {
    u64 start;
    u64 parent;
    u64 size;
};

void dt_parse_ranges(void *dt, int node, struct dt_ranges_tbl *ranges);
u64 dt_translate_address(struct dt_ranges_tbl *ranges, u64 addr);
/* The first address in @node's reg, at the #address-cells of its parent @bus; false if none */
bool dt_reg_address(void *dt, int bus, int node, u64 *addr);
u64 dt_get_address(void *dt, int node);

#endif
