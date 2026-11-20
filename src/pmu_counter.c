/* SPDX-License-Identifier: MIT */

#include "pmu_counter.h"
#include "adt.h"
#include "soc.h"
#include "spmi.h"
#include "string.h"
#include "utils.h"

/*
 * iBoot keeps a record of each boot in the primary PMU's legacy scratchpad (info-leg_scrpad): the
 * boot stage reached at +1, the failed-boot and panic counts at +2 (low and high nibble) and the
 * stage a failed boot had reached at +3. It counts a boot that ended without a clean shutdown and
 * drops to recoveryOS once the count is too high. macOS and Linux clear the counts once they run.
 */
#define PMU_SCRPAD_COUNTS 2

/* Find the primary PMU: its SPMI bus path, its SPMI address and the scratchpad offset. */
static int pmu_find_scratchpad(char *path, size_t path_len, u8 *addr, u16 *scrpad)
{
    int arm_io = adt_path_offset(adt, "/arm-io");
    if (arm_io < 0)
        return -1;

    int bnode = arm_io;
    ADT_FOREACH_CHILD(adt, bnode)
    {
        const char *bus = adt_get_name(adt, bnode);
        if (strncmp(bus, "nub-spmi", 8) && strncmp(bus, "spmi", 4))
            continue;

        int node = bnode;
        ADT_FOREACH_CHILD(adt, node)
        {
            u32 primary = 0, base, reg_len = 0;
            if (!adt_is_compatible(adt, node, "pmu,spmi") ||
                ADT_GETPROP(adt, node, "is-primary", &primary) < 0 || primary != 1)
                continue;
            /* reg is several words; only the first (the SPMI address) is used. */
            const u32 *reg = adt_getprop(adt, node, "reg", &reg_len);
            if (ADT_GETPROP(adt, node, "info-leg_scrpad", &base) < 0 || !reg || reg_len < 4 ||
                base > 0xffff - 3) {
                printf("pmu: %s/%s lacks leg_scrpad or reg\n", bus, adt_get_name(adt, node));
                return -1;
            }

            snprintf(path, path_len, "/arm-io/%s", bus);
            *addr = reg[0] & 0xff;
            *scrpad = base;
            return 0;
        }
    }
    printf("pmu: no primary SPMI PMU found\n");
    return -1;
}

/*
 * An aura chain that never reaches an OS which clears the counts must clear them itself, or
 * repeated test boots end in the recovery error screen.
 */
int pmu_clear_panic_counter(void)
{
    char path[64];
    u8 addr;
    u16 scrpad;
    if (pmu_find_scratchpad(path, sizeof(path), &addr, &scrpad))
        return -1;

    spmi_dev_t *spmi = spmi_init(path);
    if (!spmi)
        return -1;

    u16 counter = scrpad + PMU_SCRPAD_COUNTS;
    u8 val = 0, zero = 0;
    int ret = spmi_ext_read_long(spmi, addr, counter, &val, 1);
    if (!ret && val)
        ret = spmi_ext_write_long(spmi, addr, counter, &zero, 1);
    spmi_shutdown(spmi);
    if (ret) {
        printf("pmu: failed to access the panic counter at %s:%02x reg %x\n", path, addr, counter);
        return -1;
    }
    printf("pmu: panic counter at %s:%02x reg %x was %u%s\n", path, addr, counter, val,
           val ? ", cleared" : "");
    return 0;
}

/* Read iBoot's record of the previous boot: the counts byte and the failed boot's stage. */
int pmu_read_boot_record(u8 *counts, u8 *error_stage)
{
    char path[64];
    u8 addr;
    u16 scrpad;
    if (pmu_find_scratchpad(path, sizeof(path), &addr, &scrpad))
        return -1;

    spmi_dev_t *spmi = spmi_init(path);
    if (!spmi)
        return -1;

    u8 record[2] = {0};
    int ret = spmi_ext_read_long(spmi, addr, scrpad + PMU_SCRPAD_COUNTS, record, sizeof(record));
    spmi_shutdown(spmi);
    if (ret) {
        printf("pmu: failed to read the boot record at %s:%02x reg %x\n", path, addr,
               scrpad + PMU_SCRPAD_COUNTS);
        return -1;
    }
    *counts = record[0];
    *error_stage = record[1];
    return 0;
}

/*
 * T6040, T8140 and T6050 clear the count in every aura stage (pmu_clear_panic_counter()), so a
 * later stage never sees it: there it can't bound anything.
 */
bool pmu_counter_cleared_by_aura(void)
{
    return chip_id == T6040 || chip_id == T8140 || chip_id == T6050;
}

/*
 * Count this boot as failed now, before a reboot that iBoot may or may not count itself: the next
 * boot then sees at least one more failed boot. The panic count is kept.
 */
int pmu_count_failed_boot(u8 *before)
{
    char path[64];
    u8 addr;
    u16 scrpad;
    if (pmu_find_scratchpad(path, sizeof(path), &addr, &scrpad))
        return -1;

    spmi_dev_t *spmi = spmi_init(path);
    if (!spmi)
        return -1;

    u16 counter = scrpad + PMU_SCRPAD_COUNTS;
    u8 val = 0;
    int ret = spmi_ext_read_long(spmi, addr, counter, &val, 1);
    if (!ret) {
        u8 next = pmu_counts_add_failed_boot(val);
        if (next != val)
            ret = spmi_ext_write_long(spmi, addr, counter, &next, 1);
    }
    spmi_shutdown(spmi);
    if (ret) {
        printf("pmu: failed to count the failed boot at %s:%02x reg %x\n", path, addr, counter);
        return -1;
    }
    *before = val;
    return 0;
}
