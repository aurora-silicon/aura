/* SPDX-License-Identifier: MIT */
/*
 * Speaker amp configuration and the speaker assembly ID from the ADT.
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
#include "string.h"
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

/*
 * Speaker assembly ID.
 *
 * Some Macs are built with speakers from more than one vendor, and macOS picks the tuning for
 * the fitted assembly by its ID (the IORegistry speakerID of the built-in speaker). The board
 * encodes the ID in strap pins, which the ADT names on the speaker amp group's node:
 *
 *   find-speaker-id       nonzero: the Mac has a speaker ID
 *   function-speaker-idN  strap N: <controller phandle> 'GPIO' <pin> <flags>
 *   valid-speaker-ids     the IDs this model is built with (u32 list)
 *
 * Bit N of the ID is the level of strap N, least significant first (the order of the kernel's
 * speaker-id-gpios); flags bit 0 inverts a strap. The pins are read once, as inputs; a pin
 * that iBoot left without its input stage is switched to a plain input for the read and put
 * back as it was. The ID goes to the kernel as /chosen/asahi,speaker-id (one u32), and only
 * when every strap could be read and the result is one of valid-speaker-ids: a Mac without
 * straps, or one whose straps say something unexpected, gets no property, never a guess.
 */

#define SPEAKER_ID_DT_PROP    "asahi,speaker-id"
#define MAX_SPEAKER_ID_STRAPS 8
#define MAX_VALID_SPEAKER_IDS 32

#define GPIO_FOURCC 0x4750494f /* 'GPIO' */
/* function-speaker-idN flags: bit 0 is the strap's polarity, bit 8 marks an input */
#define STRAP_FLAG_ACTIVE_LOW BIT(0)
#define STRAP_FLAG_INPUT      BIT(8)
#define STRAP_FLAGS_KNOWN     (STRAP_FLAG_ACTIVE_LOW | STRAP_FLAG_INPUT)

/* Apple GPIO pin register (one u32 per pin) */
#define GPIO_PIN_DATA     BIT(0)
#define GPIO_PIN_MODE     GENMASK(3, 1)
#define GPIO_PIN_MODE_OUT 1
#define GPIO_PIN_MODE_IN  7 /* input, interrupt off */
#define GPIO_PIN_PERIPH   GENMASK(6, 5)
#define GPIO_PIN_INPUT_EN BIT(9)
#define STRAP_SETTLE_US   100

struct adt_function_gpio {
    u32 phandle;
    u32 fourcc;
    u32 pin;
    u32 flags;
} PACKED;

/* The base of the /arm-io GPIO controller whose AAPL,phandle is @phandle, and its pin count */
static int adt_gpio_by_phandle(const void *adt, u32 phandle, u64 *base, u32 *npins)
{
    int arm_io = adt_path_offset(adt, "/arm-io");
    int node = arm_io;

    if (arm_io < 0)
        return -1;

    ADT_FOREACH_CHILD(adt, node)
    {
        u32 ph;
        u32 len;
        const char *compat;
        char path[64];
        int trace[8];

        if (ADT_GETPROP(adt, node, "AAPL,phandle", &ph) != sizeof(ph) || ph != phandle)
            continue;
        compat = adt_getprop(adt, node, "compatible", &len);
        if (!compat || len < 5 || memcmp(compat, "gpio,", 5))
            return -1;
        if (ADT_GETPROP(adt, node, "#gpio-pins", npins) != sizeof(*npins))
            return -1;
        snprintf(path, sizeof(path), "/arm-io/%s", adt_get_name(adt, node));
        if (adt_path_offset_trace(adt, path, trace) < 0 ||
            adt_get_reg(adt, trace, "reg", 0, base, NULL))
            return -1;
        return 0;
    }
    return -1;
}

