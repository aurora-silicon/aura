/* SPDX-License-Identifier: GPL-2.0-only */

#include "../build/build_cfg.h"

#include "boot_storage.h"
#include "adt.h"
#include "boot_options.h"
#include "heapblock.h"
#include "kboot.h"
#include "malloc.h"
#include "nvme.h"
#include "string.h"
#include "utils.h"
#include "xnuboot.h"

#include "libfdt/libfdt.h"
#include "tinf/tinf.h"

#define BOOT_SPEC_MAX 1024
#define IMAGE_CAP     (512 * SZ_1M)
#define IMAGE_ALIGN   (2 * SZ_1M)

#ifdef CHAINLOADING
int rust_load_image(const char *spec, void **image, size_t *size, bool *proxy_request, char *trial,
                    size_t trial_capacity);
int rust_take_boot_trial(const char *uuid, char *buf, size_t capacity);
void rust_free_image(void *image, size_t size);

static int boot_image_header(const void *data, size_t file_size, u64 *text_offset, u64 *image_size)
{
    if (file_size < sizeof(struct kernel_header))
        return -1;
    const struct kernel_header *header = data;
    if (header->magic != 0x644d5241 || (header->flags & 1) || header->text_offset >= IMAGE_ALIGN ||
        (header->text_offset & (SZ_4K - 1)) || header->image_size < file_size ||
        header->image_size > IMAGE_CAP)
        return -1;
    if (header->image_size > UINT64_MAX - header->text_offset)
        return -1;
    *text_offset = header->text_offset;
    *image_size = header->image_size;
    return 0;
}

static int boot_ram_end(u64 *end)
{
    if (!cur_boot_args.mem_size || cur_boot_args.phys_base > UINT64_MAX - cur_boot_args.mem_size)
        return -1;
    *end = cur_boot_args.phys_base + cur_boot_args.mem_size;
    return 0;
}

static struct kernel_header *boot_prepare_image(const void *data, size_t file_size)
{
    u64 ram_end, text_offset, image_size;
    if (boot_ram_end(&ram_end))
        return NULL;

    bool compressed =
        file_size >= 18 && ((const u8 *)data)[0] == 0x1f && ((const u8 *)data)[1] == 0x8b;
    if (compressed) {
        printf("boot: inflating Image.gz (%lu bytes)\n", file_size);
        u64 dest = (u64)heapblock_alloc_aligned(0, IMAGE_ALIGN);
        if (dest >= ram_end)
            return NULL;
        u64 cap = min((u64)IMAGE_CAP, ram_end - dest);
        u32 expected_size;
        memcpy(&expected_size, (const u8 *)data + file_size - 4, sizeof(expected_size));
        if (!expected_size || expected_size > cap)
            return NULL;
        unsigned int source_len = file_size;
        unsigned int dest_len = cap;
        if (tinf_gzip_uncompress((void *)dest, &dest_len, data, &source_len) != TINF_OK ||
            source_len != file_size || dest_len != expected_size ||
            boot_image_header((void *)dest, dest_len, &text_offset, &image_size))
            return NULL;
        u64 span = text_offset + image_size;
        if (span > ram_end - dest || heapblock_alloc_aligned(span, IMAGE_ALIGN) != (void *)dest)
            return NULL;
        memmove((void *)(dest + text_offset), (void *)dest, dest_len);
        memset((void *)(dest + text_offset + dest_len), 0, image_size - dest_len);
        return (struct kernel_header *)(dest + text_offset);
    }

    if (boot_image_header(data, file_size, &text_offset, &image_size))
        return NULL;
    u64 span = text_offset + image_size;
    u64 dest = ALIGN_UP(heapblock_high_water(), IMAGE_ALIGN);
    if (dest >= ram_end || span > ram_end - dest)
        return NULL;
    void *block = heapblock_alloc_aligned(span, IMAGE_ALIGN);
    memset(block, 0, span);
    memcpy((u8 *)block + text_offset, data, file_size);
    return (struct kernel_header *)((u8 *)block + text_offset);
}

static int boot_check_dtb(const void *data, size_t size)
{
    if (size > UINT32_MAX || fdt_check_full(data, size) || (size_t)fdt_totalsize(data) != size)
        return -1;

    u32 target_len;
    const char *target = adt_getprop(adt, 0, "target-type", &target_len);
    if (!target || !target_len || target_len > 200)
        return -1;
    char compat[256] = "apple,";
    size_t i = 0;
    while (i < target_len && target[i]) {
        compat[6 + i] = tolower(target[i]);
        i++;
    }
    if (i == target_len)
        return -1;
    compat[6 + i] = 0;
    return fdt_node_check_compatible(data, 0, compat);
}
#endif

#ifdef CHAINLOADING
/*
 * Load and check <kernel>;<dtb>;<initramfs> (names[0..2], the initramfs "-" for none) from the
 * partition uuid; on success the kernel is prepared and *kernel and *fdt are set.
 */
