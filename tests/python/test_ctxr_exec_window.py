# SPDX-License-Identifier: MIT
"""On a confined machine the OS runs at EL2 inside the armed window, or not at all."""

from test_locked_dart_tables import function
from test_smp_timeouts import REPO, compile_and_run


def window_source():
    source = (REPO / "src/ctxr_exec.c").read_text()
    return source[source.index("#define CTXR_EXEC_AURA_SLOT"):]


WINDOW_HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
/* unsigned long matches the %%lx the source prints with, on Linux and macOS alike. */
typedef unsigned long u64;
#define SZ_4K 0x1000
#define SZ_16K 0x4000
#define SZ_1M (1 << 20)
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
static int register_reads;
static u64 read_register(u64 value) { register_reads++; return value; }
#define mrs(reg) read_register(reg)
static u64 SYS_IMP_APL_CTXR_A_LWR_EL1, SYS_IMP_APL_CTXR_A_UPR_EL1, SYS_IMP_APL_CTXR_B_LWR_EL1,
    SYS_IMP_APL_CTXR_B_UPR_EL1, SYS_IMP_APL_CTXR_C_LWR_EL1, SYS_IMP_APL_CTXR_C_UPR_EL1,
    SYS_IMP_APL_CTXR_D_LWR_EL1, SYS_IMP_APL_CTXR_D_UPR_EL1, SYS_IMP_APL_CTRR_C_LWR_EL1,
    SYS_IMP_APL_CTRR_C_UPR_EL1, SYS_IMP_APL_CTRR_D_LWR_EL1, SYS_IMP_APL_CTRR_D_UPR_EL1;
