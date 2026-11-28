/* SPDX-License-Identifier: MIT */
/*
 * Speaker amp configuration and the speaker assembly ID from the ADT.
 *
 * The ADT describes a group of speaker amps with one node: the group's first amp, which carries
 * the group's configuration, and whose speakerN properties are the phandles of the group's other
 * amps. The parts of it that the kernel's amp drivers consume go to every amp of the group in the
 * DT, byte for byte, so that no Linux tree has to carry a copy.
 *
 *   bop-config            -> apple,bop-config            (the brownout prevention block)
 *   amp-dcblocker-config  -> apple,amp-dcblocker-config  (the DC blocker setting)
 *
 * Two SN012776 group properties have one row per amp, in the group's order (the first amp, then
 * speaker1, speaker2, ...); each amp gets its own row:
 *
 *   speaker-config        -> apple,speaker-config        (4 bytes: operating level and slots)
 *   amp-tx-zd-config      -> apple,amp-tx-zd-config      (8 bytes: sense output idle slots)
 *
 * TAS5770L groups use the same four-byte speaker-config rows. Clear their rows in the input DT
 * before applying the current ADT so a rejected or absent group cannot retain a previous gain.
 * Missing properties and incomplete row arrays are omitted. Amps are matched by compatible and
 * I2C address, which must identify exactly one node. The loader validates the group before writing
 * its rows.
 */

#include "kboot_audio.h"
#include "adt.h"
#include "adt_gpio.h"
#include "string.h"
#include "types.h"
#include "utils.h"

#include "libfdt/libfdt.h"

#define MAX_GROUP_AMPS 16
#define TAS5770_DT_COMPAT "ti,tas5770l"
#define DT_SPEAKER_CONFIG "apple,speaker-config"

struct amp_prop {
    const char *adt;
    const char *dt;
    u32 row;
};

struct amp_family {
    const char *adt_compat;
    const char *dt_compat;
    const struct amp_prop *props;
    size_t nr_props;
    bool exact_rows;
    bool require_member_compatible;
};

static const struct amp_prop sn012776_props[] = {
    {"bop-config", "apple,bop-config", 0},
    {"amp-dcblocker-config", "apple,amp-dcblocker-config", 0},
    {"speaker-config", DT_SPEAKER_CONFIG, 4},
    {"amp-tx-zd-config", "apple,amp-tx-zd-config", 8},
};

static const struct amp_prop tas5770_props[] = {
    {"speaker-config", DT_SPEAKER_CONFIG, 4},
};

