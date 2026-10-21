/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#include "pmgr.h"
#include "adt.h"
#include "string.h"
#include "types.h"
#include "utils.h"

#define PMGR_RESET        BIT(31)
#define PMGR_AUTO_ENABLE  BIT(28)
#define PMGR_PS_AUTO      GENMASK(27, 24)
#define PMGR_PARENT_OFF   BIT(11)
#define PMGR_DEV_DISABLE  BIT(10)
#define PMGR_WAS_CLKGATED BIT(9)
#define PMGR_WAS_PWRGATED BIT(8)
#define PMGR_PS_ACTUAL    GENMASK(7, 4)
#define PMGR_PS_TARGET    GENMASK(3, 0)

#define PMGR_POLL_TIMEOUT 10000

#define PMGR_FLAG_VIRTUAL     0x10
#define PMGR_MAX_PARENT_DEPTH 32

struct pmgr_device {
    u8 flags;
    u16 unk1;
    u8 id1;
    union {
        struct {
            u8 parent[2];
            u8 unk2[2];
        } u8id;
        struct {
            u16 parent[2];
        } u16id;
    };
    u8 unk3[2];
    u8 addr_offset;
    u8 psreg_idx;
    u8 unk4[4];
    struct {
        u32 offset : 24;
        u32 group : 8;
    } group_and_offset;
    u8 unk5[6];
    u16 id2;
    u8 unk6[4];
    const char name[0x10];
} PACKED;

static int pmgr_initialized = 0;

static int pmgr_path[8];
static int pmgr_offset;
static int pmgr_dies;
static const char pmgr_name_t8103[] = "/arm-io/pmgr";
static const char pmgr_name_t8152[] = "/arm-io/pmgr-child";
const char *pmgr_name = pmgr_name_t8103;

static const u32 *pmgr_ps_regs = NULL;
static u32 pmgr_ps_regs_len = 0;

static const struct pmgr_device *pmgr_devices = NULL;
static u32 pmgr_devices_len = 0;

static bool pmgr_u8id = false;
static bool pmgr_use_group_and_offset = false;
static bool pmgr_protected_addr(uintptr_t addr);

static bool pmgr_protected_device(const struct pmgr_device *device)
{
    return chip_id == T8140 && (!strncmp(device->name, "DISP_CPU", sizeof(device->name)) ||
                                !strncmp(device->name, "DISPEXT0_CPU", sizeof(device->name)));
}

static uintptr_t pmgr_get_psreg(u8 idx)
{
    if (((u32)idx + 1) * 12 > pmgr_ps_regs_len) {
        printf("pmgr: Index %d is out of bounds for ps-regs\n", idx);
        return 0;
    }

    u32 reg_idx = pmgr_ps_regs[3 * idx];
    u32 reg_offset = pmgr_ps_regs[3 * idx + 1];

    u64 pmgr_reg;
    if (adt_get_reg(adt, pmgr_path, "reg", reg_idx, &pmgr_reg, NULL) < 0) {
        printf("pmgr: Error getting %s regs\n", pmgr_name);
        return 0;
    }

    return pmgr_reg + reg_offset;
}

int pmgr_set_mode(uintptr_t addr, u8 target_mode)
{
    if (!addr)
        return -1;
    if (pmgr_protected_addr(addr)) {
        if (FIELD_GET(PMGR_PS_ACTUAL, read32(addr)) == target_mode)
            return 0;
        printf("pmgr: refusing protected display CPU power transition at 0x%lx\n", addr);
        return -1;
    }
    mask32(addr, PMGR_AUTO_ENABLE | PMGR_WAS_CLKGATED | PMGR_WAS_PWRGATED | PMGR_PS_TARGET,
           FIELD_PREP(PMGR_PS_TARGET, target_mode));
    if (poll32(addr, PMGR_PS_ACTUAL, FIELD_PREP(PMGR_PS_ACTUAL, target_mode), PMGR_POLL_TIMEOUT) <
        0) {
        printf("pmgr: timeout while trying to set mode %x for device at 0x%lx: %x\n", target_mode,
               addr, read32(addr));
        return -1;
    }

    return 0;
}

