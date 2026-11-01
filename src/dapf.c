/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#include "dapf.h"
#include "adt.h"
#include "assert.h"
#include "malloc.h"
#include "memory.h"
#include "pmgr.h"
#include "string.h"
#include "utils.h"

#include "libfdt/libfdt.h"

struct dapf_t8020_config {
    u64 start;
    u64 end;
    u8 unk1;
    u8 r0_hi;
    u8 r0_lo;
    u8 unk2;
    u32 r4;
} PACKED;

static int dapf_init_t8020(const char *path, u64 base, int node)
{
    u32 length;
    const char *prop = "filter-data-instance-0";
    const struct dapf_t8020_config *config = adt_getprop(adt, node, prop, &length);

    if (!config || !length || (length % sizeof(*config)) != 0) {
        printf("dapf: Error getting ADT node %s property %s.\n", path, prop);
        return -1;
    }

    int count = length / sizeof(*config);

    for (int i = 0; i < count; i++) {
        write32(base + 0x04, config[i].r4);
        write64(base + 0x08, config[i].start);
        write64(base + 0x10, config[i].end);
        write32(base + 0x00, (config[i].r0_hi << 4) | config[i].r0_lo);
        base += 0x40;
    }
    return 0;
}

struct dapf_t8110_config {
    u64 start;
    u64 end;
    u32 r20;
    u32 unk1;
    u32 r4;
    u32 unk2[5];
    u8 unk3;
    u8 r0_hi;
    u8 r0_lo;
    u8 unk4;
} PACKED;

struct dapf_t8110b_config {
    u64 start;
    u64 end;
    u32 r20;
    u32 unk1;
    u32 r4;
    u32 unk2[5];
    u8 unk3;
    u8 r0_hi;
    u8 r0_lo;
    u8 unk4;
    u32 pad;
} PACKED;

static int dapf_init_t8110a(u64 base, struct dapf_t8110_config *config, u32 length)
{
    int count = length / sizeof(*config);

    for (int i = 0; i < count; i++) {
        write32(base + 0x04, config[i].r4);
        write64(base + 0x08, config[i].start);
        write64(base + 0x10, config[i].end);
        write32(base + 0x00, (config[i].r0_hi << 4) | config[i].r0_lo);
        write32(base + 0x20, config[i].r20);
        base += 0x40;
    }
    return 0;
}

static int dapf_init_t8110b(u64 base, struct dapf_t8110b_config *config, u32 length)
{
    int count = length / sizeof(*config);

    for (int i = 0; i < count; i++) {
        write32(base + 0x04, config[i].r4);
        write64(base + 0x08, config[i].start);
        write64(base + 0x10, config[i].end);
        write32(base + 0x00, (config[i].r0_hi << 4) | config[i].r0_lo);
        write32(base + 0x20, config[i].r20);
        base += 0x40;
    }
    return 0;
}

static int dapf_init_t8110(const char *path, u64 base, int node)
{
    u32 length = 0;
    char prop[32];
    const void *config = NULL;
    int instance;

    for (instance = 0; instance < 8; instance++) {
        snprintf(prop, sizeof(prop), "dapf-instance-%d", instance);
        config = adt_getprop(adt, node, prop, &length);
        if (config && length)
            break;
    }

    if (instance == 8) {
        printf("dapf: no non-empty dapf-instance property on %s\n", path);
        return -1;
    }
    printf("dapf: %s uses dapf-instance-%d\n", path, instance);

    // The least common multiple of 52 and 56 is 728 which is in the range of
    // the observed lengthe for "dapf-instance-0". The 52 byte variant is more
    // common and prefering that works so far.
    if (length % sizeof(struct dapf_t8110_config) == 0) {
        return dapf_init_t8110a(base, (struct dapf_t8110_config *)config, length);
    } else if (length % sizeof(struct dapf_t8110b_config) == 0) {
        return dapf_init_t8110b(base, (struct dapf_t8110b_config *)config, length);
    } else {
        printf("dapf: Invalid length for %s property %s\n", path, prop);
        return -1;
    }
}

enum dapf_instance_type {
    DAPF_INSTANCE_DART = 0x44415254,
    DAPF_INSTANCE_DAPF = 0x44415046,
    DAPF_INSTANCE_GAPF = 0x47415046,
};