static struct { bool apple_sysregs_unlocked; } features, *cpu_features = &features;
static int confined, ro_status, fb_active;
static u64 _base, _end, boot_args_addr, ro_start_v, ro_end_v;
static struct { u64 top_of_kernel_data; } cur_boot_args;
static char console[1024];
static bool ctxr_exec_confined(void) { return confined; }
static int memory_fw_ro_range(u64 *start, u64 *end)
{ *start = ro_start_v; *end = ro_end_v; return ro_status; }
static void fb_set_active(bool active) { fb_active = active; }
#define printf(...) snprintf(console + strlen(console), sizeof(console) - strlen(console), __VA_ARGS__)
''' + "%s" + r'''
#undef printf
#define BASE 0x10005254000UL
#define MIB (1UL << 20)
#define WINDOW (128 * MIB)
static const u64 start = ALIGN_UP(BASE + 16 * MIB, 2 * MIB);
/* A fresh boot on the layout of an object from aurora-installer's expandExecuteWindow, on a J700. */
static void expanded(void)
{
    window_checked = false; window_problem = NULL; window_base = window_size = 0;
    confined = 1; ro_status = 1; register_reads = 0; fb_active = 0; console[0] = 0;
    features.apple_sysregs_unlocked = false;
    _base = BASE; _end = BASE + 0x3d0000;
    SYS_IMP_APL_CTXR_D_LWR_EL1 = BASE - 0xd8000;
    SYS_IMP_APL_CTXR_D_UPR_EL1 = BASE + 16 * MIB + 2 * MIB - SZ_16K + WINDOW - SZ_4K;
    u64 above = BASE + 16 * MIB + 2 * MIB + WINDOW;
    ro_start_v = above + 0x6c0000; ro_end_v = ro_start_v + 3 * SZ_16K;
    boot_args_addr = above + 0x800000;
    cur_boot_args.top_of_kernel_data = above + 0x804000;
    /* Below this image on every chip that describes them. */
    SYS_IMP_APL_CTXR_A_LWR_EL1 = SYS_IMP_APL_CTRR_C_LWR_EL1 = BASE - 0x100000;
    SYS_IMP_APL_CTXR_A_UPR_EL1 = SYS_IMP_APL_CTRR_C_UPR_EL1 = BASE - 0x100000 + SZ_16K - SZ_4K;
    SYS_IMP_APL_CTXR_B_LWR_EL1 = BASE - 0xfc000;
    SYS_IMP_APL_CTXR_B_UPR_EL1 = BASE - 0xfc000 + SZ_16K - SZ_4K;
    SYS_IMP_APL_CTXR_C_LWR_EL1 = BASE - 0xf8000;
    SYS_IMP_APL_CTXR_C_UPR_EL1 = BASE - 0xf8000 + SZ_16K - SZ_4K;
    SYS_IMP_APL_CTRR_D_LWR_EL1 = SYS_IMP_APL_CTRR_D_UPR_EL1 = 0;
}
/* The same object padded for 256 MiB, as the Neo's proxy Stage 1 is. */
static void armed_256(void)
{
    expanded();
    SYS_IMP_APL_CTXR_D_UPR_EL1 += WINDOW;
    u64 above = BASE + 16 * MIB + 2 * MIB + 2 * WINDOW;
    ro_start_v = above + 0x6c0000; ro_end_v = ro_start_v + 3 * SZ_16K;
    boot_args_addr = above + 0x800000;
    cur_boot_args.top_of_kernel_data = above + 0x804000;
}
/* A chip with no firmware RO range of its own (memory_fw_ro_range() answers 0). */
static void other_chip(void)
{
    expanded();
    ro_status = 0;
}
static bool usable(void) { u64 b, s; return ctxr_exec_window(&b, &s); }
/* The boot stops, on the panel too, and says how to get out of it. */
static bool refused(const char *why)
{
    console[0] = 0;
    return !usable() && !ctxr_exec_os_may_start() && fb_active && strstr(console, why) &&
           strstr(console, "Recovery step") && strstr(console, "Not starting the OS");
}
int main(void)
{
    u64 base = 0, size = 0;

    /* Not confined (M1 to M3, a raw object): as ever, and no CTXR register is touched. */
    expanded();
    confined = 0;
    assert(!ctxr_exec_window(&base, &size) && !base && !size);
    assert(ctxr_exec_os_may_start());
    assert(!register_reads && !fb_active && !console[0]);

    /* The release object: the window starts past aura and its slot and is exactly 128 MiB. */
    expanded();
    assert(ctxr_exec_window(&base, &size));
    assert(base == start && !(base & (2 * MIB - 1)) && size == WINDOW);
    assert(base + size <= SYS_IMP_APL_CTXR_D_UPR_EL1 + SZ_4K);
    assert(base > BASE && base > SYS_IMP_APL_CTXR_D_LWR_EL1);
    assert(ctxr_exec_window(NULL, NULL) && ctxr_exec_os_may_start() && !fb_active);

    /* Armed for more: the OS gets the same 128 MiB at the same place. */
    armed_256();
    assert(ctxr_exec_window(&base, &size));
    assert(base == start && size == WINDOW);
    assert(base + size < SYS_IMP_APL_CTXR_D_UPR_EL1 + SZ_4K);
    assert(strstr(console, "armed bytes above it stay reserved"));
    /* What lies past the window is not its problem, or the OS's: kboot reserves it. */
    armed_256();
    boot_args_addr = start + WINDOW + 8 * MIB;
    cur_boot_args.top_of_kernel_data = start + WINDOW + 8 * MIB + SZ_16K;
    ro_start_v = start + WINDOW; ro_end_v = ro_start_v + 3 * SZ_16K;
    assert(usable());

    /* The check is decided once a boot: no second read of the registers. */
    expanded();
    assert(usable());
    int reads = register_reads;
    assert(usable() && ctxr_exec_os_may_start() && register_reads == reads);

    /* No chip is special: every chip goes by its own registers. */
    other_chip();
    assert(usable() && ctxr_exec_os_may_start());
    other_chip();
    SYS_IMP_APL_CTXR_B_LWR_EL1 = start + 8 * MIB; SYS_IMP_APL_CTXR_B_UPR_EL1 = start + 8 * MIB;
    assert(refused("read-only firmware range"));
    other_chip();
    SYS_IMP_APL_CTRR_C_LWR_EL1 = start; SYS_IMP_APL_CTRR_C_UPR_EL1 = start + SZ_16K;
    assert(refused("read-only firmware range"));
    other_chip();
    SYS_IMP_APL_CTRR_D_LWR_EL1 = start + WINDOW - SZ_4K; SYS_IMP_APL_CTRR_D_UPR_EL1 = start + WINDOW;
    assert(refused("read-only firmware range"));
    /* Unlocked sysregs do not take the CTRR reads kboot would not make either. */
    other_chip();
    features.apple_sysregs_unlocked = true;
    SYS_IMP_APL_CTRR_C_LWR_EL1 = start; SYS_IMP_APL_CTRR_C_UPR_EL1 = start + SZ_16K;
    assert(usable());

    /* Everything below stops the boot: a confined secondary would fault in the kernel. */
    expanded();
    SYS_IMP_APL_CTXR_D_LWR_EL1 = 0;
    assert(refused("reads as empty"));
    /* The object as enrolled before the padding, armed for 0x508000 bytes. */
    expanded();
    SYS_IMP_APL_CTXR_D_UPR_EL1 = SYS_IMP_APL_CTXR_D_LWR_EL1 + 0x507000;
    assert(refused("did not arm a large enough window"));
    /* A page short of the window is short; exactly the window is enough. */
    expanded();
    SYS_IMP_APL_CTXR_D_UPR_EL1 = start + WINDOW - SZ_16K - SZ_4K;
    assert(refused("did not arm a large enough window"));
    expanded();
    SYS_IMP_APL_CTXR_D_UPR_EL1 = start + WINDOW - SZ_4K;
    assert(usable());
    expanded();
    _base = SYS_IMP_APL_CTXR_D_LWR_EL1 - SZ_16K;
    assert(refused("not inside CTXR_D"));
    expanded();
    _end = start + SZ_16K;
    assert(refused("not inside CTXR_D"));
    expanded();
    _end = start;
    assert(usable());
    expanded();
    ro_status = -1;
    assert(refused("read-only firmware range"));
    expanded();
    ro_start_v = start + 8 * MIB; ro_end_v = ro_start_v + 3 * SZ_16K;
    assert(refused("read-only firmware range"));
    /* Firmware RO below the window, in aura's slot, is out of the OS's way. */
    expanded();
    ro_start_v = BASE + 0xafc000; ro_end_v = ro_start_v + 3 * SZ_16K;
    assert(usable());
    expanded();
    boot_args_addr = start + 64 * MIB;
    assert(refused("boot_args"));
    expanded();
    boot_args_addr = start - SZ_16K;
    assert(usable());
    expanded();
    cur_boot_args.top_of_kernel_data = start + 64 * MIB;
    assert(refused("heap"));
    puts("ctxr exec window OK");
    return 0;
}
'''


def test_window_decision(tmp_path):
    compile_and_run(tmp_path, "ctxr-exec-window", WINDOW_HARNESS % window_source())


def kboot_source():
    return (REPO / "src/kboot.c").read_text()


RESERVE_HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t u64;
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
static void *dt;
static u64 window, window_size, rsv[4][2];
static int nrsv, fail_at = -1;
static bool ctxr_exec_window(u64 *b, u64 *s)
{ if (!window_size) return false; *b = window; *s = window_size; return true; }
static int fdt_add_mem_rsv(void *p, u64 start, u64 size)
{
    if (nrsv == fail_at) return -1;
    rsv[nrsv][0] = start; rsv[nrsv][1] = size; nrsv++;
    return 0;
}
''' + "%s" + r'''
#define CHECK(start, size, n) do { nrsv = 0; assert(!dt_reserve_outside_exec_window(start, size)); assert(nrsv == (n)); } while (0)
int main(void)
{
    /* No window: exactly one call with the caller's arguments, as on a normal boot. */
    CHECK(0x1000, 0x5000, 1);
    assert(rsv[0][0] == 0x1000 && rsv[0][1] == 0x5000);

    window = 0x10000; window_size = 0x10000;
    /* Spanning the window: reserve both sides of it. */
    CHECK(0x1000, 0x30000, 2);
    assert(rsv[0][0] == 0x1000 && rsv[0][1] == 0xf000);
    assert(rsv[1][0] == 0x20000 && rsv[1][1] == 0x11000);
    /* CTXR_D itself ends with the window: only the part below it stays. */
    CHECK(0x1000, 0x1f000, 1);
    assert(rsv[0][0] == 0x1000 && rsv[0][1] == 0xf000);
    /* Clear of the window, on either side. */
    CHECK(0x1000, 0x2000, 1);
    assert(rsv[0][0] == 0x1000 && rsv[0][1] == 0x2000);
    CHECK(0x30000, 0x1000, 1);
    assert(rsv[0][0] == 0x30000 && rsv[0][1] == 0x1000);
    /* Starting or ending inside it, and wholly inside. */
    CHECK(0x18000, 0x10000, 1);
    assert(rsv[0][0] == 0x20000 && rsv[0][1] == 0x8000);
    CHECK(0x8000, 0x10000, 1);
    assert(rsv[0][0] == 0x8000 && rsv[0][1] == 0x8000);
    CHECK(0x12000, 0x1000, 0);
    /* A failure is reported on either side. */
    fail_at = 0; nrsv = 0;
    assert(dt_reserve_outside_exec_window(0x1000, 0x30000) == -1);
    fail_at = 1; nrsv = 0;
    assert(dt_reserve_outside_exec_window(0x1000, 0x30000) == -1);
    fail_at = -1;

    /* CTXR_D armed at 256 MiB: everything but the 128 MiB OS window stays out of the allocator. */
    u64 lower = 0x10005254000UL - 0xd8000, armed_end = 0x10006400000UL + (256UL << 20);
    window = 0x10006400000UL; window_size = 128UL << 20;
    CHECK(lower, armed_end - lower, 2);
    assert(rsv[0][0] == lower && rsv[0][0] + rsv[0][1] == window);
    assert(rsv[1][0] == window + window_size && rsv[1][0] + rsv[1][1] == armed_end);
    assert(rsv[1][1] == window_size);
    puts("ctxr exec reservations OK");
    return 0;
}
'''