static u16 pmgr_adt_get_id(const struct pmgr_device *device)
{
    if (pmgr_u8id)
        return device->id1;
    else
        return device->id2;
}

static int pmgr_find_device(u16 id, const struct pmgr_device **device)
{
    for (size_t i = 0; i < pmgr_devices_len; ++i) {
        const struct pmgr_device *i_device = &pmgr_devices[i];
        if (pmgr_adt_get_id(i_device) != id)
            continue;

        *device = i_device;
        return 0;
    }

    return -1;
}

static uintptr_t pmgr_device_get_addr(u8 die, const struct pmgr_device *device)
{
    if (die >= pmgr_dies)
        return 0;
    uintptr_t addr;
    if (pmgr_use_group_and_offset)
        addr = pmgr_get_psreg(device->group_and_offset.group);
    else
        addr = pmgr_get_psreg(device->psreg_idx);
    if (addr == 0)
        return 0;

    addr += PMGR_DIE_OFFSET * die;

    if (pmgr_use_group_and_offset)
        addr += device->group_and_offset.offset;
    else
        addr += (device->addr_offset << 3);

    return addr;
}

static bool pmgr_protected_addr(uintptr_t addr)
{
    if (chip_id != T8140 || !pmgr_devices)
        return false;
    for (size_t i = 0; i < pmgr_devices_len; i++) {
        if (!pmgr_protected_device(&pmgr_devices[i]))
            continue;
        for (u8 die = 0; die < pmgr_dies; die++) {
            if (pmgr_device_get_addr(die, &pmgr_devices[i]) == addr)
                return true;
        }
    }
    return false;
}

static void pmgr_adt_get_parents(const struct pmgr_device *device, u16 parent[2])
{
    if (pmgr_u8id) {
        parent[0] = device->u8id.parent[0];
        parent[1] = device->u8id.parent[1];
    } else {
        parent[0] = device->u16id.parent[0];
        parent[1] = device->u16id.parent[1];
    }
}

static int pmgr_validate_parents(u16 id, u16 *ancestors, size_t depth)
{
    const struct pmgr_device *device;
    u16 parents[2];

    if (depth >= PMGR_MAX_PARENT_DEPTH)
        return -1;
    if (pmgr_find_device(id, &device)) {
        /*
         * Apple's tables name parents they do not list: C0-USBCTL-V's parent 81 on
         * base M1, and ISP_CPU_CORE0/1's parent 176 on base M3. As in upstream m1n1, a
         * missing parent only fails a request that has to power it, not the whole table.
         */
        if (!depth)
            return -1;
        printf("pmgr: ignoring parent %u, which the ADT does not list\n", id);
        return 0;
    }
    for (size_t i = 0; i < depth; i++) {
        if (ancestors[i] == id)
            return -1;
    }
    ancestors[depth] = id;
    pmgr_adt_get_parents(device, parents);
    for (size_t i = 0; i < ARRAY_SIZE(parents); i++) {
        if (parents[i] && pmgr_validate_parents(parents[i], ancestors, depth + 1))
            return -1;
    }
    return 0;
}

static int pmgr_validate_topology(void)
{
    for (size_t i = 0; i < pmgr_devices_len; i++) {
        const struct pmgr_device *device = &pmgr_devices[i];
        u16 id = pmgr_adt_get_id(device);
        u16 ancestors[PMGR_MAX_PARENT_DEPTH];

        if (!id || pmgr_validate_parents(id, ancestors, 0))
            return -1;
        for (size_t j = 0; j < i; j++) {
            if (pmgr_adt_get_id(&pmgr_devices[j]) == id)
                return -1;
        }
        if (!(device->flags & PMGR_FLAG_VIRTUAL)) {
            for (u8 die = 0; die < pmgr_dies; die++) {
                if (!pmgr_device_get_addr(die, device))
                    return -1;
            }
        }
    }
    return 0;
}

