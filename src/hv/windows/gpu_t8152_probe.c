/* SPDX-License-Identifier: MIT */

/* G1 mediated forwarding decisions and the access map.  See gpu_t8152_probe.h. */

#include "gpu_t8152_probe.h"
#include "string.h"
#include "utils.h"

/*
 * Source: our own observation.  The macOS 27.2 trace O-125300 (and O-122333/O-124838) logged the
 * first read of every sgx address while the whole window was hooked: the AP read exactly these
 * ID words and the core-mask word at bring-up (gpu/FACT-LEDGER.md G-10, B-09); our J804 Linux run
 * read the same words with the same values once sgx was powered.  No other sgx register is
 * forwarded in G1: the rest of B-09 (0xde000c, 0xd06030, the 0xe90000 block inside pmgr-child,
 * the KSM block) belongs to init-data and submission (G2) and stays denied and logged.
 */
static const struct gpu_probe_reg sgx_reads[] = {
    {0xd04000, "GPU ID word 0"},
    {0xd04008, "GPU ID word 2"},
    {0xd04010, "GPU ID word 4"},
    {0xd04014, "GPU ID word 5"},
    {0xd04018, "GPU ID word 6"},
    {0xd0401c, "GPU ID word 7"},
    {0xe01500, "core mask"},
};

/* Offsets inside a GFX post-office window (ledger C-01, C-02). */
#define MBOX_RX     0x00
#define MBOX_TX     0x10
#define MBOX_STATUS 0x20

const struct gpu_probe_reg *gpu_t8152_probe_sgx_reads(u32 *count)
{
    *count = ARRAY_SIZE(sgx_reads);
    return sgx_reads;
}

static u32 decide_mailbox(const struct gpu_t8152_admission *a, u64 addr, int width, bool write,
                          bool whole)
{
    u32 role;
    if (!gpu_t8152_mailbox_window(a, addr, &role))
        return GPU_P_DENY;
    u64 off = addr - (a->mailbox.base + a->mailbox_num[role] * GPU_T8152_MBOX_STRIDE);
    if (!write) {
        if (off == MBOX_RX && width == 4)
            return GPU_P_PASS;
        if ((off == MBOX_RX || off == MBOX_RX + 8) && width == 3 && !whole)
            return GPU_P_PASS;
        if (off == MBOX_STATUS && width == 2)
            return GPU_P_PASS;
        return GPU_P_DENY;
    }
    if (off == MBOX_TX && width == 4)
        return GPU_P_PASS;
    if ((off == MBOX_TX || off == MBOX_TX + 8) && width == 3 && !whole)
        return GPU_P_PASS;
    return GPU_P_DENY;
}

static u32 decide(const struct gpu_t8152_admission *a, const struct gpu_range *table, u32 count,
                  u64 addr, int width, bool write, bool whole, u32 *cls)
{
    const struct gpu_range *r = gpu_t8152_find(table, count, addr);
    *cls = r ? r->cls : GPU_C_NONE;
    if (!r || width < 0 || width > 4)
        return GPU_P_DENY;
    u64 n = 1UL << width;
    /* Naturally aligned, and inside the one range (so never into a neighbouring class). */
    if (addr & (n - 1) || addr - r->base > r->size - n)
        return GPU_P_DENY;

    switch (r->cls) {
        case GPU_C_SGX: {
            if (write || width != 2)
                return GPU_P_DENY;
            u64 off = addr - a->sgx.base;
            for (u32 i = 0; i < ARRAY_SIZE(sgx_reads); i++)
                if (off == sgx_reads[i].offset)
                    return GPU_P_PASS;
            return GPU_P_DENY;
        }
        case GPU_C_ASC_CTRL: {
            if (r->tag >= GPU_T8152_ROLES || width != 2)
                return GPU_P_DENY;
            u64 off = addr - a->wrapper[r->tag].base;
            if (off == GPU_T8152_ASC_CPU_CONTROL)
                return write ? GPU_P_CPU_CONTROL : GPU_P_PASS;
            if (off == GPU_T8152_ASC_CPU_STATUS && !write)
                return GPU_P_PASS;
            return GPU_P_DENY;
        }
        case GPU_C_MAILBOX:
            return decide_mailbox(a, addr, width, write, whole);
        default:
            /* sgx holes, iop-vbar pages, the rest of the wrappers, anything else: never. */
            return GPU_P_DENY;
    }
}

u32 gpu_t8152_probe_decide(const struct gpu_t8152_admission *a, const struct gpu_range *table,
                           u32 count, u64 addr, int width, bool write, u32 *cls)
{
    return decide(a, table, count, addr, width, write, false, cls);
}

u32 gpu_t8152_probe_decide_g2(const struct gpu_t8152_admission *a, const struct gpu_range *table,
                              u32 count, u64 addr, int width, bool write, u32 *cls)
{
    return decide(a, table, count, addr, width, write, true, cls);
}

const char *gpu_access_disp_name(u32 disp)
{
    static const char *const names[GPU_A_COUNT] = {"denied", "passed", "mediated", "refused",
                                                   "fault",  "locked", "blocked"};
    return disp < GPU_A_COUNT ? names[disp] : "?";
}

int gpu_access_note(struct gpu_access_map *m, u64 addr, int width, bool write, u32 disp, u32 cls,
                    u64 value, bool *first)
{
    *first = false;
    for (u32 i = 0; i < m->n; i++) {
        struct gpu_access *e = &m->e[i];
        if (e->addr == addr && e->width == (u8)width && e->write == (u8)write && e->disp == disp) {
            if (e->count != UINT32_MAX)
                e->count++;
            e->last = value;
            return (int)i;
        }
    }
    if (m->n >= GPU_ACCESS_MAP_MAX) {
        m->dropped++;
        return -1;
    }
    struct gpu_access *e = &m->e[m->n];
    *e = (struct gpu_access){addr, value, value, 1, (u8)width, (u8)write, (u8)disp, (u8)cls};
    *first = true;
    return (int)m->n++;
}
