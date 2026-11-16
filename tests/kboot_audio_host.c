/* SPDX-License-Identifier: MIT */
/*
 * Host test of the speaker amp configuration from the ADT (src/kboot_audio.c), with aura's own
 * ADT reader (rust/src/adt.rs, built for the host) and libfdt.
 *
 *   kboot-audio-host <in.adt> <in.dtb> [staged]
 *   kboot-audio-host nospace <in.adt> <in.dtb>
 *
 * The first form runs kboot_audio_amp_config() (kboot_audio_amp_config_staged() with "staged")
 * and prints, for every ti,sn012776 node of the result in tree
 * order, its I2C address and its apple,bop-config, apple,amp-dcblocker-config,
 * apple,speaker-config and apple,amp-tx-zd-config ("-" when absent), one line each ("amp ..."),
 * after aura's own log lines.
 *
 * Then runs kboot_audio_speaker_id() and prints "speaker-id <value>" (/chosen/asahi,speaker-id,
 * "-" when absent) and, for every modelled GPIO pin, "gpio <address> <register>" with the
 * register as the code left it. The pins come from KBOOT_AUDIO_GPIO, a comma-separated list of
 * <address>:<register>:<pad level>. A pin's data bit reads as its pad level only while the pin
 * is an input with its input stage on; any other address is a test failure.
 *
 * "nospace" runs both amp copies on the tree with every amount of free room from none to more
 * than the group needs, so that a write runs out of room at every point of the group. The staged
 * copy must leave every amp either with the whole group's configuration or exactly as the tree
 * gave it; the direct copy must be seen to leave some amps written and others not, which shows
 * the room sizes reach a failure partway through.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adt.h"
#include "kboot_audio.h"
#include "types.h"

#include "libfdt/libfdt.h"

void *adt;
static u32 adt_size;

#define MAX_PINS 16
static struct {
    u64 addr;
    u32 reg;
    u32 pad;
} pins[MAX_PINS];
static int npins;

static void parse_pins(void)
{
    const char *s = getenv("KBOOT_AUDIO_GPIO");

    while (s && *s) {
        unsigned long long a;
        unsigned int r, p;
        int used;

        if (npins >= MAX_PINS || sscanf(s, "%lli:%i:%i%n", &a, &r, &p, &used) != 3) {
            fprintf(stderr, "bad KBOOT_AUDIO_GPIO at '%s'\n", s);
            exit(2);
        }
        pins[npins].addr = a;
        pins[npins].reg = r;
        pins[npins].pad = !!p;
        npins++;
        s += used;
        if (*s == ',')
            s++;
    }
}

static int pin_at(u64 addr)
{
    for (int i = 0; i < npins; i++)
        if (pins[i].addr == addr)
            return i;
    printf("FAIL: access to unmodelled address 0x%llx\n", (unsigned long long)addr);
    exit(1);
}

u32 read32(u64 addr)
{
    int i = pin_at(addr);
    u32 reg = pins[i].reg & ~1u;
    u32 mode = (pins[i].reg >> 1) & 7;

    /* The pad level reaches the data bit through an enabled input stage only */
    if ((pins[i].reg & BIT(9)) && mode && mode != 1)
        reg |= pins[i].pad;
    else
        reg |= pins[i].reg & 1;
    return reg;
}

void write32(u64 addr, u32 data)
{
    pins[pin_at(addr)].reg = data;
}

void udelay(u32 us)
{
    (void)us;
}

u32 adt_get_size(void)
{
    return adt_size;
}

static void *slurp(const char *path, size_t extra, size_t *size)
{
    FILE *f = fopen(path, "rb");
    long n;
    void *buf;

    if (!f || fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) {
        perror(path);
        exit(2);
    }
    buf = calloc(1, n + extra);
    if (!buf || fread(buf, 1, n, f) != (size_t)n) {
        perror(path);
        exit(2);
    }
    fclose(f);
    *size = n;
    return buf;
}