static int pmgr_set_mode_traced(u8 die, u16 id, const struct pmgr_device *device, u8 mode,
                                bool trace)
{
    uintptr_t addr = pmgr_device_get_addr(die, device);
    if (!addr)
        return -1;

    int ret = pmgr_set_mode(addr, mode);
    if (trace)
        printf("pmgr: %d.%s (%u) mode %x at 0x%lx: 0x%x%s\n", die, device->name, id, mode, addr,
               read32(addr), ret ? " failed" : "");
    return ret;
}

static int pmgr_set_mode_recursive_inner(u8 die, u16 id, u8 target_mode, bool recurse, bool trace,
                                         unsigned int depth)
{
    if (!pmgr_initialized) {
        printf("pmgr: pmgr_set_mode_recursive() called before successful pmgr_init()\n");
        return -1;
    }

    if (id == 0 || die >= pmgr_dies || depth >= PMGR_MAX_PARENT_DEPTH)
        return -1;

    const struct pmgr_device *device;

    if (pmgr_find_device(id, &device))
        return -1;

    if (target_mode == 0 && !(device->flags & PMGR_FLAG_VIRTUAL)) {
        if (pmgr_set_mode_traced(die, id, device, target_mode, trace))
            return -1;
    }

    if (recurse)
        for (int i = 0; i < 2; i++) {
            u16 parents[2];
            pmgr_adt_get_parents(device, parents);
            if (parents[i]) {
                int ret = pmgr_set_mode_recursive_inner(die, parents[i], target_mode, true, trace,
                                                        depth + 1);
                if (ret < 0)
                    return ret;
            }
        }

    if (target_mode != 0 && !(device->flags & PMGR_FLAG_VIRTUAL)) {
        if (pmgr_set_mode_traced(die, id, device, target_mode, trace))
            return -1;
    }

    return 0;
}

static int pmgr_set_mode_recursive(u8 die, u16 id, u8 target_mode, bool recurse, bool trace)
{
    return pmgr_set_mode_recursive_inner(die, id, target_mode, recurse, trace, 0);
}

static int pmgr_device_is_off_recursive(u8 die, u16 id, unsigned int depth)
{
    const struct pmgr_device *device;

    if (!pmgr_initialized || !id || depth > 16 || pmgr_find_device(id, &device))
        return -1;
    if (!(device->flags & PMGR_FLAG_VIRTUAL)) {
        uintptr_t addr = pmgr_device_get_addr(die, device);
        if (!addr)
            return -1;
        if (FIELD_GET(PMGR_PS_ACTUAL, read32(addr)) != PMGR_PS_PWRGATE)
            return 0;
    }
    u16 parents[2];
    pmgr_adt_get_parents(device, parents);
    for (int i = 0; i < 2; i++) {
        if (!parents[i])
            continue;
        int ret = pmgr_device_is_off_recursive(die, parents[i], depth + 1);
        if (ret != 1)
            return ret;
    }
    return 1;
}

int pmgr_power_enable(u32 id)
{
    u16 device = FIELD_GET(PMGR_DEVICE_ID, id);
    u8 die = FIELD_GET(PMGR_DIE_ID, id);
    return pmgr_set_mode_recursive(die, device, PMGR_PS_ACTIVE, true, false);
}

int pmgr_power_disable(u32 id)
{
    u16 device = FIELD_GET(PMGR_DEVICE_ID, id);
    u8 die = FIELD_GET(PMGR_DIE_ID, id);
    return pmgr_set_mode_recursive(die, device, PMGR_PS_PWRGATE, false, false);
}

static int pmgr_adt_find_devices(const char *path, const u32 **devices, u32 *n_devices)
{
    int node_offset = adt_path_offset(adt, path);
    if (node_offset < 0) {
        printf("pmgr: Error getting node %s\n", path);
        return -1;
    }

    *devices = adt_getprop(adt, node_offset, "clock-gates", n_devices);
    if (*devices == NULL || *n_devices == 0 || (*n_devices & 3)) {
        printf("pmgr: Error getting %s clock-gates.\n", path);
        return -1;
    }

    *n_devices /= 4;

    return 0;
}

