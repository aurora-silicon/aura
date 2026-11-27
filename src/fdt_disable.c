/* SPDX-License-Identifier: MIT */

#include "fdt_disable.h"
#include "string.h"

#include "libfdt/libfdt.h"

/*
 * Keep @node from being probed or seen as available by phandle consumers, whatever space the tree
 * has left: set its status to "disabled"; if that cannot grow the tree, re-read status and
 * overwrite "okay" in place with "fail" (the same length: not operational); otherwise turn the
 * whole node (including its status, phandle and children) into NOPs, as kboot_gpu_m4.c already
 * does. Neither fallback needs space. Returns 0, or a negative libfdt error with the tree
 * unchanged.
 */
int fdt_disable_node(void *fdt, int node)
{
    int len;
    const char *status;

    if (!fdt_setprop_string(fdt, node, "status", "disabled"))
        return 0;
    status = fdt_getprop(fdt, node, "status", &len);
    if (status && len == sizeof("okay") && !memcmp(status, "okay", len))
        return fdt_setprop_inplace(fdt, node, "status", "fail", sizeof("fail"));
    return fdt_nop_node(fdt, node);
}
