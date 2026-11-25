/* SPDX-License-Identifier: MIT */

#include "devicetree.h"
#include "utils.h"

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

/*
 * The CPU address of node's first reg entry: read at its bus's #address-cells,
 * then mapped through the ranges of every bus up to the root, each entry read
 * at the bus's own and its parent's cell widths. An empty ranges maps one to
 * one, and so does a bus none of whose entries holds the address, as the
 * nearest-ranges translation this replaces did: some trees place nodes outside
 * their soc's ranges. Below the root's children that is logged, since the
 * address is then matched against the next bus's ranges as if it already were
 * one of that bus's addresses. 0 if a bus has no ranges, a width is not one or
 * two cells, or the reg or a ranges is not a whole number of entries.
 */
u64 dt_get_address(void *dt, int node)
{
    int bus = fdt_parent_offset(dt, node);
    int len;

    if (bus < 0)
        return 0;

    int ac = fdt_address_cells(dt, bus);
    int sc = fdt_size_cells(dt, bus);
    const fdt32_t *reg = fdt_getprop(dt, node, "reg", &len);
    int entry_size = (ac + sc) * sizeof(*reg);
    if (ac < 1 || ac > 2 || sc < 0 || sc > 2 || !reg || len < entry_size || len % entry_size)
        return 0;

    u64 addr = dt_read_cells(reg, ac);

    /* The root's children already use the CPU's address space. */
    while (bus > 0) {
        int parent = fdt_parent_offset(dt, bus);
        const fdt32_t *ranges = fdt_getprop(dt, bus, "ranges", &len);

        if (parent < 0 || !ranges)
            return 0;
        if (len) {
            int child_ac = fdt_address_cells(dt, bus);
            int child_sc = fdt_size_cells(dt, bus);
            int parent_ac = fdt_address_cells(dt, parent);
            if (child_ac < 1 || child_ac > 2 || child_sc < 1 || child_sc > 2 || parent_ac < 1 ||
                parent_ac > 2)
                return 0;

            int stride = child_ac + parent_ac + child_sc;
            if (len % (int)(stride * sizeof(*ranges)))
                return 0;

            int num_entries = len / (int)(stride * sizeof(*ranges));
            int i;

            for (i = 0; i < num_entries; i++, ranges += stride) {
                u64 start = dt_read_cells(ranges, child_ac);
                u64 size = dt_read_cells(ranges + child_ac + parent_ac, child_sc);

                if (addr >= start && addr - start < size) {
                    addr = dt_read_cells(ranges + child_ac, parent_ac) + (addr - start);
                    break;
                }
            }
            if (i == num_entries && parent > 0)
                printf("FDT: %s: 0x%lx is outside the ranges of %s\n", fdt_get_name(dt, node, NULL),
                       addr, fdt_get_name(dt, bus, NULL));
        }
        bus = parent;
    }

    return addr;
}
