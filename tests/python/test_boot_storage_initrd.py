# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the production ESP loader with and without an OS initramfs."""

from pathlib import Path
import subprocess


def test_optional_initramfs(tmp_path):
    source = (Path(__file__).resolve().parents[2] / "src/boot_storage.c").read_text()
    loader = source[source.index("int boot_storage_load("):]
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
static char tree, archive[] = "070701";
static int loaded, shutdowns;
static bool invalid_initrd, missing_initrd;
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
    else if (!strcmp(spec, "esp;initrd") && !missing_initrd) {
        *data = archive; *size = invalid_initrd ? 1 : sizeof(archive);
    } else return -1;
    return 0;
}
static void rust_free_image(void *data, size_t size) { (void)data; (void)size; }
/* secboot.c: not enforcing, ESP files only measured */
enum secboot_verdict { SECBOOT_UNSIGNED = 5 };
static bool secboot_allow_unverified(const char *what) { (void)what; return true; }
static void secboot_measure(const char *c, const void *d, size_t s, enum secboot_verdict v)
{
    (void)c; (void)d; (void)s; (void)v;
}
static int boot_check_dtb(const void *data, size_t size)
{ return data == &tree && size == 1 ? 0 : -1; }
static struct kernel_header *boot_prepare_image(const void *data, size_t size)
{ return data == &image && size == sizeof(image) ? &image : NULL; }
static void kboot_set_initrd(void *data, size_t size)
{ initrd = data; initrd_size = size; }
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
    return 0;
}
'''
    binary = tmp_path / "boot-storage-initrd"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-x", "c", "-", "-o", str(binary)],
                   input=harness, text=True, check=True)
    subprocess.run([str(binary)], check=True)
