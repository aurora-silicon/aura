/* SPDX-License-Identifier: MIT */
/* Host stand-in for src/kboot.h: the boot-args values the display handoff reads. */
#ifndef KBOOT_H
#define KBOOT_H
#include "types.h"
u64 kboot_ram_base(void);
u64 kboot_ram_size(void);
u64 kboot_fb_base(void);
void kboot_loaded_file_part(u64 *base, u64 *size);
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
#endif
