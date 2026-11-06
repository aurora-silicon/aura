/* SPDX-License-Identifier: MIT */
/* Read-only 26A428 G18G firmware admission; never starts either GPU ASC. */
#include "adt.h"
#include "kboot.h"
#include "malloc.h"
#include "soc.h"
#include "string.h"
#include "utils.h"
#include "xnuboot.h"

#include "libfdt/libfdt.h"

extern void aura_sha256(const void *data, size_t size, u8 digest[32]);

struct gpu_profile {
    const char *role, *uuid;
    u32 text_size, patchbay;
    u8 digest[32];
};
struct gpu_tlv {
    char tag[5];
    u32 size;
    bool exclude;
};
#include "kboot_gpu_t8152_profile.h"

static const struct {
    const char *adt, *fdt, *name;
} regions[] = {
    {"gpu-region", "uat-ttbs", "ttbs"},
    {"gfx-shared-region", "uat-pagetables", "pagetables"},
    {"gfx-shared-l2-region", "uat-l2-pagetables", "shared-l2"},
    {"gfx-handoff", "uat-handoff", "handoff"},
    {"gfx-data", "gfx-data", "gfx-data"},
    {"gfx1-data", "gfx1-data", "gfx1-data"},
    {"gfx-data-shared-ro", "gfx-data-shared-ro", "gfx-data-shared-ro"},
    {"gfx1-data-shared-ro", "gfx1-data-shared-ro", "gfx1-data-shared-ro"},
};
static const char *mailboxes[] = {"/soc/mailbox@582600000", "/soc/mailbox@582e00000"};
static const u64 wrappers[] = {0x582600000ULL, 0x582e00000ULL};
static const char receipt[] = "aurora,g18g-gpu-firmware-profile";

static bool number(int node, const char *prop, u64 *value)
{
    u32 len;
    const void *p = adt_getprop(adt, node, prop, &len);
    *value = 0;
    if (!p || (len != 4 && len != 8))
        return false;
    memcpy(value, p, len);
    return true;
}

static bool string_is(int node, const char *prop, const char *value)
{
    u32 len;
    const void *p = adt_getprop(adt, node, prop, &len);
    return p && len == strlen(value) + 1 && !memcmp(p, value, len);
}

static bool range_ok(u64 base, u64 size, u64 low, u64 high)
{
    return size && !((base | size) & (SZ_16K - 1)) && base >= low && base < high &&
           size <= high - base;
}

static bool overlaps(u64 a, u64 n, u64 b, u64 m)
{
    return a < b + m && b < a + n;
}

static bool qualify_text(unsigned role, const struct adt_segment_ranges *seg, const u64 base[8],
                         const u64 size[8], u64 vm)
{
    const struct gpu_profile *profile = &profiles[role];
    u8 *copy = malloc(profile->text_size);
    if (!copy)
        return false;
    memcpy(copy, (const void *)seg->phys, profile->text_size);
    u32 offset = profile->patchbay;
    bool valid = false;
    for (size_t i = 0; i < ARRAY_SIZE(tlvs); i++) {
        u32 length;
        if (offset > profile->text_size || profile->text_size - offset < 8)
            goto out;
        memcpy(&length, copy + offset + 4, 4);
        if (memcmp(copy + offset, tlvs[i].tag, 4) || length != tlvs[i].size ||
            length > sizeof(u64) || length > profile->text_size - offset - 8)
            goto out;
        u64 value = 0, expected = 0;
        bool check = true;
        memcpy(&value, copy + offset + 8, length);
        /* Tags are stored in reverse byte order in the firmware patchbay. */
        if (!memcmp(tlvs[i].tag, "BPTP", 4))
            expected = base[1] + role * (size[1] / 2);
        else if (!memcmp(tlvs[i].tag, "SPTP", 4))
            expected = size[1] / 2;
        else if (!memcmp(tlvs[i].tag, "BlpP", 4))
            expected = base[3];
        else if (!memcmp(tlvs[i].tag, "S2xG", 4))
            expected = base[2];
        else if (!memcmp(tlvs[i].tag, "BVTP", 4))
            expected = vm;
        else if (!memcmp(tlvs[i].tag, "dArW", 4))
            expected = wrappers[role];
        else if (!memcmp(tlvs[i].tag, "_COS", 4))
            expected = T8152;
        else
            check = false;
        if (check && value != expected) {
            printf("GPU: %s TLV %.4s disagrees with this boot's ADT\n", profile->role, tlvs[i].tag);
            goto out;
        }
        if (tlvs[i].exclude)
            memset(copy + offset + 8, 0, length);
        offset += 8 + length;
    }
    if (offset != profile->patchbay + 0x23a)
        goto out;
    memset(copy + 0x224, 0, 4);
    memset(copy + 0x23c, 0, 8);
    u8 digest[32];
    aura_sha256(copy, profile->text_size, digest);
    valid = !memcmp(digest, profile->digest, sizeof(digest));
out:
    free(copy);
    return valid;
}

