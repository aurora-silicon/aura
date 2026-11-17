/*
 * Modeled boundary for the shadow-framebuffer replay. Production hv_fb_init(),
 * hv_fb_convert_slice(), hv_fb_idle_due(), hv_fb_convert_idle() and hv_fb_report() are appended
 * verbatim by run.py, plus the previous converter from c470c540 renamed old_*.
 *
 * Memory model: `mem` is DRAM behind the shadow. The guest's non-cacheable writes land there
 * directly; its write-back writes sit dirty in `gdirty` until a clean reaches them. EL2's
 * write-back alias is `cview`, a separate copy that only cache maintenance refreshes from DRAM, so
 * a read without a preceding clean+invalidate returns whatever was cached before.
 */
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned long u64;
typedef long s64;
typedef uint32_t u32;
typedef uint8_t u8;

#define MAX_CPUS 10
#define ALIGNED(x) __attribute__((aligned(x)))
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((u64)(a) - 1))
#define SZ_16K 0x4000
#define HV_WINDOWS_FB_SHADOW 1
#define MAIR_IDX_NORMAL_NC 2
#define PERM_RW 3
#define ISR_EL1 1
#define CNTFRQ_EL0 2

struct video_args { u64 base, display, stride, width, height, depth; };
struct boot_args { struct video_args video; };
static struct boot_args cur_boot_args;
static u64 hv_windows_flags = HV_WINDOWS_FB_SHADOW;

enum { W = 64, H = 48, STRIDE_PX = 72 }; /* small panel with padding columns */
static u32 mem[SZ_16K / 4] ALIGNED(SZ_16K); /* hv_fb_init() allocates and clears whole 16 KiB pages */
static u32 cview[STRIDE_PX * H], gdirty[STRIDE_PX * H];
static bool gdirty_valid[STRIDE_PX * H];
static u32 scanout[STRIDE_PX * H], scanout_old[STRIDE_PX * H];
static bool alias_available = true;

static unsigned cpu;
static u64 clock_now = 1000000000, isr_pending_at = ~0UL;
static u64 maint_cost_per_byte = 2; /* modeled ns of clean+invalidate per byte */
static unsigned civac_calls, mapping_nc_calls, hw_map_calls;

static unsigned smp_id(void) { return cpu; }
static u64 read_sysreg(int r) { return r == CNTFRQ_EL0 ? 1000000000 : (clock_now >= isr_pending_at ? 0x80 : 0); }
#define mrs(r) read_sysreg(r)
#define sysop(op) ((void)0)
u64 hv_host_counter(void) { return clock_now; }

static void *memalign(size_t align, size_t size) { (void)align; assert(size <= sizeof(mem)); return mem; }
static void mmu_add_mapping(u64 from, u64 to, size_t size, int attr, int perm)
{
    (void)to; (void)size; (void)perm;
    assert(from == (u64)mem && attr == MAIR_IDX_NORMAL_NC);
    mapping_nc_calls++;
}
static int hv_map_hw(u64 from, u64 to, u64 size) { (void)from; (void)to; (void)size; hw_map_calls++; return 0; }
static void fb_capture_set_shadow(u64 base, u32 w, u32 h, u64 s) { (void)base; (void)w; (void)h; (void)s; }

/* The shadow's write-back alias: a separate cache view of the same DRAM. */
static const u32 *hv_fb_cached_alias(const u32 *p) { return alias_available && p == mem ? cview : NULL; }

static void maintain(void *addr, size_t len, bool clean)
{
    u8 *a = addr;
    if (a < (u8 *)cview || a >= (u8 *)cview + sizeof(cview))
        return; /* maintenance through another VA leaves the cached view as it was */
    size_t first = (a - (u8 *)cview) / 4, n = len / 4;
    assert(first + n <= STRIDE_PX * H);
    for (size_t i = first; i < first + n; i++) {
        if (gdirty_valid[i] && clean)
            mem[i] = gdirty[i]; /* the guest's dirty line reaches DRAM */
        gdirty_valid[i] = false; /* invalidate drops it from the caches either way */
        cview[i] = mem[i];       /* the next load fills from DRAM */
    }
    clock_now += len * maint_cost_per_byte;
}
static void dc_civac_range(void *addr, size_t len) { civac_calls++; maintain(addr, len, true); }
static void dc_ivac_range(void *addr, size_t len) { maintain(addr, len, false); }

static char console[1 << 16];
static size_t console_len;
static int model_printf(const char *f, ...)
{
    va_list a;
    va_start(a, f);
    int n = vsnprintf(console + console_len, sizeof(console) - console_len, f, a);
    va_end(a);
    console_len += n;
    return n;
}
#define printf model_printf
#define get_ticks hv_host_counter

/* Statics the previous converter (c470c540) used, renamed old_fb_*. */
static u32 *old_fb_shadow = mem, *old_fb_scanout = scanout_old;
static u32 old_fb_stride_px = STRIDE_PX, old_fb_height = H, old_fb_cursor, old_fb_lines;
