# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the production ESP loader with and without an OS initramfs, and its aura-try trial."""

from pathlib import Path
import subprocess


def test_optional_initramfs(tmp_path):
    source = (Path(__file__).resolve().parents[2] / "src/boot_storage.c").read_text()
    # The files loader and the trial sit in their own #ifdef CHAINLOADING block.
    loader = "#ifdef CHAINLOADING\n" + source[source.index("static int boot_storage_files("):]
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
#define CHAINLOADING 1
#define T8140 0x8140
#define BOOT_SPEC_MAX 1024
struct kernel_header { int unused; };
static struct kernel_header image;
static char tree, trial_tree, archive[] = "070701";
static int loaded, shutdowns;
static bool invalid_initrd, missing_initrd;
static const char *armed, *chosen;
static char chosen_value[BOOT_SPEC_MAX + 1];
static void *initrd;
static size_t initrd_size;
static bool nvme_init(void) { return true; }
static bool nvme_shutdown(void) { shutdowns++; return true; }
static int rust_load_image(const char *spec, void **data, size_t *size, bool *proxy_request,
                           char *trial, size_t trial_capacity)
{
    assert(!proxy_request && !trial && !trial_capacity);
    loaded++;
    if (!strcmp(spec, "esp;image")) { *data = &image; *size = sizeof(image); }
    else if (!strcmp(spec, "esp;dtb")) { *data = &tree; *size = 1; }
    else if (!strcmp(spec, "esp;trial-dtb")) { *data = &trial_tree; *size = 1; }
    else if (!strcmp(spec, "esp;bad-dtb")) { *data = archive; *size = 1; }
    else if (!strcmp(spec, "esp;bad-initrd")) { *data = archive; *size = 1; }
    else if (!strcmp(spec, "esp;initrd") && !missing_initrd) {
        *data = archive; *size = invalid_initrd ? 1 : sizeof(archive);
    } else return -1;
    return 0;
}
static void rust_free_image(void *data, size_t size) { (void)data; (void)size; }
static int boot_check_dtb(const void *data, size_t size)
{ return (data == &tree || data == &trial_tree) && size == 1 ? 0 : -1; }
static struct kernel_header *boot_prepare_image(const void *data, size_t size)
{ return data == &image && size == sizeof(image) ? &image : NULL; }
static void kboot_set_initrd(void *data, size_t size)
{ initrd = data; initrd_size = size; }
static bool aura_trial_enabled(void) { return false; }
/* An armed marker is spent when taken, as trial.rs does on disk. */
static int rust_take_boot_trial(const char *uuid, char *buf, size_t capacity)
{
    assert(!strcmp(uuid, "esp") && capacity == BOOT_SPEC_MAX + 1);
    if (!armed)
        return -1;
    strcpy(buf, armed);
    armed = NULL;
    return 0;
}
/* Copies the value, as the FDT property does: the caller frees its buffer. */
static int kboot_set_chosen(const char *name, const char *value)
{
    assert(!strcmp(name, "aurora,trial") && strlen(value) < sizeof(chosen_value));
    chosen = strcpy(chosen_value, value);
    return 0;
}
''' + loader + r'''
int main(void)
{
    struct kernel_header *kernel = NULL;
    void *dtb = NULL;
    assert(boot_storage_load("esp;image;dtb;initrd", &kernel, &dtb) == 0);
    assert(loaded == 3 && kernel == &image && dtb == &tree);
    assert(initrd == archive && initrd_size == sizeof(archive));

    /* An EFI payload must not inherit even the previous valid initramfs. */
    loaded = 0;
    assert(boot_storage_load("esp;image;dtb;-", &kernel, &dtb) == 0);
    assert(loaded == 2 && initrd == NULL && initrd_size == 0);
    assert(shutdowns == 0);

    /* Only the explicit dash omits the file; bad/missing files still fail. */
    invalid_initrd = true;
    assert(boot_storage_load("esp;image;dtb;initrd", &kernel, &dtb) == -1);
    invalid_initrd = false;
    missing_initrd = true;
    assert(boot_storage_load("esp;image;dtb;initrd", &kernel, &dtb) == -1);
    assert(shutdowns == 2);
    loaded = 0;
    assert(boot_storage_load("esp;image;dtb;", &kernel, &dtb) == -1);
    assert(boot_storage_load("esp;image;dtb;-;extra", &kernel, &dtb) == -1);
    assert(loaded == 0);

    /* A trial boots its files once and is recorded in chosen. */
    missing_initrd = false;
    armed = "image;trial-dtb;initrd";
    assert(boot_storage_load("esp;image;dtb;initrd", &kernel, &dtb) == 0);
    assert(loaded == 3 && dtb == &trial_tree && initrd == archive);
    assert(chosen && !strcmp(chosen, "image;trial-dtb;initrd") && !armed);

    /* Trial files get the configured files' DTB and initramfs checks; failing one, the
     * configured files boot in the same boot. */
    const char *bad[] = { "image;bad-dtb;initrd", "image;trial-dtb;bad-initrd" };
    for (size_t i = 0; i < 2; i++) {
        loaded = 0;
        chosen = NULL;
        armed = bad[i];
        assert(boot_storage_load("esp;image;dtb;initrd", &kernel, &dtb) == 0);
        assert(loaded == 6 && dtb == &tree && !chosen && !armed);
    }

    /* A trial line without three names is left alone. */
    loaded = 0;
    armed = "image;trial-dtb";
    assert(boot_storage_load("esp;image;dtb;-", &kernel, &dtb) == 0);
    assert(loaded == 2 && dtb == &tree && !chosen);
    assert(shutdowns == 2);
    return 0;
}
'''
    binary = tmp_path / "boot-storage-initrd"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-x", "c", "-", "-o", str(binary)],
                   input=harness, text=True, check=True)
    subprocess.run([str(binary)], check=True)