struct dapf_instance {
    u32 type;
    u32 instance;
    char name[8];
} PACKED;

static_assert(sizeof(struct dapf_instance) == 16, "Invalid DAPF instance size");

/* A T8132 DAPF bank has 16 slices of 0x40 bytes. */
#define T8132_DAPF_SLICES 16

struct dapf_slice {
    u64 start;
    u64 end;
    u32 r0;
    u32 r4;
    u32 r20;
};

static void dapf_t8132_put(u64 base, const struct dapf_slice *slice)
{
    write32(base + 0x04, slice->r4);
    write64(base + 0x08, slice->start);
    write64(base + 0x10, slice->end);
    write32(base + 0x00, slice->r0);
    write32(base + 0x20, slice->r20);
}

static const struct dapf_t8110_config *dapf_t8132_config(const char *path, int node, u32 instance,
                                                         u32 *count)
{
    char prop[32];
    u32 length;

    snprintf(prop, sizeof(prop), "dapf-instance-%u", instance);
    const struct dapf_t8110_config *config = adt_getprop(adt, node, prop, &length);
    if (!config || !length || length % sizeof(*config) ||
        length / sizeof(*config) > T8132_DAPF_SLICES) {
        printf("dapf: Invalid T8132 %s for %s\n", prop, path);
        return NULL;
    }

    *count = length / sizeof(*config);
    for (u32 i = 0; i < *count; i++) {
        if (config[i].end < config[i].start) {
            printf("dapf: Invalid T8132 %s slice %u for %s\n", prop, i, path);
            return NULL;
        }
    }
    return config;
}

/*
 * Program a genuine T8132 DAPF bank with the t8110 records of its
 * dapf-instance property, in the t8110 register order, then read every slice
 * back. The low two bits of an end and all but the low byte of r0 do not read
 * back. On any mismatch, every slice gets its previous values back.
 */
static int dapf_program_t8132(const char *path, u64 base, int node, u32 instance)
{
    struct dapf_slice want[T8132_DAPF_SLICES], had[T8132_DAPF_SLICES];
    u32 count;
    const struct dapf_t8110_config *config = dapf_t8132_config(path, node, instance, &count);

    if (!config)
        return -1;

    for (u32 i = 0; i < count; i++) {
        u64 slice = base + 0x40 * i;

        had[i] = (struct dapf_slice){
            .start = read64(slice + 0x08),
            .end = read64(slice + 0x10),
            .r0 = read32(slice + 0x00),
            .r4 = read32(slice + 0x04),
            .r20 = read32(slice + 0x20),
        };
        want[i] = (struct dapf_slice){
            .start = config[i].start,
            .end = config[i].end,
            .r0 = (config[i].r0_hi << 4) | config[i].r0_lo,
            .r4 = config[i].r4,
            .r20 = config[i].r20,
        };
    }

    for (u32 i = 0; i < count; i++)
        dapf_t8132_put(base + 0x40 * i, &want[i]);

    for (u32 i = 0; i < count; i++) {
        u64 slice = base + 0x40 * i;

        if (read32(slice + 0x04) != want[i].r4 || read64(slice + 0x08) != want[i].start ||
            (read64(slice + 0x10) & ~3UL) != (want[i].end & ~3UL) ||
            (read32(slice + 0x00) & 0xff) != want[i].r0 || read32(slice + 0x20) != want[i].r20) {
            for (u32 j = 0; j < count; j++)
                dapf_t8132_put(base + 0x40 * j, &had[j]);
            printf("dapf: T8132 DAPF slice %u for %s did not read back; restored\n", i, path);
            return -1;
        }
    }

    printf("dapf: Programmed %u T8132 DAPF slices for %s\n", count, path);
    return 0;
}

/*
 * Validate the typed instance metadata of a T8132 DART and the bank selected
 * by index: 1 for a genuine DAPF bank, its instance number in *instance; 0 for
 * a selected GAPF bank with no DAPF instance, whose state is preserved; -1
 * otherwise.
 */