static int pmgr_save_parents(u8 die, u16 id, struct pmgr_saved_modes *saved, size_t depth)
{
    const struct pmgr_device *device;
    if (depth >= PMGR_MAX_PARENT_DEPTH || pmgr_find_device(id, &device))
        return -1;
    u16 parents[2];
    pmgr_adt_get_parents(device, parents);
    for (size_t i = 0; i < ARRAY_SIZE(parents); i++)
        if (parents[i] && pmgr_save_parents(die, parents[i], saved, depth + 1))
            return -1;
    if (device->flags & PMGR_FLAG_VIRTUAL)
        return 0;
    uintptr_t addr = pmgr_device_get_addr(die, device);
    if (!addr)
        return -1;
    for (size_t i = 0; i < saved->count; i++)
        if (saved->modes[i].addr == addr)
            return 0;
    if (saved->count == ARRAY_SIZE(saved->modes))
        return -1;
    u32 reg = read32(addr);
    saved->modes[saved->count++] = (struct pmgr_saved_mode){
        .addr = addr,
        .target = FIELD_GET(PMGR_PS_TARGET, reg),
    };
    return 0;
}

void pmgr_restore_modes(struct pmgr_saved_modes *saved)
{
    while (saved->count) {
        struct pmgr_saved_mode *entry = &saved->modes[--saved->count];
        u32 reg = read32(entry->addr);
        // The saved target is the firmware's requested steady state. Its actual
        // mode may still have been moving when the snapshot was taken.
        if ((FIELD_GET(PMGR_PS_ACTUAL, reg) != entry->target ||
             FIELD_GET(PMGR_PS_TARGET, reg) != entry->target) &&
            pmgr_set_mode(entry->addr, entry->target))
            printf("pmgr: failed to restore 0x%lx after ISP error\n", entry->addr);
    }
}

int pmgr_adt_power_enable_traced_saved(const char *path, struct pmgr_saved_modes *saved)
{
    const u32 *devices;
    u32 n_devices;
    if (pmgr_adt_find_devices(path, &devices, &n_devices))
        return -1;
    saved->count = 0;
    for (u32 i = 0; i < n_devices; i++) {
        u16 id = FIELD_GET(PMGR_DEVICE_ID, devices[i]);
        u8 die = FIELD_GET(PMGR_DIE_ID, devices[i]);
        if (pmgr_save_parents(die, id, saved, 0))
            return -1;
    }
    if (!pmgr_adt_power_enable_traced(path))
        return 0;
    pmgr_restore_modes(saved);
    return -1;
}

static int pmgr_adt_devices_set_mode(const char *path, u8 target_mode, int recurse, bool trace)
{
    const u32 *devices;
    u32 n_devices;
    int ret = 0;

    if (pmgr_adt_find_devices(path, &devices, &n_devices) < 0)
        return -1;

    for (u32 i = 0; i < n_devices; ++i) {
        u16 device = FIELD_GET(PMGR_DEVICE_ID, devices[i]);
        u8 die = FIELD_GET(PMGR_DIE_ID, devices[i]);
        if (pmgr_set_mode_recursive(die, device, target_mode, recurse, trace))
            ret = -1;
    }

    return ret;
}

static int pmgr_adt_device_set_mode(const char *path, u32 index, u8 target_mode, int recurse)
{
    const u32 *devices;
    u32 n_devices;
    int ret = 0;

    if (pmgr_adt_find_devices(path, &devices, &n_devices) < 0)
        return -1;

    if (index >= n_devices)
        return -1;

    u16 device = FIELD_GET(PMGR_DEVICE_ID, devices[index]);
    u8 die = FIELD_GET(PMGR_DIE_ID, devices[index]);
    if (pmgr_set_mode_recursive(die, device, target_mode, recurse, false))
        ret = -1;

    return ret;
}

int pmgr_adt_power_enable(const char *path)
{
    int ret = pmgr_adt_devices_set_mode(path, PMGR_PS_ACTIVE, true, false);
    return ret;
}

int pmgr_adt_power_disable(const char *path)
{
    return pmgr_adt_devices_set_mode(path, PMGR_PS_PWRGATE, false, false);
}

int pmgr_adt_power_disable_recursive(const char *path)
{
    return pmgr_adt_devices_set_mode(path, PMGR_PS_PWRGATE, true, false);
}

