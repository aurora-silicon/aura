# SPDX-License-Identifier: MIT
"""Host checks of sptm_virt_low_floor() against the firmware RO range memory_fw_ro_range() reports.

Both functions are taken from the tree as they are. The layout numbers are the ones logged on a
T8132 (J604) launch: the RO range is 3 x 16 KiB at stage 1 base + 0x874000, the image ends at
base + 0x1a0000 and the carve-out starts at 0x103bb600000. The base is where iBoot loaded stage 1,
so the sweep walks every 16 KiB base across the span the launches have drawn.
"""

from test_smp_timeouts import REPO, compile_and_run


def function(path, signature):
    source = (REPO / path).read_text()
    start = source.index(signature)
    return source[start:source.index("\n}\n", start) + 3]


def guard_enum():
    header = (REPO / "src/exception.h").read_text()
    start = header.index("enum exc_guard_t {")
    return header[start:header.index("};", start) + 2]


HARNESS = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t u64;
typedef uint32_t u32;
#define SZ_4K 0x1000UL
#define SZ_16K 0x4000UL
#define SZ_32M 0x2000000UL
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((u64)(a) - 1))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
@GUARD@
enum { T8112 = 0x8112, T8132 = 0x8132, T8140 = 0x8140 };
static int chip_id = T8132;
static struct { u64 phys_base, mem_size, top_of_kernel_data; } cur_boot_args;
static volatile enum exc_guard_t exc_guard;
static volatile int exc_count;
static bool has_ctrr = true, el2 = true;
static u64 reg_lwr, reg_upr, stub_end, stub_heap_hw, stub_heap_lim;
static int reads;
#define SYS_IMP_APL_CTRR_C_LWR_EL2 1
#define SYS_IMP_APL_CTRR_C_UPR_EL2 2
/* A core without the registers faults; the guard marks it and the read yields nothing. */
#define mrs(r)                                                                                  \
    ({                                                                                          \
        assert(exc_guard == (GUARD_MARK | GUARD_SILENT));                                       \
        reads++;                                                                                \
        if (!has_ctrr)                                                                          \
            exc_count++;                                                                        \
        has_ctrr ? ((r) == SYS_IMP_APL_CTRR_C_LWR_EL2 ? reg_lwr : reg_upr) : 0xdeadUL;          \
    })
static bool is_boot_cpu(void) { return true; }
static bool in_el2(void) { return el2; }
static bool sptm_virt_carveout_low(void) { return false; }
static u64 heapblock_high_water(void) { return stub_heap_hw; }
static u64 heapblock_limit(void) { return stub_heap_lim; }
#define _end ((void *)stub_end)
/* Only the line for a rejected CTRR_C pair is kept: how often it came and with which registers. */
static int rejects;
static u64 rej_lwr, rej_upr;
static void stub_printf(const char *fmt, ...)
{
    va_list ap;

    if (strncmp(fmt, "MMU: unusable CTRR_C range", 26))
        return;
    va_start(ap, fmt);
    rej_lwr = va_arg(ap, u64);
    rej_upr = va_arg(ap, u64);
    va_end(ap);
    rejects++;
}
#define printf(...) stub_printf(__VA_ARGS__)
static bool firmware_ro_checked, firmware_ro_valid;
static int firmware_ro_none;
static u64 firmware_ro_start, firmware_ro_end;
@RO_REJECT@
@RO_RANGE@
@FLOOR@
#undef printf

#define DRAM 0x10000000000UL
#define LIMIT 0x103bb600000UL
#define RO_OFF 0x874000UL
#define RO_SIZE (3 * SZ_16K)
#define IMAGE 0x1a0000UL

static u64 run(u64 base, bool ctrr_set)
{
    firmware_ro_checked = firmware_ro_valid = false;
    cur_boot_args.phys_base = DRAM;
    cur_boot_args.mem_size = 0x400000000UL;
    cur_boot_args.top_of_kernel_data = base + IMAGE;
    stub_end = base + IMAGE;
    stub_heap_hw = LIMIT + 0x100000;
    stub_heap_lim = LIMIT + 0x200000;
    reg_lwr = ctrr_set ? base + RO_OFF : 0;
    reg_upr = ctrr_set ? base + RO_OFF + RO_SIZE - SZ_4K : 0;
    return sptm_virt_low_floor(LIMIT);
}
'''


def build(tmp_path, name, body):
    source = (HARNESS
              .replace("@GUARD@", guard_enum())
              .replace("@RO_REJECT@", function("src/memory.c", "static int firmware_ro_reject(u64 lower, u64 upper)"))
              .replace("@RO_RANGE@", function("src/memory.c", "int memory_fw_ro_range(u64 *start, u64 *end)"))
              .replace("@FLOOR@", function("src/sptm_virt.c", "static u64 sptm_virt_low_floor(u64 limit)"))
              + body)
    compile_and_run(tmp_path, name, source)


def test_floor_clears_the_ro_range_on_every_base(tmp_path):
    build(tmp_path, "floor_sweep", r'''
