/* SPDX-License-Identifier: MIT */
/*
 * LEGACY-ASAHI(2027-02-28): aurora_legacy_chosen_aliases() on a real libfdt tree. A /chosen
 * published under the aurora, names gets the pre-rename asahi, names next to the ones released
 * kernels and U-Boot read, with the same bytes; nothing else gets an alias.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "kboot_legacy.h"
#include "libfdt/libfdt.h"

static unsigned char blob[65536];

static const char *str(void *dt, const char *path, const char *name)
{
    int node = fdt_path_offset(dt, path);
    return node < 0 ? NULL : fdt_getprop(dt, node, name, NULL);
}

int main(void)
{
    void *dt = blob;
    assert(!fdt_create_empty_tree(dt, sizeof(blob)));
    int chosen = fdt_add_subnode(dt, 0, "chosen");
    assert(chosen >= 0);
    assert(!fdt_setprop_string(dt, chosen, "aurora,os-fw-version", "26.6.2"));
    assert(!fdt_setprop_string(dt, chosen, "aurora,aura-stage1-version", "s1"));
    assert(!fdt_setprop_string(dt, chosen, "aurora,aura-stage2-version", "s2"));
    assert(!fdt_setprop_string(dt, chosen, "aurora,efi-system-partition", "UUID"));
    assert(!fdt_setprop_string(dt, chosen, "aurora,t8122-gpu", "1"));
    assert(!fdt_setprop_u32(dt, chosen, "aurora,speaker-id", 3));
    /* names that never had an asahi, form stay as they are */
    assert(!fdt_setprop_string(dt, chosen, "aurora,stage1-log", "x"));
    assert(!fdt_setprop_string(dt, chosen, "aurora,external-display-preflight", "x"));
    assert(!fdt_setprop_string(dt, chosen, "bootargs", "quiet"));
    /* an owner who also set the old name keeps theirs */
    assert(!fdt_setprop_string(dt, chosen, "aurora,show-notch", "1"));
    assert(!fdt_setprop_string(dt, chosen, "asahi,show-notch", "0"));

    chosen = fdt_path_offset(dt, "/chosen");
    int pmp = fdt_add_subnode(dt, chosen, "aurora,t8122-pmp");
    assert(pmp >= 0);
    assert(!fdt_setprop_u32(dt, pmp, "value", 0x1234));
    int sub = fdt_add_subnode(dt, pmp, "child");
    assert(sub >= 0);
    assert(!fdt_setprop_string(dt, sub, "name2", "deep"));
    chosen = fdt_path_offset(dt, "/chosen");
    assert(fdt_add_subnode(dt, chosen, "aurora,t8122-gpu-diagnostics") >= 0);

    int n = aurora_legacy_chosen_aliases(dt);
    printf("aliases: %d\n", n);
    assert(n == 7); /* 6 properties (show-notch kept) + 1 node */

    assert(!strcmp(str(dt, "/chosen", "asahi,os-fw-version"), "26.6.2"));
    assert(!strcmp(str(dt, "/chosen", "asahi,m1n1-stage1-version"), "s1"));
    assert(!strcmp(str(dt, "/chosen", "asahi,m1n1-stage2-version"), "s2"));
    assert(!strcmp(str(dt, "/chosen", "asahi,efi-system-partition"), "UUID"));
    assert(!strcmp(str(dt, "/chosen", "asahi,t8122-gpu"), "1"));
    assert(fdt32_to_cpu(*(const fdt32_t *)str(dt, "/chosen", "asahi,speaker-id")) == 3);
    assert(!strcmp(str(dt, "/chosen", "asahi,show-notch"), "0"));
    assert(!str(dt, "/chosen", "asahi,stage1-log"));
    assert(!str(dt, "/chosen", "asahi,external-display-preflight"));
    assert(!strcmp(str(dt, "/chosen", "aurora,os-fw-version"), "26.6.2"));
    assert(fdt32_to_cpu(*(const fdt32_t *)str(dt, "/chosen/asahi,t8122-pmp", "value")) == 0x1234);
    assert(!strcmp(str(dt, "/chosen/asahi,t8122-pmp/child", "name2"), "deep"));
    assert(fdt_path_offset(dt, "/chosen/aurora,t8122-pmp") >= 0);
    assert(fdt_path_offset(dt, "/chosen/asahi,t8122-gpu-diagnostics") < 0);

    /* A second call adds nothing new */
    assert(aurora_legacy_chosen_aliases(dt) == 1); /* the node already there counts, no copy */
    assert(fdt_path_offset(dt, "/chosen/asahi,t8122-pmp/child") >= 0);

    /* A full tree: -1, and the aurora, names are untouched */
    static unsigned char small[512];
    assert(!fdt_create_empty_tree(small, sizeof(small)));
    chosen = fdt_add_subnode(small, 0, "chosen");
    for (int i = 0; i < 64; i++) {
        char name[48];
        snprintf(name, sizeof(name), "aurora,t8122-x%d", i);
        if (fdt_setprop_string(small, fdt_path_offset(small, "/chosen"), name, "1"))
            break;
    }
    assert(aurora_legacy_chosen_aliases(small) == -1);
    assert(str(small, "/chosen", "aurora,t8122-x0"));

    puts("kboot_legacy_host: PASS");
    return 0;
}