static void print_prop(const void *dt, int node, const char *name)
{
    int len;
    const unsigned char *v = fdt_getprop(dt, node, name, &len);

    printf(" %s=", name);
    if (!v) {
        printf("-");
        return;
    }
    for (int i = 0; i < len; i++)
        printf("%02x", v[i]);
}

/* Append @name's value on @node to @out ("-" when absent) */
static void describe_prop(char *out, size_t size, const void *dt, int node, const char *name)
{
    int len;
    const unsigned char *v = fdt_getprop(dt, node, name, &len);
    size_t used = strlen(out);

    used += snprintf(out + used, size - used, " %s=", name);
    if (!v) {
        snprintf(out + used, size - used, "-");
        return;
    }
    for (int i = 0; i < len && used < size; i++)
        used += snprintf(out + used, size - used, "%02x", v[i]);
}

/* The "amp ..." lines for every ti,sn012776 node of @dt, in tree order */
static void describe(char *out, size_t size, const void *dt)
{
    out[0] = 0;
    for (int node = fdt_node_offset_by_compatible(dt, -1, "ti,sn012776"); node >= 0;
         node = fdt_node_offset_by_compatible(dt, node, "ti,sn012776")) {
        const fdt32_t *reg = fdt_getprop(dt, node, "reg", NULL);
        size_t used = strlen(out);

        snprintf(out + used, size - used, "amp 0x%x", reg ? fdt32_to_cpu(reg[0]) : 0);
        describe_prop(out, size, dt, node, "apple,bop-config");
        describe_prop(out, size, dt, node, "apple,amp-dcblocker-config");
        describe_prop(out, size, dt, node, "apple,speaker-config");
        describe_prop(out, size, dt, node, "apple,amp-tx-zd-config");
        used = strlen(out);
        snprintf(out + used, size - used, "\n");
    }
}

#define DESC_SIZE 16384

/*
 * Run one copy on @orig opened into a buffer of @capacity bytes and say what the amps got:
 * 'a' all of the group's configuration (@all), 'n' none of it (@none), 'p' part of it; 'x' when
 * the tree itself does not fit (libfdt's layout can need a few bytes more than the packed size).
 */
static char run_once(const void *orig, size_t capacity, bool staged, const char *all,
                     const char *none)
{
    static char got[DESC_SIZE];
    void *dt = calloc(1, capacity);
    char what;

    if (!dt) {
        fprintf(stderr, "nospace: out of memory\n");
        exit(1);
    }
    if (fdt_open_into(orig, dt, capacity)) {
        free(dt);
        return 'x';
    }
    if (staged ? kboot_audio_amp_config_staged(dt, adt) : kboot_audio_amp_config(dt, adt)) {
        fprintf(stderr, "nospace: the copy failed\n");
        exit(1);
    }
    if (fdt_check_full(dt, fdt_totalsize(dt))) {
        fprintf(stderr, "nospace: the result is not a valid device tree\n");
        exit(1);
    }
    describe(got, sizeof(got), dt);
    free(dt);
    what = !strcmp(got, all) ? 'a' : !strcmp(got, none) ? 'n' : 'p';
    return what;
}

