/* SPDX-License-Identifier: MIT */
/*
 * The MTE tag storage, for kboot. On SoCs with MTE2, iBoot keeps dram-size / 32 at the top of the
 * RAM it hands over for the tags, and writes the base to TagOffset_EL2 ([41:20], locked by bit 63).
 * The tags of PA p live at base + (p - dram-base) / 32. The OS must not use that range as RAM:
 * /memory ends at the base and a no-map node describes the storage. If any check fails, MTE stays
 * off.
 *
 * Kept out of kboot.c: the raw image puts kboot.o's code in the .init slot, which has no room.
 */

#include "kboot_mte.h"
#include "adt.h"
#include "ctxr_exec.h"
#include "exception.h"
#include "heapblock.h"
#include "string.h"
#include "utils.h"
#include "vsprintf.h"
#include "xnuboot.h"

#include "libfdt/libfdt.h"

#define bail(...)                                                                                  \
    do {                                                                                           \
        printf(__VA_ARGS__);                                                                       \
        return -1;                                                                                 \
    } while (0)

#define SYS_IMP_APL_TAG_OFFSET_EL2 sys_reg(3, 0, 11, 9, 0)
#define TAG_OFFSET_BASE            GENMASK(41, 20)
#define TAG_OFFSET_LOCK            BIT(63)
#define MTE_PFR1_FIELD             GENMASK(11, 8)
#define MTE_PFR1_MTE2              2

static bool mte_range_overlaps(u64 start, u64 end, u64 base, u64 top)
{
    return start < top && end > base;
}

static const char *mte_check_tag_storage(void *dt, u64 os_end, u64 dram_base, u64 dram_size,
                                         u64 ram_start, u64 ram_end, u64 *storage_base)
{
    int exc_before = exc_count;
    u64 val;

    if (!in_el2())
        return "not at EL2";
    exc_guard = GUARD_MARK | GUARD_SILENT;
    val = mrs(SYS_IMP_APL_TAG_OFFSET_EL2);
    sysop("isb");
    exc_guard = GUARD_OFF;
    if (exc_count != exc_before)
        return "TagOffset_EL2 read faulted";
    printf("kboot: MTE: TagOffset_EL2 = 0x%lx\n", val);

    u64 base = val & TAG_OFFSET_BASE, size = dram_size / 32;
    if (!(val & TAG_OFFSET_LOCK))
        return "TagOffset_EL2 is not locked";
    if (!base || !size || dram_size % 32 || size & (SZ_1M - 1))
        return "bad tag storage base or size";
    if (base < dram_base || base - dram_base > dram_size - size)
        return "tag storage outside DRAM";
    /* The RAM window starts below the storage and ends inside it. */
    if (ram_start >= base || ram_end <= base || ram_end > base + size)
        return "RAM window does not end inside the tag storage";
    /* aura's pages above the window must not hold the tags of RAM below the storage. */
    if (ram_end < base + (base - dram_base) / 32)
        return "pages above the RAM window overlap live tags";

    u32 dies = 0;
    int node = adt_path_offset(adt, "/arm-io");
    if (node < 0 || ADT_GETPROP(adt, node, "die-count", &dies) < 0 || dies != 1)
        return "not a single-die SoC";

    /* iBoot's carve-outs: one is the storage, one is the RAM it hands over, ending with it. */
    bool storage = false, handed = false;
    node = adt_path_offset(adt, "/chosen/carveout-memory-map");
    if (node < 0)
        return "no carve-out map";
    ADT_FOREACH_PROPERTY(adt, node, prop)
    {
        u64 r[2];
        if (strncmp(prop->name, "region-id-", 10) || prop->size != sizeof(r))
            continue;
        memcpy(r, prop->value, sizeof(r));
        storage |= r[0] == base && r[1] == size;
        handed |= r[0] == ram_start && r[1] == base + size - ram_start;
    }
    if (!storage)
        return "no carve-out is the tag storage";
    if (!handed)
        return "iBoot's RAM does not end with the tag storage";

    /* Everything Linux needs stays in the RAM below the storage. */
    u64 low_end = max((u64)_end, max(heapblock_high_water(), cur_boot_args.top_of_kernel_data));
    u64 win_base, win_size;
    if (ctxr_exec_window(&win_base, &win_size))
        low_end = max(low_end, win_base + win_size);
    low_end = max(low_end, os_end);
    if (low_end > base)
        return "aura, the heap, the initrd or the device tree lies in the tag storage";

    int resv = fdt_path_offset(dt, "/reserved-memory");
    if (resv < 0)
        return "no /reserved-memory";
    fdt_for_each_subnode(node, dt, resv)
    {
        int len;
        const fdt64_t *reg = fdt_getprop(dt, node, "reg", &len);
        if (reg && len >= 16 &&
            mte_range_overlaps(fdt64_ld(&reg[0]), fdt64_ld(&reg[0]) + fdt64_ld(&reg[1]), base,
                               ram_end))
            return "a reserved-memory node lies in the tag storage";
    }

    *storage_base = base;
    return NULL;
}