static int dapf_select_t8132(const char *path, int index, int node, int *dart_path, u32 *instance)
{
    u32 length;
    const struct dapf_instance *instances = adt_getprop(adt, node, "instance", &length);

    if (!instances || !length || length % sizeof(*instances) || index < 0 ||
        (u32)index >= length / sizeof(*instances)) {
        printf("dapf: Invalid T8132 instance metadata for %s\n", path);
        return -1;
    }

    bool has_dart = false, has_dapf = false;
    u32 count = length / sizeof(*instances);
    for (u32 i = 0; i < count; i++) {
        switch (instances[i].type) {
            case DAPF_INSTANCE_DART:
                has_dart = true;
                break;
            case DAPF_INSTANCE_DAPF:
                has_dapf = true;
                break;
            case DAPF_INSTANCE_GAPF:
                break;
            default:
                printf("dapf: Unknown T8132 instance type for %s\n", path);
                return -1;
        }
        for (u32 j = 0; j < i; j++) {
            if (instances[i].type == instances[j].type &&
                instances[i].instance == instances[j].instance) {
                printf("dapf: Duplicate T8132 instance for %s\n", path);
                return -1;
            }
        }
        u64 base, size;
        if (adt_get_reg(adt, dart_path, "reg", i, &base, &size) < 0 || !size ||
            base + size < base ||
            (instances[i].type != DAPF_INSTANCE_DART && ((base & 0x3fff) || size != 0x4000))) {
            printf("dapf: Invalid T8132 instance resource for %s\n", path);
            return -1;
        }
    }

    if (!has_dart) {
        printf("dapf: Missing T8132 DART instance for %s\n", path);
        return -1;
    }

    if (!has_dapf && instances[index].type == DAPF_INSTANCE_GAPF) {
        /* Native PMP has GAPF only; its DAPF property does not create an APF. */
        printf("dapf: No T8132 DAPF instance for %s; preserving filter state\n", path);
        return 0;
    }

    if (instances[index].type != DAPF_INSTANCE_DAPF) {
        printf("dapf: Selected T8132 resource is not DAPF for %s\n", path);
        return -1;
    }

    *instance = instances[index].instance;
    return 1;
}

/*
 * Program the T8132 DAPF bank of a consumer whose handoff the OS requested
 * and whose firmware the caller checked is halted (the J713 AOP).
 */
int dapf_init_t8132_halted(const char *path, int index)
{
    int dart_path[8];
    int node = adt_path_offset_trace(adt, path, dart_path);
    u32 instance;
    u64 base;

    if (chip_id != T8132 || node < 0) {
        printf("dapf: No T8132 DAPF %s\n", path);
        return -1;
    }
    if (dapf_select_t8132(path, index, node, dart_path, &instance) != 1)
        return -1;
    if (adt_get_reg(adt, dart_path, "reg", index, &base, NULL) < 0) {
        printf("dapf: Error getting DAPF %s base address.\n", path);
        return -1;
    }
    return dapf_program_t8132(path, base, node, instance);
}

/*
 * The windows of a T8132 DAPF bank, merged to whole 16K pages and sorted:
 * the MMIO its consumer's firmware reaches through the DART at the windows'
 * physical addresses. Returns their count, at most max, or -1.
 */
int dapf_t8132_windows(const char *path, int index, struct dapf_window *out, int max)
{
    int dart_path[8];
    int node = adt_path_offset_trace(adt, path, dart_path);
    u32 instance, count;

    if (chip_id != T8132 || node < 0 || max <= 0 ||
        dapf_select_t8132(path, index, node, dart_path, &instance) != 1)
        return -1;

    const struct dapf_t8110_config *config = dapf_t8132_config(path, node, instance, &count);
    if (!config)
        return -1;

    int used = 0;
    for (u32 i = 0; i < count; i++) {
        /* The end of a DAPF slice is inclusive. */
        struct dapf_window window = {
            .start = ALIGN_DOWN(config[i].start, SZ_16K),
            .end = ALIGN_UP(config[i].end + 1, SZ_16K),
        };
        int pos = used;

        if (window.end <= window.start)
            return -1;
        while (pos > 0 && out[pos - 1].start > window.start)
            pos--;
        if (pos > 0 && window.start <= out[pos - 1].end) {
            /* Coalesce from the window that grew. */
            pos--;
            out[pos].end = max(out[pos].end, window.end);
        } else {
            if (used == max)
                return -1;
            memmove(&out[pos + 1], &out[pos], (used - pos) * sizeof(*out));
            out[pos] = window;
            used++;
        }
        /* A grown window may now reach the ones after it. */
        while (pos + 1 < used && out[pos + 1].start <= out[pos].end) {
            out[pos].end = max(out[pos].end, out[pos + 1].end);
            memmove(&out[pos + 1], &out[pos + 2], (used - pos - 2) * sizeof(*out));
            used--;
        }
    }
    return used;
}

