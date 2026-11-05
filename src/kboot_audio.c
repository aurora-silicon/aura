/* SPDX-License-Identifier: MIT */
/*
 * Speaker amp configuration from the ADT.
 *
 * The ADT describes a group of SN012776 speaker amps with one node: the group's first amp,
 * compatible "audio-control,sn012776", which carries the group's configuration, and whose
 * speakerN properties are the phandles of the group's other amps. macOS sets up every amp of
 * the group from that configuration. The parts of it that the kernel's amp driver programs go
 * to every amp of the group in the DT, byte for byte, so that no Linux tree has to carry a
 * copy:
 *
 *   bop-config            -> apple,bop-config            (the brownout prevention block)
 *   amp-dcblocker-config  -> apple,amp-dcblocker-config  (the DC blocker setting)
 *
 * Two of the group's properties have one row per amp, in the group's order (the first amp, then
 * speaker1, speaker2, ...); each amp gets its own row:
 *
 *   speaker-config        -> apple,speaker-config        (4 bytes: operating level and slots)
 *   amp-tx-zd-config      -> apple,amp-tx-zd-config      (8 bytes: sense output idle slots)
 *
 * A property the group does not have (or one without a row for every amp) is
 * removed from its amps, so the kernel never sees a value that did not come from this Mac. Amps
 * are matched to the DT's ti,sn012776 nodes by I2C address, which must name exactly one node. A
 * group with an amp that cannot be matched is left as it is: the kernel gets either the whole
 * group's configuration or none of it. Nothing here stops the boot.
 */

#include "kboot_audio.h"
#include "adt.h"
#include "types.h"
#include "utils.h"

#include "libfdt/libfdt.h"

#define AMP_ADT_COMPAT "audio-control,sn012776"
#define AMP_DT_COMPAT  "ti,sn012776"
#define MAX_GROUP_AMPS 16

static const struct {
    const char *adt;
    const char *dt;
} amp_group_props[] = {
    {"bop-config", "apple,bop-config"},
    {"amp-dcblocker-config", "apple,amp-dcblocker-config"},
};

static const struct {
    const char *adt;
    const char *dt;
    u32 row;
} amp_row_props[] = {
    {"speaker-config", "apple,speaker-config", 4},
    {"amp-tx-zd-config", "apple,amp-tx-zd-config", 8},
};

/* The I2C address of an ADT amp node: the first word of its reg */
static int adt_amp_addr(const void *adt, int node, u32 *addr)
{
    u32 len;
    const u32 *reg = adt_getprop(adt, node, "reg", &len);

    if (!reg || len < sizeof(*reg))
        return -1;
    *addr = reg[0];
    return 0;
}

/* The ADT node under /arm-io's buses whose AAPL,phandle is @phandle */
static int adt_amp_by_phandle(const void *adt, int arm_io, u32 phandle)
{
    int bus = arm_io;

    ADT_FOREACH_CHILD(adt, bus)
    {
        int dev = bus;

        ADT_FOREACH_CHILD(adt, dev)
        {
            u32 ph;

            if (ADT_GETPROP(adt, dev, "AAPL,phandle", &ph) == sizeof(ph) && ph == phandle)
                return dev;
        }
    }
    return -1;
}

/*
 * The DT's ti,sn012776 node at I2C address @addr. Returns its offset, -1 if there is none and
 * -2 if more than one node has that address.
 */
static int dt_amp_by_addr(const void *dt, u32 addr)
{
    int found = -1;

    for (int node = fdt_node_offset_by_compatible(dt, -1, AMP_DT_COMPAT); node >= 0;
         node = fdt_node_offset_by_compatible(dt, node, AMP_DT_COMPAT)) {
        int len;
        const fdt32_t *reg = fdt_getprop(dt, node, "reg", &len);

        if (!reg || len < (int)sizeof(*reg) || fdt32_to_cpu(reg[0]) != addr)
            continue;
        if (found >= 0)
            return -2;
        found = node;
    }
    return found;
}

/* Set @name on the DT amp at @addr to @val (remove it when @val is NULL) */
static int dt_amp_setprop(void *dt, u32 addr, const char *name, const void *val, u32 len)
{
    /* Offsets move with every change to the tree: look the amp up each time */
    int node = dt_amp_by_addr(dt, addr);
    int ret;

    if (node < 0)
        return -1;
    if (val)
        ret = fdt_setprop(dt, node, name, val, len);
    else
        ret = fdt_delprop(dt, node, name);
    if (ret && !(ret == -FDT_ERR_NOTFOUND && !val)) {
        printf("kboot: audio: could not set %s on the amp at 0x%x: %d\n", name, addr, ret);
        return -1;
    }
    return 0;
}

