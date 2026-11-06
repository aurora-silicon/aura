/* SPDX-License-Identifier: MIT */
/* Replay the actual Linux/Windows PHY and reset sequence, without hardware. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define TYPES_H
#define UTILS_H
#define XDT_H
#define PMGR_H
#define SMC_H
#define TUNABLES_H
#define LIBFDT_H
#define __EXCEPTION_H__
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#define T8142         0x8142
#define BIT(n)        (1ULL << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
static u32 chip_id = T8142, board_id = 0x24;
static void *adt;
enum exc_guard_t { GUARD_OFF, GUARD_MARK = 2, GUARD_SILENT = 0x100 };
static enum exc_guard_t exc_guard;
static int exc_count;
static bool prefix, inject_fault, inject_barrier_fault;
static unsigned resets, validations, writes, ltssm_starts;
static void access(u64 a)
{
    if (prefix)
        assert(a >= 0x48c000000ULL && a < 0x490000000ULL);
}
static u32 read32(u64 a)
{
    access(a);
    printf("read %llx\n", (unsigned long long)a);
    return a == 0x1cb0100000ULL ? 0x1900106b : 1;
}
static void write32(u64 a, u32 v)
{
    access(a);
    if (a == 0x488028080ULL && v == 1)
        ltssm_starts++;
    writes++;
    printf("write %llx %x\n", (unsigned long long)a, v);
}
static void set32(u64 a, u32 v)
{
    access(a);
    if (inject_fault && a == 0x1cb00008bcULL) {
        assert(exc_guard == (GUARD_MARK | GUARD_SILENT));
        exc_count++;
        inject_fault = false;
    }
    writes++;
    printf("set %llx %x\n", (unsigned long long)a, v);
}
static void clear32(u64 a, u32 v)
{
    access(a);
    writes++;
    printf("clear %llx %x\n", (unsigned long long)a, v);
}
static void mask32(u64 a, u32 m, u32 v)
{
    access(a);
    writes++;
    printf("mask %llx %x %x\n", (unsigned long long)a, m, v);
}
static int poll32(u64 a, u32 m, u32 v, u32 timeout)
{
    access(a);
    printf("poll %llx %x %x %u\n", (unsigned long long)a, m, v, timeout);
    return 0;
}
static void udelay(u32 t)
{
    printf("us %u\n", t);
}
static void mdelay(u32 t)
{
    printf("ms %u\n", t);
}
static void model_sysop(const char *op)
{
    if (inject_barrier_fault && !strcmp(op, "dsb sy")) {
        assert(exc_guard == (GUARD_MARK | GUARD_SILENT));
        exc_count++;
        inject_barrier_fault = false;
    }
}
#define sysop(op) model_sysop(op)
static int adt_path_offset_trace(const void *d, const char *s, int *p)
{
    (void)d;
    (void)s;
    p[0] = 1;
    return 1;
}
static int adt_get_reg(const void *d, int *p, const char *k, int i, u64 *a, u64 *z)
{
    (void)d;
    (void)p;
    (void)k;
    (void)z;
    const u64 addr[] = {0x1cb0000000, 0x48c000000, 0x48f000000, 0x48f020000,
                        0x48f040000,  0x48e000000, 0,           0,
                        0x488028000,  0,           0x48f010000};
    assert(i >= 0 && i < 11);
    *a = addr[i];
    return 0;
}
static int pmgr_adt_power_enable(const char *p)
{
    printf("power %s\n", p);
    return 0;
}
#ifndef BASELINE
static int pmgr_adt_is_powered(const char *p)
{
    (void)p;
    return 1;
}
static int tunables_validate_local(const char *p, const char *k, u64 n)
{
    (void)p;
    (void)k;
    assert(n);
    validations++;
    return 0;
}
#endif
static int tunables_apply_local_addr(const char *p, const char *k, u64 a)
{
    access(a);
    printf("tunable %s %s %llx\n", p, k, (unsigned long long)a);
    return 0;
}
typedef struct {
    int unused;
} smc_dev_t;
static smc_dev_t smc;
static smc_dev_t *smc_init(void)
{
    assert(!prefix);
    puts("smc init");
    return &smc;
}
static int smc_write_u32(smc_dev_t *s, u32 k, u32 v)
{
    assert(s == &smc && !prefix);
    resets++;
    printf("smc write %x %x\n", k, v);
    return 0;
}
static void smc_shutdown(smc_dev_t *s)
{
    assert(s == &smc);
    puts("smc shutdown");
}
static int fdt_node_offset_by_compatible(const void *d, int off, const char *c)
{
    (void)d;
    (void)off;
    (void)c;
    return 1;
}
static const void *fdt_getprop(const void *d, int off, const char *k, int *len)
{
    (void)d;
    (void)off;
    (void)k;
    *len = 5;
    return "okay";
}
static int fdt_setprop_string(void *d, int off, const char *k, const char *v)
{
    (void)d;
    (void)off;
    (void)k;
    (void)v;
    assert(false);
    return -1;
}
#include "../../../src/j813_n1.c"
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "linux")) {
        assert(j813_n1_prepare(NULL) == 0 && resets == 2);
    } else {
#ifndef BASELINE
        assert(!j813_n1_validate_windows() && validations == 8 && !writes && !resets);
        prefix = true;
        assert(!j813_n1_prepare_fabric_windows() && !resets);
        prefix = false;
        bool ready = false;
        if (!strcmp(argv[1], "windows-fault")) {
            inject_fault = true;
            assert(j813_n1_finish_windows(&ready) < 0 && !ready && !ltssm_starts);
            return 0;
        }
        if (!strcmp(argv[1], "windows-barrier-fault")) {
            inject_barrier_fault = true;
            assert(j813_n1_finish_windows(&ready) < 0 && !ready && !ltssm_starts);
            return 0;
        }
        assert(!j813_n1_finish_windows(&ready) && ready && resets == 2);
        assert(j813_n1_prepare_fabric_windows() < 0);
        assert(j813_n1_finish_windows(&ready) < 0 && !ready);
#else
        assert(false);
#endif
    }
    return 0;
}
