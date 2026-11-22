/* SPDX-License-Identifier: MIT */
/*
 * The node tree of a path-keyed fake ADT (struct fnode nodes[nnodes], offset = index + 1): the
 * children of a node are the nodes one path component below it, in the order they were added.
 * A dropped node (empty path) has none and is nobody's child. Included by the host after its
 * nodes[] and nnodes.
 */
#ifndef ADT_TREE_H
#define ADT_TREE_H

static bool host_adt_is_child(int parent, int i)
{
    const char *p = parent < 1 || parent > nnodes ? NULL : nodes[parent - 1].path;
    const char *c = nodes[i].path;
    size_t n = p ? strlen(p) : 0;
    if (!p || !p[0] || !c[0] || strncmp(c, p, n) || c[n] != '/' || !c[n + 1])
        return false;
    return !strchr(c + n + 1, '/');
}

static int host_adt_parent(int node)
{
    if (node < 1 || node > nnodes)
        return -1;
    for (int i = 0; i < nnodes; i++)
        if (host_adt_is_child(i + 1, node - 1))
            return i + 1;
    return -1;
}

int adt_get_child_count(const void *a, int node)
{
    UNUSED(a);
    int n = 0;
    for (int i = 0; i < nnodes; i++)
        n += host_adt_is_child(node, i);
    return n;
}

int adt_first_child_offset(const void *a, int node)
{
    UNUSED(a);
    for (int i = 0; i < nnodes; i++)
        if (host_adt_is_child(node, i))
            return i + 1;
    return -1;
}

int adt_next_sibling_offset(const void *a, int node)
{
    UNUSED(a);
    int parent = host_adt_parent(node);
    for (int i = node; parent > 0 && i < nnodes; i++)
        if (host_adt_is_child(parent, i))
            return i + 1;
    return -1;
}

const char *adt_get_name(const void *a, int node)
{
    UNUSED(a);
    if (node < 1 || node > nnodes || !nodes[node - 1].path[0])
        return NULL;
    return strrchr(nodes[node - 1].path, '/') + 1;
}
#endif