static int setup_amp_group(void *dt, const void *adt, int arm_io, int group)
{
    const char *name = adt_get_name(adt, group);
    u32 addrs[MAX_GROUP_AMPS];
    int n = 0;

    if (adt_amp_addr(adt, group, &addrs[n++])) {
        printf("kboot: audio: %s has no I2C address, left alone\n", name);
        return -1;
    }

    for (int k = 1;; k++) {
        char prop[16];
        u32 phandle;
        int amp;

        snprintf(prop, sizeof(prop), "speaker%d", k);
        if (ADT_GETPROP(adt, group, prop, &phandle) != sizeof(phandle))
            break;
        if (n >= MAX_GROUP_AMPS) {
            printf("kboot: audio: %s has more than %d amps, left alone\n", name, MAX_GROUP_AMPS);
            return -1;
        }
        amp = adt_amp_by_phandle(adt, arm_io, phandle);
        if (amp < 0 || adt_amp_addr(adt, amp, &addrs[n])) {
            printf("kboot: audio: %s: %s is not an amp, group left alone\n", name, prop);
            return -1;
        }
        n++;
    }

    for (int i = 0; i < n; i++) {
        int node = dt_amp_by_addr(dt, addrs[i]);

        for (int j = 0; j < i; j++) {
            if (addrs[j] == addrs[i]) {
                printf("kboot: audio: %s: two amps at 0x%x, group left alone\n", name, addrs[i]);
                return -1;
            }
        }
        if (node < 0) {
            printf("kboot: audio: %s: amp at 0x%x %s in the DT, group left alone\n", name, addrs[i],
                   node == -2 ? "is not unique" : "is missing");
            return -1;
        }
    }

    for (size_t p = 0; p < ARRAY_SIZE(amp_group_props); p++) {
        u32 len = 0;
        const void *val = adt_getprop(adt, group, amp_group_props[p].adt, &len);

        for (int i = 0; i < n; i++) {
            if (dt_amp_setprop(dt, addrs[i], amp_group_props[p].dt, val, len))
                return -1;
        }
    }

    /* One row per amp, in the group's order */
    for (size_t p = 0; p < ARRAY_SIZE(amp_row_props); p++) {
        u32 len = 0;
        u32 row = amp_row_props[p].row;
        const u8 *rows = adt_getprop(adt, group, amp_row_props[p].adt, &len);

        if (rows && len < (u32)n * row) {
            printf("kboot: audio: %s: %s has %u bytes for %d amps, not passed on\n", name,
                   amp_row_props[p].adt, len, n);
            rows = NULL;
        }
        for (int i = 0; i < n; i++) {
            if (dt_amp_setprop(dt, addrs[i], amp_row_props[p].dt, rows ? rows + i * row : NULL,
                               row))
                return -1;
        }
    }

    printf("kboot: audio: %s: amp configuration for %d amps\n", name, n);
    return 0;
}

int kboot_audio_amp_config(void *dt, const void *adt)
{
    int arm_io = adt_path_offset(adt, "/arm-io");
    int bus = arm_io;

    if (arm_io < 0)
        return 0;

    ADT_FOREACH_CHILD(adt, bus)
    {
        int dev = bus;

        ADT_FOREACH_CHILD(adt, dev)
        {
            if (!adt_is_compatible(adt, dev, AMP_ADT_COMPAT))
                continue;
            /* Only a group's first amp has speakerN or the group's configuration */
            if (!adt_getprop(adt, dev, "speaker1", NULL) &&
                !adt_getprop(adt, dev, "bop-config", NULL) &&
                !adt_getprop(adt, dev, "amp-dcblocker-config", NULL) &&
                !adt_getprop(adt, dev, "speaker-config", NULL) &&
                !adt_getprop(adt, dev, "amp-tx-zd-config", NULL))
                continue;
            setup_amp_group(dt, adt, arm_io, dev);
        }
    }
    return 0;
}

/* Export an independently collected AID for userspace speaker-tuning binding.
 * ADT words are little endian on Apple Silicon; FDT cells are big endian.
 * Refuse ambiguity (including equal values) and discard stale DTB data. */
static int acoustic_id_walk(const void *adt, int node, u32 *aid, int depth, u32 *budget)
{
    if (depth > 64 || !*budget)
        return -1;
    --*budget;
    int count = 0;
    const char *name = adt_get_name(adt, node);
    u32 len;
    const void *raw = adt_getprop(adt, node, "acoustic-id", &len);
    if (name && !strcmp(name, "audio") && raw) {
        if (len != sizeof(*aid))
            return -1;
        memcpy(aid, raw, sizeof(*aid));
        if (!*aid || *aid >= 100000)
            return -1;
        count = 1;
    }
    int child = node;
    ADT_FOREACH_CHILD(adt, child)
    {
        int n = acoustic_id_walk(adt, child, aid, depth + 1, budget);
        if (n < 0 || count + n > 1)
            return -1;
        count += n;
    }
    return count;
}

int kboot_audio_identity(void *dt, const void *adt)
{
    const char *prop = "asahi,acoustic-id";
    int chosen = fdt_path_offset(dt, "/chosen");
    if (chosen >= 0) {
        int ret = fdt_delprop(dt, chosen, prop);
        if (ret && ret != -FDT_ERR_NOTFOUND)
            return ret;
    }
    u32 aid, budget = 65536;
    if (acoustic_id_walk(adt, 0, &aid, 0, &budget) != 1)
        return 0;
    if (chosen < 0)
        chosen = fdt_add_subnode(dt, 0, "chosen");
    if (chosen < 0)
        return chosen;
    return fdt_setprop_u32(dt, chosen, prop, aid);
}
