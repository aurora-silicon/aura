/* SPDX-License-Identifier: MIT */
/*
 * libFuzzer target for aura's ADT reader (rust/src/adt.rs, built for the host by
 * tests/fuzz/build.sh). The input is one ADT image; adt_get_size() is its length, as
 * iBoot's boot arguments give it on a Mac.
 *
 * The target uses the reader the way aura's C code does: it walks the whole tree with
 * ADT_FOREACH_CHILD and ADT_FOREACH_PROPERTY, reads every property, looks every node up again
 * by its path and by a set of common paths, reads its reg entries, checks compatibles and
 * rewrites properties in place. Every offset it hands back to the reader is one the reader
 * returned, so any crash, sanitizer report, Rust panic or failed check is a reader bug.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adt.h"

void *adt;
static u32 adt_size;

u32 adt_get_size(void)
{
    return adt_size;
}

#define WALK_MAX_DEPTH 64
#define PATH_MAX_LEN   256
#define MAX_PATHS      64

static char paths[MAX_PATHS][PATH_MAX_LEN];
static int path_nodes[MAX_PATHS];
static int npaths;
static volatile u32 sink;

#define check(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);               \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

/* The reader takes C strings and requires them to be UTF-8; aura passes ASCII literals */
static bool name_ok(const char *name, size_t max)
{
    size_t len = strnlen(name, max);

    if (len == 0 || len == max)
        return false;
    for (size_t i = 0; i < len; i++)
        if (name[i] < 0x20 || name[i] > 0x7e)
            return false;
    return true;
}

static bool in_image(const void *p, size_t len)
{
    const u8 *b = p, *start = adt;

    return b >= start && len <= adt_size && (size_t)(b - start) <= adt_size - len;
}

static void touch_property(int node, const struct adt_property *prop)
{
    char name[33];
    u32 len = 0;

    /* What ADT_FOREACH_PROPERTY hands out must lie inside the image */
    check(in_image(prop, sizeof(*prop)));
    check(in_image(prop->value, prop->size));
    for (u32 i = 0; i < prop->size; i++)
        sink += prop->value[i];

    /* The offset-based accessors see the same property */
    int poff = (const u8 *)prop - (const u8 *)adt;
    const char *pname = NULL;
    u32 plen = 0;
    check(adt_get_property_by_offset(adt, poff) == prop);
    check(adt_getprop_by_offset(adt, poff, &pname, &plen) == prop->value);
    check(pname == prop->name && plen == prop->size);

    memcpy(name, prop->name, 32);
    name[32] = 0;
    if (!name_ok(name, 33))
        return;

    const void *val = adt_getprop(adt, node, name, &len);
    if (val) {
        check(in_image(val, len));
        for (u32 i = 0; i < len; i++)
            sink += ((const u8 *)val)[i];
    }
    const struct adt_property *p = adt_get_property(adt, node, name);
    if (p)
        check(in_image(p, sizeof(*p)) && in_image(p->value, p->size));

    u8 small[16];
    int ret = adt_getprop_copy(adt, node, name, small, len <= sizeof(small) ? len : sizeof(small));
    if (val && len <= sizeof(small))
        check(ret == (int)len && (len == 0 || !memcmp(small, val, len)));

    /* A same-size rewrite lands in place and reads back */
    if (val && len && len <= 64) {
        u8 old[64], neu[64];
        memcpy(old, val, len);
        for (u32 i = 0; i < len; i++)
            neu[i] = old[i] ^ 0x5a;
        if (adt_setprop(adt, node, name, neu, len) == (int)len) {
            const void *now = adt_getprop(adt, node, name, NULL);
            /* An earlier property of the same name wins both lookups */
            check(now && !memcmp(now, neu, len));
            check(adt_setprop(adt, node, name, old, len) == (int)len);
        }
        check(adt_setprop(adt, node, name, neu, len + 1) < 0);
    }
}

static void touch_node(int node)
{
    static const char *const compats[] = {"apple,arm-platform", "uart-1,samsung", "pmgr1,t8103",
                                          "", "a"};

    for (size_t i = 0; i < sizeof(compats) / sizeof(*compats); i++) {
        sink += adt_is_compatible(adt, node, compats[i]);
        for (size_t idx = 0; idx < 3; idx++)
            sink += adt_is_compatible_at(adt, node, compats[i], idx);
    }

    u32 v32;
    u64 v64;
    sink += ADT_GETPROP(adt, node, "#address-cells", &v32);
    sink += ADT_GETPROP(adt, node, "reg", &v64);
    sink += adt_subnode_offset(adt, node, "cpu0");
    sink += adt_subnode_offset(adt, node, "missing");
}

