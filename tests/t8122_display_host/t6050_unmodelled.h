/* SPDX-License-Identifier: MIT */
/* T8122/T6041 fixtures do not model the T6050 handoff. Reaching its APIs is a failure. */
#ifndef T6050_UNMODELLED_H
#define T6050_UNMODELLED_H
#include <stdlib.h>

#include "adt.h"
#include "kboot.h"
#include "pmgr.h"
#include "xnuboot.h"

/* Link the full production source; reaching a T6050 helper below still aborts. */
struct boot_args cur_boot_args;

int kboot_fdt_path_exact(const void *fdt, const char *path)
{
    UNUSED(fdt);
    UNUSED(path);
    abort();
}

int adt_getprop_copy(const void *tree, int node, const char *name, void *out, size_t len)
{
    UNUSED(tree);
    UNUSED(node);
    UNUSED(name);
    UNUSED(out);
    UNUSED(len);
    abort();
}

int pmgr_adt_is_powered(const char *path)
{
    UNUSED(path);
    abort();
}

int pmgr_adt_collect_psregs(const char *path, u64 *out, u8 *flags, int max)
{
    UNUSED(path);
    UNUSED(out);
    UNUSED(flags);
    UNUSED(max);
    abort();
}

/*
 * adt_get_child_count(), adt_first_child_offset(), adt_next_sibling_offset() and adt_get_name()
 * come from adt_tree.h, which every host that includes this header also includes: the T8132
 * handoff walks the fake ADT's node tree.
 */
#endif
