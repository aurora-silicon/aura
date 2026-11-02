/* SPDX-License-Identifier: MIT */

#include "devicetree.h"

#include "libfdt/libfdt.h"

static u64 dt_read_cells(const fdt32_t *cells, int count)
{
    u64 value = 0;

    while (count--)
        value = (value << 32) | fdt32_ld(cells++);
    return value;
}

void dt_parse_ranges(void *dt, int node, struct dt_ranges_tbl *ranges)
{
    int len;
    const fdt32_t *cells = fdt_getprop(dt, node, "ranges", &len);
    int parent = fdt_parent_offset(dt, node);

    if (!cells || len <= 0 || parent < 0)
        return;

    /* Each entry is (child address, parent address, size), at the nodes' own cell widths. */
    int child_ac = fdt_address_cells(dt, node);
    int child_sc = fdt_size_cells(dt, node);
    int parent_ac = fdt_address_cells(dt, parent);
    if (child_ac < 1 || child_ac > 2 || child_sc < 1 || child_sc > 2 || parent_ac < 1 ||
        parent_ac > 2)
        return;

    int stride = child_ac + parent_ac + child_sc;
    int num_entries = len / (int)(stride * sizeof(fdt32_t));
    int idx = 0;

    for (int i = 0; i < num_entries && idx < DT_MAX_RANGES; i++, cells += stride) {
        u64 size = dt_read_cells(cells + child_ac + parent_ac, child_sc);
        if (!size)
            continue;
        ranges[idx].start = dt_read_cells(cells, child_ac);
        ranges[idx].parent = dt_read_cells(cells + child_ac, parent_ac);
        ranges[idx].size = size;
        idx++;
    }
}

u64 dt_translate(struct dt_ranges_tbl *ranges, const fdt64_t *reg)
{
    u64 addr = fdt64_ld(reg);
    for (int idx = 0; idx < DT_MAX_RANGES; ++idx) {
        if (ranges[idx].size == 0)
            break;
        if (addr >= ranges[idx].start && addr < ranges[idx].start + ranges[idx].size)
            return ranges[idx].parent - ranges[idx].start + addr;
    }

    return addr;
}

u64 dt_get_address(void *dt, int node)
{
    int parent = fdt_parent_offset(dt, node);

    // find parent with "ranges" property
    while (parent >= 0) {
        if (fdt_getprop(dt, parent, "ranges", NULL))
            break;

        parent = fdt_parent_offset(dt, parent);
    }

    if (parent < 0)
        return 0;

    // parse ranges for address translation
    struct dt_ranges_tbl ranges[DT_MAX_RANGES] = {0};
    dt_parse_ranges(dt, parent, ranges);

    const fdt64_t *reg = fdt_getprop(dt, node, "reg", NULL);
    if (!reg)
        return 0;

    return dt_translate(ranges, reg);
}