static int boot_storage_files(const char *uuid, char *const names[3], struct kernel_header **kernel,
                              void **fdt)
{
    bool have_initrd = strcmp(names[2], "-") != 0;
    size_t cap = strlen(uuid) + 2 * BOOT_SPEC_MAX + 2;
    char *file_spec = malloc(cap);
    void *files[3] = {0};
    size_t sizes[3] = {0};
    int ret = -1;

    if (!file_spec)
        return -1;
    for (size_t i = 0; i < (have_initrd ? 3 : 2); i++) {
        int len = snprintf(file_spec, cap, "%s;%s", uuid, names[i]);
        if (len < 0 || (size_t)len >= cap ||
            rust_load_image(file_spec, &files[i], &sizes[i], NULL, NULL, 0) || !files[i] ||
            !sizes[i])
            goto out;
    }

    if (boot_check_dtb(files[1], sizes[1]))
        goto out;
    if (have_initrd &&
        !((sizes[2] >= 18 && ((u8 *)files[2])[0] == 0x1f && ((u8 *)files[2])[1] == 0x8b) ||
          (sizes[2] >= 6 && (!memcmp(files[2], "070701", 6) || !memcmp(files[2], "070702", 6)))))
        goto out;

    struct kernel_header *prepared = boot_prepare_image(files[0], sizes[0]);
    if (!prepared)
        goto out;
    kboot_set_initrd(files[2], sizes[2]);
    *kernel = prepared;
    *fdt = files[1];
    files[1] = files[2] = NULL;
    printf("boot: ESP image and DTB validated%s\n",
           have_initrd ? " with initramfs" : " without initramfs");
    ret = 0;

out:
    for (size_t i = 0; i < 3; i++)
        rust_free_image(files[i], sizes[i]);
    free(file_spec);
    return ret;
}

/*
 * aura-try for boot=: an armed marker at the ESP root naming "<kernel>;<dtb>;<initramfs>" boots
 * those files once instead of the configured ones (trial.rs). The marker is spent before they
 * are read, so if the trial never comes up, the next boot takes the configured files again.
 * Linux finds the line in chosen/aurora,trial.
 */
static bool boot_storage_trial(const char *uuid, struct kernel_header **kernel, void **fdt)
{
    if (aura_trial_enabled())
        return false;
    /* The line as taken, then a copy split into its three names. */
    char *trial = malloc(2 * (BOOT_SPEC_MAX + 1));
    char *names[3];
    if (!trial || rust_take_boot_trial(uuid, trial, BOOT_SPEC_MAX + 1))
        goto none;
    names[0] = strcpy(trial + BOOT_SPEC_MAX + 1, trial);
    names[1] = strchr(names[0], ';');
    names[2] = names[1] ? strchr(names[1] + 1, ';') : NULL;
    if (!names[2])
        goto none;
    *names[1]++ = 0;
    *names[2]++ = 0;
    printf("boot: aura-try: booting %s once\n", trial);
    if (boot_storage_files(uuid, names, kernel, fdt)) {
        printf("boot: aura-try files failed; booting the configured files\n");
        goto none;
    }
    if (kboot_set_chosen("aurora,trial", trial) < 0)
        printf("boot: could not record the trial in chosen\n");
    free(trial);
    return true;

none:
    free(trial);
    return false;
}
#endif

int boot_storage_load(const char *spec, struct kernel_header **kernel, void **fdt)
{
#ifndef CHAINLOADING
    UNUSED(spec);
    UNUSED(kernel);
    UNUSED(fdt);
    printf("boot: internal storage is not supported in this build\n");
    return -1;
#else
    if (!spec || !kernel || !fdt)
        return -1;
    size_t spec_len = strnlen(spec, BOOT_SPEC_MAX + 1);
    if (!spec_len || spec_len > BOOT_SPEC_MAX)
        return -1;

    char *fields = malloc(spec_len + 1);
    if (!fields)
        return -1;
    memcpy(fields, spec, spec_len + 1);
    char *part[4];
    char *cursor = fields;
    for (size_t i = 0; i < 4; i++) {
        part[i] = cursor;
        char *sep = strchr(cursor, ';');
        if (i < 3) {
            if (!sep || sep == cursor)
                goto fail_specs;
            *sep = 0;
            cursor = sep + 1;
        } else if (sep || !*cursor) {
            goto fail_specs;
        }
    }

    /* EFI payloads such as U-Boot let the selected OS supply its initramfs ("-"). */
    if (!nvme_init())
        goto fail_specs;
    if (!boot_storage_trial(part[0], kernel, fdt) &&
        boot_storage_files(part[0], &part[1], kernel, fdt))
        goto fail;
    free(fields);
    return 0;

fail:
    printf("boot: internal storage load failed; returning to proxy\n");
    nvme_shutdown();
fail_specs:
    free(fields);
    return -1;
#endif
}
