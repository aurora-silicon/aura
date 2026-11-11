/* SPDX-License-Identifier: MIT */

#ifndef KBOOT_H
#define KBOOT_H

#include "types.h"

// declarations for kboot_t6020_compat.c
int dt_fixup_t6020_compat(void *dt);

struct kernel_header {
    u32 code[2];     /* Executable code */
    u64 text_offset; /* Image load offset, little endian */
    u64 image_size;  /* Effective Image size, little endian */
    u64 flags;       /* kernel flags, little endian */
    u64 res2;        /* reserved */
    u64 res3;        /* reserved */
    u64 res4;        /* reserved */
    u32 magic;       /* Magic number, little endian, "ARM\x64" */
    u32 res5;        /* reserved (used for PE COFF offset) */
};

#define KERNEL_HEADER_MAGIC 0x644d5241

void kboot_set_initrd(void *start, size_t size);
int kboot_set_chosen(const char *name, const char *value);
void kboot_delete_chosen(const char *name);
void kboot_set_config_lost(void);
int kboot_set_uboot(const char *name, const char *value);
int dt_gpu_t8122_26_calibrate(void *dt);
int dt_set_gpu_t8122_26(void *dt);
int kboot_prepare_dt(void *fdt);
int kboot_boot(void *kernel);

/* Boot-args RAM range and boot framebuffer base, for the kboot_* SoC helpers */
u64 kboot_ram_base(void);
u64 kboot_ram_size(void);
u64 kboot_fb_base(void);

/*
 * Where stage 1 loaded the file part of this image (the part start.S copies when it moves the
 * image above the chainloaded one): the old base if it moved, otherwise where it runs.
 */
void kboot_loaded_file_part(u64 *base, u64 *size);

/* The node at an absolute path or alias, matching each node name exactly (see kboot.c) */
int kboot_fdt_path_exact(const void *fdt, const char *path);

// from rust/usb4.rs
u64 rust_usb4_router_uuid(void);
int rust_platform_uuid(u8 *out);

#endif
