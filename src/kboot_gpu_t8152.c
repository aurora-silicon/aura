/* SPDX-License-Identifier: MIT */
/* Read-only G18G firmware admission (26A428, 26B123); never starts either GPU ASC. */
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
struct gpu_build {
    /* /chosen receipt the Linux driver maps to this build's InitData layout. */
    const char *receipt;
    struct gpu_profile profiles[2];
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

static bool qualify_text(const struct gpu_profile *profile, unsigned role,
                         const struct adt_segment_ranges *seg, const u64 base[8], const u64 size[8],
                         u64 vm)
{
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

    /* The iop nub uuids name the images iBoot loaded; both must be one build's. */
    const struct gpu_build *build = NULL;
    int nub = adt_path_offset(adt, "/arm-io/gfx-asc/iop-gfx-nub");
    for (size_t b = 0; nub >= 0 && b < ARRAY_SIZE(builds); b++)
        if (string_is(nub, "uuid", builds[b].profiles[0].uuid))
            build = &builds[b];
    if (!build)
        goto refused;
    const struct gpu_profile *profiles = build->profiles;

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
        if (!qualify_text(&profiles[r], r, segments[r], base, size, vm))
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
    if (fdt_setprop_string(dt, fdt_path_offset(dt, "/chosen"), receipt, build->receipt))
        goto refused;
    printf("FDT: G18G eight regions and both firmware TEXT identities verified (%s)\n",
           build->receipt);
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

/*
 * The GPU's performance states, from this Mac's ADT (/arm-io/sgx), on the agx node for the
 * kernel, independent of the firmware admission above:
 *
 *   apple,perf-states       = <hz mV> for each state, from perf-states {u32 freq, u32 volt}
 *   apple,perf-states-sram  = <mV> for each state, from perf-states-sram {u32 freq, u32 volt}
 *
 * Both tables must have exactly gpu-num-perf-states + 1 rows, state 0 at frequency 0, the other
 * states at strictly increasing frequencies, and the SRAM table must list the same frequencies.
 * Anything else publishes neither property (and removes any the tree had), logs why, and the
 * boot goes on: never fatal. The values are passed on as they are; the kernel judges them.
 */
#define T8152_MAX_PERF_STATES 32

struct t8152_perf_state {
    u32 freq, volt;
};

int dt_set_gpu_perf_states_t8152(void *dt)
{
    struct t8152_perf_state core[T8152_MAX_PERF_STATES], sram[T8152_MAX_PERF_STATES];
    fdt32_t out[2 * T8152_MAX_PERF_STATES], out_sram[T8152_MAX_PERF_STATES];
    const char *why = "the ADT has no gpu,t8152 /arm-io/sgx";
    const void *p, *q;
    u32 len, sram_len;
    u64 n;
    int gpu = fdt_node_offset_by_compatible(dt, -1, "apple,agx-t8152");

    /* Tested on J804 only; J873g trees keep what they have. */
    if (gpu < 0 || chip_id != T8152 || board_id != 0x22)
        return 0;
    int sgx = adt_path_offset(adt, "/arm-io/sgx");
    if (sgx < 0 || !string_is(sgx, "compatible", "gpu,t8152"))
        goto refused;
    why = "gpu-num-perf-states is missing or out of range";
    if (!number(sgx, "gpu-num-perf-states", &n) || !n || n + 1 > T8152_MAX_PERF_STATES)
        goto refused;
    n++;
    why = "perf-states or perf-states-sram does not have gpu-num-perf-states + 1 rows";
    p = adt_getprop(adt, sgx, "perf-states", &len);
    q = adt_getprop(adt, sgx, "perf-states-sram", &sram_len);
    if (!p || !q || len != n * sizeof(*core) || sram_len != n * sizeof(*sram))
        goto refused;
    memcpy(core, p, len);
    memcpy(sram, q, sram_len);
    why = "state 0 is not at frequency 0";
    if (core[0].freq || sram[0].freq)
        goto refused;
    for (u32 i = 0; i < n; i++) {
        why = "the frequencies do not strictly increase";
        if (i && core[i].freq <= core[i - 1].freq)
            goto refused;
        why = "perf-states-sram lists other frequencies than perf-states";
        if (sram[i].freq != core[i].freq)
            goto refused;
        out[2 * i] = cpu_to_fdt32(core[i].freq);
        out[2 * i + 1] = cpu_to_fdt32(core[i].volt);
        out_sram[i] = cpu_to_fdt32(sram[i].volt);
    }

    why = "the device tree could not be updated";
    if (fdt_setprop(dt, gpu, "apple,perf-states", out, n * 2 * sizeof(*out)))
        goto refused;
    gpu = fdt_node_offset_by_compatible(dt, -1, "apple,agx-t8152");
    if (gpu < 0 || fdt_setprop(dt, gpu, "apple,perf-states-sram", out_sram, n * sizeof(*out_sram)))
        goto refused;
    printf("FDT: G18G %u performance states copied from the ADT\n", (u32)n);
    return 0;

refused:
    /* Both or neither: never a table the ADT did not give whole. */
    gpu = fdt_node_offset_by_compatible(dt, -1, "apple,agx-t8152");
    if (gpu >= 0)
        fdt_delprop(dt, gpu, "apple,perf-states");
    gpu = fdt_node_offset_by_compatible(dt, -1, "apple,agx-t8152");
    if (gpu >= 0)
        fdt_delprop(dt, gpu, "apple,perf-states-sram");
    printf("FDT: G18G performance states not published: %s\n", why);
    return 0;
}
