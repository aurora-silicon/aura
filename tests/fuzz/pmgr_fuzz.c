/* SPDX-License-Identifier: MIT */
/*
 * libFuzzer target for the ADT's power manager tables as aura reads them: src/pmgr.c (the
 * /arm-io/pmgr ps-regs, devices and their parents, and the power requests aura makes from the
 * clock-gates of a node) and src/sptm_virt_s2r_soc.c (the SoC blocks of the sleep path), with
 * aura's ADT reader (rust/src/adt.rs).
 *
 * Input: a flags byte, 16 bytes of register seeds, then the ADT. The power state registers are
 * simulated: each one starts from the next seed, and the state written as target becomes the
 * actual state at once, or never when flags bit 0 is set (every wait then times out).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adt.h"
#include "pmgr.h"
#include "soc.h"
#include "sptm_virt_s2r_soc.h"
#include "types.h"

void *adt;
static u32 adt_size;
u32 board_id, chip_id;
static u8 flags;
static const u8 *seeds;
static volatile u64 sink;

u32 adt_get_size(void)
{
    return adt_size;
}

int fuzz_log(const char *fmt, ...)
{
    static char line[512];
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    return n;
}

u64 host_sysreg(const char *name)
{
    (void)name;
    return 0;
}

#define NSEEDS 16
#define NREGS  512

static struct {
    u64 addr;
    u32 val;
} regs[NREGS];
static int nregs;

static u32 *reg(u64 addr)
{
    for (int i = 0; i < nregs; i++)
        if (regs[i].addr == addr)
            return &regs[i].val;
    /* Past the model's size every new register is the same scratch word */
    int i = nregs < NREGS ? nregs++ : NREGS - 1;
    u8 s = seeds[i % NSEEDS];
    regs[i].addr = addr;
    regs[i].val = (s & 0xf) | ((s & 0xf) << 4) | ((s & 0x80) ? BIT(28) : 0);
    return &regs[i].val;
}

u32 read32(u64 addr)
{
    return *reg(addr);
}

void write32(u64 addr, u32 val)
{
    /* PMGR_PS_TARGET becomes PMGR_PS_ACTUAL, unless the block is stuck */
    if (!(flags & 1))
        val = (val & ~0xf0u) | ((val & 0xf) << 4);
    *reg(addr) = val;
}

u64 read64(u64 addr)
{
    return read32(addr) | ((u64)read32(addr + 4) << 32);
}

u8 read8(u64 addr)
{
    return read32(addr);
}

void udelay(u32 us)
{
    (void)us;
}

static void power(const char *path)
{
    u64 psregs[32];
    u8 psflags[32];

    sink += pmgr_adt_collect_psregs(path, psregs, psflags, 32);
    sink += pmgr_adt_is_powered(path);
    sink += pmgr_adt_power_is_off(path);
    sink += pmgr_adt_power_enable(path);
    sink += pmgr_adt_power_enable_index(path, 1);
    struct pmgr_saved_modes saved;
    if (!pmgr_adt_power_enable_traced_saved(path, &saved))
        pmgr_restore_modes(&saved);
    struct pmgr_gate gate;
    sink += pmgr_adt_gate(path, "clock-gates", 0, &gate);
    if (flags & 2)
        sink += pmgr_adt_reset(path);
    sink += pmgr_adt_power_disable_index(path, 0);
    sink += pmgr_adt_power_disable(path);
    if (flags & 4)
        sink += pmgr_adt_power_disable_recursive(path);
}

int LLVMFuzzerTestOneInput(const u8 *data, size_t size)
{
    if (size < 1 + NSEEDS || size - 1 - NSEEDS > SZ_1M)
        return 0;
    flags = data[0];
    seeds = data + 1;
    size -= 1 + NSEEDS;
    adt = malloc(size ? size : 1);
    memcpy(adt, data + 1 + NSEEDS, size);
    adt_size = size;
    nregs = 0;
    chip_id = (flags & 8) ? T8140 : T8103;

    if (adt_check_header(adt))
        goto out;

    struct s2r_soc soc;
    const char *missing = NULL;
    u32 phys[2] = {0x10 + (flags >> 4), 0x20};
    memset(&soc, 0, sizeof(soc));
    sink += s2r_soc_from_adt(adt, phys, 2, &soc, &missing);
    memset(&soc, 0, sizeof(soc));
    sink += s2r_soc_link_only(adt, phys, 2, &soc);

    if (pmgr_init())
        goto out;
    sink += pmgr_get_feature("cpu-features");

    int node = adt_path_offset(adt, "/arm-io");
    int count = 0;
    if (node >= 0) {
        ADT_FOREACH_CHILD(adt, node)
        {
            const char *name = adt_get_name(adt, node);
            char path[64];
            if (!name || snprintf(path, sizeof(path), "/arm-io/%s", name) >= (int)sizeof(path))
                continue;
            power(path);
            if (++count == 32)
                break;
        }
    }
    for (int i = 0; i < soc.n_link; i++)
        power(soc.link_paths[i]);
    sink += pmgr_power_on(0, "UART0");
    sink += pmgr_power_is_on(0, "UART0");
    sink += pmgr_reset(0, "UART0");
    sink += pmgr_psreg_by_name(0, "UART0");

out:
    free(adt);
    adt = NULL;
    return 0;
}