def test_reservations_leave_the_window_to_the_os(tmp_path):
    body = function(kboot_source(), "dt_reserve_outside_exec_window")
    compile_and_run(tmp_path, "ctxr-exec-reserve", RESERVE_HARNESS % body)


LOW_WINDOWS_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef uint64_t u64;
#define SZ_4K 0x1000UL
#define SZ_16K 0x4000UL
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define mrs(reg) (reg)
static u64 SYS_IMP_APL_CTXR_A_LWR_EL1, SYS_IMP_APL_CTXR_A_UPR_EL1, SYS_IMP_APL_CTXR_B_LWR_EL1,
    SYS_IMP_APL_CTXR_B_UPR_EL1, SYS_IMP_APL_CTXR_C_LWR_EL1, SYS_IMP_APL_CTXR_C_UPR_EL1;
static void *dt;
static u64 rsv[4][2];
static int nrsv, fail_at = -1;
static int fdt_add_mem_rsv(void *p, u64 start, u64 size)
{
    if (nrsv == fail_at) return -1;
    rsv[nrsv][0] = start; rsv[nrsv][1] = size; nrsv++;
    return 0;
}
#define printf(...) ((void)0)
''' + "%s" + r'''
#undef printf
int main(void)
{
    /* Disarmed windows read as zero and reserve nothing. */
    assert(!dt_reserve_ctxr_low_windows() && !nrsv);

    /* Each window on its own, whole 16K pages, wherever the three of them are. */
    SYS_IMP_APL_CTXR_A_LWR_EL1 = 0x100000; SYS_IMP_APL_CTXR_A_UPR_EL1 = 0x103000;
    SYS_IMP_APL_CTXR_C_LWR_EL1 = 0x204000; SYS_IMP_APL_CTXR_C_UPR_EL1 = 0x204fff;
    assert(!dt_reserve_ctxr_low_windows() && nrsv == 2);
    assert(rsv[0][0] == 0x100000 && rsv[0][1] == SZ_16K);
    assert(rsv[1][0] == 0x204000 && rsv[1][1] == SZ_16K);

    /* A window whose bounds make no sense is left alone. */
    nrsv = 0;
    SYS_IMP_APL_CTXR_B_LWR_EL1 = 0x180000; SYS_IMP_APL_CTXR_B_UPR_EL1 = 0x17c000;
    assert(!dt_reserve_ctxr_low_windows() && nrsv == 2);

    nrsv = 0; fail_at = 1;
    assert(dt_reserve_ctxr_low_windows() == -1);
    puts("ctxr low windows OK");
    return 0;
}
'''


def test_other_chips_reserve_windows_a_to_c(tmp_path):
    body = function(kboot_source(), "dt_reserve_ctxr_low_windows")
    compile_and_run(tmp_path, "ctxr-low-windows", LOW_WINDOWS_HARNESS % body)


EMIT_HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t u64;
typedef uint64_t fdt64_t;
#define bail(...) do { return -1; } while (0)
#define cpu_to_fdt64(x) (x)
#define printf(...) ((void)0)
static void *dt;
static u64 base_v, size_v, written[2];
static const char *written_name;
static int props, fail;
static bool ctxr_exec_window(u64 *b, u64 *s)
{ if (!size_v) return false; *b = base_v; *s = size_v; return true; }
static int fdt_setprop(void *p, int node, const char *name, const void *value, int length)
{
    if (fail) return -1;
    props++; written_name = name;
    assert(length == sizeof(written));
    memcpy(written, value, length);
    return 0;
}
''' + "%s" + r'''
#undef printf
int main(void)
{
    /* No window, no property: a kernel on M1 to M3 sees the tree it always saw. */
    assert(!dt_set_ctxr_exec_window(0) && !props);

    /* A real window, and nothing else. */
    base_v = 0x10006400000UL; size_v = 128UL << 20;
    assert(!dt_set_ctxr_exec_window(0) && props == 1);
    assert(!strcmp(written_name, "aurora,ctxr-exec-window"));
    assert(written[0] == base_v && written[1] == size_v);

    fail = 1;
    assert(dt_set_ctxr_exec_window(0) == -1);
    puts("ctxr exec window property OK");
    return 0;
}
'''


