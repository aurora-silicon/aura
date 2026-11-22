/* SPDX-License-Identifier: GPL-2.0-only */

/*
 * G2 carrier additions: recipe checks, the fw-mmio declarations, the boot-control location check
 * and the firmware message classification.  See gpu_t8152_g2.h.  No value of the private recipe
 * is in this file: it checks and anchors whatever gpu_t8152_g2_recipe() returns.
 */

#include "gpu_t8152_g2.h"
#include "adt.h"
#include "gpu_t8152_power.h"
#include "string.h"
#include "utils.h"

#define MAX_NODE_REGS 1024

static bool overlaps(u64 a, u64 n, u64 b, u64 m)
{
    return n && m && a < b + m && b < a + n;
}

int gpu_t8152_g2_check(const struct gpu_g2_recipe *r)
{
    if (!r || !r->available) {
        printf("HV: GPU: this carrier was built without the private G2 recipe "
               "(J804_PRIVATE_DATA/gpu/carrier/g18g-j804-gpu-g2.h): --windows-gpu g2 is "
               "disabled\n");
        return -1;
    }
    bool ok = r->nwindows > 0 && r->nwindows <= GPU_G2_MAX_WINDOWS &&
              r->nrefused <= GPU_G2_MAX_REFUSED && r->nroots <= GPU_G2_MAX_ROOTS &&
              !(r->boot_offset & 3) && r->boot_set && (r->boot_expect & r->boot_set) == r->boot_set;
    for (u32 i = 0; ok && i < r->nwindows; i++) {
        const struct gpu_g2_window *w = &r->window[i];
        ok = w->node && w->node[0] && w->size && w->reg_first <= w->reg_last &&
             w->reg_last < MAX_NODE_REGS && !(w->offset & 3) && !(w->size & 3) &&
             w->offset <= UINT64_MAX - w->size;
        for (u32 k = 0; ok && k < i; k++)
            ok = r->window[k].slot != w->slot;
    }
    for (u32 i = 0; ok && i < r->nroots; i++)
        ok = r->root[i].region < GPU_T8152_REGIONS && !(r->root[i].offset & (GPU_T8152_PAGE - 1));
    for (u32 i = 0; ok && i < r->nrefused; i++)
        ok = r->refused[i].ep <= 0xff && r->refused[i].type <= 0xffff;
    if (!ok) {
        printf("HV: GPU: the private G2 recipe is malformed\n");
        return -1;
    }
    return 0;
}

/* The hull [lo, hi) of regs [first, last] of /arm-io/<node>; false if a reg is missing. */
static bool node_hull(const char *node, u32 first, u32 last, u64 *first_base, u64 *lo, u64 *hi)
{
    int armio = adt_path_offset(adt, "/arm-io");
    if (armio < 0)
        return false;
    int child = armio, found = -1;
    ADT_FOREACH_CHILD(adt, child)
    {
        const char *name = adt_get_name(adt, child);
        if (name && !strcmp(name, node)) {
            if (found >= 0)
                return false; /* two nodes of that name: ambiguous */
            found = child;
        }
    }
    if (found < 0)
        return false;
    int path[3] = {armio, found, 0};
    *lo = UINT64_MAX;
    *hi = 0;
    for (u32 idx = first; idx <= last; idx++) {
        u64 b, s;
        if (adt_get_reg(adt, path, "reg", (int)idx, &b, &s) < 0 || !s || b > UINT64_MAX - s)
            return false;
        if (idx == first)
            *first_base = b;
        if (b < *lo)
            *lo = b;
        if (b + s > *hi)
            *hi = b + s;
    }
    return true;
}