int dapf_init(const char *path, int index)
{
    int ret = -1;
    int dart_path[8];
    int node = adt_path_offset_trace(adt, path, dart_path);
    if (node < 0) {
        printf("dapf: Error getting DAPF %s node.\n", path);
        return -1;
    }

    if (chip_id == T8132) {
        u32 instance;
        int ret = dapf_select_t8132(path, index, node, dart_path, &instance);

        if (ret <= 0)
            return ret;
        /* Do not admit the legacy writer, even for a genuine T8132 DAPF bank. */
        printf("dapf: T8132 DAPF programming is unsupported for %s; preserving filter state\n",
               path);
        return -1;
    }

    u32 pwr;
    if (!adt_getprop(adt, node, "clock-gates", &pwr))
        pwr = 0;
    if (pwr && (pmgr_adt_power_enable(path) < 0))
        return -1;

    /*
     * The ADT's "instance" property names each reg entry as 8-byte
     * (byte-reversed FOURCC, name) pairs, e.g. "TRAD" "DART", "FPAD" "DAPF",
     * "FPAG" "PS_PROT". When it is present, use the entry tagged DAPF rather
     * than a fixed index: T6040's dart-pmp has only DART and PS_PROT entries,
     * and programming its second reg as a DAPF raises an SError.
     */
    u32 inst_len;
    const char *inst = adt_getprop(adt, node, "instance", &inst_len);
    if (inst && inst_len >= 16 && !(inst_len % 16)) {
        int found = -1;
        for (u32 i = 0; i < inst_len / 16; i++) {
            if (!memcmp(inst + i * 16, "FPAD", 4)) {
                found = i;
                break;
            }
        }
        if (found < 0) {
            printf("dapf: %s has no DAPF instance; leaving it to firmware\n", path);
            ret = 0;
            goto out_nodapf;
        }
        if (found != index)
            printf("dapf: %s: using DAPF reg %d from the ADT (not %d)\n", path, found, index);
        index = found;
    }

    u64 base;
    if (adt_get_reg(adt, dart_path, "reg", index, &base, NULL) < 0) {
        printf("dapf: Error getting DAPF %s base address.\n", path);
        goto out;
    }

    if (adt_is_compatible(adt, node, "dart,t8020")) {
        ret = dapf_init_t8020(path, base, node);
    } else if (adt_is_compatible(adt, node, "dart,t6000")) {
        ret = dapf_init_t8020(path, base, node);
    } else if (adt_is_compatible(adt, node, "dart,t8110")) {
        ret = dapf_init_t8110(path, base, node);
    } else {
        printf("dapf: DAPF %s at 0x%lx is of an unknown type\n", path, base);
        goto out;
    }

out:
    if (pwr && pmgr_adt_power_disable(path)) {
        printf("dapf: failed to power down %s\n", path);
        ret = -1;
    }

    if (!ret)
        printf("dapf: Initialized %s\n", path);

    return ret;

out_nodapf:
    if (pwr && pmgr_adt_power_disable(path)) {
        printf("dapf: failed to power down %s\n", path);
        return -1;
    }
    return 0;
}

struct entry {
    const char *path;
    int index;
    const char *alias;
};

struct entry dapf_entries[] = {
    /* Keep dart-ane out: its Linux driver owns that DAPF on T8140. */
    {"/arm-io/dart-aop", 1, "aop"}, {"/arm-io/dart-mtp", 1, "mtp"},  {"/arm-io/dart-pmp", 1, "pmp"},
    {"/arm-io/dart-isp", 5, "isp"}, {"/arm-io/dart-isp0", 5, "isp"}, {NULL, -1, NULL},
};

