# SPDX-License-Identifier: MIT
"""Place chainloaded images around the T8140 firmware RO range.

The production chainload_image() and its load header check run against a
modelled T8140 memory map with the offsets of a recorded J700 boot (iBoot
26.6.2): phys_base, then the device tree, stage 1's base 0xfa0000 above
phys_base, the 48 KiB firmware RO range 0xa18000 above that base and ending at
top_of_kernel_data, and stage 1's live heap above it. The RO range, and RAM
below stage 1's base, are mapped inaccessible, so any access to them fails the
test. After a successful call the chainload stub's copy and start.S's move to
run_base (exercised on its own by test_start_run_base.py) are modelled, and
the result is checked against what the stage 2 handoff relies on.

The M3 cases (T8122, T6030, T6031) add the coprocessor log buffers iBoot leaves
running: the __OS_LOG segments of /arm-io/<dev>/<nub> segment-ranges, the
panel DCP's 0x120000 above stage 1's base. Failure modes they check:
- the stub's copy to stage 1's base, or start.S's copy of the footprint to a
  run base, writes over a live log (the log pages are mapped inaccessible);
- the firmware block (SEPFW, preoslog, boot_args, ADT copy) is packed onto one;
- the staged copy or a parked firmware block is itself allocated on one;
- an image runs from its staged copy where RVBAR is locked (secondaries would
  reset into the retired stage 1) or without a load header (an older stage 2);
- a segment that is not a log, an empty one, one that only touches the copy,
  a truncated record or one near the top of the address space moves anything;
- the next stage's top_of_kernel_data leaves its staged copy or firmware block
  unreserved, or start.S's M3 move would not land above them.
"""
import re

from test_smp_timeouts import REPO, compile_and_run


def function(source, name):
    match = re.search(r"^(?:static )?(?:int|u64|bool) " + name + r"\([^;{]*\)\s*\{", source, re.M)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def load_header():
    header = (REPO / "src/chainload.h").read_text()
    defines = "\n".join(line for line in header.splitlines()
                        if line.startswith("#define CHAINLOAD_"))
    struct = header[header.index("struct chainload_header {"):]
    return defines + "\n" + struct[:struct.index("};") + 2] + "\n"


def _between(text, start, end):
    start = text.index(start)
    return text[start:text.index(end, start)]


def live_rtkit_record():
    """The record layout, its property names and its maker, from rtkit.h, display.h and display.c,
    and chainload.c's size of the entries it adds to the ADT copy."""
    rtkit_h = (REPO / "src/rtkit.h").read_text()
    display_h = (REPO / "src/display.h").read_text()
    display_c = (REPO / "src/display.c").read_text()
    chainload_c = (REPO / "src/chainload.c").read_text()
    live_prop = next(line for line in display_h.splitlines()
                     if line.startswith("#define DISPLAY_LIVE_ADT_PROP "))
    return "\n".join((
        _between(rtkit_h, "#define RTKIT_LIVE_MAX_BUFFERS", "bool rtkit_get_live_state("),
        live_prop,
        _between(display_h, "#define DISPLAY_LIVE_RTKIT_ADT_PROP", "int display_live_rtkit_record("),
        _between(display_c, "_Static_assert(sizeof(struct display_live_rtkit_record)",
                 "/*\n * The record of the DCP this stage handed on running"),
        _between(chainload_c, "/* DISPLAY_LIVE_ADT_PROP and DISPLAY_LIVE_RTKIT_ADT_PROP",
                 "/* A copied tree has"),
        "typedef struct { const char dcp_alias[8]; } display_config_t;",
        "static const display_config_t *display_get_config(void);",
        "static const struct rtkit_live_state *dcp_live_rtkit_state(void);",
        function(display_c, "display_live_rtkit_record"),
    )) + "\n"


