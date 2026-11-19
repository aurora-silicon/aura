/* SPDX-License-Identifier: MIT */

/*
 * LEGACY-ASAHI(2027-02-28): transitional device tree aliases.
 *
 * aura publishes its /chosen properties and nodes under the "aurora," prefix. Kernels,
 * U-Boot builds and userspace released before that rename read the same data under
 * "asahi," (with "m1n1" where the name now says "aura"). Until every supported
 * distribution ships readers of the new names, this one function adds the old name next
 * to each new one a released reader is known to use. Nothing else in aura writes or reads
 * the old names. Remove this file, its call sites and the Makefile entry after the date.
 */

#include "kboot_legacy.h"
#include "malloc.h"
#include "string.h"
#include "types.h"
#include "utils.h"

#include "libfdt/libfdt.h"

/* /chosen properties: exact names, after "aurora," */
static const char *const legacy_props[] = {
    "os-fw-version",        "system-fw-version",
    "iboot1-version",       "iboot2-version",
    "aura-stage1-version",  "aura-stage2-version",
    "efi-system-partition", "speaker-id",
    "show-notch",           "m3-defaults",
    "wifi-mac-address",     "kblang-code",
    "aura-oslog-overlap",   NULL,
};

/* /chosen property families: every switch and result of the M3-generation handoffs */
static const char *const legacy_prop_families[] = {
    "t6030-", "t6031-", "t6041-", "t6050-", "t8122-", "j613-", "j615-", "j713-", NULL,
};

/* /chosen subnodes read by released kernels (display gate, GPU admission) */
static const char *const legacy_nodes[] = {
    "t6030-pmp",
    "t6031-pmp",
    "t8122-pmp",
    "t8122-gpu-powered-identity",
    "t6031-gpu-powered-identity",
    NULL,
};

#define NEW_PREFIX     "aurora,"
#define NEW_PREFIX_LEN 7
#define MAX_NAME       128
#define MAX_PATH       256
#define MAX_DEPTH      4

bool aurora_legacy_wants_prop(const char *name)
{
    if (strncmp(name, NEW_PREFIX, NEW_PREFIX_LEN))
        return false;
    const char *rest = name + NEW_PREFIX_LEN;
    for (int i = 0; legacy_props[i]; i++)
        if (!strcmp(rest, legacy_props[i]))
            return true;
    for (int i = 0; legacy_prop_families[i]; i++)
        if (!strncmp(rest, legacy_prop_families[i], strlen(legacy_prop_families[i])))
            return true;
    return false;
}

int aurora_legacy_name(const char *name, char *out, size_t size)
{
    if (strncmp(name, NEW_PREFIX, NEW_PREFIX_LEN))
        return -1;
    const char *rest = name + NEW_PREFIX_LEN;
    const char *aura = strstr(rest, "aura-");
    int n;
    if (aura == rest) /* aura-stage1-version -> m1n1-stage1-version */
        n = snprintf(out, size, "asahi,m1n1-%s", rest + 5);
    else
        n = snprintf(out, size, "asahi,%s", rest);
    return (n < 0 || (size_t)n >= size) ? -1 : 0;
}

/* Copy the node at src_path, properties and subnodes, to a new subnode dst_name of dst_parent_path
 */