int pmgr_adt_power_is_off(const char *path)
{
    const u32 *devices;
    u32 n_devices;

    if (pmgr_adt_find_devices(path, &devices, &n_devices) < 0)
        return -1;
    for (u32 i = 0; i < n_devices; i++) {
        u16 device = FIELD_GET(PMGR_DEVICE_ID, devices[i]);
        u8 die = FIELD_GET(PMGR_DIE_ID, devices[i]);
        int ret = pmgr_device_is_off_recursive(die, device, 0);
        if (ret != 1)
            return ret;
    }
    return 1;
}

int pmgr_adt_power_enable_traced(const char *path)
{
    return pmgr_adt_devices_set_mode(path, PMGR_PS_ACTIVE, true, true);
}

int pmgr_adt_power_enable_index(const char *path, u32 index)
{
    int ret = pmgr_adt_device_set_mode(path, index, PMGR_PS_ACTIVE, true);
    return ret;
}

int pmgr_adt_power_disable_index(const char *path, u32 index)
{
    return pmgr_adt_device_set_mode(path, index, PMGR_PS_PWRGATE, false);
}

static void pmgr_gate_fill(u8 die, u16 id, struct pmgr_gate *gate)
{
    const struct pmgr_device *device;

    gate->id = id;
    gate->die = die;
    gate->found = id && !pmgr_find_device(id, &device);
    gate->virt = false;
    gate->name = NULL;
    gate->psreg = 0;
    gate->parent[0] = gate->parent[1] = 0;
    if (!gate->found)
        return;
    gate->name = device->name;
    gate->virt = device->flags & PMGR_FLAG_VIRTUAL;
    pmgr_adt_get_parents(device, gate->parent);
    if (!gate->virt)
        gate->psreg = pmgr_device_get_addr(die, device);
}

/*
 * Read only, ADT only: entry `index` of the clock-gates or power-gates list `prop` of the ADT node
 * at `path`, resolved through the ADT's PMGR table. Returns -1 past the last entry, without that
 * list, or before pmgr_init(); 0 otherwise, also for an entry that is not a PMGR device.
 */
int pmgr_adt_gate(const char *path, const char *prop, u32 index, struct pmgr_gate *gate)
{
    u32 len = 0, entry;

    if (!pmgr_initialized)
        return -1;
    int node = adt_path_offset(adt, path);
    if (node < 0)
        return -1;
    const u32 *entries = adt_getprop(adt, node, prop, &len);
    if (!entries || index >= len / 4)
        return -1;
    memcpy(&entry, &entries[index], sizeof(entry));
    gate->entry = entry;
    pmgr_gate_fill(FIELD_GET(PMGR_DIE_ID, entry), FIELD_GET(PMGR_DEVICE_ID, entry), gate);
    return 0;
}

/* Read only, ADT only: PMGR device `id` on `die`, such as a parent of another one */
int pmgr_device_gate(u8 die, u16 id, struct pmgr_gate *gate)
{
    if (!pmgr_initialized || !id)
        return -1;
    gate->entry = FIELD_PREP(PMGR_DIE_ID, die) | id;
    pmgr_gate_fill(die, id, gate);
    return 0;
}

static int pmgr_reset_device(int die, const struct pmgr_device *dev)
{
    if (pmgr_protected_device(dev))
        return -1;
    if (die < 0 || die >= pmgr_dies) {
        printf("pmgr: invalid die id %d for device %s\n", die, dev->name);
        return -1;
    }

    uintptr_t addr = pmgr_device_get_addr(die, dev);
    if (!addr)
        return -1;

    u32 reg = read32(addr);
    if (FIELD_GET(PMGR_PS_ACTUAL, reg) != PMGR_PS_ACTIVE) {
        printf("pmgr: will not reset disabled device %d.%s\n", die, dev->name);
        return -1;
    }

    printf("pmgr: resetting device %d.%s\n", die, dev->name);

    set32(addr, PMGR_DEV_DISABLE);
    set32(addr, PMGR_RESET);
    udelay(10);
    clear32(addr, PMGR_RESET);
    clear32(addr, PMGR_DEV_DISABLE);

    return 0;
}

