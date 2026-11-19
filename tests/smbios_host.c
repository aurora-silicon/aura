/* SPDX-License-Identifier: MIT */
/*
 * Host test of kboot.c's SMBIOS node (dt_set_cpus() to dt_set_smbios()) on one Mac's ADT and
 * device tree. tests/run-smbios.sh builds it with the functions copied out of src/kboot.c.
 *
 *   smbios-host <mac.adt> <mac.dtb> [sme]
 *
 * Prints every property of /smbios/smbios, then checks that the tables U-Boot builds from it
 * would carry the values Linux tools read.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adt.h"
#include "types.h"
#include "utils.h"

#include "libfdt/libfdt.h"

void *adt;
static size_t adt_size;
static int host_sme;

u32 adt_get_size(void)
{
    return adt_size;
}

int host_set_smbios(void *dt);
void host_product_name(const char *in, const char *chip, char *out, size_t size);

u64 host_sysreg(const char *name)
{
    if (strstr(name, "ID_AA64PFR1"))
        return host_sme ? (1UL << 24) : 0;
    fprintf(stderr, "unexpected system register %s\n", name);
    exit(2);
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

static void dump(void *dt, int node, const char *path)
{
    int prop, child;

    fdt_for_each_property_offset(prop, dt, node)
    {
        const char *name;
        int len;
        const unsigned char *val = fdt_getprop_by_offset(dt, prop, &name, &len);
        int printable = len > 0 && val[len - 1] == 0;

        for (int i = 0; printable && i < len - 1; i++)
            if (val[i] < 0x20 || val[i] > 0x7e)
                printable = 0;
        printf("%s %s = ", path, name);
        if (printable) {
            printf("\"%s\"\n", val);
        } else if (len == 4) {
            printf("<0x%x>\n", fdt32_ld((const fdt32_t *)val));
        } else if (len == 8) {
            printf("<0x%llx>\n", (unsigned long long)fdt64_ld((const fdt64_t *)val));
        } else {
            printf("[%d bytes]\n", len);
        }
    }
    fdt_for_each_subnode(child, dt, node)
    {
        char sub[256];

        snprintf(sub, sizeof(sub), "%s/%s", path, fdt_get_name(dt, child, NULL));
        dump(dt, child, sub);
    }
}

static int fails;

static void expect_str(void *dt, const char *path, const char *prop, const char *want)
{
    int node = fdt_path_offset(dt, path);
    const char *got = node >= 0 ? fdt_getprop(dt, node, prop, NULL) : NULL;

    if (!got || (want && strcmp(got, want))) {
        printf("FAIL %s %s: %s, want %s\n", path, prop, got ? got : "(absent)",
               want ? want : "(any)");
        fails++;
    } else {
        printf("ok %s %s\n", path, prop);
    }
}

static void expect_u32(void *dt, const char *path, const char *prop, u32 want)
{
    int node = fdt_path_offset(dt, path), len;
    const fdt32_t *val = node >= 0 ? fdt_getprop(dt, node, prop, &len) : NULL;

    if (!val || len != 4 || fdt32_ld(val) != want) {
        printf("FAIL %s %s: %s, want %u\n", path, prop, val && len == 4 ? "other" : "absent", want);
        fails++;
    } else {
        printf("ok %s %s = %u\n", path, prop, want);
    }
}

static void expect_nonzero(void *dt, const char *path, const char *prop)
{
    int node = fdt_path_offset(dt, path), len;
    const fdt32_t *val = node >= 0 ? fdt_getprop(dt, node, prop, &len) : NULL;

    if (!val || (len == 4 && !fdt32_ld(val)) || (len == 8 && !fdt64_ld((const fdt64_t *)val))) {
        printf("FAIL %s %s: absent or zero\n", path, prop);
        fails++;
    } else {
        printf("ok %s %s\n", path, prop);
    }
}

/* Product names lose the release date; Apple's names take the chip in its place */
static void test_product_names(void)
{
    static const struct {
        const char *in, *chip, *want;
    } cases[] = {
        {"MacBook Pro (16-inch, M4 Pro, Nov 2024)", NULL, "MacBook Pro (16-inch, M4 Pro)"},
        {"MacBook Pro (14-inch, M3 Max, 14 CPU cores, Nov 2023)", NULL,
         "MacBook Pro (14-inch, M3 Max, 14 CPU cores)"},
        {"MacBook Pro (16-inch, M1 Pro, 2021)", NULL, "MacBook Pro (16-inch, M1 Pro)"},
        {"MacBook Air (13-inch, M2, 2022)", NULL, "MacBook Air (13-inch, M2)"},
        {"MacBook Air (M1, 2020)", NULL, "MacBook Air (M1)"},
        {"iMac (24-inch, 2x USB-C, M4, 2024)", NULL, "iMac (24-inch, 2x USB-C, M4)"},
        {"Mac Studio (M3 Ultra, 2025)", NULL, "Mac Studio (M3 Ultra)"},
        {"Mac mini (M6)", NULL, "Mac mini (M6)"},
        {"MacBook Neo", NULL, "MacBook Neo"},
        {"TV 4K (1st Generation)", NULL, "TV 4K (1st Generation)"},
        {"MacBook Pro (16-inch, 2021)", "M1 Pro", "MacBook Pro (16-inch, M1 Pro)"},
        {"MacBook Air (M2, 2022)", "M2", "MacBook Air (M2)"},
        {"Mac mini (2023)", "M2 Pro", "Mac mini (M2 Pro)"},
        {"Mac mini (2023)", NULL, "Mac mini"},
        {"MacBook Neo", "A18 Pro", "MacBook Neo"},
    };
    char out[96];

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        host_product_name(cases[i].in, cases[i].chip, out, sizeof(out));
        if (strcmp(out, cases[i].want)) {
            printf("FAIL product name \"%s\" (%s): \"%s\", want \"%s\"\n", cases[i].in,
                   cases[i].chip ? cases[i].chip : "-", out, cases[i].want);
            fails++;
        } else {
            printf("ok product name \"%s\"\n", out);
        }
    }
}