int main(void)
{
    unsigned bases = 0, moved = 0, crossed_before = 0;

    for (u64 base = 0x10005124000UL; base <= 0x10007100000UL; base += SZ_16K, bases++) {
        u64 ro_end = base + RO_OFF + RO_SIZE;
        u64 floor = run(base, true);
        u64 without = run(base, false);

        assert(memory_fw_ro_range(NULL, NULL) == 0);
        assert(!(floor & (SZ_32M - 1)));
        assert(floor >= ro_end);           /* SPTM never starts inside the RO range */
        assert(floor >= without);          /* the range only ever raises the floor */
        if (floor != without) {
            moved++;
            /* every base the range moves is one where the old floor sat below the range's end */
            assert(without < ro_end);
        }
        if (without < ro_end)
            crossed_before++;
    }
    assert(bases == 2040);
    assert(moved == crossed_before);
    printf("%u bases, floor raised on %u\n", bases, moved);
    assert(moved == 440);
    /* the crossing base logged on the J604 */
    assert(run(0x10005c28000UL, true) == 0x10008000000UL);
    assert(run(0x10005c28000UL, false) == 0x10006000000UL);
    return 0;
}
''')


def test_ctrr_read_fault_reads_as_no_range(tmp_path):
    build(tmp_path, "floor_fault", r'''
int main(void)
{
    u64 start = 1, end = 1, base = 0x10005c28000UL;

    /* A core without CTRR_C (M1, M2): the read faults, is marked, and means no range. */
    has_ctrr = false;
    chip_id = T8112;
    exc_count = 0;
    assert(run(base, true) == 0x10006000000UL);
    assert(exc_count == 2 && exc_guard == GUARD_OFF);
    assert(memory_fw_ro_range(&start, &end) == 0 && start == 1 && end == 1);

    /* T8140 keeps reporting a range it cannot use as an error. */
    chip_id = T8140;
    firmware_ro_checked = false;
    assert(memory_fw_ro_range(&start, &end) == -1 && exc_guard == GUARD_OFF);

    /* With the registers present the same read reports the range. */
    has_ctrr = true;
    chip_id = T8132;
    run(base, true);
    assert(memory_fw_ro_range(&start, &end) == 1);
    assert(start == base + RO_OFF && end == base + RO_OFF + RO_SIZE);

    /* An EL1 guest leaves the range to its host and never touches the registers. */
    el2 = false;
    reads = 0;
    run(base, true);
    assert(reads == 0 && memory_fw_ro_range(NULL, NULL) == 0);
    return 0;
}
''')


def test_rejected_ctrr_pair_is_reported_once(tmp_path):
    build(tmp_path, "floor_reject", r'''
static int probe(u64 lwr, u64 upr)
{
    reg_lwr = lwr;
    reg_upr = upr;
    firmware_ro_checked = firmware_ro_valid = false;
    rejects = 0;
    rej_lwr = rej_upr = 0;
    return memory_fw_ro_range(NULL, NULL);
}

int main(void)
{
    u64 base = 0x10005c28000UL, lwr = base + RO_OFF, upr = base + RO_OFF + RO_SIZE - SZ_4K;
    u64 ram_end = DRAM + 0x400000000UL;
    const struct {
        u64 lwr, upr;
    } bad[] = {
        {lwr + 0x800, upr},              /* not on a 4 KiB boundary */
        {lwr, UINT64_MAX - SZ_4K + 1},   /* the end would wrap */
        {upr + SZ_4K, lwr},              /* ends before it starts */
        {lwr, 0},                        /* only one register set */
        {DRAM - SZ_16K, DRAM + SZ_16K},  /* starts below RAM */
        {lwr, ram_end},                  /* runs past the end of RAM */
    };

    run(base, true);

    /* An unset pair and a usable pair both say nothing. */
    assert(probe(0, 0) == 0 && rejects == 0);
    assert(probe(lwr, upr) == 1 && rejects == 0);

    /* A set pair that fails a check still reads as none, and is printed once with its values. */
    for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
        assert(probe(bad[i].lwr, bad[i].upr) == 0);
        assert(rejects == 1 && rej_lwr == bad[i].lwr && rej_upr == bad[i].upr);
        assert(memory_fw_ro_range(NULL, NULL) == 0 && rejects == 1);
    }

    /* Boot args the pair cannot be checked against count as a reject too. */
    cur_boot_args.mem_size = 0;
    assert(probe(lwr, upr) == 0 && rejects == 1 && rej_lwr == lwr && rej_upr == upr);
    run(base, true);
    cur_boot_args.top_of_kernel_data = ram_end;
    assert(probe(lwr, upr) == 0 && rejects == 1 && rej_lwr == lwr && rej_upr == upr);
    run(base, true);

    /* A core without the registers faults under the guard: no range, and nothing printed. */
    has_ctrr = false;
    assert(probe(lwr, upr) == 0 && rejects == 0);
    has_ctrr = true;

    /* T8140 keeps its error return; the line only adds what the registers held. */
    chip_id = T8140;
    assert(probe(lwr, upr) == 1 && rejects == 0);
    assert(probe(lwr, upr + SZ_16K) == -1 && rejects == 1 && rej_upr == upr + SZ_16K);
    assert(probe(0, 0) == -1 && rejects == 0);
    return 0;
}
''')
