/* SPDX-License-Identifier: MIT */
/*
 * Host stand-in for aura's ADT reader (rust/src/adt.rs) over a real binary ADT blob, for the
 * G0 GPU host tests.  It walks the same layout (node header, 32-byte property names, values
 * padded to 4) and translates reg entries through every ancestor's ranges the way
 * get_reg_container() does.  Written from that file's behaviour; shares no code with it.
 */
#ifndef WINDOWS_GPU_ADT_STANDIN_H
#define WINDOWS_GPU_ADT_STANDIN_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adt.h"

void *adt;
static size_t adt_bytes;

static void standin_load_adt(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    adt_bytes = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    adt = malloc(adt_bytes);
    if (!adt || fread(adt, 1, adt_bytes, f) != adt_bytes) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(2);
    }
    fclose(f);
}

static u32 standin_len(const struct adt_property *p)
{
    return p->size & 0x7fffffffu;
}

static void standin_bounds(int off, size_t need)
{
    if (off < 0 || (size_t)off + need > adt_bytes) {
        fprintf(stderr, "ADT stand-in: offset %d out of bounds\n", off);
        abort();
    }
}

int adt_get_property_count(const void *a, int off)
{
    standin_bounds(off, 8);
    return (int)((const struct adt_node_hdr *)((const u8 *)a + off))->property_count;
}

int adt_get_child_count(const void *a, int off)
{
    standin_bounds(off, 8);
    return (int)((const struct adt_node_hdr *)((const u8 *)a + off))->child_count;
}

int adt_first_property_offset(const void *a, int off)
{
    (void)a;
    return off + (int)sizeof(struct adt_node_hdr);
}

int adt_next_property_offset(const void *a, int off)
{
    standin_bounds(off, sizeof(struct adt_property));
    const struct adt_property *p = ADT_PROP(a, off);
    return off + (int)sizeof(*p) + (int)((standin_len(p) + 3) & ~3u);
}

int adt_first_child_offset(const void *a, int off)
{
    int n = adt_get_property_count(a, off);
    int p = adt_first_property_offset(a, off);
    while (n--)
        p = adt_next_property_offset(a, p);
    return p;
}

int adt_next_sibling_offset(const void *a, int off)
{
    int n = adt_get_child_count(a, off);
    int c = adt_first_child_offset(a, off);
    while (n--)
        c = adt_next_sibling_offset(a, c);
    return c;
}

const void *adt_getprop(const void *a, int node, const char *name, u32 *lenp)
{
    if (node < 0)
        return NULL;
    int n = adt_get_property_count(a, node);
    int p = adt_first_property_offset(a, node);
    while (n--) {
        const struct adt_property *prop = ADT_PROP(a, p);
        if (!strncmp(prop->name, name, sizeof(prop->name))) {
            if (lenp)
                *lenp = standin_len(prop);
            return prop->value;
        }
        p = adt_next_property_offset(a, p);
    }
    return NULL;
}

const char *adt_get_name(const void *a, int node)
{
    return adt_getprop(a, node, "name", NULL);
}

static int standin_subnode(const void *a, int parent, const char *name, size_t len)
{
    int n = adt_get_child_count(a, parent);
    int c = adt_first_child_offset(a, parent);
    while (n--) {
        const char *cn = adt_get_name(a, c);
        if (cn && strlen(cn) == len && !strncmp(cn, name, len))
            return c;
        c = adt_next_sibling_offset(a, c);
    }
    return -1;
}

int adt_path_offset_trace(const void *a, const char *path, int *offsets)
{
    int node = 0, depth = 0;
    const char *p = path;
    while (*p == '/')
        p++;
    while (*p) {
        const char *end = strchr(p, '/');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        node = standin_subnode(a, node, p, len);
        if (node < 0)
            return -1;
        if (offsets && depth < 7)
            offsets[depth] = node;
        depth++;
        p += len;
        while (*p == '/')
            p++;
    }
    if (offsets)
        offsets[depth < 7 ? depth : 7] = 0;
    return node;
}

int adt_path_offset(const void *a, const char *path)
{
    return adt_path_offset_trace(a, path, NULL);
}

static bool standin_u32(const void *a, int node, const char *name, u32 *v)
{
    u32 len;
    const void *p = adt_getprop(a, node, name, &len);
    if (!p || len != 4)
        return false;
    memcpy(v, p, 4);
    return true;
}

static u64 standin_cells(const u8 *p, u32 cells)
{
    u64 v = 0;
    for (u32 i = 0; i < cells; i++) {
        u32 w;
        memcpy(&w, p + 4 * i, 4);
        v |= (u64)w << (32 * i);
    }
    return v;
}

int adt_get_reg(const void *a, int *path, const char *prop, int idx, u64 *addr, u64 *size)
{
    int depth = 0;
    if (!path || !path[0] || idx < 0)
        return -1;
    while (depth < 8 && path[depth])
        depth++;
    int cursor = depth - 1;
    int node = path[cursor];
    int parent = cursor > 0 ? path[cursor - 1] : 0;
    u32 ac, sc;
    if (!standin_u32(a, parent, "#address-cells", &ac) ||
        !standin_u32(a, parent, "#size-cells", &sc) || ac < 1 || ac > 2 || sc > 2)
        return -1;
    u32 len;
    const u8 *reg = adt_getprop(a, node, prop, &len);
    if (!reg || !len)
        return -1;
    if (len < (u32)(idx + 1) * (ac + sc) * 4)
        return -1;
    const u8 *e = reg + (size_t)idx * (ac + sc) * 4;
    u64 ad = standin_cells(e, ac), sz = standin_cells(e + 4 * ac, sc);
    while (parent != 0) {
        cursor--;
        node = parent;
        parent = cursor > 0 ? path[cursor - 1] : 0;
        u32 rlen;
        const u8 *ranges = adt_getprop(a, node, "ranges", &rlen);
        if (!ranges)
            break;
        u32 pac;
        if (!standin_u32(a, parent, "#address-cells", &pac) || pac < 1 || pac > 2)
            return -1;
        u32 entry = 4 * (pac + ac + sc);
        for (u32 off = 0; off + entry <= rlen; off += entry) {
            u64 ca = standin_cells(ranges + off, ac);
            u64 pa = standin_cells(ranges + off + 4 * ac, pac);
            u64 cs = standin_cells(ranges + off + 4 * (ac + pac), sc);
            if (ad >= ca && ad + sz <= ca + cs) {
                ad = ad - ca + pa;
                break;
            }
        }
        if (!standin_u32(a, parent, "#size-cells", &sc))
            return -1;
        ac = pac;
    }
    if (addr)
        *addr = ad;
    if (size)
        *size = sz;
    return 0;
}

#endif