int dt_set_gpu_t8152(void *dt)
{
    int gpu = fdt_node_offset_by_compatible(dt, -1, "apple,agx-t8152");
    if (gpu == -FDT_ERR_NOTFOUND)
        return 0;
    int chosen = adt_path_offset(adt, "/chosen");
    int sgx = adt_path_offset(adt, "/arm-io/sgx");
    int defaults = adt_path_offset(adt, "/defaults");
    int reserved = fdt_path_offset(dt, "/reserved-memory");
    u64 dram, dram_size, width, mode, vm;
    if (gpu < 0 || chosen < 0 || sgx < 0 || defaults < 0 || reserved < 0 || chip_id != T8152 ||
        board_id != 0x24 || !string_is(sgx, "compatible", "gpu,t8152") ||
        !number(chosen, "dram-base", &dram) || !number(chosen, "dram-size", &dram_size) ||
        !dram_size || dram > UINT64_MAX - dram_size ||
        !number(defaults, "uat-vaddr-size", &width) || width != 43 ||
        !number(sgx, "agx-address-space-mgmt-mode", &mode) || mode != 1 ||
        !number(sgx, "rtkit-private-vm-region-base", &vm) || fdt_address_cells(dt, reserved) != 2 ||
        fdt_size_cells(dt, reserved) != 2)
        goto refused;

    for (unsigned i = 0; i < 2; i++) {
        char key[48];
        const char *prefix = i ? "gfx1" : "gfx";
        const char *suffix[] = {"base", "size"};
        for (unsigned j = 0; j < 2; j++) {
            snprintf(key, sizeof(key), "%s-data-shared-rw-%s", prefix, suffix[j]);
            u64 value;
            if (adt_getprop(adt, sgx, key, NULL) && (!number(sgx, key, &value) || value))
                goto refused;
        }
    }
    u64 base[8], size[8];
    int refs_len;
    const fdt32_t *refs = fdt_getprop(dt, gpu, "memory-region", &refs_len);
    if (!refs || refs_len != 8 * sizeof(*refs) ||
        fdt_stringlist_count(dt, gpu, "memory-region-names") != 8)
        goto refused;
    for (unsigned i = 0; i < 8; i++) {
        char key[64], path[80];
        snprintf(key, sizeof(key), "%s-base", regions[i].adt);
        if (!number(sgx, key, &base[i]))
            goto refused;
        snprintf(key, sizeof(key), "%s-size", regions[i].adt);
        if (!number(sgx, key, &size[i]) || !range_ok(base[i], size[i], dram, dram + dram_size))
            goto refused;
        for (unsigned j = 0; j < i; j++)
            if (overlaps(base[i], size[i], base[j], size[j]))
                goto refused;
        snprintf(path, sizeof(path), "/reserved-memory/%s", regions[i].fdt);
        int node = fdt_path_offset(dt, path);
        int idx = fdt_stringlist_search(dt, gpu, "memory-region-names", regions[i].name);
        if (node < 0 || idx < 0 || idx >= 8 ||
            !!fdt_getprop(dt, node, "no-map", NULL) != (i >= 4) ||
            fdt_node_offset_by_phandle(dt, fdt32_ld(refs + idx)) != node)
            goto refused;
    }
    if (size[1] < 0x44000 || (size[1] & 0x7fff))
        goto refused;

    const struct adt_segment_ranges *segments[2];
    for (unsigned r = 0; r < 2; r++) {
        char path[80];
        int trace[8];
        snprintf(path, sizeof(path), "/arm-io/%s", profiles[r].role);
        int node = adt_path_offset_trace(adt, path, trace);
        u64 mb;
        if (node < 0 || !string_is(node, "compatible", "iop,ascwrap-v8") ||
            !string_is(node, "msgbox-using-postoffice", "true") ||
            !number(node, "msgbox-mailbox-num", &mb) || mb != r + 1 ||
            !string_is(node, "segment-names", "__TEXT;__DATA;__DATA_SHARED_RO"))
            goto refused;
        u64 expected[][2] = {
            {wrappers[r], 0x74000}, {wrappers[r] - 0x5b0000, 8}, {0x316020000ULL, 0x10000}};
        for (unsigned i = 0; i < 3; i++) {
            u64 addr, span;
            if (adt_get_reg(adt, trace, "reg", i, &addr, &span) || addr != expected[i][0] ||
                span != expected[i][1])
                goto refused;
        }
        u32 len;
        segments[r] = adt_getprop(adt, node, "segment-ranges", &len);
        if (!segments[r] || len != 3 * sizeof(**segments))
            goto refused;
        snprintf(path, sizeof(path), "/arm-io/%s/iop-%s-nub", profiles[r].role, r ? "gfx1" : "gfx");
        node = adt_path_offset(adt, path);
        if (node < 0 || !string_is(node, "uuid", profiles[r].uuid))
            goto refused;
        for (unsigned i = 0; i < 3; i++) {
            const struct adt_segment_ranges *s = segments[r] + i;
            if (s->phys != s->remap || !range_ok(s->phys, s->size, dram, dram + dram_size) ||
                s->phys + s->size > cur_boot_args.phys_base)
                goto refused;
        }
        if (segments[r][0].size != profiles[r].text_size || segments[r][1].phys != base[4 + r] ||
            segments[r][1].size != size[4 + r] || segments[r][2].phys != base[6 + r] ||
            segments[r][2].size != size[6 + r])
            goto refused;
        for (unsigned i = 0; i < 8; i++)
            if (overlaps(segments[r][0].phys, segments[r][0].size, base[i], size[i]))
                goto refused;
        node = fdt_path_offset(dt, mailboxes[r]);
        if (node < 0 || fdt_node_check_compatible(dt, node, "apple,t8152-gfx-ascwrap-v8") ||
            fdt_stringlist_search(dt, node, "status", "disabled") != 0)
            goto refused;
    }
    if (overlaps(segments[0][0].phys, segments[0][0].size, segments[1][0].phys,
                 segments[1][0].size))
        goto refused;
    for (unsigned r = 0; r < 2; r++)
        if (!qualify_text(r, segments[r], base, size, vm))
            goto refused;

    /* Publish only after all regions, identities and boot-local TLVs pass. */
    for (unsigned i = 0; i < 8; i++) {
        char path[80];
        snprintf(path, sizeof(path), "/reserved-memory/%s", regions[i].fdt);
        int node = fdt_path_offset(dt, path);
        fdt64_t reg[] = {cpu_to_fdt64(base[i]), cpu_to_fdt64(size[i])};
        if (fdt_setprop(dt, node, "reg", reg, sizeof(reg)) ||
            fdt_setprop_string(dt, node, "status", "okay"))
            goto refused;
    }
    for (unsigned r = 0; r < 2; r++)
        if (fdt_setprop_string(dt, fdt_path_offset(dt, mailboxes[r]), "status", "okay"))
            goto refused;
    if (fdt_setprop_string(dt, fdt_path_offset(dt, "/chosen"), receipt,
                           "G18G-RTKit-3514.0.15.release"))
        goto refused;
    printf("FDT: G18G eight regions and both firmware TEXT identities verified\n");
    return 0;

refused:
    /* A partially prepared DT must never advertise admitted firmware. */
    fdt_delprop(dt, fdt_path_offset(dt, "/chosen"), receipt);
    for (unsigned r = 0; r < 2; r++) {
        int node = fdt_path_offset(dt, mailboxes[r]);
        if (node >= 0)
            fdt_setprop_string(dt, node, "status", "disabled");
    }
    printf("FDT: refusing unrecognized G18G firmware or memory layout\n");
    return -1;
}