def test_chainload_header_rejects_footprint_outside_file(tmp_path):
    source = (REPO / "src/chainload.c").read_text()
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint8_t u8;
#define SZ_16K 0x4000
''' + load_header() + function(source, "chainload_run_footprint") + r'''
int main(void)
{
    unsigned char image[0x4000] = {0};
    struct chainload_header hdr = {
        CHAINLOAD_HDR_MAGIC, CHAINLOAD_HDR_VERSION, CHAINLOAD_HDR_F_RUN_BASE, 0x4000, 0
    };
    memcpy(image + CHAINLOAD_HDR_OFFSET, &hdr, sizeof(hdr));
    assert(chainload_run_footprint(image, sizeof(image), NULL) == sizeof(image));
    /* Even a complete header cannot describe bytes beyond the supplied file. */
    assert(chainload_run_footprint(image, sizeof(image) - 1, NULL) == 0);
    assert(chainload_run_footprint(image, CHAINLOAD_HDR_OFFSET + sizeof(hdr) - 1, NULL) == 0);
    return 0;
}
'''
    compile_and_run(tmp_path, "chainload-header-file-bounds", harness)


def test_chainload_places_images_around_t8140_firmware_ro(tmp_path):
    source = (REPO / "src/chainload.c").read_text()
    adt_h = (REPO / "src/adt.h").read_text()
    adt_defs = _between(adt_h, "#define ADT_FOREACH_CHILD", "#define ADT_FOREACH_PROPERTY") + \
        _between(adt_h, "struct adt_segment_ranges {", "#endif")
    bodies = function(source, "chainload_run_footprint") + "\n" + \
        function(source, "chainload_meets_live_log") + "\n" + \
        function(source, "chainload_set_adt_range") + "\n" + function(source, "chainload_image")
    harness = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

typedef unsigned long u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
_Static_assert(sizeof(u64) == 8, "production u64 ABI");
#define SZ_16K (1 << 14)
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) > (b)) ? (a) : (b))
#define T8103 0x8103
#define T8112 0x8112
#define T8122 0x8122
#define T8140 0x8140
#define BIT(x) (1UL << (x))
#define T6000 0x6000
#define T6020 0x6020
#define T6030 0x6030
#define T6031 0x6031
#define PACKED __attribute__((packed))
#define sysop(op) ((void)0)
#define ADT_GETPROP_ARRAY(adt, node, name, arr) adt_getprop_copy(adt, node, name, (arr), sizeof(arr))
''' + load_header() + r'''
struct boot_args {
    u16 revision, version;
    u64 virt_base, phys_base, mem_size, top_of_kernel_data;
    void *devtree;
    u32 devtree_size;
    u8 rest[0x300];
};
_Static_assert(offsetof(struct boot_args, phys_base) == 16, "boot_args layout");
_Static_assert(offsetof(struct boot_args, top_of_kernel_data) == 32, "boot_args layout");

/* Offsets of a recorded J700 boot (iBoot 26.6.2), from phys_base 0x1000342c000 */
#define RAM_SIZE      0x8000000UL
#define ADT_OFF       0xed8000UL  /* /chosen devtree 0x10004304000 + 0x68000 */
#define HOME_OFF      0xfa0000UL  /* stage 1 base 0x100043cc000 */
#define RO_OFF        0x19b8000UL /* firmware RO 0x10004de4000 */
#define RO_SIZE       0xc000UL    /* ends at top_of_kernel_data */
#define SEPFW_SIZE    0x5dc000UL
#define SEPFW_OFF     (RO_OFF - SZ_16K - SEPFW_SIZE)
#define HEAP_USED     0x2400000UL /* stage 1 heap: framebuffer copy, loaded file, buffers */
static u64 FOOTPRINT = 0x410000UL;
#define STUB_SIZE     64

static struct boot_args cur_boot_args;
static int chip_id, ro_status, adt_node = 1, setprop_ret;
static u64 ram, home, ro_start, ro_end, live_heap, heap_next, sepfw[2];
static u64 force_alloc, alloc_fail, allocs, frees, last_alloc;
static u8 adt_buf[128];
#define TEST_SEPFW_OFF 64
#define TEST_DEVTREE_OFF 80
static char message[4096];
static void *adt = adt_buf;
static bool handed_on, inherited_dcp;
static u64 retained_heap_base;
static u8 stub_code[STUB_SIZE];
static struct {
    void *entry;
    u64 args[5];
    bool restore_logo;
} next_stage;

#define _chainload_stub_start stub_code
#define _chainload_stub_end (stub_code + STUB_SIZE)

struct adt_property { char name[32]; u32 size; u8 value[]; };
struct adt_node_hdr { u32 property_count, child_count; };
''' + adt_defs + r'''/* RVBAR: locked on T8140, set by smp_start_cpu() on M1 to M3 */
static struct midr_part_features { bool apple_sysregs_unlocked; } features;
static const struct midr_part_features *cpu_features = &features;

/* /arm-io and one nub under each of its devices, whose segment-ranges hold the logs */
#define ARM_IO   100
#define DEV0     200
#define NUB0     300
#define MAX_DEVS 4
static int ndevs, arm_io = ARM_IO;
static struct adt_segment_ranges nub_segs[MAX_DEVS][4];
static u32 nub_len[MAX_DEVS];
static void add_segment(int dev, u64 phys, u32 size, u32 flags)
{
    assert(dev < MAX_DEVS && nub_len[dev] < sizeof(nub_segs[dev]));
    struct adt_segment_ranges r = {phys, 0, phys, size, flags};
    memcpy((u8 *)nub_segs[dev] + nub_len[dev], &r, sizeof(r));
    nub_len[dev] += sizeof(r);
    ndevs = dev + 1 > ndevs ? dev + 1 : ndevs;
}
static int adt_get_child_count(const void *dt, int node)
{
    if (node == ARM_IO)
        return ndevs;
    return node >= DEV0 && node < DEV0 + ndevs;
}
static int adt_first_child_offset(const void *dt, int node)
{
    return node == ARM_IO ? DEV0 : NUB0 + node - DEV0;
}
static int adt_next_sibling_offset(const void *dt, int node) { return node + 1; }
''' + live_rtkit_record() + r'''
/* The DCP this stage handed on running; described unless rtkit_get_live_state() failed */
static bool described;
static const display_config_t display_config = {"dcpext"};
static const struct rtkit_live_state live_session = {
    2, {0x17, 0, 0, 0, 0, 0, 0, 0x80000000}, 2,
    {{1, 0, 0x802424000, 0x9e5010000, 0x10000}, {2, 1, 0x810004000, 0x80696c000, 0x4000}}};
static const display_config_t *display_get_config(void) { return &display_config; }
static const struct rtkit_live_state *dcp_live_rtkit_state(void)
{
    return handed_on && described ? &live_session : NULL;
}
static int display_prepare_live_handoff(void) { return 0; }
static bool display_handed_on_live(void) { return handed_on; }
static bool display_needs_live_receiver(void) { return handed_on || inherited_dcp; }
static bool display_inherited_live_heap(u64 *start)
{
    if (inherited_dcp)
        *start = retained_heap_base;
    return inherited_dcp;
}
static int adt_get_property_count(const void *dt, int node) { return 1; }
static int adt_first_property_offset(const void *dt, int node) { return node + 8; }
static u64 heapblock_high_water(void) { return live_heap; }
static int memory_fw_ro_range(u64 *start, u64 *end)
{
    if (ro_status == 1) {
        *start = ro_start;
        *end = ro_end;
    }
    return ro_status;
}
static int adt_path_offset(const void *dt, const char *path)
{
    assert(dt == adt);
    if (!strcmp(path, "/chosen"))
        return 0;
    if (!strcmp(path, "/arm-io"))
        return arm_io;
    assert(!strcmp(path, "/chosen/memory-map"));
    return adt_node;
}
/* This ADT has no preoslog and no BootArgs entry: their handling must leave the layout alone. */
static const void *adt_getprop(const void *dt, int node, const char *name, u32 *len)
{
    if (node >= NUB0 && node < NUB0 + ndevs) {
        assert(dt == adt && !strcmp(name, "segment-ranges"));
        if (!nub_len[node - NUB0])
            return NULL;
        *len = nub_len[node - NUB0];
        return nub_segs[node - NUB0];
    }
    assert(dt == adt && node == adt_node);
    if (handed_on && (!strcmp(name, "SEPFW") || !strcmp(name, "DeviceTree"))) {
        if (len)
            *len = 2 * sizeof(u64);
        return adt_buf + (!strcmp(name, "SEPFW") ? TEST_SEPFW_OFF : TEST_DEVTREE_OFF);
    }
    assert(!strcmp(name, "preoslog") || !strcmp(name, "BootArgs"));
    return NULL;
}
static int adt_getprop_copy(const void *dt, int node, const char *name, void *out, size_t len)
{
    assert(dt == adt && node == adt_node && !strcmp(name, "SEPFW") && len == sizeof(sepfw));
    memcpy(out, sepfw, len);
    return len;
}
static int adt_setprop(void *dt, int node, const char *name, void *val, size_t len)
{
    assert(dt == adt && node == adt_node && !strcmp(name, "SEPFW") && len == sizeof(sepfw));
    if (!setprop_ret)
        memcpy(sepfw, val, len);
    return setprop_ret;
}
static void *model_memalign(size_t align, size_t size)
{
    assert(align == SZ_16K);
    if (alloc_fail)
        return NULL;
    u64 p = force_alloc ? force_alloc : ALIGN_UP(heap_next, align);
    if (!force_alloc)
        heap_next = p + size;
    allocs++;
    last_alloc = p;
    return (void *)p;
}
static void model_free(void *p)
{
    if (p)
        frees++;
}
/* Every line of one call, so a warning before the summary line is seen too */
static int model_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    size_t used = strlen(message);
    if (used > sizeof(message) / 2)
        used = 0;
    int ret = vsnprintf(message + used, sizeof(message) - used, fmt, ap);
    va_end(ap);
    return ret;
}
static void dc_cvac_range(void *p, size_t n) { (void)p; (void)n; }
static void ic_ivau_range(void *p, size_t n) { (void)p; (void)n; }
#define memalign model_memalign
#define free model_free
#define printf model_printf
#define _base ((char *)home)
''' + bodies + r'''
#undef memalign
#undef free
#undef printf
#undef _base

static char *vars[] = {
    "chosen.asahi,efi-system-partition=befb6483-aac4-4ea1-8bc3-7e1f675e5429",
    "nvme.adopt=live-rtkit-v1",
};
#define NVARS (sizeof(vars) / sizeof(vars[0]))
static u64 sepfw_src;

static void map_ram(void)
{
    static u64 mapped;
    if (!mapped) {
        size_t span = RAM_SIZE + 0x1000000;
        u8 *p = mmap(NULL, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        assert(p != MAP_FAILED);
        mapped = ALIGN_UP((u64)p, 0x1000000UL);
    }
    ram = mapped;
    assert(!mprotect((void *)ram, RAM_SIZE, PROT_READ | PROT_WRITE));
}

/* T8140 unless chip says otherwise; stage 1 heap ends heap_used above top_of_kernel_data */
static void setup(int chip, u64 heap_used)
{
    map_ram();
    chip_id = chip;
    memset(&cur_boot_args, 0, sizeof(cur_boot_args));
    memset(&next_stage, 0, sizeof(next_stage));
    cur_boot_args.revision = 3;
    cur_boot_args.phys_base = ram;
    cur_boot_args.mem_size = RAM_SIZE;
    cur_boot_args.virt_base = 0x1d42c000;
    home = ram + HOME_OFF;
    ro_start = ram + RO_OFF;
    ro_end = ro_start + RO_SIZE;
    cur_boot_args.top_of_kernel_data = ro_end;
    ro_status = chip == T8140 ? 1 : 0;
    live_heap = ro_end + heap_used;
    heap_next = live_heap;
    force_alloc = alloc_fail = allocs = frees = last_alloc = 0;
    setprop_ret = 0;
    handed_on = inherited_dcp = false;
    described = true;
    features.apple_sysregs_unlocked = chip != T8140;
    ndevs = 0;
    arm_io = ARM_IO;
    memset(nub_len, 0, sizeof(nub_len));
    memset(adt_buf, 0, sizeof(adt_buf));
    message[0] = 0;
    sepfw_src = ram + SEPFW_OFF;
    sepfw[0] = sepfw_src;
    sepfw[1] = SEPFW_SIZE;
    memset((void *)home, 0x5a, RO_OFF - HOME_OFF);
    for (u64 i = 0; i < SEPFW_SIZE; i += 8)
        *(u64 *)(sepfw_src + i) = 0x5345504657000000UL | i;
    for (size_t i = 0; i < sizeof(stub_code); i++)
        stub_code[i] = 0xc0 + i;
    if (ro_status == 1 && sysconf(_SC_PAGESIZE) <= SZ_16K) {
        /* Nothing may touch the RO range or RAM below stage 1 */
        assert(!mprotect((void *)ro_start, RO_SIZE, PROT_NONE));
        assert(!mprotect((void *)ram, HOME_OFF, PROT_NONE));
    }
}

/* Image file: header at CHAINLOAD_HDR_OFFSET, then a payload after footprint bytes */
static u8 *make_image(size_t size, u64 magic, u32 version, u32 flags, u64 footprint)
{
    u8 *image = malloc(size);
    assert(image);
    for (size_t i = 0; i < size; i++)
        image[i] = (u8)(i * 7 + (i >> 16));
    if (size >= CHAINLOAD_HDR_OFFSET + sizeof(struct chainload_header)) {
        struct chainload_header hdr = {magic, version, flags, footprint, 0};
        memcpy(image + CHAINLOAD_HDR_OFFSET, &hdr, sizeof(hdr));
    }
    return image;
}

static u64 image_size_for(size_t size)
{
    u64 n = size + 4;
    for (size_t i = 0; i < NVARS; i++)
        n += strlen(vars[i]) + 1;
    return ALIGN_UP(n, SZ_16K);
}

static u64 run_base_field(u64 image)
{
    u64 v;
    memcpy(&v, (void *)(image + CHAINLOAD_HDR_OFFSET + CHAINLOAD_HDR_RUN_BASE_OFF), sizeof(v));
    return v;
}

static bool overlaps(u64 a, u64 a_end, u64 b, u64 b_end) { return a < b_end && b < a_end; }

/* Where an address of the copy the stub makes is before the stub has run */
static u64 staged_view(u64 addr)
{
    u64 dest = next_stage.args[2];
    if (addr >= dest && addr < dest + next_stage.args[3])
        return next_stage.args[1] + (addr - dest);
    return addr;
}

/* What every successful staging must leave for the stub and the next stage */
static void check_staging(const u8 *image, size_t size, u64 fw_base)
{
    u64 isz = image_size_for(size);
    u64 fw_size = ALIGN_UP(SEPFW_SIZE, SZ_16K) + SZ_16K;
    u64 staged = next_stage.args[1], dest = next_stage.args[2], copy = next_stage.args[3];
    struct boot_args *ba = (struct boot_args *)staged_view(next_stage.args[0]);

    assert(next_stage.entry == (void *)(staged + copy));
    assert(!memcmp(next_stage.entry, stub_code, STUB_SIZE));
    assert(next_stage.args[4] == dest + CHAINLOAD_ENTRY_OFFSET);
    assert(dest % SZ_16K == 0 && staged % SZ_16K == 0);
    assert(dest >= home && dest + copy <= ram + RAM_SIZE);
    assert(!memcmp((void *)staged, image, CHAINLOAD_HDR_OFFSET + CHAINLOAD_HDR_RUN_BASE_OFF));
    assert(!memcmp((void *)(staged + CHAINLOAD_HDR_OFFSET + CHAINLOAD_HDR_SIZE),
                   image + CHAINLOAD_HDR_OFFSET + CHAINLOAD_HDR_SIZE,
                   size - CHAINLOAD_HDR_OFFSET - CHAINLOAD_HDR_SIZE));
    u8 *p = (u8 *)staged + size;
    for (size_t i = 0; i < NVARS; i++) {
        size_t len = strlen(vars[i]);
        assert(!memcmp(p, vars[i], len) && p[len] == '\n');
        p += len + 1;
    }
    for (; p < (u8 *)staged + isz; p++)
        assert(!*p);
    assert(sepfw[0] == fw_base && sepfw[1] == SEPFW_SIZE);
    assert(!memcmp((void *)staged_view(fw_base), (void *)sepfw_src, SEPFW_SIZE));
    assert(next_stage.args[0] == fw_base + ALIGN_UP(SEPFW_SIZE, SZ_16K));
    assert(ba->phys_base == cur_boot_args.phys_base && ba->mem_size == cur_boot_args.mem_size);
    assert(ba->virt_base == cur_boot_args.virt_base);
    u64 hw = max(max(dest + isz, fw_base + fw_size), max(live_heap, cur_boot_args.top_of_kernel_data));
    if (ro_status == 1)
        hw = max(hw, ro_end);
    assert(ba->top_of_kernel_data == hw);
    if (ro_status == 1) {
        assert(!overlaps(dest, dest + copy, ro_start, ro_end));
        assert(!overlaps(fw_base, fw_base + fw_size, ro_start, ro_end));
    }
}

/* The chainload stub, then start.S: returns where the image runs and where its payload is */
static void run_next_stage(u64 *run, u64 *payload)
{
    u64 dest = next_stage.args[2];
    memmove((void *)dest, (void *)next_stage.args[1], next_stage.args[3]);
    u64 rb = run_base_field(dest);
    *run = dest;
    *payload = dest + FOOTPRINT;
    if (rb) {
        assert(rb % SZ_16K == 0 && rb + FOOTPRINT <= dest);
        memcpy((void *)rb, (void *)dest, FOOTPRINT);
        *run = rb;
    }
}

static void big_image_runs_from_stage1_base(size_t size)
{
    setup(T8140, HEAP_USED);
    u8 *image = make_image(size, CHAINLOAD_HDR_MAGIC, CHAINLOAD_HDR_VERSION,
                           CHAINLOAD_HDR_F_RUN_BASE, FOOTPRINT);
    u64 isz = image_size_for(size), fw_size = ALIGN_UP(SEPFW_SIZE, SZ_16K) + SZ_16K;
    /* It cannot go where a smaller image goes */
    assert(home + isz > ro_start);

    assert(chainload_image(image, size, vars, NVARS) == 0);
    u64 dest = next_stage.args[2];
    assert(allocs == 1 && !frees && dest == last_alloc && dest == next_stage.args[1]);
    assert(dest >= live_heap && dest >= ro_end);
    assert(next_stage.args[3] == isz + fw_size);
    assert(run_base_field(dest) == home);
    check_staging(image, size, dest + isz);
    assert(strstr(message, "runs its first"));
    /* Stage 1's image and the memory up to the RO range are left alone */
    for (u64 a = home; a < sepfw_src; a += 4096)
        assert(*(u8 *)a == 0x5a);

    u64 run, payload;
    run_next_stage(&run, &payload);
    /* The code runs exactly where a smaller image runs, below the RO range */
    assert(run == home && run + FOOTPRINT <= ro_start);
    assert(run_base_field(run) == home);
    assert(!memcmp((void *)run, image, CHAINLOAD_HDR_OFFSET + CHAINLOAD_HDR_RUN_BASE_OFF));
    assert(!memcmp((void *)(run + CHAINLOAD_HDR_OFFSET + CHAINLOAD_HDR_SIZE),
                   image + CHAINLOAD_HDR_OFFSET + CHAINLOAD_HDR_SIZE,
                   FOOTPRINT - CHAINLOAD_HDR_OFFSET - CHAINLOAD_HDR_SIZE));
    /* payload_start(): relocated_from + (_payload_start - _base) */
    assert(payload == dest + FOOTPRINT && !memcmp((void *)payload, image + FOOTPRINT, size - FOOTPRINT));

    /* kboot_prepare_dt on T8140 reserves [_base, max(_end, top_of_kernel_data)) */
    struct boot_args *ba = (struct boot_args *)next_stage.args[0];
    u64 reserved_end = max(run + FOOTPRINT, ba->top_of_kernel_data);
    assert(run <= ro_start && reserved_end >= ro_end);
    assert(reserved_end >= live_heap && run <= cur_boot_args.top_of_kernel_data);
    assert(reserved_end >= dest + isz + fw_size);
    /* smp_prepare_rvbar: the locked RVBAR (stage 1's vectors) is the running image's */
    assert(run == home);
    free(image);
}

static void fitting_image_is_unchanged(size_t size, bool parks)
{
    setup(T8140, HEAP_USED);
    u8 *image = make_image(size, CHAINLOAD_HDR_MAGIC, CHAINLOAD_HDR_VERSION,
                           CHAINLOAD_HDR_F_RUN_BASE, FOOTPRINT);
    u64 isz = image_size_for(size), fw_size = ALIGN_UP(SEPFW_SIZE, SZ_16K) + SZ_16K;
    assert(home + isz <= ro_start);

    assert(chainload_image(image, size, vars, NVARS) == 0);
    assert(next_stage.args[2] == home);
    assert(run_base_field(next_stage.args[1]) == 0);
    if (parks) {
        /* Its firmware block is parked in the heap; only the image is copied */
        assert(allocs == 2 && next_stage.args[3] == isz);
        assert(sepfw[0] >= live_heap);
        check_staging(image, size, sepfw[0]);
    } else {
        assert(allocs == 1 && next_stage.args[3] == isz + fw_size);
        check_staging(image, size, home + isz);
    }
    assert(!strstr(message, "runs its first"));
    u64 run, payload;
    run_next_stage(&run, &payload);
    assert(run == home && payload == home + FOOTPRINT);
    free(image);
}

static void refused(int chip, u8 *image, size_t size, const char *why)
{
    u64 old_sepfw[2] = {sepfw[0], sepfw[1]};
    (void)chip;
    next_stage.entry = (void *)1;
    assert(chainload_image(image, size, vars, NVARS) == -1);
    assert(!next_stage.entry);
    assert(sepfw[0] == old_sepfw[0] && sepfw[1] == old_sepfw[1]);
    assert(allocs == frees);
    if (why)
        assert(strstr(message, why));
}

static void no_room_is_refused(void)
{
    const size_t big = 11426016;
    u8 *image;

    /* An image without the load header (an older stage 2) cannot run elsewhere */
    setup(T8140, HEAP_USED);
    image = make_image(big, 0, 0, 0, 0);
    refused(T8140, image, big, "cannot run below it");
    free(image);
    /* Nor can one with another header version or without the flag */
    setup(T8140, HEAP_USED);
    image = make_image(big, CHAINLOAD_HDR_MAGIC, CHAINLOAD_HDR_VERSION + 1,
                       CHAINLOAD_HDR_F_RUN_BASE, FOOTPRINT);
    refused(T8140, image, big, "cannot run below it");
    free(image);
    setup(T8140, HEAP_USED);
    image = make_image(big, CHAINLOAD_HDR_MAGIC, CHAINLOAD_HDR_VERSION, 0, FOOTPRINT);
    refused(T8140, image, big, "cannot run below it");
    free(image);
    /* A footprint that is unaligned, larger than the file or than the room below RO */
    u64 footprints[] = {0, FOOTPRINT + 0x2000, big + SZ_16K, RO_OFF - HOME_OFF + SZ_16K};
    for (size_t i = 0; i < sizeof(footprints) / sizeof(footprints[0]); i++) {
        setup(T8140, HEAP_USED);
        image = make_image(big, CHAINLOAD_HDR_MAGIC, CHAINLOAD_HDR_VERSION,
                           CHAINLOAD_HDR_F_RUN_BASE, footprints[i]);
        refused(T8140, image, big, "cannot run below it");
        free(image);
    }
    /* A file that already asks for a run base is malformed */
    setup(T8140, HEAP_USED);
    image = make_image(big, CHAINLOAD_HDR_MAGIC, CHAINLOAD_HDR_VERSION,
                       CHAINLOAD_HDR_F_RUN_BASE, FOOTPRINT);
    memcpy(image + CHAINLOAD_HDR_OFFSET + CHAINLOAD_HDR_RUN_BASE_OFF, &home, sizeof(home));
    refused(T8140, image, big, "cannot run below it");
    free(image);

    image = make_image(big, CHAINLOAD_HDR_MAGIC, CHAINLOAD_HDR_VERSION,
                       CHAINLOAD_HDR_F_RUN_BASE, FOOTPRINT);
    u64 copy = image_size_for(big) + ALIGN_UP(SEPFW_SIZE, SZ_16K) + SZ_16K;
    /* The heap cannot hold the staged copy below the end of RAM */
    setup(T8140, HEAP_USED);
    force_alloc = ram + RAM_SIZE - copy + SZ_16K;
    refused(T8140, image, big, "no room to stage");
    /* ... with the stub that follows it */
    setup(T8140, HEAP_USED);
    force_alloc = ram + RAM_SIZE - copy;
    refused(T8140, image, big, "no room to stage");
    /* ... or has no memory at all */
    setup(T8140, HEAP_USED);
    alloc_fail = 1;
    refused(T8140, image, big, NULL);
    /* A staged copy may not lie on the RO range or below the run range */
    setup(T8140, HEAP_USED);
    force_alloc = ro_start - SZ_16K;
    refused(T8140, image, big, "no room to stage");
    setup(T8140, HEAP_USED);
    force_alloc = home + FOOTPRINT - SZ_16K;
    refused(T8140, image, big, "no room to stage");
    setup(T8140, HEAP_USED);
    force_alloc = home - copy;
    refused(T8140, image, big, "no room to stage");
    /* A stage 1 base that is not 16 KiB aligned cannot be a run base */
    setup(T8140, HEAP_USED);
    home += 0x1000;
    refused(T8140, image, big, "cannot run below it");
    /* An ADT update failure still refuses */
    setup(T8140, HEAP_USED);
    setprop_ret = -1;
    refused(T8140, image, big, NULL);
    /* Without a valid RO range nothing is staged */
    setup(T8140, HEAP_USED);
    ro_status = -1;
    refused(T8140, image, big, NULL);
    assert(!allocs);
    free(image);
}

static void other_chips_are_unchanged(void)
{
    const int chips[] = {T8103, T8112, T8122, T6000, T6020, T6030};
    const size_t big = 11426016;
    for (size_t c = 0; c < sizeof(chips) / sizeof(chips[0]); c++) {
        u8 *image = make_image(big, CHAINLOAD_HDR_MAGIC, CHAINLOAD_HDR_VERSION,
                               CHAINLOAD_HDR_F_RUN_BASE, FOOTPRINT);
        u64 isz = image_size_for(big), fw_size = ALIGN_UP(SEPFW_SIZE, SZ_16K) + SZ_16K;

        /*
         * Room for the staged copy: it stays where it was staged with its firmware block, and
         * the image moves only its code and data to stage 1's base, so the stub copies nothing.
         */
        setup(chips[c], 0x1000000);
        cur_boot_args.top_of_kernel_data = home + isz + fw_size;
        live_heap = heap_next = cur_boot_args.top_of_kernel_data + 0x1000000;
        assert(chainload_image(image, big, vars, NVARS) == 0);
        assert(allocs == 1 && next_stage.args[2] == next_stage.args[1]);
        assert(next_stage.args[3] == isz + fw_size);
        assert(run_base_field(next_stage.args[1]) == home);
        check_staging(image, big, next_stage.args[1] + isz);

        /* Firmware block would reach the live heap: parked, as before */
        setup(chips[c], 0x1000000);
        cur_boot_args.top_of_kernel_data = home + isz;
        live_heap = heap_next = cur_boot_args.top_of_kernel_data + 0x1000000;
        assert(chainload_image(image, big, vars, NVARS) == 0);
        assert(allocs == 2 && next_stage.args[2] == home && next_stage.args[3] == isz);
        assert(run_base_field(next_stage.args[1]) == 0);
        check_staging(image, big, sepfw[0]);

        /*
         * The image itself would reach the live heap: it runs from its staged copy and moves
         * only its code and data to stage 1's base, which stay below the heap.
         */
        setup(chips[c], 0x1000000);
        u64 heap_start = cur_boot_args.top_of_kernel_data;
        assert(home + isz > heap_start);
        assert(chainload_image(image, big, vars, NVARS) == 0);
        u64 dest = next_stage.args[2];
        assert(allocs == 1 && dest == next_stage.args[1] && dest >= live_heap);
        assert(next_stage.args[3] == isz + fw_size);
        assert(run_base_field(dest) == home);
        check_staging(image, big, dest + isz);
        u64 run, payload;
        run_next_stage(&run, &payload);
        assert(run == home && run + FOOTPRINT <= heap_start && payload == dest + FOOTPRINT);

        /* ... but not when its code and data would reach the heap themselves */
        setup(chips[c], 0x1000000);
        cur_boot_args.top_of_kernel_data = home + FOOTPRINT - SZ_16K;
        live_heap = heap_next = cur_boot_args.top_of_kernel_data + 0x1000000;
        refused(chips[c], image, big, "overlaps the live heap");
        assert(!allocs);
        free(image);

        /* ... nor when it has no load header (an older stage 2): refused, as before */
        image = make_image(big, 0, 0, 0, 0);
        setup(chips[c], 0x1000000);
        refused(chips[c], image, big, "overlaps the live heap");
        assert(!allocs);
        free(image);
    }
}

/*
 * T6000 (M1 Pro): iBoot, and m1n1 v1.5.2 when it chainloads, pack the boot object, SEPFW and
 * boot_args, and stage 1's heap starts right after them. A stage 1 smaller than its stage 2 plus
 * that block leaves too little room below the heap to copy the stage 2 to its base.
 */
static void t6000_stage1_before_larger_stage2(size_t stage1, size_t stage2, u64 footprint,
                                              bool staged)
{
    setup(T6000, 0x1000000);
    u64 stage1_end = ALIGN_UP(stage1 + 4, SZ_16K);
    sepfw_src = sepfw[0] = home + stage1_end;
    for (u64 i = 0; i < SEPFW_SIZE; i += 8)
        *(u64 *)(sepfw_src + i) = 0x5345504657000000UL | i;
    u64 heap_start = sepfw_src + ALIGN_UP(SEPFW_SIZE, SZ_16K) + SZ_16K;
    cur_boot_args.top_of_kernel_data = heap_start;
    live_heap = heap_next = heap_start + 0x1000000;
    FOOTPRINT = footprint;
    u8 *image = make_image(stage2, CHAINLOAD_HDR_MAGIC, CHAINLOAD_HDR_VERSION,
                           CHAINLOAD_HDR_F_RUN_BASE, footprint);
    u64 isz = image_size_for(stage2);

    assert(chainload_image(image, stage2, vars, NVARS) == 0);
    u64 dest = next_stage.args[2];
    if (staged) {
        assert(home + isz > heap_start);
        assert(dest == next_stage.args[1] && dest >= live_heap);
        assert(run_base_field(dest) == home);
        check_staging(image, stage2, dest + isz);
    } else {
        assert(home + isz <= heap_start);
        assert(dest == home && !run_base_field(next_stage.args[1]));
        check_staging(image, stage2, sepfw[0]);
    }
    u64 run, payload;
    run_next_stage(&run, &payload);
    assert(run == home && run + footprint <= heap_start);
    assert(!memcmp((void *)payload, image + footprint, stage2 - footprint));
    free(image);
    FOOTPRINT = 0x410000UL;
}

/* Optional built-image check: use the real header and payload, not synthetic bytes. */
static void composed_image_is_placed(void)
{
    const char *path = getenv("NEO_STAGE2_FILE");
    if (!path)
        return;
    FILE *f = fopen(path, "rb");
    assert(f && !fseek(f, 0, SEEK_END));
    long length = ftell(f);
    assert(length > CHAINLOAD_HDR_OFFSET + CHAINLOAD_HDR_SIZE && (u64)length < RAM_SIZE / 2);
    rewind(f);
    size_t size = (size_t)length;
    u8 *image = malloc(size);
    assert(image && fread(image, 1, size, f) == size);
    assert(!fclose(f));
    struct chainload_header hdr;
    memcpy(&hdr, image + CHAINLOAD_HDR_OFFSET, sizeof(hdr));
    assert(hdr.magic == CHAINLOAD_HDR_MAGIC && hdr.footprint <= size);
    FOOTPRINT = hdr.footprint;
    setup(T8140, HEAP_USED);
    bool large = home + image_size_for(size) > ro_start;
    assert(chainload_image(image, size, vars, NVARS) == 0);
    check_staging(image, size, sepfw[0]);
    u64 dest = next_stage.args[2];
    if (large) {
        assert(dest >= live_heap && run_base_field(dest) == home);
    } else {
        assert(dest == home && !run_base_field(next_stage.args[1]));
    }
    u64 run, payload;
    run_next_stage(&run, &payload);
    assert(run == home && run + FOOTPRINT <= ro_start);
    assert(payload == dest + FOOTPRINT);
    assert(!memcmp((void *)payload, image + FOOTPRINT, size - FOOTPRINT));
    struct boot_args *ba = (struct boot_args *)staged_view(next_stage.args[0]);
    assert(ba->top_of_kernel_data >= ro_end && ba->top_of_kernel_data >= live_heap);
    assert(ba->top_of_kernel_data >= dest + image_size_for(size));
    printf("composed image: %zu bytes, footprint %#lx, %s\n", size, FOOTPRINT,
           large ? "staged above firmware RO" : "unchanged fitting path");
    free(image);
}

static void carries_live_display_in_chosen(void)
{
    setup(T8112, HEAP_USED);
    handed_on = true;
    struct adt_node_hdr *node = (void *)adt_buf;
    node->property_count = 1;
    struct adt_property *name = (void *)(node + 1);
    memcpy(name->name, "name", sizeof("name"));
    name->size = sizeof("chosen");
    memcpy(name->value, "chosen", sizeof("chosen"));
    cur_boot_args.devtree_size = sizeof(adt_buf);
    memcpy(adt_buf + TEST_SEPFW_OFF, sepfw, sizeof(sepfw));
    u64 original_tree_range[2] = {ram + ADT_OFF, sizeof(adt_buf)};
    memcpy(adt_buf + TEST_DEVTREE_OFF, original_tree_range, sizeof(original_tree_range));

    u8 image[SZ_16K] = {0};
    struct chainload_header hdr = {
        CHAINLOAD_HDR_MAGIC, CHAINLOAD_HDR_VERSION, CHAINLOAD_HDR_F_RUN_BASE, SZ_16K, 0
    };
    memcpy(image + CHAINLOAD_HDR_OFFSET, &hdr, sizeof(hdr));
    assert(chainload_image(image, sizeof(image), NULL, 0) == -1);
    assert(!next_stage.entry && strstr(message, "cannot inherit the running DCP"));
    /* A receiver built before the RTKit record would boot the kernel without describing it */
    hdr.flags |= CHAINLOAD_HDR_F_LIVE_DCP;
    memcpy(image + CHAINLOAD_HDR_OFFSET, &hdr, sizeof(hdr));
    message[0] = 0;
    assert(chainload_image(image, sizeof(image), NULL, 0) == -1);
    assert(!next_stage.entry && strstr(message, "cannot inherit the running DCP"));
    hdr.flags |= CHAINLOAD_HDR_F_LIVE_RTKIT;
    memcpy(image + CHAINLOAD_HDR_OFFSET, &hdr, sizeof(hdr));
    /* A session this stage cannot describe is handed on to no one */
    described = false;
    assert(chainload_image(image, sizeof(image), NULL, 0) == -1);
    assert(!next_stage.entry && strstr(message, "cannot describe the running DCP's RTKit session"));
    assert(allocs == 0);
    described = true;
    assert(chainload_image(image, sizeof(image), NULL, 0) == 0);
    struct boot_args *ba = (void *)staged_view(next_stage.args[0]);
    u64 dst = (u64)ba->devtree - ba->virt_base + ba->phys_base;
    assert(dst >= live_heap || dst + ba->devtree_size <= cur_boot_args.top_of_kernel_data);
    assert(ba->top_of_kernel_data >= live_heap);
    assert(ba->top_of_kernel_data >= dst + ba->devtree_size);
    struct adt_node_hdr *next = (void *)staged_view(dst);
    struct adt_property *prop = (void *)(next + 1);
    u64 retained;
    assert(next->property_count == 3 && node->property_count == 1);
    assert(!strcmp(prop->name, DISPLAY_LIVE_ADT_PROP) && prop->size == sizeof(retained));
    memcpy(&retained, prop->value, sizeof(retained));
    assert(retained == cur_boot_args.top_of_kernel_data);
    /* The RTKit record follows as a property of its own, then the tree's first property. */
    struct display_live_rtkit_record rec;
    prop = (void *)(prop->value + sizeof(retained));
    assert(!strcmp(prop->name, DISPLAY_LIVE_RTKIT_ADT_PROP) && prop->size == sizeof(rec));
    memcpy(&rec, prop->value, sizeof(rec));
    assert(rec.version == DISPLAY_LIVE_RTKIT_VERSION && rec.size == sizeof(rec));
    assert(!rec.reserved[0] && !rec.reserved[1] && !strcmp(rec.alias, "dcpext"));
    assert(!memcmp(&rec.rtkit, &live_session, sizeof(live_session)));
    assert(!memcmp(prop->value + sizeof(rec), name, sizeof(*name) + 8));
    assert((u8 *)prop->value + sizeof(rec) == (u8 *)next + sizeof(*next) + LIVE_DCP_ADT_ENTRY_SIZE);
    const u8 *copied = (const u8 *)next + LIVE_DCP_ADT_ENTRY_SIZE;
    u64 tree_range[2];
    memcpy(tree_range, copied + TEST_DEVTREE_OFF, sizeof(tree_range));
    assert(tree_range[0] == dst && tree_range[1] == SZ_16K);
    assert(!memcmp(adt_buf + TEST_DEVTREE_OFF, original_tree_range, sizeof(tree_range)));
    assert(!memcmp(adt_buf + TEST_SEPFW_OFF, sepfw, sizeof(sepfw)));
}

/* A stage that inherited a running DCP passes its tree on as is: the next one needs both bits */
static void inherited_dcp_needs_a_record_receiver(void)
{
    setup(T8112, HEAP_USED);
    inherited_dcp = true;
    retained_heap_base = cur_boot_args.top_of_kernel_data - SZ_16K;
    u8 image[SZ_16K] = {0};
    struct chainload_header hdr = {CHAINLOAD_HDR_MAGIC, CHAINLOAD_HDR_VERSION,
                                   CHAINLOAD_HDR_F_RUN_BASE | CHAINLOAD_HDR_F_LIVE_DCP, SZ_16K, 0};
    memcpy(image + CHAINLOAD_HDR_OFFSET, &hdr, sizeof(hdr));
    assert(chainload_image(image, sizeof(image), NULL, 0) == -1);
    assert(!next_stage.entry && strstr(message, "cannot inherit the running DCP"));
    hdr.flags |= CHAINLOAD_HDR_F_LIVE_RTKIT;
    memcpy(image + CHAINLOAD_HDR_OFFSET, &hdr, sizeof(hdr));
    assert(chainload_image(image, sizeof(image), NULL, 0) == 0 && next_stage.entry);
}

/*
 * M3 (T8122, T6030, T6031): the panel DCP's __OS_LOG 0x120000 above stage 1's base and the next
 * coprocessor's right after it, both below top_of_kernel_data, as iBoot leaves them.
 */
#define LOG_OFF  0x120000UL
#define LOG_SIZE 0x28000UL
static void add_m3_logs(void)
{
    /* The DCP's text segment is no log: it may lie on the copy */
    add_segment(0, home, 0x4000, 0x0);
    add_segment(0, home + LOG_OFF, LOG_SIZE, 0xa);
    add_segment(1, home + LOG_OFF + LOG_SIZE, LOG_SIZE, 0xa);
}
static void protect_m3_logs(int prot)
{
    if (sysconf(_SC_PAGESIZE) <= SZ_16K)
        assert(!mprotect((void *)(home + LOG_OFF), 2 * LOG_SIZE, prot));
}

/* What start.S does with an image left where it was staged: an M3 ESP stage 2 moves itself */
static void check_runs_staged(const u8 *image, size_t size)
{
    u64 isz = image_size_for(size), fw_size = ALIGN_UP(SEPFW_SIZE, SZ_16K) + SZ_16K;
    u64 dest = next_stage.args[2];
    assert(dest == next_stage.args[1] && dest >= live_heap);
    assert(next_stage.args[3] == isz + fw_size);
    assert(run_base_field(dest) == 0);
    check_staging(image, size, dest + isz);
    assert(strstr(message, "runs where it is staged") && !strstr(message, "WARNING"));
    u64 run, payload;
    run_next_stage(&run, &payload);
    assert(run == dest && payload == dest + FOOTPRINT);
    assert(!memcmp((void *)payload, image + FOOTPRINT, size - FOOTPRINT));
    /* start.S moves the file part to ALIGN_UP(top_of_kernel_data, 64K): above all of it */
    struct boot_args *ba = (struct boot_args *)next_stage.args[0];
    assert(ALIGN_UP(ba->top_of_kernel_data, 0x10000UL) >= dest + isz + fw_size);
    /* Stage 1's own image below the logs was left alone */
    for (u64 a = home; a < home + LOG_OFF; a += 4096)
        assert(*(u8 *)a == 0x5a);
}

static void m3_live_logs_are_never_written(int chip)
{
    const size_t size = 0x380000;
    u8 *image;

    /* A stage 2 whose code and data (footprint) alone reach the logs: it runs staged */
    FOOTPRINT = 0x200000;
    setup(chip, 0x1000000);
    add_m3_logs();
    protect_m3_logs(PROT_NONE);
    assert(home + image_size_for(size) <= cur_boot_args.top_of_kernel_data);
    image = make_image(size, CHAINLOAD_HDR_MAGIC, CHAINLOAD_HDR_VERSION,
                       CHAINLOAD_HDR_F_RUN_BASE | CHAINLOAD_HDR_F_LIVE_DCP |
                           CHAINLOAD_HDR_F_LIVE_RTKIT,
                       FOOTPRINT);
    assert(chainload_image(image, size, vars, NVARS) == 0);
    assert(allocs == 1);
    check_runs_staged(image, size);
    protect_m3_logs(PROT_READ | PROT_WRITE);

    /* The same without the logs in the ADT: the run base layout of before */
    setup(chip, 0x1000000);
    assert(chainload_image(image, size, vars, NVARS) == 0);
    assert(run_base_field(next_stage.args[1]) == home && !strstr(message, "WARNING"));

    /* Segments that do not meet the copy, or are no logs, change nothing */
    setup(chip, 0x1000000);
    add_segment(0, home - LOG_SIZE, LOG_SIZE, 0xa);   /* ends at our base */
    add_segment(0, home + LOG_OFF, 0, 0xa);           /* empty */
    add_segment(0, home + LOG_OFF, LOG_SIZE, 0xb);    /* other flags */
    add_segment(1, ~0UL - 0x1000, 0x2000, 0xa);       /* wraps the address space */
    add_segment(2, home + 0x20000000UL, 0x4000, 0xa); /* far above the copy */
    add_segment(3, home + LOG_OFF, LOG_SIZE, 0xa);
    nub_len[3] = sizeof(struct adt_segment_ranges) - 1; /* truncated record */
    assert(chainload_image(image, size, vars, NVARS) == 0);
    assert(run_base_field(next_stage.args[1]) == home && !strstr(message, "WARNING"));
    /* ... nor does an ADT without /arm-io */
    setup(chip, 0x1000000);
    add_m3_logs();
    arm_io = -1;
    assert(chainload_image(image, size, vars, NVARS) == 0);
    assert(run_base_field(next_stage.args[1]) == home);

    /* A log that only the first byte past the footprint would reach is still met */
    setup(chip, 0x1000000);
    add_segment(0, home + FOOTPRINT - 1, 1, 0xa);
    assert(chainload_image(image, size, vars, NVARS) == 0);
    assert(run_base_field(next_stage.args[1]) == 0 && strstr(message, "runs where it is staged"));
    /* ... as is one that starts below our base and ends one byte above it */
    setup(chip, 0x1000000);
    add_segment(0, home - 0x4000, 0x4001, 0xa);
    assert(chainload_image(image, size, vars, NVARS) == 0);
    assert(run_base_field(next_stage.args[1]) == 0 && strstr(message, "runs where it is staged"));

    /* The staged copy may not be allocated on a log itself */
    setup(chip, 0x1000000);
    add_m3_logs();
    protect_m3_logs(PROT_NONE);
    force_alloc = home + LOG_OFF - SZ_16K;
    refused(chip, image, size, "no room to stage");
    protect_m3_logs(PROT_READ | PROT_WRITE);

    /* Where RVBAR is locked the image keeps the layout of before, and says why */
    setup(chip, 0x1000000);
    add_m3_logs();
    features.apple_sysregs_unlocked = false;
    assert(chainload_image(image, size, vars, NVARS) == 0);
    assert(run_base_field(next_stage.args[1]) == home && strstr(message, "WARNING"));
    free(image);

    /* An older stage 2 (no load header) is copied to our base as before, with a warning */
    image = make_image(size, 0, 0, 0, 0);
    setup(chip, 0x1000000);
    add_m3_logs();
    assert(chainload_image(image, size, vars, NVARS) == 0);
    assert(next_stage.args[2] == home && strstr(message, "WARNING"));
    free(image);

    /*
     * An image that ends below the logs whose firmware block would reach them: the image is
     * copied to our base, its firmware block parked in the heap.
     */
    const size_t small = 0x100000 - 0x400;
    FOOTPRINT = 0x80000;
    image = make_image(small, CHAINLOAD_HDR_MAGIC, CHAINLOAD_HDR_VERSION,
                       CHAINLOAD_HDR_F_RUN_BASE, FOOTPRINT);
    setup(chip, 0x1000000);
    add_m3_logs();
    protect_m3_logs(PROT_NONE);
    assert(home + image_size_for(small) <= home + LOG_OFF);
    assert(chainload_image(image, small, vars, NVARS) == 0);
    assert(allocs == 2 && next_stage.args[2] == home);
    assert(next_stage.args[3] == image_size_for(small) && sepfw[0] >= live_heap);
    assert(run_base_field(next_stage.args[1]) == 0);
    check_staging(image, small, sepfw[0]);
    u64 run, payload;
    run_next_stage(&run, &payload);
    assert(run == home && payload == home + FOOTPRINT);
    /* ... and that block may not be parked on a log */
    setup(chip, 0x1000000);
    add_m3_logs();
    force_alloc = home + LOG_OFF;
    refused(chip, image, small, NULL);
    assert(allocs == 1);
    protect_m3_logs(PROT_READ | PROT_WRITE);
    free(image);

    /* The image would also reach the live heap and its footprint the logs: it runs staged */
    const size_t big = 11426016;
    FOOTPRINT = 0x410000UL;
    image = make_image(big, CHAINLOAD_HDR_MAGIC, CHAINLOAD_HDR_VERSION,
                       CHAINLOAD_HDR_F_RUN_BASE, FOOTPRINT);
    setup(chip, 0x1000000);
    add_m3_logs();
    protect_m3_logs(PROT_NONE);
    assert(home + image_size_for(big) > cur_boot_args.top_of_kernel_data);
    assert(chainload_image(image, big, vars, NVARS) == 0);
    check_runs_staged(image, big);
    protect_m3_logs(PROT_READ | PROT_WRITE);
    /* ... which a locked RVBAR cannot do: the run base layout of before, with a warning */
    setup(chip, 0x1000000);
    add_m3_logs();
    features.apple_sysregs_unlocked = false;
    assert(chainload_image(image, big, vars, NVARS) == 0);
    assert(run_base_field(next_stage.args[1]) == home && strstr(message, "WARNING"));
    free(image);
    FOOTPRINT = 0x410000UL;
}

/* Optional built-image check: a real M3 ESP stage 2 (AURA_M3_STAGE2_FILE) against the logs */
static void composed_m3_image_is_placed(void)
{
    const char *path = getenv("AURA_M3_STAGE2_FILE");
    if (!path)
        return;
    FILE *f = fopen(path, "rb");
    assert(f && !fseek(f, 0, SEEK_END));
    long length = ftell(f);
    assert(length > CHAINLOAD_HDR_OFFSET + CHAINLOAD_HDR_SIZE && (u64)length < RAM_SIZE / 2);
    rewind(f);
    size_t size = (size_t)length;
    u8 *image = malloc(size);
    assert(image && fread(image, 1, size, f) == size);
    assert(!fclose(f));
    struct chainload_header hdr;
    memcpy(&hdr, image + CHAINLOAD_HDR_OFFSET, sizeof(hdr));
    assert(hdr.magic == CHAINLOAD_HDR_MAGIC && hdr.footprint <= size);
    FOOTPRINT = hdr.footprint;
    setup(T6030, 0x1000000);
    add_m3_logs();
    protect_m3_logs(PROT_NONE);
    assert(chainload_image(image, size, vars, NVARS) == 0);
    bool staged = image_size_for(size) > LOG_OFF;
    if (staged)
        check_runs_staged(image, size);
    else
        assert(next_stage.args[2] == home && !run_base_field(next_stage.args[1]));
    protect_m3_logs(PROT_READ | PROT_WRITE);
    printf("composed M3 image: %zu bytes, footprint %#lx, %s\n", size, FOOTPRINT,
           staged ? "runs staged above the heap" : "copied below the logs");
    free(image);
    FOOTPRINT = 0x410000UL;
}

int main(void)
{
    /* The failing combined stage 2 (aura + 115 DTBs + U-Boot + variables) */
    big_image_runs_from_stage1_base(11426016);
    big_image_runs_from_stage1_base(11413326);
    /* The largest image whose copy still ends below the RO range is unchanged ... */
    size_t largest = RO_OFF - HOME_OFF - 4 - 96;
    fitting_image_is_unchanged(largest, true);
    /* ... and one byte more is staged for a run base */
    big_image_runs_from_stage1_base(largest + 1);
    /* The stage 2 files that booted on a J700 */
    fitting_image_is_unchanged(9277502, true);
    fitting_image_is_unchanged(9469559, true);
    /* One small enough for the packed firmware block */
    fitting_image_is_unchanged(0x400000 - 0x8000, false);
    no_room_is_refused();
    other_chips_are_unchanged();
    /* An M1 Pro stage 2 (aura + DTBs + U-Boot): 0x808000 bytes, footprint 0x430000 */
    t6000_stage1_before_larger_stage2(0x1a0000, 0x808000, 0x430000, true);  /* heap stacks */
    t6000_stage1_before_larger_stage2(0x100088, 0x808000, 0x430000, true);  /* m1n1 1.5.2 size */
    t6000_stage1_before_larger_stage2(0x3b0000, 0x808000, 0x430000, false); /* m1n1 v2.0.0 size */
    t6000_stage1_before_larger_stage2(0x430000, 0x808000, 0x430000, false); /* aura 1.0.0b1 */
    carries_live_display_in_chosen();
    inherited_dcp_needs_a_record_receiver();
    composed_image_is_placed();
    m3_live_logs_are_never_written(T8122);
    m3_live_logs_are_never_written(T6030);
    m3_live_logs_are_never_written(T6031);
    composed_m3_image_is_placed();
    puts("chainload run base matrix PASS");
    return 0;
}
'''
    compile_and_run(tmp_path, "chainload-run-base", harness)
