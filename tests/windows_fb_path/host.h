/*
 * Modeled boundary for the shadow-framebuffer replay. Production hv_fb_init(), the cached-read
 * self-test (hv_fb_selftest*), hv_fb_convert_slice(), hv_fb_idle_due(), hv_fb_convert_idle() and
 * hv_fb_report() are appended verbatim by run.py, plus the previous converter from c470c540
 * renamed old_*.
 *
 * Memory model: `mem` is DRAM behind the shadow. The guest's non-cacheable writes land there
 * directly; its write-back writes sit dirty in `gdirty` until a clean reaches them. Each CPU has its
 * own cache of EL2's write-back alias (cache_val/cache_valid). A read of a line the CPU does not
 * hold fills from another CPU that holds it (a coherent snoop; a stale copy wins) or else from
 * DRAM, and non-cacheable writes touch no cache. `cview` is what the running CPU reads through the
 * alias: rebuilt when a modeled smp call switches CPUs, refreshed only by cache maintenance, so a
 * read without a preceding clean+invalidate returns whatever was cached. A CPU that ran a reading
 * step keeps what it read.
 *
 * Maintenance models (e2e.c variants): every CPU (correct hardware); none (DC by VA disabled, as
 * Apple's DISABLE_DC_MVA chicken bit does); only the issuing CPU; only the issuing cluster; and
 * 32-byte lines under the 64-byte stride of dc_civac_range(). Set/way flushes always work.
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
#define SZ_1M 0x100000
#define HV_WINDOWS_FB_SHADOW 1
#define MAIR_IDX_NORMAL_NC 2
#define PERM_RW 3
#define ISR_EL1 1
#define CNTFRQ_EL0 2
#define MPIDR_EL1 3
#define SCTLR_EL1 4
#define SCTLR_M 1UL
#define MPIDR_CORE_TYPE_P (1UL << 16)
#define MPIDR_CORE_TYPE_M (1UL << 18)
#define SMP_TIMEOUT -2
#define DCSW_OP_DCCISW 0x1

struct video_args { u64 base, display, stride, width, height, depth; };
struct boot_args { struct video_args video; };
static struct boot_args cur_boot_args;
static u64 hv_windows_flags = HV_WINDOWS_FB_SHADOW;

/*
 * A panel just over 1 MiB, so the self-test's region fits in the shadow; padding columns kept, and
 * rows 64-byte aligned like the J813 panel's (2560 px, 10240-byte stride).
 */
enum { W = 512, H = 512, STRIDE_PX = 528, WORDS = STRIDE_PX * H, TEST_WORDS = SZ_1M / 4 };
static u32 mem[ALIGN_UP(WORDS * 4, SZ_16K) / 4] ALIGNED(SZ_16K); /* whole 16 KiB pages */
static u32 cview[WORDS], gdirty[WORDS];
static bool gdirty_valid[WORDS];
static u32 scanout[WORDS], scanout_old[WORDS];
static bool alias_available = true;

static unsigned cpu; /* the CPU running production code: smp_id() */
static u64 clock_now = 1000000000, isr_pending_at = ~0UL;
static u64 maint_cost_per_byte = 2; /* modeled ns of clean+invalidate per byte */
static unsigned civac_calls, mapping_nc_calls, hw_map_calls, setway_flushes;

enum maint_model { MAINT_ALL, MAINT_NONE, MAINT_LOCAL, MAINT_CLUSTER, MAINT_LINE32 };
static enum maint_model maint_model = MAINT_ALL;
static u32 cache_val[MAX_CPUS][WORDS];
static bool cache_valid[MAX_CPUS][WORDS];
static unsigned cache_held[MAX_CPUS]; /* valid words per CPU */
static unsigned view_cpu;             /* whose view `cview` is */
static u64 model_mpidr[MAX_CPUS];
static bool alive[MAX_CPUS], mmu_on[MAX_CPUS];
static bool hv_started_cpus[MAX_CPUS], hv_setup_pending[MAX_CPUS];

enum exc_guard_t { GUARD_OFF = 0, GUARD_SKIP, GUARD_MARK, GUARD_RETURN, GUARD_TYPE_MASK = 0xff, GUARD_SILENT = 0x100 };
static volatile enum exc_guard_t exc_guard;
static volatile int exc_count;

static unsigned smp_id(void) { return cpu; }
static u64 read_sysreg(int r)
{
    switch (r) {
        case CNTFRQ_EL0: return 1000000000;
        case MPIDR_EL1: return model_mpidr[cpu] | (1UL << 31);
        case SCTLR_EL1: return mmu_on[cpu] ? SCTLR_M : 0;
        default: return clock_now >= isr_pending_at ? 0x80 : 0; /* ISR_EL1 */
    }
}
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