static int dapf_init_j713(void *dt)
{
    int ret = 0;
    int count = 0;
    static const struct {
        const char *path;
        const char *consumer;
        int index;
    } entries[] = {
        {"/arm-io/dart-aop", "/arm-io/aop", 1},   {"/arm-io/dart-mtp", "/arm-io/mtp", 1},
        {"/arm-io/dart-pmp", "/arm-io/pmp", 1},   {"/arm-io/dart-isp", "/arm-io/isp", 5},
        {"/arm-io/dart-isp0", "/arm-io/isp0", 5}, {NULL, NULL, -1},
    };
    const typeof(entries[0]) *entry = entries;
    int chosen = dt ? fdt_path_offset(dt, "/chosen") : -1;
    bool described =
        chosen >= 0 && fdt_getprop(dt, chosen, "linux-enablement-mac,dapf-handoff", NULL);

    if (!described) {
        printf("dapf: No J713 consumer handoff requested; preserving firmware state\n");
        return 0;
    }

    while (entry->path != NULL) {
        if (adt_path_offset(adt, entry->path) < 0) {
            entry++;
            continue;
        }

        /* kboot programs the AOP's with its firmware handoff, once the AOP is
         * known to be halted. */
        if (!strcmp(entry->consumer, "/arm-io/aop")) {
            entry++;
            continue;
        }

        if (described) {
            if (fdt_stringlist_search(dt, chosen, "linux-enablement-mac,dapf-handoff",
                                      entry->consumer) < 0) {
                printf("dapf: Skipping %s; consumer %s is not requested for handoff\n", entry->path,
                       entry->consumer);
                entry++;
                continue;
            }

            if (fdt_stringlist_search(dt, chosen, "linux-enablement-mac,dapf-power-on-handoff",
                                      entry->consumer) >= 0) {
                if (pmgr_adt_power_enable(entry->consumer) < 0) {
                    printf("dapf: Failed to power requested consumer %s\n", entry->consumer);
                    ret = -1;
                    entry++;
                    continue;
                }
                printf("dapf: Preserving requested consumer %s in the active state\n",
                       entry->consumer);
            }

            int powered = pmgr_adt_is_powered(entry->consumer);
            if (powered <= 0) {
                printf("dapf: Skipping %s; consumer %s is inactive or its state is unknown\n",
                       entry->path, entry->consumer);
                entry++;
                continue;
            }
        }

        if (dapf_init(entry->path, entry->index) < 0) {
            ret = -1;
        }
        entry++;
        count += 1;
    }
    return ret ? ret : count;
}

int dapf_init_fdt(const void *fdt)
{
    if (chip_id == T8132) {
        if (!fdt || fdt_node_check_compatible(fdt, 0, "apple,j713"))
            return 0; /* No other M4 board's power/DAPF contract is qualified. */
        return dapf_init_j713((void *)fdt);
    }

    int ret = 0;
    int count = 0;
    struct entry *entry = dapf_entries;

    while (entry->path != NULL) {
        if (chip_id == T8140 && strcmp(entry->path, "/arm-io/dart-mtp") &&
            strcmp(entry->path, "/arm-io/dart-pmp")) {
            printf("dapf: preserving inherited %s state on T8140\n", entry->path);
            entry++;
            continue;
        }
        /* Only prepare DMA for devices handed to the next stage. In
         * particular, an initial J813 FDT has no AOP, MTP, PMP or ISP.
         * Their firmware-owned protection registers must remain intact.
         */
        if (fdt) {
            int node = fdt_path_offset(fdt, entry->alias);
            if (node < 0) {
                entry++;
                continue;
            }
            const char *status = fdt_getprop(fdt, node, "status", NULL);
            if (status && strcmp(status, "okay") && strcmp(status, "ok")) {
                entry++;
                continue;
            }
        }
        if (adt_path_offset(adt, entry->path) < 0) {
            entry++;
            continue;
        }
        if (dapf_init(entry->path, entry->index) < 0) {
            ret = -1;
        }
        entry++;
        count += 1;
    }
    return ret ? ret : count;
}

int dapf_init_all(void)
{
    return dapf_init_fdt(NULL);
}