int main(int argc, char **argv)
{
    size_t dt_size;
    void *dt;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <mac.adt> <mac.dtb> [sme]\n", argv[0]);
        return 2;
    }
    host_sme = argc > 3 && !strcmp(argv[3], "sme");
    adt = slurp(argv[1], 0, &adt_size);
    dt = slurp(argv[2], 0, &dt_size);
    dt = realloc(dt, dt_size + 65536);
    if (!dt || fdt_open_into(dt, dt, dt_size + 65536))
        return 2;

    /* The stage 1 version in /chosen, which the inventory must carry unchanged */
    char stage1[64] = "";
    int chosen = fdt_path_offset(dt, "/chosen"), slen;
    const char *s1 =
        chosen >= 0 ? fdt_getprop(dt, chosen, "aurora,aura-stage1-version", &slen) : NULL;
    if (s1)
        snprintf(stage1, sizeof(stage1), "%s", s1);

    if (host_set_smbios(dt)) {
        printf("FAIL dt_set_smbios\n");
        return 1;
    }
    /* A second pass, as a reloaded aura would make, must leave one node of each */
    if (host_set_smbios(dt)) {
        printf("FAIL second dt_set_smbios\n");
        return 1;
    }

    dump(dt, fdt_path_offset(dt, "/smbios"), "/smbios");
    test_product_names();

    expect_str(dt, "/smbios/smbios/system", "product", NULL);
    expect_str(dt, "/smbios/smbios/system", "sku", NULL);
    expect_str(dt, "/smbios/smbios/system", "family", NULL);
    expect_str(dt, "/smbios/smbios/processor", "version", NULL);
    expect_str(dt, "/smbios/smbios/system", "uuid", NULL);
    expect_str(dt, "/smbios/smbios/bios", "version", "aura 1.0.0b1-host");
    expect_str(dt, "/smbios/smbios/firmware-inventory/stage2", "version", "1.0.0b1-host");
    expect_str(dt, "/smbios/smbios/firmware-inventory/system", "version",
               "27.0 (mBoot-20457.1.29)");
    /* speed (apple,data-rate in the DT) and width (dcs_num_channels in the ADT) where given */
    expect_nonzero(dt, "/smbios/smbios/cache/l2-cache", "installed-size2");
    expect_u32(dt, "/smbios/smbios/firmware-inventory/stage2", "characteristics", 1);
    expect_u32(dt, "/smbios/smbios/firmware-inventory/u-boot", "characteristics", 1);
    expect_u32(dt, "/smbios/smbios/firmware-inventory/system", "characteristics", 0);
    expect_u32(dt, "/smbios/smbios/firmware-inventory/os", "characteristics", 0);
    if (stage1[0])
        expect_u32(dt, "/smbios/smbios/firmware-inventory/stage1", "characteristics", 0);
    if (fdt_path_offset(dt, "/smbios/smbios/firmware-inventory/tcon") >= 0)
        expect_u32(dt, "/smbios/smbios/firmware-inventory/tcon", "characteristics", 0);
    if (stage1[0])
        expect_str(dt, "/smbios/smbios/firmware-inventory/stage1", "version", stage1);
    expect_nonzero(dt, "/smbios/smbios/processor", "core-count");
    expect_nonzero(dt, "/smbios/smbios/processor", "max-speed");
    expect_nonzero(dt, "/smbios/smbios/memory-device/dram", "size");
    expect_nonzero(dt, "/smbios/smbios/memory-array/dram", "maximum-capacity");
    expect_nonzero(dt, "/smbios/smbios/memory-array-mapped-address/dram",
                   "extended-ending-address");
    if (fdt_subnode_offset(dt, fdt_path_offset(dt, "/smbios/smbios"), "system") < 0 ||
        fdt_path_offset(dt, "/smbios/smbios/memory-device/dram") < 0) {
        printf("FAIL node layout\n");
        fails++;
    }
    printf("%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
    return !!fails;
}
