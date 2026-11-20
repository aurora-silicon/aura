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
 * Every aura stage clears iBoot's count (main.c), so only the first one sees it. Each stage reads
 * it before its clear and hands on the larger of that and what the stage before it handed on:
 * a later stage then knows how many boots in a row have failed. A stage that got no count from the
 * stage before it can't tell a first failure from a count an older Stage 1 cleared.
 */
static bool own_valid, passed_valid;
static u8 own_failed, passed_failed;

void pmu_snapshot_failed_boots(void)
{
    u8 counts, error_stage;
    own_valid = !pmu_read_boot_record(&counts, &error_stage);
    own_failed = own_valid ? PMU_FAILED_BOOTS(counts) : 0;
}

/* aura.failed-boots=<count>, decimal, 0 to 15 */
void pmu_take_failed_boots_var(const char *val)
{
    unsigned count = 0;
    const char *p = val;
    for (; *p >= '0' && *p <= '9' && count <= 15; p++)
        count = count * 10 + (*p - '0');
    if (p == val || *p || count > 15) {
        printf("pmu: ignoring aura.failed-boots=%s\n", val);
        return;
    }
    passed_valid = true;
    passed_failed = count;
}

/* Only a count handed on by an earlier stage: this stage's own read follows its clear. */
bool pmu_failed_boots_passed(u8 *count)
{
    /*
     * LAB ONLY (J713 default, never ship): this Air's enrolled Stage 1 (3d517e63) hands no count
     * on and doesn't clear iBoot's count on T8132, so this stage's own read before its clear is
     * the real count.
     */
    if (!passed_valid && own_valid && chip_id == T8132) {
        *count = own_failed;
        return true;
    }
    if (!passed_valid)
        return false;
    *count = passed_failed > own_failed ? passed_failed : own_failed;
    return true;
}

const char *pmu_failed_boots_var(void)
{
    static char var[sizeof("aura.failed-boots=255")];
    if (!own_valid && !passed_valid)
        return NULL;
    u8 count = passed_failed > own_failed ? passed_failed : own_failed;
    snprintf(var, sizeof(var), "aura.failed-boots=%u", count);
    return var;
}