int gpu_t8152_g2_declare(const struct gpu_g2_recipe *r, const struct gpu_t8152_admission *a,
                         const struct gpu_range *table, u32 count, u64 guest_lo, u64 guest_hi,
                         struct gpu_range *out, u32 cap)
{
    if (gpu_t8152_g2_check(r))
        return -1;
    if (r->nwindows > cap) {
        printf("HV: GPU: %u fw-mmio windows do not fit (%u)\n", r->nwindows, cap);
        return -1;
    }
    for (u32 i = 0; i < r->nwindows; i++) {
        const struct gpu_g2_window *w = &r->window[i];
        u64 first, lo, hi;
        if (!node_hull(w->node, w->reg_first, w->reg_last, &first, &lo, &hi)) {
            printf("HV: GPU: fw-mmio slot %u: no single /arm-io/%s with regs %u..%u\n", w->slot,
                   w->node, w->reg_first, w->reg_last);
            return -1;
        }
        u64 base = first + w->offset;
        if (base < first || base < lo || base > hi || w->size > hi - base) {
            printf("HV: GPU: fw-mmio slot %u [0x%lx, +0x%lx) leaves /arm-io/%s regs %u..%u "
                   "[0x%lx, 0x%lx)\n",
                   w->slot, base, w->size, w->node, w->reg_first, w->reg_last, lo, hi);
            return -1;
        }
        /* What the firmware's page tables can reach: whole 16 KiB pages. */
        u64 plo = ALIGN_DOWN(base, GPU_T8152_PAGE);
        u64 phi = ALIGN_UP(base + w->size, GPU_T8152_PAGE);
        if (overlaps(plo, phi - plo, a->dram_base, a->dram_size) ||
            overlaps(plo, phi - plo, guest_lo, guest_hi - guest_lo)) {
            printf("HV: GPU: fw-mmio slot %u [0x%lx, 0x%lx) touches DRAM or guest RAM\n", w->slot,
                   plo, phi);
            return -1;
        }
        for (u32 k = 0; k < count; k++) {
            const struct gpu_range *t = &table[k];
            if (t->cls == GPU_C_SGX || t->cls == GPU_C_SGX_HOLE)
                continue;
            if (overlaps(plo, phi - plo, t->base, t->size)) {
                printf("HV: GPU: fw-mmio slot %u [0x%lx, 0x%lx) touches %s range %s [0x%lx, "
                       "0x%lx): refused\n",
                       w->slot, plo, phi, gpu_class_name(t->cls), t->name, t->base,
                       t->base + t->size);
                return -1;
            }
        }
        out[i] = (struct gpu_range){base, w->size, GPU_C_FW_MMIO, w->slot,
                                    GPU_RF_DECLARED | (w->writable ? 0 : GPU_RF_READ_ONLY),
                                    w->node};
    }
    return (int)r->nwindows;
}

int gpu_t8152_g2_boot_check(const struct gpu_g2_recipe *r, const struct gpu_t8152_admission *a,
                            const struct gpu_range *table, u32 count, u64 *addr)
{
    if (gpu_t8152_g2_check(r))
        return -1;
    if (r->boot_offset > a->sgx.size - 4) {
        printf("HV: GPU: the boot-control register is outside the sgx window\n");
        return -1;
    }
    u64 pa = a->sgx.base + r->boot_offset;
    const struct gpu_range *t = gpu_t8152_find(table, count, pa);
    if (!t || t->cls != GPU_C_SGX_HOLE || !gpu_t8152_pmgr_covers(pa, 4)) {
        printf("HV: GPU: the boot-control register sgx+0x%x is not inside a pmgr sgx hole; "
               "refused\n",
               r->boot_offset);
        return -1;
    }
    *addr = pa;
    return 0;
}

bool gpu_t8152_g2_boot_accepts(const struct gpu_g2_recipe *r, u32 value)
{
    return value == r->boot_accept[0] || value == r->boot_accept[1];
}

u32 gpu_t8152_msg_kind(u64 ep, u64 msg)
{
    u32 type = (u32)(msg >> 48);
    switch (ep & 0xff) {
        case GPU_MSG_EP_MGMT:
            return GPU_MK_MGMT;
        case GPU_MSG_EP_CRASHLOG:
            return GPU_MK_CRASHLOG;
        case GPU_MSG_EP_GFX:
            return type == GPU_MSG_INIT     ? GPU_MK_INIT :
                   type == GPU_MSG_INIT_ACK ? GPU_MK_INIT_ACK :
                   type == GPU_MSG_EVENT    ? GPU_MK_EVENT :
                                              GPU_MK_GFX;
        case GPU_MSG_EP_DOORBELL:
            return type == GPU_MSG_POWER_END ? GPU_MK_POWER_END :
                   type == GPU_MSG_DOORBELL  ? GPU_MK_DOORBELL :
                   type == GPU_MSG_FWCTL     ? GPU_MK_FWCTL :
                                               GPU_MK_DOORBELL_OTHER;
        default:
            return GPU_MK_OTHER;
    }
}

const char *gpu_msg_kind_name(u32 kind)
{
    static const char *const names[GPU_MK_COUNT] = {
        "mgmt",     "crashlog",  "init-data",    "init-ack",     "event", "gfx-other",
        "post-init", "doorbell", "fw-control", "ep21-other", "other-ep", "half",
    };
    return kind < GPU_MK_COUNT ? names[kind] : "?";
}

bool gpu_t8152_g2_refused(const struct gpu_g2_recipe *r, u64 ep, u64 msg)
{
    for (u32 i = 0; r && r->available && i < r->nrefused; i++)
        if ((ep & 0xff) == r->refused[i].ep && (msg >> 48) == r->refused[i].type)
            return true;
    return false;
}

bool gpu_t8152_g2_iop_power_down(u64 ep, u64 msg)
{
    return (ep & 0xff) == GPU_MSG_EP_MGMT && GPU_MGMT_TYPE(msg) == GPU_MGMT_IOP_PWR &&
           (msg & 0xffff) != GPU_MGMT_PWR_ON;
}