static void walk(int node, int depth, char *path, size_t plen)
{
    check(node >= 0 && (u32)node < adt_size);
    check(adt_get_property_count(adt, node) > 0);
    check(adt_get_child_count(adt, node) >= 0);

    ADT_FOREACH_PROPERTY(adt, node, prop)
    {
        touch_property(node, prop);
    }
    touch_node(node);

    const char *name = depth ? adt_get_name(adt, node) : "";
    if (!name)
        return;
    size_t nlen = strlen(name);
    bool linkable = depth && nlen && plen + 1 + nlen < PATH_MAX_LEN && !strchr(name, '/') &&
                    name_ok(name, nlen + 1);
    if (linkable) {
        path[plen] = '/';
        memcpy(path + plen + 1, name, nlen + 1);
        plen += 1 + nlen;
        if (npaths < MAX_PATHS) {
            memcpy(paths[npaths], path, plen + 1);
            path_nodes[npaths++] = node;
        }
    }

    if (depth >= WALK_MAX_DEPTH || (depth && !linkable))
        return;

    int child = node;
    ADT_FOREACH_CHILD(adt, child)
    {
        walk(child, depth + 1, path, plen);
    }
    path[plen] = 0;
}

static bool names_match(const char *name, const char *query)
{
    size_t len = strlen(query);

    if (!name || strncmp(name, query, len))
        return false;
    return !name[len] || (name[len] == '@' && !strchr(query, '@'));
}

static void lookup(const char *path)
{
    int trace[8];

    int node = adt_path_offset(adt, path);
    memset(trace, 0x5a, sizeof(trace));
    int traced = adt_path_offset_trace(adt, path, trace);
    /* The trace has room for eight nodes; a deeper path is BadPath (-5) there only */
    check(node == traced || traced == -5);
    if (traced < 0)
        return;
    check((u32)node < adt_size);
    if (node == 0)
        return;

    for (int i = 0; i < 3; i++) {
        u64 addr, size;
        sink += adt_get_reg(adt, trace, "reg", i, &addr, &size);
    }
    sink += adt_get_reg(adt, trace, "reg", 0x40000000, NULL, NULL);
}

int LLVMFuzzerTestOneInput(const u8 *data, size_t size)
{
    static const char *const common[] = {
        "/",
        "/arm-io",
        "/arm-io/pmgr",
        "/arm-io/aic",
        "/arm-io/uart0",
        "/cpus",
        "/cpus/cpu0",
        "/chosen",
        "/chosen/memory-map",
        "/device-tree",
        "/product",
        "/arm-io/dart-disp0/mapper-disp0",
        /* Odd ones: repeated and trailing slashes, a relative path, unit addresses, too deep */
        "//arm-io//pmgr/",
        "arm-io",
        "/arm-io/dart@1",
        "/arm-io/dart",
        "/a/b/c/d/e/f/g/h/i",
        "/d11/d10/d9/d8/d7/d6/d5/d4/d3/d2/d1/d0/leaf",
    };
    char path[PATH_MAX_LEN];

    if (size < sizeof(struct adt_node_hdr) || size > SZ_1M)
        return 0;

    /* The reader needs a 4-byte aligned image; malloc() gives more */
    adt = malloc(size);
    memcpy(adt, data, size);
    adt_size = size;
    npaths = 0;

    if (adt_check_header(adt) == 0) {
        path[0] = 0;
        walk(0, 0, path, 0);
        for (size_t i = 0; i < sizeof(common) / sizeof(*common); i++)
            lookup(common[i]);
        for (int i = 0; i < npaths; i++) {
            lookup(paths[i]);
            /*
             * A node's own path may resolve to an earlier node of the same name (first match
             * wins, and "x" also matches "x@1"), but whatever it finds must match the name.
             */
            int found = adt_path_offset(adt, paths[i]);
            if (found >= 0)
                check(names_match(adt_get_name(adt, found), adt_get_name(adt, path_nodes[i])));
        }
    }

    free(adt);
    adt = NULL;
    return 0;
}
