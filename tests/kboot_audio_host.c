/* SPDX-License-Identifier: MIT */
/*
 * Host test of the speaker amp configuration from the ADT (src/kboot_audio.c), with aura's own
 * ADT reader (rust/src/adt.rs, built for the host) and libfdt.
 *
 *   kboot-audio-host <in.adt> <in.dtb>
 *
 * Runs kboot_audio_amp_config() and prints, for every ti,sn012776 node of the result in tree
 * order, its I2C address and its apple,bop-config, apple,amp-dcblocker-config and
 * apple,speaker-config ("-" when absent), one line each ("amp ..."), after aura's own log lines.
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

int main(int argc, char **argv)
{
    size_t asz, dsz;
    void *dt;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <in.adt> <in.dtb>\n", argv[0]);
        return 2;
    }
    adt = slurp(argv[1], 0, &asz);
    adt_size = asz;
    dt = slurp(argv[2], 64 * 1024, &dsz);
    if (fdt_open_into(dt, dt, dsz + 64 * 1024)) {
        fprintf(stderr, "bad dtb\n");
        return 2;
    }

    if (kboot_audio_amp_config(dt, adt)) {
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
        printf("\n");
    }
    return 0;
}