int pmgr_adt_reset(const char *path)
{
    const u32 *devices;
    u32 n_devices;
    int ret = 0;

    if (pmgr_adt_find_devices(path, &devices, &n_devices) < 0)
        return -1;

    for (u32 i = 0; i < n_devices; ++i) {
        const struct pmgr_device *device;
        u16 id = FIELD_GET(PMGR_DEVICE_ID, devices[i]);
        u8 die = FIELD_GET(PMGR_DIE_ID, devices[i]);

        if (pmgr_find_device(id, &device)) {
            ret = -1;
            continue;
        }

        if (pmgr_reset_device(die, device))
            ret = -1;
    }

    return ret;
}

int pmgr_reset(int die, const char *name)
{
    const struct pmgr_device *dev = NULL;

    for (unsigned int i = 0; i < pmgr_devices_len; ++i) {
        if (strncmp(pmgr_devices[i].name, name, 0x10) == 0) {
            dev = &pmgr_devices[i];
            break;
        }
    }

    if (!dev)
        return -1;

    return pmgr_reset_device(die, dev);
}

int pmgr_power_on(int die, const char *name)
{
    if (die < 0 || die >= pmgr_dies)
        return -1;
    const struct pmgr_device *dev = NULL;

    for (unsigned int i = 0; i < pmgr_devices_len; ++i) {
        if (strncmp(pmgr_devices[i].name, name, 0x10) == 0) {
            dev = &pmgr_devices[i];
            break;
        }
    }

    if (!dev)
        return -1;

    uintptr_t addr = pmgr_device_get_addr(die, dev);

    if (!addr)
        return -1;

    return pmgr_set_mode(addr, PMGR_PS_ACTIVE);
}

int pmgr_power_is_on(int die, const char *name)
{
    if (die < 0 || die >= pmgr_dies)
        return -1;
    for (size_t i = 0; i < pmgr_devices_len; i++) {
        const struct pmgr_device *dev = &pmgr_devices[i];
        if (strncmp(dev->name, name, sizeof(dev->name)))
            continue;
        uintptr_t addr = pmgr_device_get_addr(die, dev);
        if (!addr)
            return -1;
        return FIELD_GET(PMGR_PS_ACTUAL, read32(addr)) == PMGR_PS_ACTIVE;
    }
    return -1;
}

int pmgr_adt_path_offset_trace(const void *adt_ptr, int *path)
{
    int offset = adt_path_offset_trace(adt_ptr, pmgr_name, path);
    if (offset < 0)
        return offset;

    /* M6 and hopefully later SoC moved the used pmgr functionality to "/arm-io/pmgr-child"
     * and have a generic looking "pmgr2,arch" as compatible in "/arm-io/pmgr".
     */
    if (!adt_is_compatible(adt, offset, "pmgr2,arch"))
        return offset;
    pmgr_name = pmgr_name_t8152;

    return adt_path_offset_trace(adt, pmgr_name, path);
}

