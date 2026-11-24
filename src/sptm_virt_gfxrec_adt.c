/* SPDX-License-Identifier: MIT */

/*
 * What the GPU/PMP record (sptm_virt_gfxrec.c) takes from this machine's ADT, apart from the hooks
 * so the host runs it on a real ADT (tests/python/test_sptm_virt_gfxrec_host.py): the reg that
 * covers a block, and the power-state words of the PMGR devices named GFX, AGX or SGX.
 */

#include "adt.h"
#include "pmgr.h"
#include "sptm_virt_gfxrec.h"
#include "string.h"
#include "utils.h"

void sptm_virt_gfxrec_adt_cover(u64 pa, u64 len, char *out, size_t n)
{
    int node = adt_path_offset(adt, "/arm-io");
    u64 best = ~0UL;

    out[0] = 0;
    if (node < 0)
        return;
    ADT_FOREACH_CHILD(adt, node)
    {
        char path[64];
        int trace[8];
        snprintf(path, sizeof(path), "/arm-io/%s", adt_get_name(adt, node));
        if (adt_path_offset_trace(adt, path, trace) < 0)
            continue;
        for (int i = 0; i < 32; i++) {
            u64 a, sz;
            if (adt_get_reg(adt, trace, "reg", i, &a, &sz) < 0)
                break;
            if (pa >= a && pa + len <= a + sz && sz < best) {
                best = sz;
                snprintf(out, n, "%s reg[%d] +0x%lx", (char *)path, i, (unsigned long)(pa - a));
            }
        }
    }
}

int sptm_virt_gfxrec_adt_ps(u64 *ps, char (*names)[17], int max)
{
    const char *name;
    uintptr_t addr;
    int k = 0;

    for (u32 i = 0; k < max && pmgr_device_at(i, &name, &addr) == 0; i++) {
        char nm[17];
        memcpy(nm, name, 16);
        nm[16] = 0;
        if (!addr || (!strstr(nm, "GFX") && !strstr(nm, "AGX") && !strstr(nm, "SGX")))
            continue;
        ps[k] = addr;
        memcpy(names[k], nm, sizeof(nm));
        k++;
    }
    return k;
}
