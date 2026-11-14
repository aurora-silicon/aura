/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#ifndef TUNABLES_H
#define TUNABLES_H

#include "types.h"

/*
 * This function applies the tunables usually passed in the node "tunable".
 * They usually apply to multiple entries from the "reg" node.
 *
 * Example usage for the USB DRD node:
 *  tunables_apply_global("/arm-io/usb-drd0", "tunable");
 */
int tunables_apply_global(const char *path, const char *prop);

/*
 * This function applies the tunables specified in device-specific tunable properties.
 * These only apply to a single MMIO region from the "reg" node which needs to
 * be specified.
 *
 * Example usage for two tunables from the USB DRD DART node:
 *  tunables_apply_local("/arm-io/dart-usb0", "dart-tunables-instance-0", 0);
 *  tunables_apply_local("/arm-io/dart-usb0", "dart-tunables-instance-1", 1);
 *
 */
int tunables_apply_local(const char *path, const char *prop, u32 reg_idx);

/*
 * This functions does the same as tunables_apply_local except that it allows
 * to specify the base address to which the tunables will be applied to instead
 * of extracting it from the "regs" property.
 *
 * Example usage for two tunables for the USB DRD DART node:
 *  tunables_apply_local_addr("/arm-io/dart-usb0", "dart-tunables-instance-0", 0x382f00000);
 *  tunables_apply_local_addr("/arm-io/dart-usb0", "dart-tunables-instance-1", 0x382f80000);
 */
int tunables_apply_local_addr(const char *path, const char *prop, uintptr_t base);

/* Validate local ADT records against a register window before any MMIO writes.
 * Returns 1 if the property is absent, 0 if valid, and -1 if malformed. */
int tunables_validate_local(const char *path, const char *prop, u64 span);

/* A bit that reads back 0 after a write (a strobe or write-only bit), measured per record */
struct tunable_excuse {
    u32 offset;
    u64 bits;
};

/* Read back every local record of `prop` at `base`: its masked bits must hold the written value,
 * less the `excuses` bits of that record (a list ending with bits 0, or NULL). Returns the number
 * of records that differ, -1 without the property or for a record size it cannot read. */
int tunables_check_local_addr(const char *path, const char *prop, uintptr_t base,
                              const struct tunable_excuse *excuses);

/* Apply 12-byte ATC ADT records within a validated MMIO window. */
int tunables_apply_compact_addr(const char *path, const char *prop, uintptr_t base, u64 span);
int tunables_validate_compact(const char *path, const char *prop, u64 span);

int tunables_apply_static(void);

#endif
