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
void kboot_request_hibernate_layout(bool on);
int kboot_set_chosen(const char *name, const char *value);
void kboot_delete_chosen(const char *name);
void kboot_set_config_lost(void);
int kboot_set_uboot(const char *name, const char *value);
int dt_gpu_t8122_26_calibrate(void *dt);
#define T8122_27_VALUES 2
int dt_gpu_t8122_27_machine_values_read(u32 val[T8122_27_VALUES]);
struct t8122_27_value {
    const char *path, *adt, *dt;
};
extern const struct t8122_27_value t8122_27_values[T8122_27_VALUES];
void t8122_read_powered_identity(void *dt, int gpu);
int dt_set_gpu_t8122_26(void *dt);

/*
 * GPU firmware segments for the T8132, T6040 and T6041 handoffs (kboot_gpu.c): /arm-io/gfx-asc
 * segment-ranges __TEXT and __DATA, and the __DATA_SHARED_RO right after __DATA when the ADT lists
 * a third segment that /arm-io/sgx gfx-data-shared-ro-{base,size} describes. Chosen by that ADT
 * shape, not by the chip or the firmware release; any other shape is refused (nonzero return).
 */
struct adt_segment_ranges;
struct gpu_fw_layout {
    const struct adt_segment_ranges *seg; /* segment-ranges: count entries */
    u32 count;                            /* 2, or 3 with __DATA_SHARED_RO */
    u64 text_base, text_size;
    u64 data_base, data_size;
    u64 ro_size; /* __DATA_SHARED_RO right after __DATA, or 0 */
};
int gpu_fw_layout(int sgx, struct gpu_fw_layout *fw);
int kboot_prepare_dt(void *fdt);
int kboot_boot(void *kernel);
/* Reserve the DMA a USB controller that could not be stopped keeps (main.c, after USB shutdown). */
int kboot_reserve_usb_dma(void);

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

/*
 * What kboot.c's M4 internal DCP handoff did, for the external display processors' handoff
 * (kboot_display_handoff.c), which acts only beside a complete one or on a board with no panel.
 */
enum dcp_internal {
    DCP_INTERNAL_NONE,     /* refused, or published without its display maps or firmware clocks */
    DCP_INTERNAL_COMPLETE, /* published whole: the inherited display maps and, on firmware 27, the
                              firmware clocks */
    DCP_INTERNAL_NO_PANEL, /* not attempted: ADT /arm-io/disp0 is external (no panel) */
};
int dt_set_dcpext_handoff(void *dt, enum dcp_internal internal, bool fw_clocks);

// from rust/usb4.rs
u64 rust_usb4_router_uuid(void);
int rust_platform_uuid(u8 *out);

#endif
