/* SPDX-License-Identifier: MIT */
/*
 * PMP values for a DT without the pmp alias (T6050, whose PMP is pmp0): pair each enabled PMP
 * with its ADT node by register window and copy this Mac's values onto it, as kboot.c's alias
 * path does, without touching its status. A board DT carries one Mac's values, and they vary from
 * Mac to Mac (the DRAM vendor, for one). Never fatal and all or nothing: an unpaired or ambiguous
 * window, a missing nub, a malformed value or a full DT leaves the node as the DT has it.
 *
 * The code goes in .rodata (AURA_XTEXT): the raw image puts kboot.o's code in the .init slot, and
 * neither that nor .text has room for it.
 */

#include "kboot_pmp.h"
#include "adt.h"
#include "devicetree.h"
#include "malloc.h"
#include "string.h"
#include "types.h"
#include "utils.h"
#include "vsprintf.h"

#include "libfdt/libfdt.h"

static AURA_XTEXT bool adt_name_is_component(const char *s, u32 nlen)
{
    if (!s || nlen < 2 || s[nlen - 1] != '\0')
        return false;
    for (u32 i = 0; i < nlen - 1; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x21 || c > 0x7e || c == '/')
            return false;
    }
    return true;
}

static AURA_XTEXT int adt_find_pmp_nub(int pmp_anode, const char *name)
{
    char expected[64];
    int elen = snprintf(expected, sizeof(expected), "iop-%s-nub", name);

    if (elen <= 0 || (size_t)elen >= sizeof(expected))
        return -1;
    int child = pmp_anode;
    ADT_FOREACH_CHILD(adt, child)
    {
        u32 nlen = 0;
        const char *cname = adt_getprop(adt, child, "name", &nlen);
        if (cname && nlen == (u32)elen + 1 && !memcmp(cname, expected, nlen))
            return child;
    }
    return -1;
}

/*
 * The ADT node of device_type "pmp" that has a register window at @address, with its name in
 * @name: -1 if none has it, -2 if more than one does. The window joins the two trees, as for the
 * DARTs: names need not match (T6050's DT has pmp@284e00000, whose first reg, the PMP window
 * 0x284500000, is the ADT pmp0's third).
 */
static AURA_XTEXT int adt_find_pmp_by_window(u64 address, char *name, size_t name_size)
{
    int path[8];
    int arm_io = adt_path_offset_trace(adt, "/arm-io", path);
    int depth = 0, found = -1;

    if (arm_io < 0)
        return -1;
    while (depth < (int)ARRAY_SIZE(path) && path[depth])
        depth++;
    if (depth + 1 >= (int)ARRAY_SIZE(path))
        return -1;
    path[depth + 1] = 0;

    int node = arm_io;
    ADT_FOREACH_CHILD(adt, node)
    {
        u32 len = 0, nlen = 0;
        const char *type = adt_getprop(adt, node, "device_type", &len);

        if (!type || len != sizeof("pmp") || memcmp(type, "pmp", sizeof("pmp")))
            continue;
        const char *child = adt_getprop(adt, node, "name", &nlen);
        if (nlen > name_size || !adt_name_is_component(child, nlen))
            continue;
        path[depth] = node;
        for (int i = 0; i < 16; i++) {
            u64 base;

            if (adt_get_reg(adt, path, "reg", i, &base, NULL) < 0)
                break;
            if (base != address)
                continue;
            if (found >= 0)
                return -2;
            found = node;
            memcpy(name, child, nlen);
            break;
        }
    }
    return found;
}

AURA_XTEXT int kboot_pmp_set_by_window(void *dt, int dt_bufsize,
                                       int (*fill)(int pmp_node, int nub_anode))
{
    int node = -1, enabled = 0;

    while ((node = fdt_node_offset_by_compatible(dt, node, "apple,t6000-pmp-v2")) >= 0) {
        int len;
        const char *status = fdt_getprop(dt, node, "status", &len);
        if (status ? (len <= 0 || status[len - 1] != '\0' ||
                      (strcmp(status, "okay") && strcmp(status, "ok")))
                   : len != -FDT_ERR_NOTFOUND)
            continue;
        enabled++;

        char path[64], name[32];
        if (fdt_get_path(dt, node, path, sizeof(path)))
            snprintf(path, sizeof(path), "pmp");
        u64 address = dt_get_address(dt, node);
        int anode = address ? adt_find_pmp_by_window(address, name, sizeof(name)) : -1;
        if (anode < 0) {
            printf("FDT: %s: %s at 0x%lx; keeping the DT's values\n", path,
                   !address      ? "no register window"
                   : anode == -2 ? "more than one ADT PMP has its window"
                                 : "no ADT PMP has its window",
                   address);
            continue;
        }
        int nub_anode = adt_find_pmp_nub(anode, name);
        void *saved = nub_anode < 0 ? NULL : malloc(dt_bufsize);
        if (!saved) {
            printf("FDT: %s: %s; keeping the DT's values\n", path,
                   nub_anode < 0 ? "no ADT PMP nub" : "no memory for the PMP values");
            continue;
        }
        memcpy(saved, dt, dt_bufsize);
        if (fill(node, nub_anode)) {
            memcpy(dt, saved, dt_bufsize);
            printf("FDT: %s: ADT %s values not copied; keeping the DT's values\n", path, name);
        } else {
            const fdt32_t *bdid = fdt_getprop(dt, node, "apple,board-id", NULL);
            const fdt32_t *dvid = fdt_getprop(dt, node, "apple,dram-vendor-id", NULL);
            printf("FDT: %s: this Mac's PMP values from ADT %s (board-id %u, dram-vendor-id %u)\n",
                   path, name, fdt32_ld(bdid), fdt32_ld(dvid));
        }
        free(saved);
    }
    if (!enabled)
        printf("FDT: pmp not found in devtree\n");
    return 0;
}
