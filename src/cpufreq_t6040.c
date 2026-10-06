/* SPDX-License-Identifier: MIT */

#include "adt.h"
#include "cpufreq.h"
#include "soc.h"
#include "utils.h"

/* J616s 26A428: CPM command apertures from the live ADT. The active P
 * cluster's 1 -> 2 -> 1 transition was verified by command readback and
 * a fixed-work benchmark. Keep firmware voltage/PLL/throttling ownership.
 * Higher states require thermal-policy qualification and remain disabled. */
#define T6040_CLOCK_COUNT    3
#define T6040_MAX_TEST_STATE 2
#define T6040_CMD_BUSY       BIT(31)
#define T6040_CMD_SET        BIT(25)

struct t6040_clock {
    u64 reg;
    const u32 *opps;
    u32 count;
    u32 expected;
    bool failed;
};

static struct t6040_clock clocks[T6040_CLOCK_COUNT];
static bool ready;

int cpufreq_t6040_init(void)
{
    if (ready)
        return 0;
    int path[8];
    int node = adt_path_offset_trace(adt, "/arm-io/pmgr", path);
    if (chip_id != T6040 || board_id != 6 || !adt_is_compatible(adt, 0, "J616sAP") || node < 0 ||
        !adt_is_compatible(adt, node, "pmgr1,t6041"))
        return -1;
    u32 reg_len;
    if (!adt_getprop(adt, node, "reg", &reg_len) || reg_len % 16)
        return -1;
    const char *tables[] = {"voltage-states1-sram", "voltage-states5-sram",
                            "voltage-states13-sram"};
    for (unsigned int c = 0; c < T6040_CLOCK_COUNT; c++) {
        u64 wanted = 0x210e20000ULL + c * 0x1000000ULL;
        unsigned int matches = 0;
        for (u32 i = 0; i < reg_len / 16; i++) {
            u64 base, size;
            if (adt_get_reg(adt, path, "reg", i, &base, &size))
                return -1;
            if (base == wanted && size == 0x2000)
                matches++;
        }
        if (matches != 1)
            return -1;
        u32 len;
        const u32 *opps = adt_getprop(adt, node, tables[c], &len);
        if (!opps || len % 8 || len / 8 < 2 || len / 8 > 31)
            return -1;
        for (u32 i = 0; i < len / 8; i++) {
            if (!opps[2 * i] || opps[2 * i] > 5000000 || !opps[2 * i + 1] ||
                (i && opps[2 * i] <= opps[2 * (i - 1)]))
                return -1;
        }
        u64 raw = read64(wanted + 0x20);
        u32 state = raw & 31;
        if ((raw & T6040_CMD_BUSY) || !state || state > len / 8)
            return -1;
        clocks[c] = (struct t6040_clock){
            .reg = wanted + 0x20, .opps = opps, .count = len / 8, .expected = state};
    }
    ready = true;
    return 0;
}

/* ADT frequency for the accepted command state, not measured frequency. */
u64 cpufreq_t6040_get_hz(unsigned int cluster)
{
    if (cluster >= T6040_CLOCK_COUNT || cpufreq_t6040_init())
        return 0;
    struct t6040_clock *c = &clocks[cluster];
    u64 raw = read64(c->reg);
    u32 state = raw & 31;
    if (c->failed || (raw & T6040_CMD_BUSY) || !state || state > c->count)
        return 0;
    return (u64)c->opps[2 * (state - 1)] * 1000;
}

int cpufreq_t6040_set_pstate(unsigned int cluster, unsigned int state)
{
    if (cluster >= T6040_CLOCK_COUNT || cpufreq_t6040_init())
        return -1;
    struct t6040_clock *c = &clocks[cluster];
    if (c->failed || !state || state > c->count || state > T6040_MAX_TEST_STATE)
        return -1;
    if (poll64(c->reg, T6040_CMD_BUSY, 0, 2000))
        goto failed;
    u64 raw = read64(c->reg);
    /* Stop if a retained firmware owner changed the expected request. */
    if ((raw & 31) != c->expected)
        goto failed;
    if (state == c->expected)
        return 0;
    write64(c->reg, (raw & ~31ULL) | T6040_CMD_SET | state);
    if (poll64(c->reg, T6040_CMD_BUSY, 0, 2000) || (read64(c->reg) & 31) != state)
        goto failed;
    c->expected = state;
    return 0;
failed:
    c->failed = true;
    return -1;
}
