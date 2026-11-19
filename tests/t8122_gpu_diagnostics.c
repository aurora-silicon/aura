/* SPDX-License-Identifier: MIT */
/* Synthetic ADT inputs + real libfdt. No device or firmware access. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/adt.h"
#include "../src/firmware.h"
#include "../src/kboot_gpu_t8122.h"
#include "../src/libfdt/libfdt.h"

void *adt;
static int mode, reads, image_reads, fail_alloc;
u64 kboot_ram_base(void) { return 0x800000000; }
u64 kboot_ram_size(void) { return 0x100000000; }
void *__real_malloc(size_t size);
void *__wrap_malloc(size_t size)
{
    return fail_alloc ? NULL : __real_malloc(size);
}
static unsigned char data[32769];
static const char *const paths[] = {
    "/arm-io", "/arm-io/sgx", "/arm-io/gfx-asc", "/arm-io/pmgr",
    "/arm-io/mapper-gfx-asc", "/arm-io/gfx-asc/iop-gfx-nub", "/chosen", "/",
};

int adt_path_offset(const void *tree, const char *path)
{
    (void)tree;
    reads++;
    for (int i = 0; i < 8; i++)
        if (!strcmp(paths[i], path))
            return (mode == 1 && i == 2) || (mode == 4 && i >= 5) ? -1 : i;
    return -1;
}

const void *adt_getprop(const void *tree, int node, const char *name, u32 *size)
{
    (void)tree;
    if (!strcmp(name, "segment-ranges") && (node == 2 || node == 5)) {
        *size = 64;
        return data;
    }
    if (!strcmp(name, "segment-names") && (node == 2 || node == 5)) {
        static const char names[] = "__TEXT;__DATA";
        *size = sizeof(names);
        return names;
    }
    if (!strcmp(name, "gpu-fast-die0-target") && node == 1) {
        *size = 4;
        return data;
    }
    if (strcmp(name, "reg"))
        return NULL;
    *size = mode == 2 ? sizeof(data) : mode == 3 ? 32768 : 16;
    data[0] = (unsigned char)(node + 1);
    return data;
}

bool firmware_gfx_image_info(struct gfx_fw_image_info *out)
{
    image_reads++;
    if (mode == 4)
        return false;
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < 16; i++)
        out->uuid[i] = i;
    out->patchbay_offset = 0x1000;
    out->patchbay_size = 0x200;
    out->text_size = 0x64000;
    return true;
}

bool firmware_gfx_rtkit_version(char out[64])
{
    if (mode == 4)
        return false;
    strcpy(out, "RTKit-2419.140.12");
    return true;
}

static void prepare(void *dt, int size, const void *opt, int len)
{
    assert(!fdt_create_empty_tree(dt, size));
    int node = fdt_add_subnode(dt, 0, "chosen");
    assert(node >= 0);
    assert(!fdt_setprop_string(dt, node, "bootargs", "unchanged"));
    if (opt)
        assert(!fdt_setprop(dt, node, "aurora,t8122-gpu-diag", opt, len));
    mode = reads = image_reads = 0;
}

int main(void)
{
    static unsigned char dt[131072], before[sizeof(dt)];
    const char *export_path = "/chosen/aurora,t8122-gpu-diagnostics";

    assert(!fdt_create_empty_tree(dt, sizeof(dt)));
    memcpy(before, dt, sizeof(dt));
    assert(!dt_gpu_t8122_diagnostics(dt));
    assert(!reads && !image_reads && !memcmp(before, dt, sizeof(dt)));

    prepare(dt, sizeof(dt), "1", 2);
    memcpy(before, dt, sizeof(dt));
    fail_alloc = 1;
    assert(!dt_gpu_t8122_diagnostics(dt));
    fail_alloc = 0;
    assert(!reads && !image_reads && !memcmp(before, dt, sizeof(dt)));

    /* Exact string opt-in only, including its NUL terminator. */
    const char *opts[] = {NULL, "0", "1", "1xx", "1"};
    int lengths[] = {0, 2, 1, 3, 2};
    for (int i = 0; i < 4; i++) {
        prepare(dt, sizeof(dt), opts[i], lengths[i]);
        memcpy(before, dt, sizeof(dt));
        assert(!dt_gpu_t8122_diagnostics(dt));
        assert(!reads && !image_reads && !memcmp(before, dt, sizeof(dt)));
    }

    prepare(dt, sizeof(dt), "1", 2);
    assert(!dt_gpu_t8122_diagnostics(dt));
    int root = fdt_path_offset(dt, export_path), len;
    assert(root >= 0);
    assert(fdt_getprop(dt, root, "complete", &len) && len == 0);
    const fdt32_t *version = fdt_getprop(dt, root, "schema-version", &len);
    assert(version && len == 4 && fdt32_to_cpu(*version) == 1);
    for (int i = 0; i < 5; i++) {
        char path[128];
        const char *name = strrchr(paths[i], '/') + 1;
        snprintf(path, sizeof(path), "%s/%s", export_path, name);
        int node = fdt_path_offset(dt, path);
        const unsigned char *p = fdt_getprop(dt, node, "adt,reg", &len);
        assert(p && len == 16 && p[0] == i + 1);
        assert(!fdt_getprop(dt, node, "reg", &len));
        assert(!fdt_getprop(dt, node, "compatible", &len));
    }
    int asc = fdt_path_offset(dt, "/chosen/aurora,t8122-gpu-diagnostics/gfx-asc");
    assert(fdt_getprop(dt, asc, "adt,segment-ranges", &len) && len == 64);
    int nub = fdt_path_offset(dt, "/chosen/aurora,t8122-gpu-diagnostics/gfx-nub");
    assert(fdt_getprop(dt, nub, "adt,segment-names", &len) && len == 14);
    assert(!fdt_getprop(dt, nub, "adt,reg", &len));
    int sgx = fdt_path_offset(dt, "/chosen/aurora,t8122-gpu-diagnostics/sgx");
    assert(fdt_getprop(dt, sgx, "adt,gpu-fast-die0-target", &len) && len == 4);
    assert(!fdt_getprop(dt, sgx, "adt,segment-ranges", &len));
    const void *usable_prop = fdt_getprop(dt, root, "usable-memory", &len);
    assert(usable_prop && len == 16);
    fdt64_t usable[2];
    memcpy(usable, usable_prop, sizeof(usable));
    assert(fdt64_to_cpu(usable[0]) == kboot_ram_base());
    assert(fdt64_to_cpu(usable[1]) == kboot_ram_size());
    const fdt32_t *revision = fdt_getprop(dt, root, "exporter-revision", &len);
    assert(revision && len == 4 && fdt32_to_cpu(*revision) == 4);
    assert(image_reads == 1);
    const unsigned char *uuid = fdt_getprop(dt, root, "image-uuid", &len);
    assert(uuid && len == 16 && uuid[0] == 0 && uuid[15] == 15);
    const fdt32_t *text_size = fdt_getprop(dt, root, "image-text-size", &len);
    assert(text_size && len == 4 && fdt32_to_cpu(*text_size) == 0x64000);
    assert(!strcmp(fdt_getprop(dt, root, "image-rtkit-version", &len), "RTKit-2419.140.12"));
    int chosen = fdt_path_offset(dt, "/chosen");
    assert(!strcmp(fdt_getprop(dt, chosen, "bootargs", NULL), "unchanged"));
    /* Repeated calls do not overwrite earlier evidence. */
    memcpy(before, dt, sizeof(dt));
    reads = image_reads = 0;
    assert(!dt_gpu_t8122_diagnostics(dt));
    assert(!reads && !image_reads && !memcmp(before, dt, sizeof(dt)));

    /* Missing ASC, oversized property and aggregate overflow roll back bytes. */
    for (int failure = 1; failure <= 3; failure++) {
        prepare(dt, sizeof(dt), "1", 2);
        mode = failure;
        memcpy(before, dt, sizeof(dt));
        assert(!dt_gpu_t8122_diagnostics(dt));
        assert(!memcmp(before, dt, sizeof(dt)));
    }

    prepare(dt, sizeof(dt), "1", 2);
    mode = 4;
    assert(!dt_gpu_t8122_diagnostics(dt));
    root = fdt_path_offset(dt, export_path);
    assert(root >= 0 && fdt_getprop(dt, root, "complete", &len));
    assert(fdt_path_offset(dt, "/chosen/aurora,t8122-gpu-diagnostics/gfx-nub") < 0);
    assert(!fdt_getprop(dt, root, "image-uuid", &len));

    /* Exercise every out-of-space boundary, including partial child writes. */
    for (int size = 256; size <= 4096; size += 8) {
        prepare(dt, size, "1", 2);
        memcpy(before, dt, size);
        assert(!dt_gpu_t8122_diagnostics(dt));
        root = fdt_path_offset(dt, export_path);
        if (root < 0)
            assert(!memcmp(before, dt, size));
        else
            assert(fdt_getprop(dt, root, "complete", &len) && len == 0);
    }
    puts("PASS: opt-in, metadata, idempotency, allocation failure, bounds and 481 capacity boundaries");
}