static int nospace(void *orig)
{
    static char all[DESC_SIZE], none[DESC_SIZE];
    size_t full = fdt_totalsize(orig);
    void *packed = calloc(1, full);
    size_t min;
    int staged_counts[3] = {0}, direct_partial = 0, rooms = 0;

    describe(none, sizeof(none), orig);
    /* The whole group's configuration, with room to spare */
    {
        void *dt = calloc(1, full);

        memcpy(dt, orig, full);
        kboot_audio_amp_config_staged(dt, adt);
        describe(all, sizeof(all), dt);
        free(dt);
    }
    if (!strcmp(all, none)) {
        fprintf(stderr, "nospace: the copy changes nothing on this tree\n");
        return 1;
    }
    memcpy(packed, orig, full);
    if (fdt_pack(packed)) {
        fprintf(stderr, "nospace: cannot pack the tree\n");
        return 1;
    }
    /* fdt_open_into() only moves an in-order tree, so it opens from the packed copy */
    min = fdt_totalsize(packed);

    /* From no room at all to past what the whole group needs (it then always succeeds) */
    for (size_t room = 0; room <= full - min; room += 4) {
        char s = run_once(packed, min + room, true, all, none);
        char d = run_once(packed, min + room, false, all, none);

        if (s == 'x' || d == 'x')
            continue;
        rooms++;
        if (s == 'p') {
            fprintf(stderr,
                    "nospace: staged copy left the group half written with %zu bytes free\n", room);
            free(packed);
            return 1;
        }
        staged_counts[s == 'a' ? 0 : 1]++;
        direct_partial += d == 'p';
        if (s == 'a' && d == 'a')
            break;
    }
    free(packed);
    if (!staged_counts[0] || !staged_counts[1] || !direct_partial) {
        fprintf(stderr,
                "nospace: %d room sizes, staged all %d none %d, direct partial %d: "
                "no failure partway through was reached\n",
                rooms, staged_counts[0], staged_counts[1], direct_partial);
        return 1;
    }
    printf("nospace: %d room sizes: staged copy all-or-nothing (all %d, none %d); direct copy "
           "half written at %d\n",
           rooms, staged_counts[0], staged_counts[1], direct_partial);
    return 0;
}

int main(int argc, char **argv)
{
    size_t asz, dsz;
    bool staged = false, space = false;
    void *dt;

    if (argc == 4 && !strcmp(argv[1], "nospace")) {
        space = true;
        argv++;
    } else if (argc == 4 && !strcmp(argv[3], "staged")) {
        staged = true;
    } else if (argc != 3) {
        fprintf(stderr, "usage: %s [nospace] <in.adt> <in.dtb> [staged]\n", argv[0]);
        return 2;
    }
    adt = slurp(argv[1], 0, &asz);
    adt_size = asz;
    dt = slurp(argv[2], 64 * 1024, &dsz);
    if (fdt_open_into(dt, dt, dsz + 64 * 1024)) {
        fprintf(stderr, "bad dtb\n");
        return 2;
    }
    if (space)
        return nospace(dt);

    parse_pins();
    if (staged ? kboot_audio_amp_config_staged(dt, adt) : kboot_audio_amp_config(dt, adt)) {
        printf("kboot_audio_amp_config failed\n");
        return 1;
    }
    if (fdt_check_full(dt, fdt_totalsize(dt))) {
        printf("result is not a valid device tree\n");
        return 1;
    }

    for (int node = fdt_node_offset_by_compatible(dt, -1, "ti,sn012776"); node >= 0;
         node = fdt_node_offset_by_compatible(dt, node, "ti,sn012776")) {
        const fdt32_t *reg = fdt_getprop(dt, node, "reg", NULL);

        printf("amp 0x%x", reg ? fdt32_to_cpu(reg[0]) : 0);
        print_prop(dt, node, "apple,bop-config");
        print_prop(dt, node, "apple,amp-dcblocker-config");
        print_prop(dt, node, "apple,speaker-config");
        print_prop(dt, node, "apple,amp-tx-zd-config");
        printf("\n");
    }

    if (kboot_audio_speaker_id(dt, adt)) {
        printf("kboot_audio_speaker_id failed\n");
        return 1;
    }
    if (fdt_check_full(dt, fdt_totalsize(dt))) {
        printf("result is not a valid device tree\n");
        return 1;
    }
    int chosen = fdt_path_offset(dt, "/chosen");
    int len = 0;
    const fdt32_t *id = chosen < 0 ? NULL : fdt_getprop(dt, chosen, "asahi,speaker-id", &len);
    if (!id)
        printf("speaker-id -\n");
    else if (len != sizeof(*id))
        printf("speaker-id bad-length-%d\n", len);
    else
        printf("speaker-id %u\n", fdt32_to_cpu(*id));
    for (int i = 0; i < npins; i++)
        printf("gpio 0x%llx 0x%08x\n", (unsigned long long)pins[i].addr, pins[i].reg);
    return 0;
}