int kboot_mte_reserve(void *dt, u64 os_end, u64 dram_base, u64 dram_size, u64 ram_start,
                      u64 *ram_end)
{
    if (FIELD_GET(MTE_PFR1_FIELD, mrs(SYS_ID_AA64PFR1_EL1)) < MTE_PFR1_MTE2)
        return 0;

    u64 base = 0;
    const char *why =
        mte_check_tag_storage(dt, os_end, dram_base, dram_size, ram_start, *ram_end, &base);
    if (!why) {
        char path[64];
        fdt64_t reg[2] = {cpu_to_fdt64(base), cpu_to_fdt64(*ram_end - base)};
        fdt64_t whole[2] = {cpu_to_fdt64(base), cpu_to_fdt64(dram_size / 32)};

        /* Adding a node moves the offsets after it: look each one up right before its use. */
        snprintf(path, sizeof(path), "/reserved-memory/mte-tag-storage@%lx", base);
        int node = fdt_add_subnode(dt, fdt_path_offset(dt, "/reserved-memory"),
                                   path + sizeof("/reserved-memory/") - 1);
        if (node < 0 || fdt_setprop_string(dt, node, "compatible", "apple,mte-tag-storage") ||
            fdt_setprop(dt, node, "reg", reg, sizeof(reg)) ||
            fdt_setprop_empty(dt, node, "no-map") ||
            fdt_setprop(dt, fdt_path_offset(dt, "/chosen"), "aurora,mte-tag-storage", whole,
                        sizeof(whole))) {
            why = "could not add the reservation";
            /* Leave no half-made reservation behind. */
            node = fdt_path_offset(dt, path);
            if (node >= 0 && fdt_del_node(dt, node))
                bail("FDT: couldn't remove the partial %s\n", path);
        }
    }
    if (!why) {
        printf("kboot: MTE tag storage 0x%lx..0x%lx reserved; RAM ends at 0x%lx\n", base,
               base + dram_size / 32, base);
        *ram_end = base;
        return 0;
    }

    printf("kboot: MTE off: %s\n", why);
    int chosen = fdt_path_offset(dt, "/chosen");
    if (chosen < 0)
        bail("FDT: /chosen not found\n");
    int len = 0;
    const char *args = fdt_getprop(dt, chosen, "bootargs", &len);
    if (!args || len < 1 || !strstr(args, "arm64.nomte")) {
        static const char nomte[] = " arm64.nomte";
        char *prop;
        if (!args || len < 1)
            len = 1;
        if (fdt_setprop_placeholder(dt, chosen, "bootargs", len + sizeof(nomte) - 1,
                                    (void **)&prop))
            bail("FDT: couldn't add arm64.nomte to bootargs\n");
        memcpy(prop + len - 1, nomte, sizeof(nomte));
    }
    char note[96];
    snprintf(note, sizeof(note), "off: %s", why);
    if (fdt_setprop_string(dt, chosen, "aurora,mte", note))
        bail("FDT: couldn't set /chosen/aurora,mte\n");
    return 0;
}
