/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#ifndef PMGR_H
#define PMGR_H

#include "types.h"

#define PMGR_DIE_OFFSET 0x2000000000

#define PMGR_DEVICE_ID GENMASK(15, 0)
#define PMGR_DIE_ID    GENMASK(31, 28)

#define PMGR_PS_ACTIVE  0xf
#define PMGR_PS_CLKGATE 0x4
#define PMGR_PS_PWRGATE 0x0

extern const char *pmgr_name;

int pmgr_init(void);

int pmgr_power_enable(u32 id);
int pmgr_power_disable(u32 id);

int pmgr_adt_path_offset_trace(const void *adt_ptr, int *path);

int pmgr_adt_power_enable(const char *path);
int pmgr_adt_power_disable(const char *path);
int pmgr_adt_power_disable_recursive(const char *path);
/* 1: all real devices in this path's parent closure are power-gated; 0: active; -1: unknown. */
int pmgr_adt_power_is_off(const char *path);
int pmgr_adt_power_enable_traced(const char *path);
struct pmgr_saved_mode {
    uintptr_t addr;
    u8 target;
};

struct pmgr_saved_modes {
    struct pmgr_saved_mode modes[64];
    size_t count;
};

int pmgr_adt_power_enable_traced_saved(const char *path, struct pmgr_saved_modes *saved);
void pmgr_restore_modes(struct pmgr_saved_modes *saved);
int pmgr_adt_power_enable_index(const char *path, u32 index);
int pmgr_adt_power_disable_index(const char *path, u32 index);
int pmgr_adt_reset(const char *path);

/* One PMGR device, as an ADT clock-gates/power-gates entry or a parent names it. Read only. */
struct pmgr_gate {
    u32 entry; /* die and device id, as in the ADT */
    u16 id;
    u8 die;
    bool found;       /* the id is a device of the ADT's PMGR table */
    bool virt;        /* a virtual device: no power state register of its own */
    const char *name; /* up to 16 chars, not always NUL-terminated: print with %.16s */
    u64 psreg;        /* power state register, 0 if virtual or unknown */
    u16 parent[2];    /* parent device ids, 0 for none */
};

int pmgr_adt_gate(const char *path, const char *prop, u32 index, struct pmgr_gate *gate);
int pmgr_device_gate(u8 die, u16 id, struct pmgr_gate *gate);

int pmgr_reset(int die, const char *name);
int pmgr_power_on(int die, const char *name);
int pmgr_power_is_on(int die, const char *name);

int pmgr_set_mode(uintptr_t addr, u8 target_mode);

u32 pmgr_get_feature(const char *name);

int pmgr_set_voltage_ctl(uint32_t offset);

#endif