def test_window_property_needs_a_real_window(tmp_path):
    body = function(kboot_source(), "dt_set_ctxr_exec_window")
    compile_and_run(tmp_path, "ctxr-exec-emit", EMIT_HARNESS % body)


MOVE_HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint64_t u64;
struct kernel_header { uint32_t code[2]; u64 text_offset; u64 image_size; };
#define bail(...) do { return -1; } while (0)
#define sysop(op) ((void)0)
static u64 window, window_size;
static int cleaned, invalidated;
static bool ctxr_exec_window(u64 *b, u64 *s)
{ if (!window_size) return false; *b = window; *s = window_size; return true; }
static void dc_cvac_range(void *p, size_t s) { cleaned = (p == (void *)(window + ((struct kernel_header *)p)->text_offset)); }
static void ic_ivau_range(void *p, size_t s) { invalidated = 1; }
''' + "%s" + r'''
int main(void)
{
    enum { SIZE = 0x8000 };
    static char source[SIZE], target[4 * SIZE];
    struct kernel_header *hdr = (void *)source;
    void *kernel = source;

    /* No window: the kernel stays where the payload put it. */
    hdr->image_size = SIZE;
    assert(!kboot_move_to_exec_window(&kernel) && kernel == source);
    assert(!cleaned && !invalidated);

    window = (u64)target; window_size = sizeof(target);
    memset(source + sizeof(*hdr), 0x5a, SIZE - sizeof(*hdr));
    assert(!kboot_move_to_exec_window(&kernel));
    assert(kernel == target && !memcmp(target, source, SIZE) && cleaned && invalidated);

    /* The image goes text_offset past the 2 MiB aligned base, as arm64 booting asks. */
    kernel = source; hdr->text_offset = SIZE;
    assert(!kboot_move_to_exec_window(&kernel) && kernel == target + SIZE);

    /* Nothing is copied past the end of the armed window. */
    kernel = source; memset(target, 0, sizeof(target));
    hdr->text_offset = 0; hdr->image_size = sizeof(target) + 1;
    assert(kboot_move_to_exec_window(&kernel) == -1 && kernel == source);
    hdr->text_offset = 3 * SIZE + 1; hdr->image_size = SIZE;
    assert(kboot_move_to_exec_window(&kernel) == -1 && kernel == source);
    hdr->text_offset = UINT64_MAX; hdr->image_size = SIZE;
    assert(kboot_move_to_exec_window(&kernel) == -1 && kernel == source);
    hdr->text_offset = 0; hdr->image_size = 0;
    assert(kboot_move_to_exec_window(&kernel) == -1 && kernel == source);
    for (size_t i = 0; i < sizeof(target); i++)
        assert(!target[i]);
    puts("ctxr exec kernel move OK");
    return 0;
}
'''


def test_kernel_moves_into_the_window(tmp_path):
    body = function(kboot_source(), "kboot_move_to_exec_window")
    compile_and_run(tmp_path, "ctxr-exec-move", MOVE_HARNESS % body)


def test_no_path_enters_a_confined_kernel_without_a_window():
    source = kboot_source()
    # Both ways into the kernel stop first, before any hardware or memory is touched.
    prepare = source[source.index("int kboot_prepare_dt(void *fdt)\n{"):]
    assert prepare.index("ctxr_exec_os_may_start()") < prepare.index("isp_init()")
    boot = function(source, "kboot_boot")
    assert boot.index("ctxr_exec_os_may_start()") < boot.index("kboot_boot_t8152(kernel)")
    assert boot.index("ctxr_exec_os_may_start()") < boot.index("mcc_enable_cache()")
    # Both handoffs have the kernel in place before its address is published.
    assert boot.index("kboot_move_to_exec_window(&kernel)") < boot.index("next_stage.entry = kernel;")
    t8152 = function(source, "kboot_boot_t8152")
    assert t8152.index("kboot_move_to_exec_window(&kernel)") < t8152.index("next_stage.entry = kernel;")


def test_nothing_reserves_the_window_from_the_os():
    source = kboot_source()
    # Each range that can span the window goes around it: the heap, the image, CTXR_D,
    # a described bootloader handoff and the J713 high-water interval.
    assert source.count("dt_reserve_outside_exec_window(") == 6
    body = function(source, "dt_reserve_bootloader_handoff")
    assert "dt_reserve_outside_exec_window(start, size)" in body and "fdt_add_mem_rsv" not in body
    assert "dt_reserve_outside_exec_window(start, end - start))\n            bail(\"FDT: couldn't reserve the J713" in source
    # The firmware RO range stays reserved in every mode.
    assert "fdt_add_mem_rsv(dt, ro_start, ro_end - ro_start)" in source
    # T8140's firmware RO range covers windows A to C; other chips reserve them on their own.
    assert "chip_id != T8140 && dt_reserve_ctxr_low_windows()" in source


def test_no_option_turns_it_off():
    for name in ["src/boot_options.c", "src/boot_options.h", "src/payload.c", "src/kboot.c"]:
        text = (REPO / name).read_text()
        for gone in ["aura-ctxr-exec", "aura_ctxr_exec", "ctxr_exec_override", "chosen.aura,ctxr-exec",
                     "ctxr-exec-enforce"]:
            assert gone not in text, (name, gone)


def test_a_confined_kernel_keeps_spin_table():
    source = kboot_source()
    assert "psci" not in source.lower()
    assert "smp_get_release_addr(idx)" in function(source, "dt_set_cpus")
