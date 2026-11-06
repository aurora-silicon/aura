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
"""
import re

from test_smp_timeouts import REPO, compile_and_run


def function(source, name):
    match = re.search(r"^(?:static )?(?:int|u64) " + name + r"\([^;{]*\)\s*\{", source, re.M)
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
    assert(chainload_run_footprint(image, sizeof(image)) == sizeof(image));
    /* Even a complete header cannot describe bytes beyond the supplied file. */
    assert(chainload_run_footprint(image, sizeof(image) - 1) == 0);
    assert(chainload_run_footprint(image, CHAINLOAD_HDR_OFFSET + sizeof(hdr) - 1) == 0);
    return 0;
}
'''
    compile_and_run(tmp_path, "chainload-header-file-bounds", harness)


def test_chainload_places_images_around_t8140_firmware_ro(tmp_path):
    source = (REPO / "src/chainload.c").read_text()
    bodies = function(source, "chainload_run_footprint") + "\n" + \
        function(source, "chainload_image")
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
#define T6000 0x6000
#define T6020 0x6020
#define T6030 0x6030
#define sysop(op) ((void)0)
#define ADT_GETPROP_ARRAY(adt, node, name, arr) adt_getprop_copy(adt, node, name, (arr), sizeof(arr))
''' + load_header() + r'''
struct boot_args {
    u16 revision, version;
    u64 virt_base, phys_base, mem_size, top_of_kernel_data;
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
static char adt_dummy, message[256];
static void *adt = &adt_dummy;
static u8 stub_code[STUB_SIZE];
static struct {
    void *entry;
    u64 args[5];
    bool restore_logo;
} next_stage;

#define _chainload_stub_start stub_code
#define _chainload_stub_end (stub_code + STUB_SIZE)

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
    assert(dt == adt && !strcmp(path, "/chosen/memory-map"));
    return adt_node;
}
/* This ADT has no preoslog and no BootArgs entry: their handling must leave the layout alone. */
static const void *adt_getprop(const void *dt, int node, const char *name, u32 *len)
{
    assert(dt == adt && node == adt_node);
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
static int model_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(message, sizeof(message), fmt, ap);
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
    composed_image_is_placed();
    puts("chainload run base matrix PASS");
    return 0;
}
'''
    compile_and_run(tmp_path, "chainload-run-base", harness)