int pmgr_init(void)
{
    pmgr_initialized = 0;
    pmgr_name = pmgr_name_t8103;
    pmgr_ps_regs = NULL;
    pmgr_ps_regs_len = 0;
    pmgr_devices = NULL;
    pmgr_devices_len = 0;
    pmgr_u8id = false;
    pmgr_use_group_and_offset = false;

    int node = adt_path_offset(adt, "/arm-io");
    if (node < 0) {
        printf("pmgr: Error getting /arm-io node\n");
        return -1;
    }
    if (ADT_GETPROP(adt, node, "die-count", &pmgr_dies) < 0)
        pmgr_dies = 1;
    if (pmgr_dies < 1 || pmgr_dies > 16)
        return -1;

    pmgr_offset = pmgr_adt_path_offset_trace(adt, pmgr_path);
    if (pmgr_offset < 0) {
        printf("pmgr: Error getting %s node\n", pmgr_name);
        return -1;
    }
    pmgr_ps_regs = adt_getprop(adt, pmgr_offset, "ps-regs", &pmgr_ps_regs_len);
    if (pmgr_ps_regs == NULL || pmgr_ps_regs_len == 0) {
        pmgr_use_group_and_offset = true;
        pmgr_ps_regs = adt_getprop(adt, pmgr_offset, "ps-groups", &pmgr_ps_regs_len);
        if (pmgr_ps_regs == NULL || pmgr_ps_regs_len == 0) {
            printf("pmgr: Error getting %s ps-regs\n.", pmgr_name);
            return -1;
        }
    }
    if (pmgr_ps_regs_len % 12)
        return -1;

    pmgr_devices = adt_getprop(adt, pmgr_offset, "devices", &pmgr_devices_len);
    if (pmgr_devices == NULL || pmgr_devices_len == 0) {
        printf("pmgr: Error getting %s devices.\n", pmgr_name);
        return -1;
    }

    if (pmgr_devices_len % sizeof(*pmgr_devices))
        return -1;

    pmgr_devices_len /= sizeof(*pmgr_devices);

    // detect whether u8 or u16 PMGR IDs are used by comparing the IDs of the
    // first 2 devices
    if (pmgr_devices_len >= 2)
        pmgr_u8id = pmgr_devices[0].id1 != pmgr_devices[1].id1;

    if (pmgr_validate_topology()) {
        printf("pmgr: Invalid ADT power-state topology\n");
        return -1;
    }

    printf("pmgr: Cleaning up device states...\n");

    for (u8 die = 0; die < pmgr_dies; ++die) {
        for (size_t i = 0; i < pmgr_devices_len; ++i) {
            const struct pmgr_device *device = &pmgr_devices[i];

            if ((device->flags & PMGR_FLAG_VIRTUAL))
                continue;

            if (pmgr_protected_device(device))
                continue;

            uintptr_t addr = pmgr_device_get_addr(die, device);
            if (!addr)
                return -1;

            u32 reg = read32(addr);

            if (reg & PMGR_AUTO_ENABLE || FIELD_GET(PMGR_PS_TARGET, reg) == PMGR_PS_ACTIVE) {
                for (int j = 0; j < 2; j++) {
                    u16 parent[2];
                    pmgr_adt_get_parents(device, parent);
                    if (parent[j]) {
                        const struct pmgr_device *pdevice;
                        if (pmgr_find_device(parent[j], &pdevice)) {
                            printf("pmgr: Failed to find parent #%d for %s\n", parent[j],
                                   device->name);
                            return -1;
                        }

                        if ((pdevice->flags & PMGR_FLAG_VIRTUAL))
                            continue;

                        if (pmgr_protected_device(pdevice))
                            continue;

                        addr = pmgr_device_get_addr(die, pdevice);
                        if (!addr)
                            return -1;

                        reg = read32(addr);

                        if (!(reg & PMGR_AUTO_ENABLE) &&
                            FIELD_GET(PMGR_PS_TARGET, reg) != PMGR_PS_ACTIVE) {
                            printf("pmgr: Enabling %d.%s, parent of active device %s\n", die,
                                   pdevice->name, device->name);
                            if (pmgr_set_mode(addr, PMGR_PS_ACTIVE))
                                return -1;
                        }
                    }
                }
            }
        }
    }

    pmgr_initialized = 1;
    printf("pmgr: initialized, %d devices on %u dies found.\n", pmgr_devices_len, pmgr_dies);

    return 0;
}

u32 pmgr_get_feature(const char *name)
{
    u32 val = 0;

    int node = adt_path_offset(adt, "/arm-io/pmgr");
    if (node < 0)
        return 0;

    if (ADT_GETPROP(adt, node, name, &val) < 0)
        return 0;

    return val;
}

int pmgr_set_voltage_ctl(uint32_t offset)
{
    u64 pmgr_reg;

    if (!pmgr_initialized) {
        printf("pmgr: pmgr_set_voltage_ctl() called before successful pmgr_init()\n");
        return -1;
    }

    if (adt_get_reg(adt, pmgr_path, "reg", 0, &pmgr_reg, NULL) < 0) {
        printf("Error getting /arm-io/pmgr regs\n");
        return -1;
    }

    write32(pmgr_reg + offset, 1);

    return 0;
}
