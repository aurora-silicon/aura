/* SPDX-License-Identifier: MIT */
/* Host stand-in for src/pmgr.h: the PMGR table lookups the display handoff uses. */
#ifndef PMGR_H
#define PMGR_H
#include "types.h"
#define PMGR_PS_ACTIVE 0xf
struct pmgr_gate {
    u32 entry;
    u16 id;
    u8 die;
    bool found;
    bool virt;
    const char *name;
    u64 psreg;
    u16 parent[2];
};
int pmgr_adt_gate(const char *path, const char *prop, u32 index, struct pmgr_gate *gate);
int pmgr_device_gate(u8 die, u16 id, struct pmgr_gate *gate);
int pmgr_adt_is_powered(const char *path);
int pmgr_adt_collect_psregs(const char *path, u64 *out, u8 *flags, int max);
#endif