/* Read strap @idx of @node into *@level. */
static int read_speaker_id_strap(const void *adt, int node, int idx, u32 *level)
{
    const char *name = adt_get_name(adt, node);
    struct adt_function_gpio fn;
    char prop[32];
    u64 base, reg;
    u32 npins, cfg, val;

    snprintf(prop, sizeof(prop), "function-speaker-id%d", idx);
    if (ADT_GETPROP(adt, node, prop, &fn) != sizeof(fn) || fn.fourcc != GPIO_FOURCC) {
        printf("kboot: audio: %s: %s is not a GPIO\n", name, prop);
        return -1;
    }
    if (fn.flags & ~STRAP_FLAGS_KNOWN || !(fn.flags & STRAP_FLAG_INPUT)) {
        printf("kboot: audio: %s: %s has unknown flags 0x%x\n", name, prop, fn.flags);
        return -1;
    }
    if (adt_gpio_by_phandle(adt, fn.phandle, &base, &npins) || fn.pin >= npins) {
        printf("kboot: audio: %s: %s: no GPIO controller %u pin %u\n", name, prop, fn.phandle,
               fn.pin);
        return -1;
    }

    reg = base + 4 * fn.pin;
    cfg = read32(reg);
    if (FIELD_GET(GPIO_PIN_PERIPH, cfg) || FIELD_GET(GPIO_PIN_MODE, cfg) == GPIO_PIN_MODE_OUT) {
        printf("kboot: audio: %s: %s: pin %u is not an input (0x%08x)\n", name, prop, fn.pin, cfg);
        return -1;
    }
    if (!(cfg & GPIO_PIN_INPUT_EN) || !FIELD_GET(GPIO_PIN_MODE, cfg)) {
        u32 in = (cfg & ~GPIO_PIN_MODE) | FIELD_PREP(GPIO_PIN_MODE, GPIO_PIN_MODE_IN) |
                 GPIO_PIN_INPUT_EN;
        write32(reg, in);
        udelay(STRAP_SETTLE_US);
        val = read32(reg);
        write32(reg, cfg);
    } else {
        val = cfg;
    }

    *level = !!(val & GPIO_PIN_DATA) ^ !!(fn.flags & STRAP_FLAG_ACTIVE_LOW);
    printf("kboot: audio: %s: %s = %u (pin %u: 0x%08x -> 0x%08x)\n", name, prop, *level, fn.pin,
           cfg, val);
    return 0;
}

static int setup_speaker_id(void *dt, const void *adt, int node)
{
    const char *name = adt_get_name(adt, node);
    u32 valid[MAX_VALID_SPEAKER_IDS];
    u32 len, id = 0;
    int nstraps = 0, chosen;
    bool listed = false;

    for (;; nstraps++) {
        char prop[32];

        snprintf(prop, sizeof(prop), "function-speaker-id%d", nstraps);
        if (!adt_getprop(adt, node, prop, NULL))
            break;
    }
    if (!nstraps || nstraps > MAX_SPEAKER_ID_STRAPS) {
        printf("kboot: audio: %s: %d speaker ID straps, no speaker ID\n", name, nstraps);
        return -1;
    }

    const void *v = adt_getprop(adt, node, "valid-speaker-ids", &len);
    if (!v || !len || len % sizeof(u32) || len > sizeof(valid)) {
        printf("kboot: audio: %s: no valid-speaker-ids, no speaker ID\n", name);
        return -1;
    }
    memcpy(valid, v, len);

    for (int i = 0; i < nstraps; i++) {
        u32 level;

        if (read_speaker_id_strap(adt, node, i, &level))
            return -1;
        id |= level << i;
    }

    for (u32 i = 0; i < len / sizeof(u32); i++)
        listed |= valid[i] == id;
    if (!listed) {
        printf("kboot: audio: %s: speaker ID %u is not a valid one, not passed on\n", name, id);
        return -1;
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

            if (ADT_GETPROP(adt, dev, "find-speaker-id", &find) != sizeof(find) || !find)
                continue;
            /* One speaker ID per Mac; a refusal is logged and never stops the boot */
            setup_speaker_id(dt, adt, dev);
            return 0;
        }
    }
    return 0;
}