static int copy_node(void *fdt, const char *src_path, const char *dst_parent_path,
                     const char *dst_name, int depth)
{
    char dst_path[MAX_PATH], sub_src[MAX_PATH], sub_name[MAX_NAME], pname[MAX_NAME];
    int n;

    if (depth > MAX_DEPTH)
        return -1;
    n = snprintf(dst_path, sizeof(dst_path), "%s/%s", dst_parent_path, dst_name);
    if (n < 0 || (size_t)n >= sizeof(dst_path))
        return -1;

    int parent = fdt_path_offset(fdt, dst_parent_path);
    if (parent < 0)
        return -1;
    if (fdt_subnode_offset(fdt, parent, dst_name) >= 0)
        return 0; /* already there (an older aura image, or a second call) */
    if (fdt_add_subnode(fdt, parent, dst_name) < 0)
        return -1;

    /* Offsets move with every write: find the i-th property again each time */
    for (int i = 0;; i++) {
        int src = fdt_path_offset(fdt, src_path), k = 0, off, len;
        const char *name = NULL;
        const void *val = NULL;
        if (src < 0)
            return -1;
        fdt_for_each_property_offset(off, fdt, src)
        {
            if (k++ == i) {
                val = fdt_getprop_by_offset(fdt, off, &name, &len);
                break;
            }
        }
        if (!val)
            break;
        if (strlen(name) >= sizeof(pname))
            return -1;
        strcpy(pname, name);
        void *copy = malloc(len ? len : 1);
        if (!copy)
            return -1;
        memcpy(copy, val, len);
        int dst = fdt_path_offset(fdt, dst_path);
        int ret = dst < 0 ? dst : fdt_setprop(fdt, dst, pname, copy, len);
        free(copy);
        if (ret < 0)
            return -1;
    }

    for (int i = 0;; i++) {
        int src = fdt_path_offset(fdt, src_path), k = 0, sub;
        const char *name = NULL;
        if (src < 0)
            return -1;
        fdt_for_each_subnode(sub, fdt, src)
        {
            if (k++ == i) {
                name = fdt_get_name(fdt, sub, NULL);
                break;
            }
        }
        if (!name)
            break;
        if (strlen(name) >= sizeof(sub_name))
            return -1;
        strcpy(sub_name, name);
        n = snprintf(sub_src, sizeof(sub_src), "%s/%s", src_path, sub_name);
        if (n < 0 || (size_t)n >= sizeof(sub_src))
            return -1;
        if (copy_node(fdt, sub_src, dst_path, sub_name, depth + 1))
            return -1;
    }
    return 0;
}

int aurora_legacy_chosen_aliases(void *fdt)
{
    char name[MAX_NAME], legacy[MAX_NAME], src_path[MAX_PATH];
    int added = 0;

    if (fdt_path_offset(fdt, "/chosen") < 0)
        return 0;

    /* Properties: walk by index, since every write moves the offsets */
    for (int i = 0;; i++) {
        int chosen = fdt_path_offset(fdt, "/chosen"), k = 0, off, len;
        const char *pname = NULL;
        const void *val = NULL;
        fdt_for_each_property_offset(off, fdt, chosen)
        {
            if (k++ == i) {
                val = fdt_getprop_by_offset(fdt, off, &pname, &len);
                break;
            }
        }
        if (!val)
            break;
        if (!aurora_legacy_wants_prop(pname) || strlen(pname) >= sizeof(name))
            continue;
        strcpy(name, pname);
        if (aurora_legacy_name(name, legacy, sizeof(legacy)))
            continue;
        if (fdt_getprop(fdt, chosen, legacy, NULL))
            continue; /* an owner set the old name as well: theirs stays */
        void *copy = malloc(len ? len : 1);
        if (!copy)
            return -1;
        memcpy(copy, val, len);
        int ret = fdt_setprop(fdt, chosen, legacy, copy, len);
        free(copy);
        if (ret < 0)
            return -1;
        /* libfdt appends the new property after the last one: the walk reaches it and skips it */
        added++;
    }

    for (int i = 0; legacy_nodes[i]; i++) {
        int n = snprintf(src_path, sizeof(src_path), "/chosen/" NEW_PREFIX "%s", legacy_nodes[i]);
        if (n < 0 || (size_t)n >= sizeof(src_path))
            return -1;
        if (fdt_path_offset(fdt, src_path) < 0)
            continue;
        n = snprintf(legacy, sizeof(legacy), "asahi,%s", legacy_nodes[i]);
        if (n < 0 || (size_t)n >= sizeof(legacy))
            return -1;
        if (copy_node(fdt, src_path, "/chosen", legacy, 0))
            return -1;
        added++;
    }
    return added;
}