static const struct amp_family amp_families[] = {
    {
        .adt_compat = "audio-control,sn012776",
        .dt_compat = "ti,sn012776",
        .props = sn012776_props,
        .nr_props = ARRAY_SIZE(sn012776_props),
    },
    {
        .adt_compat = "audio-control,tas5770",
        .dt_compat = TAS5770_DT_COMPAT,
        .props = tas5770_props,
        .nr_props = ARRAY_SIZE(tas5770_props),
        .exact_rows = true,
        .require_member_compatible = true,
    },
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
 * The DT's @compat node at I2C address @addr. Returns its offset, -1 if there is none and -2 if
 * more than one node has that address.
 */
static int dt_amp_by_addr(const void *dt, const char *compat, u32 addr)
{
    int found = -1;

    for (int node = fdt_node_offset_by_compatible(dt, -1, compat); node >= 0;
         node = fdt_node_offset_by_compatible(dt, node, compat)) {
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
static int dt_amp_setprop(void *dt, const char *compat, u32 addr, const char *name, const void *val,
                          u32 len)
{
    /* Offsets move with every change to the tree: look the amp up each time */
    int node = dt_amp_by_addr(dt, compat, addr);
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

static int setup_amp_group(void *dt, const void *adt, int arm_io, int group,
                           const struct amp_family *family)
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
        if (amp < 0 ||
            (family->require_member_compatible &&
             !adt_is_compatible(adt, amp, family->adt_compat)) ||
            adt_amp_addr(adt, amp, &addrs[n])) {
            printf("kboot: audio: %s: %s does not resolve to a matching amp, group left alone\n",
                   name, prop);
            return -1;
        }
        n++;
    }

    for (int i = 0; i < n; i++) {
        int node = dt_amp_by_addr(dt, family->dt_compat, addrs[i]);

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

    for (size_t p = 0; p < family->nr_props; p++) {
        u32 len = 0;
        u32 row = family->props[p].row;
        const u8 *val = adt_getprop(adt, group, family->props[p].adt, &len);

        u32 required = (u32)n * row;

        /* Preserve the SN012776 reader's support for trailing firmware padding. */
        if (row && val && (len < required || (family->exact_rows && len != required))) {
            printf("kboot: audio: %s: %s has %u bytes for %d amps, not passed on\n", name,
                   family->props[p].adt, len, n);
            val = NULL;
        }
        for (int i = 0; i < n; i++) {
            const void *amp_val = val && row ? val + i * row : val;
            u32 amp_len = row ? row : len;

            if (dt_amp_setprop(dt, family->dt_compat, addrs[i], family->props[p].dt, amp_val,
                               amp_len))
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

    /* Deleting a property leaves the containing node at the same offset. */
    for (int node = fdt_node_offset_by_compatible(dt, -1, TAS5770_DT_COMPAT); node >= 0;
         node = fdt_node_offset_by_compatible(dt, node, TAS5770_DT_COMPAT)) {
        int ret = fdt_delprop(dt, node, DT_SPEAKER_CONFIG);

        if (ret && ret != -FDT_ERR_NOTFOUND) {
            printf("kboot: audio: could not clear incoming TAS speaker configuration: %d\n", ret);
            return -1;
        }
    }

    if (arm_io < 0)
        return 0;

    ADT_FOREACH_CHILD(adt, bus)
    {
        int dev = bus;

        ADT_FOREACH_CHILD(adt, dev)
        {
            for (size_t i = 0; i < ARRAY_SIZE(amp_families); i++) {
                const struct amp_family *family = &amp_families[i];
                bool configured = adt_getprop(adt, dev, "speaker1", NULL);

                if (!adt_is_compatible(adt, dev, family->adt_compat))
                    continue;
                /* Only a group's first amp has speakerN or group configuration */
                for (size_t p = 0; !configured && p < family->nr_props; p++)
                    configured = adt_getprop(adt, dev, family->props[p].adt, NULL);
                if (configured)
                    setup_amp_group(dt, adt, arm_io, dev, family);
                break;
            }
        }
    }
    return 0;
}

/*
 * Speaker assembly ID.
 *
 * Some Macs are built with speakers from more than one vendor, and macOS picks the tuning for
 * the fitted assembly by its ID (the IORegistry speakerID of the built-in speaker, the audio
 * HAL's spid). The board encodes the ID in two strap pins, which the ADT names on the speaker
 * amp group's node:
 *
 *   find-speaker-id       1: the Mac has a speaker ID (any other value: it does not)
 *   function-speaker-id0  bit 0 of the ID: <controller phandle> 'GPIO' <pin> <control word>
 *   function-speaker-id1  bit 1 of the ID
 *   valid-speaker-ids     the IDs this model is built with (u32 list)
 *
 * This reads them the way macOS does. Each strap that resolves to a pin of an /arm-io GPIO
 * controller is switched to a plain GPIO input (pull left as it is) and sampled once, with no
 * delay and no restore; a strap that does not resolve reads as 0. The control word's second
 * byte gives the polarity: nonzero reads a high pin as 1, zero inverts it. The ID goes to the
 * kernel as /chosen/asahi,speaker-id (one u32): the sampled value when valid-speaker-ids lists
 * it, else 65535, which is what macOS publishes then. A Mac without find-speaker-id = 1, or
 * with neither strap resolving, gets no property.
 */

#define SPEAKER_ID_DT_PROP "asahi,speaker-id"
#define SPEAKER_ID_STRAPS  2
#define SPEAKER_ID_INVALID 0xffff
#define MAX_VALID_SPEAKERS 32

/* Sample strap @idx of @node into *@level. Returns -1 when the strap does not resolve. */
static int read_speaker_id_strap(const void *adt, int node, int idx, u32 *level)
{
    const char *name = adt_get_name(adt, node);
    struct adt_function_gpio fn;
    char prop[32];
    u64 base, reg;
    u32 npins, cfg, in, val;

    snprintf(prop, sizeof(prop), "function-speaker-id%d", idx);
    if (ADT_GETPROP(adt, node, prop, &fn) != sizeof(fn) || fn.fourcc != ADT_GPIO_FOURCC) {
        printf("kboot: audio: %s: no GPIO %s\n", name, prop);
        return -1;
    }
    if (adt_gpio_by_phandle(adt, fn.phandle, &base, &npins) || fn.pin >= npins) {
        printf("kboot: audio: %s: %s: no GPIO controller %u pin %u\n", name, prop, fn.phandle,
               fn.pin);
        return -1;
    }

    /* A plain GPIO input with its pull as it is, sampled once */
    reg = base + 4 * fn.pin;
    cfg = read32(reg);
    in = (cfg & ~(ADT_GPIO_PIN_MODE | ADT_GPIO_PIN_PERIPH)) |
         FIELD_PREP(ADT_GPIO_PIN_MODE, ADT_GPIO_PIN_MODE_IN) | ADT_GPIO_PIN_INPUT_EN;
    if (in != cfg)
        write32(reg, in);
    val = read32(reg);

    *level = !!(val & ADT_GPIO_PIN_DATA) ^ !FIELD_GET(ADT_GPIO_CTRL_POLARITY, fn.control);
    printf("kboot: audio: %s: %s = %u (pin %u: 0x%08x -> 0x%08x)\n", name, prop, *level, fn.pin,
           cfg, val);
    return 0;
}

static int setup_speaker_id(void *dt, const void *adt, int node)
{
    const char *name = adt_get_name(adt, node);
    u32 valid[MAX_VALID_SPEAKERS];
    u32 len = 0, id = 0, nvalid = 0;
    int resolved = 0, chosen;
    bool listed = false;

    for (int i = 0; i < SPEAKER_ID_STRAPS; i++) {
        u32 level;

        if (read_speaker_id_strap(adt, node, i, &level))
            continue;
        id |= level << i;
        resolved++;
    }
    if (!resolved) {
        printf("kboot: audio: %s: no speaker ID strap resolves, no speaker ID\n", name);
        return -1;
    }

    const void *v = adt_getprop(adt, node, "valid-speaker-ids", &len);
    if (v) {
        nvalid = min(len / sizeof(u32), ARRAY_SIZE(valid));
        memcpy(valid, v, nvalid * sizeof(u32));
    }
    for (u32 i = 0; i < nvalid; i++)
        listed |= valid[i] == id;
    if (!listed) {
        printf("kboot: audio: %s: speaker ID %u is not a valid one, passing %u\n", name, id,
               SPEAKER_ID_INVALID);
        id = SPEAKER_ID_INVALID;
    }

    chosen = fdt_path_offset(dt, "/chosen");
    if (chosen < 0 || fdt_setprop_u32(dt, chosen, SPEAKER_ID_DT_PROP, id)) {
        printf("kboot: audio: could not set /chosen/" SPEAKER_ID_DT_PROP "\n");
        return -1;
    }
    printf("kboot: audio: %s: speaker ID %u\n", name, id);
    return 0;
}

int kboot_audio_speaker_id(void *dt, const void *adt)
{
    int arm_io = adt_path_offset(adt, "/arm-io");
    int bus = arm_io;
    int chosen = fdt_path_offset(dt, "/chosen");

    /* The ID only ever comes from this Mac's straps */
    if (chosen >= 0)
        fdt_delprop(dt, chosen, SPEAKER_ID_DT_PROP);
    if (arm_io < 0)
        return 0;

    ADT_FOREACH_CHILD(adt, bus)
    {
        int dev = bus;

        ADT_FOREACH_CHILD(adt, dev)
        {
            u32 find;

            /* The speaker amp group's first amp (audio-speaker) carries it */
            if (ADT_GETPROP(adt, dev, "find-speaker-id", &find) != sizeof(find) || find != 1)
                continue;
            /* One speaker ID per Mac; a refusal is logged and never stops the boot */
            setup_speaker_id(dt, adt, dev);
            return 0;
        }
    }
    return 0;
}