/* The shadow's write-back alias: per-CPU cache views of the same DRAM. */
static const u32 *hv_fb_cached_alias(const u32 *p) { return alias_available && p == mem ? cview : NULL; }

static unsigned holders; /* bit c: CPU c holds at least one word */
static bool same_cluster(unsigned a, unsigned b) { return ((model_mpidr[a] ^ model_mpidr[b]) & 0xffff00) == 0; }
static void drop(unsigned c, size_t i)
{
    if (cache_valid[c][i]) {
        cache_valid[c][i] = false;
        if (!--cache_held[c])
            holders &= ~(1u << c);
    }
}
/* What CPU c reads for a word it does not hold. */
static u32 fill(unsigned c, size_t i)
{
    if (holders & ~(1u << c))
        for (unsigned k = 0; k < MAX_CPUS; k++)
            if (k != c && cache_held[k] && cache_valid[k][i] && cache_val[k][i] != mem[i])
                return cache_val[k][i];
    return mem[i];
}
static void view_load(unsigned c)
{
    view_cpu = c;
    for (size_t i = 0; i < WORDS; i++)
        cview[i] = cache_valid[c][i] ? cache_val[c][i] : fill(c, i);
}
/* CPU c read the whole view: it now holds every line it read. */
static void view_keep(unsigned c)
{
    assert(c == view_cpu);
    for (size_t i = 0; i < WORDS; i++) {
        if (!cache_valid[c][i]) {
            cache_valid[c][i] = true;
            cache_held[c]++;
        }
        cache_val[c][i] = cview[i];
    }
    holders |= 1u << c;
}
/* Clean+invalidate by set/way of CPU c's caches (EL2 never writes the alias: nothing dirty). */
static void flush_core(unsigned c)
{
    for (size_t i = 0; i < WORDS; i++)
        drop(c, i);
    if (c == view_cpu)
        for (size_t i = 0; i < WORDS; i++)
            cview[i] = fill(c, i);
}

static void maintain(void *addr, size_t len, bool clean)
{
    u8 *a = addr;
    if (a < (u8 *)cview || a >= (u8 *)cview + sizeof(cview))
        return; /* maintenance through another VA leaves the cached view as it was */
    size_t first = (a - (u8 *)cview) / 4, n = len / 4;
    assert(first + n <= WORDS);
    clock_now += len * maint_cost_per_byte;
    if (maint_model == MAINT_NONE)
        return;
    for (size_t i = first; i < first + n; i++) {
        if (maint_model == MAINT_LINE32 && (i * 4) % 64 >= 32)
            continue; /* a 32-byte line the 64-byte stride stepped over */
        if (gdirty_valid[i] && clean)
            mem[i] = gdirty[i]; /* the guest's dirty line reaches DRAM */
        gdirty_valid[i] = false; /* invalidate drops it from the caches either way */
        for (unsigned k = 0; holders && k < MAX_CPUS; k++)
            if (cache_held[k] && (k == view_cpu || maint_model == MAINT_ALL || maint_model == MAINT_LINE32 ||
                                  (maint_model == MAINT_CLUSTER && same_cluster(k, view_cpu))))
                drop(k, i);
        cview[i] = fill(view_cpu, i); /* the next load fills */
    }
}
static void dc_civac_range(void *addr, size_t len) { civac_calls++; maintain(addr, len, true); }
static void dc_ivac_range(void *addr, size_t len) { maintain(addr, len, false); }

/* Secondaries, their MMU and set/way flushes; smp_call4()/smp_wait_timed() live in e2e.c. */
static int smp_call4(int target, void *func, u64 a0, u64 a1, u64 a2, u64 a3);
static int smp_wait_timed(int target, u64 *retval, u32 timeout_ms);
static bool smp_is_alive(int c) { return c >= 0 && c < MAX_CPUS && alive[c]; }
static u64 smp_get_mpidr(int c) { return c >= 0 && c < MAX_CPUS ? model_mpidr[c] & 0xffffff : 0; }
static void mmu_init_secondary_local(void) { assert(cpu != 0); mmu_on[cpu] = true; }
static u64 mmu_disable(void)
{
    if (!mmu_on[cpu])
        return 0;
    mmu_on[cpu] = false;
    flush_core(cpu);
    return SCTLR_M;
}
static void dcsw_op_all(u64 op)
{
    assert(op == DCSW_OP_DCCISW);
    setway_flushes++;
    flush_core(cpu);
}

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
