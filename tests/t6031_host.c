/* SPDX-License-Identifier: MIT */
/*
 * Host test of the T6031 (M3 Max J514C/J516C) handoff on one Mac's ADT: the __OS_LOG reservations
 * and display handoff (kboot_display_handoff.c), the GPU firmware facts, the bounded powered
 * identity read and the GPU setup (kboot_gpu.c, kboot_gpu_t6031.c), the PMP values (kboot.c's
 * dt_pass_pmp(), copied out by the run script) and the firmware image UUIDs (kboot_t6031.c).
 * aura's own ADT reader (rust/src/adt.rs, built for the host), PMGR code (pmgr.c), firmware
 * identification (firmware.c) and libfdt run unchanged. Only the bus is simulated.
 *
 *   t6031-host <mac.adt> <in.dtb> <scenario>      (tests/run-t6031-handoff.sh builds both)
 *   t6031-host <mac.adt> <in.dtb> list
 *
 * The ADT comes from the Mac's allowlist through tests/t6031_adt.py; what the allowlist lacks
 * (/chosen, the carveout map, /vram, the image UUIDs) is synthetic and listed by that script.
 * What no ADT can say is simulated here and marked SIMULATED where it is set: the PMGR power
 * states, the DCP and DART registers and page tables (which display stream maps which segment
 * follows the J613), the GPU ID, topology and core mask registers (a 40-core G15C) and the GPU
 * firmware's image-info header and RTKit version. The scenarios check what aura publishes and,
 * on every failure path, that the hardware is left as it was found.
 */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adt.h"
#include "exception.h"
#include "firmware.h"
#include "kboot_gpu_t6031.h"
#include "kboot_t6031.h"
#include "mcc.h"
#include "pmgr.h"
#include "soc.h"
#include "types.h"
#include "utils.h"

#include "libfdt/libfdt.h"

#undef printf

int dt_set_display_handoff(void *dt);
int dt_set_gpu(void *dt);
void host_pmp_t6031(void *dt); /* the run script's copy of kboot.c's dt_set_pmp_t6031() */

/* ---------- aura globals and stubs ---------- */

void *adt;
static u32 adt_size;
u32 board_id, chip_id;
volatile enum exc_guard_t exc_guard;
volatile int exc_count;
uint64_t ram_base;
u64 boot_flags, mem_size_actual;
size_t mcc_carveout_count;
struct mcc_carveout mcc_carveouts[4];

u32 adt_get_size(void)
{
    return adt_size;
}

static char log_buf[1 << 18];
static size_t log_len;

int debug_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(log_buf + log_len, sizeof(log_buf) - log_len, fmt, ap);
    va_end(ap);
    if (n > 0)
        log_len = min(log_len + n, sizeof(log_buf) - 1);
    return n;
}

/*
 * Boot arguments and stage 1's load address, from this Mac's report: usable RAM from the first
 * System RAM range of iomem.txt (as on a J613, the GPU firmware lies just below it) to the end of
 * aura's top-of-memory allocations; the boot framebuffer of system.txt.
 */
static u64 ram_min = 0x1000223c000UL, ram_len = 0x10bca4b0000UL - 0x1000223c000UL;
static u64 fb_base = 0x10bd1485c00UL;
u64 kboot_ram_base(void)
{
    return ram_min;
}
u64 kboot_ram_size(void)
{
    return ram_len;
}
u64 kboot_fb_base(void)
{
    return fb_base;
}
void kboot_loaded_file_part(u64 *base, u64 *size)
{
    *base = 0x10004f5c000UL; /* the 16K aura keeps for its old reset vector, in iomem.txt */
    *size = 0xe0000;         /* this build's file part */
}
u64 top_of_memory_alloc(size_t size)
{
    (void)size;
    abort();
}
int32_t rust_gpu_initdata_size(uint32_t a, uint32_t b, size_t *c, size_t *d, size_t *e)
{
    (void)a, (void)b, (void)c, (void)d, (void)e;
    return -1;
}
struct initdata_inputs;
int32_t rust_fill_gpu_initdata(struct initdata_inputs *a, void *b, void *c, void *d)
{
    (void)a, (void)b, (void)c, (void)d;
    return -1;
}

/* ---------- the simulated bus ---------- */

#define PS_GFX      0x408e80110UL /* GFX's power state register, from the J516C ADT's PMGR table */
#define PS_AFR      0x408e80100UL
#define PS_GPX      0x408e80000UL
#define PS_DISP_SYS 0x292280200UL
#define PS_DISP_FE  0x292280298UL
#define PS_DISP_CPU 0x2922802a0UL

#define SGX        0x408000000UL
#define DCP_COPROC 0x386c00000UL
#define DART_DCP   0x38530c000UL
#define DART_DISP0 0x385304000UL

enum { R_PLAIN, R_PS, R_GPU, R_FAULT };

struct reg {
    u64 addr;
    u32 val;
    int kind;
};
static struct reg regs[1024];
static int nregs;

struct region {
    u64 base, size;
    u8 *buf;
};
static struct region dram[16];
static int ndram;

struct write {
    u64 addr;
    u32 old, val;
};
static struct write writes[4096];
static int nwrites, unguarded, gpu_reads, gpu_reads_unpowered;
static u64 stuck_off, stuck_on; /* PS registers whose actual state ignores a power-on/off */

static bool ps_window(u64 a)
{
    return (a >= 0x292280000UL && a < 0x292300000UL) || (a >= 0x292800000UL && a < 0x292a00000UL) ||
           (a >= 0x2a0280000UL && a < 0x2a0300000UL) || (a >= 0x408e80000UL && a < 0x408e90000UL);
}

static struct reg *reg_find(u64 a)
{
    for (int i = 0; i < nregs; i++)
        if (regs[i].addr == a)
            return &regs[i];
    if (!ps_window(a))
        return NULL;
    /* A PMGR power state this test does not set: power-gated, no automatic power management */
    assert(nregs < (int)ARRAY_SIZE(regs));
    regs[nregs] = (struct reg){a, 0, R_PS};
    return &regs[nregs++];
}

static void reg_set(u64 a, u32 v, int kind)
{
    struct reg *r = reg_find(a);
    if (!r) {
        assert(nregs < (int)ARRAY_SIZE(regs));
        r = &regs[nregs++];
        r->addr = a;
    }
    r->val = v;
    r->kind = kind;
}

static u32 reg_val(u64 a)
{
    struct reg *r = reg_find(a);
    assert(r);
    return r->val;
}

static u8 *dram_at(u64 a, u32 len)
{
    for (int i = 0; i < ndram; i++)
        if (a >= dram[i].base && a + len <= dram[i].base + dram[i].size)
            return dram[i].buf + (a - dram[i].base);
    return NULL;
}

static void dram_add(u64 base, u64 size)
{
    assert(ndram < (int)ARRAY_SIZE(dram));
    dram[ndram++] = (struct region){base, size, calloc(1, size)};
}

static u32 fault(const char *what, u64 a)
{
    if ((exc_guard & GUARD_TYPE_MASK) == GUARD_MARK) {
        exc_count++;
    } else {
        unguarded++;
        fprintf(stdout, "UNGUARDED %s at 0x%lx\n", what, (unsigned long)a);
    }
    return 0xffffffff;
}

static bool gfx_on(void)
{
    return FIELD_GET(GENMASK(7, 4), reg_val(PS_GFX)) == 0xf;
}

u32 host_read32(u64 a)
{
    u8 *p = dram_at(a, 4);
    if (p) {
        u32 v;
        memcpy(&v, p, 4);
        return v;
    }
    struct reg *r = reg_find(a);
    if (!r || r->kind == R_FAULT)
        return fault("read32", a);
    if (r->kind == R_GPU) {
        gpu_reads++;
        if (!gfx_on()) {
            gpu_reads_unpowered++;
            return fault("read32 of an unpowered GPU register", a);
        }
    }
    return r->val;
}

u64 host_read64(u64 a)
{
    u8 *p = dram_at(a, 8);
    if (!p)
        return fault("read64", a);
    u64 v;
    memcpy(&v, p, 8);
    return v;
}

u8 host_read8(u64 a)
{
    u8 *p = dram_at(a, 1);
    if (!p)
        return fault("read8", a);
    return *p;
}

void host_write32(u64 a, u32 v)
{
    struct reg *r = reg_find(a);
    if (!r || (r->kind != R_PS && r->kind != R_PLAIN)) {
        fault("write32", a);
        return;
    }
    assert(nwrites < (int)ARRAY_SIZE(writes));
    writes[nwrites++] = (struct write){a, r->val, v};
    if (r->kind == R_PS) {
        /* The target and automatic power management bits stick; the actual state follows */
        u32 target = v & 0xf, actual = target;
        if (a == stuck_off && target == 0xf)
            actual = FIELD_GET(GENMASK(7, 4), r->val);
        if (a == stuck_on && target != 0xf)
            actual = 0xf;
        v = (v & ~(GENMASK(7, 4) | BIT(8) | BIT(9))) | (actual << 4);
    }
    r->val = v;
}

void udelay(u32 d)
{
    (void)d;
}

/* ---------- the ADT ---------- */

static struct adt_property *adt_prop(const char *path, const char *name)
{
    int node = adt_path_offset(adt, path);
    assert(node >= 0);
    return (struct adt_property *)adt_get_property(adt, node, name);
}

static void adt_hide(const char *path, const char *name)
{
    struct adt_property *p = adt_prop(path, name);
    assert(p);
    p->name[0] = 'x'; /* no longer found by name */
}

static void adt_set_u32(const char *path, const char *name, u32 v)
{
    struct adt_property *p = adt_prop(path, name);
    assert(p && p->size == 4);
    memcpy(p->value, &v, 4);
}

static const struct adt_segment_ranges *adt_segs(const char *path, int *n)
{
    struct adt_property *p = adt_prop(path, "segment-ranges");
    assert(p && p->size % sizeof(struct adt_segment_ranges) == 0);
    *n = p->size / sizeof(struct adt_segment_ranges);
    return (const struct adt_segment_ranges *)p->value;
}

static void adt_pair(const char *path, const char *name, u64 *a, u64 *b)
{
    struct adt_property *p = adt_prop(path, name);
    assert(p && p->size == 16);
    memcpy(a, p->value, 8);
    memcpy(b, p->value + 8, 8);
}

static const char *adt_str(const char *path, const char *name)
{
    struct adt_property *p = adt_prop(path, name);
    return p ? (const char *)p->value : NULL;
}

/* ---------- the simulated Mac ---------- */

#define PTE_ADDR(x) (((x) >> 4) & 0xffffffffc00UL)

struct pt {
    u64 lo, next;
};

static void pt_init(struct pt *t, u64 lo, u64 hi)
{
    dram_add(lo, hi - lo);
    t->lo = lo;
    t->next = lo + 0x4000;
}

static void pt_map(struct pt *t, u64 dva, u64 pa, u64 size)
{
    for (u64 o = 0; o < size; o += 0x4000) {
        u64 d = dva + o, i = d >> 25, j = (d >> 14) & 2047;
        u64 e1 = host_read64(t->lo + 8 * i);
        if (!(e1 & 1)) {
            e1 = PTE_ADDR(t->next) | 1;
            t->next += 0x4000;
            memcpy(dram_at(t->lo + 8 * i, 8), &e1, 8);
        }
        u64 l2 = (e1 & 0xffffffffc00UL) << 4, e2 = PTE_ADDR(pa + o) | 3;
        assert(dram_at(l2 + 8 * j, 8));
        memcpy(dram_at(l2 + 8 * j, 8), &e2, 8);
    }
}

static u32 ttbr_of(u64 l1)
{
    return (u32)(((l1 >> 14) << 2) | 1);
}

/* The display streams' segments: SIMULATED, as two J613s map theirs */
#define SID0_SEGS ((1u << 4) | (1u << 6))
#define SID4_SEGS (1u << 5)

/* SIMULATED GPU identity: a 40-core G15C (family 7, variant 4), one die of 4 clusters */
static u32 sim_id_version = 0x07041200, sim_counts1 = 0x0001040a, sim_counts2 = 0x00040404;
static u32 sim_mask[3] = {0xffffffff, 0xff, 0};
static const u8 sim_image_uuid[16] = {0x5e, 0x5e, 0x5e, 0x5e, 0, 0, 0x40, 0,
                                      0x80, 0,    0,    0,    0, 0, 0,    0x01};
static const char *sim_rtkit = "RTKit-2419.140.12.release";

struct opts {
    bool stray_map, dcp_stopped, display_off, gpu_fault;
};

/* Display power off as kboot finds it: set after pmgr_init(), which powers parents of active
 * devices */
static bool display_off_after_init;

static void world(const struct opts *o)
{
    nregs = ndram = nwrites = unguarded = gpu_reads = gpu_reads_unpowered = 0;
    stuck_off = stuck_on = 0;
    exc_guard = GUARD_OFF;
    exc_count = 0;
    log_len = 0;
    log_buf[0] = 0;
    chip_id = T6031;
    os_firmware = fw_versions[V14_7];
    ram_base = 0x10000000000UL;
    mem_size_actual = 0xc00000000UL;
    mcc_carveout_count = 0;

    /* SIMULATED PMGR: GPX and AFR on (AFR with automatic power management), GFX off, display on */
    reg_set(PS_GPX, 0xff, R_PS);
    reg_set(PS_AFR, 0x100000ff, R_PS);
    reg_set(PS_GFX, 0x0, R_PS);
    reg_set(PS_DISP_SYS, 0xff, R_PS);
    reg_set(PS_DISP_FE, 0xff, R_PS);
    reg_set(PS_DISP_CPU, 0xff, R_PS);
    display_off_after_init = o->display_off;

    /* SIMULATED GPU ID registers, readable only while GFX is on */
    int gk = o->gpu_fault ? R_FAULT : R_GPU;
    reg_set(SGX + 0xd04000, sim_id_version, gk);
    reg_set(SGX + 0xd04010, sim_counts1, gk);
    reg_set(SGX + 0xd04014, sim_counts2, gk);
    for (int i = 0; i < 3; i++)
        reg_set(SGX + 0xe01500 + 4 * i, sim_mask[i], gk);

    /* SIMULATED firmware image-info header and RTKit version, at the ADT's GFX text segment */
    int n;
    const struct adt_segment_ranges *gseg = adt_segs("/arm-io/gfx-asc", &n);
    assert(n == 2);
    u64 text = gseg[0].phys;
    dram_add(text + 0x4000, 0x4000);
    dram_add(text + 0x5c000, 0x8000);
    u32 hdr[14] = {0x14000011, 0x64697575, 5, 0x5c100, 0x5c200};
    memcpy(&hdr[5], sim_image_uuid, 16);
    hdr[11] = 0x5d54a;
    hdr[12] = 0x1e8;
    hdr[13] = gseg[0].size;
    memcpy(dram_at(text + 0x4200, sizeof(hdr)), hdr, sizeof(hdr));
    strcpy((char *)dram_at(text + 0x5c100, 64), sim_rtkit);

    /* SIMULATED DCP and DARTs: running, dart-disp0 locked (kboot.c's dt_set_display does that) */
    reg_set(DCP_COPROC + 0x44, o->dcp_stopped ? 0 : 0x10, R_PLAIN);
    reg_set(DCP_COPROC + 0x48, 0x3000ac, R_PLAIN);
    reg_set(DART_DCP + 0x200, 0x1e0, R_PLAIN);
    reg_set(DART_DISP0 + 0x200, 0x1e1, R_PLAIN);
    for (int sid = 0; sid < 16; sid++) {
        reg_set(DART_DCP + 0x1000 + 4 * sid, 0, R_PLAIN);
        reg_set(DART_DCP + 0x1400 + 4 * sid, 0, R_PLAIN);
        reg_set(DART_DISP0 + 0x1000 + 4 * sid, 0, R_PLAIN);
        reg_set(DART_DISP0 + 0x1400 + 4 * sid, 0, R_PLAIN);
    }
    u64 lo, hi;
    struct pt p5, p0, p4;
    adt_pair("/arm-io/dart-dcp", "pt-region-5", &lo, &hi);
    pt_init(&p5, lo, hi);
    adt_pair("/arm-io/dart-disp0", "pt-region-0", &lo, &hi);
    pt_init(&p0, lo, hi);
    adt_pair("/arm-io/dart-disp0", "pt-region-4", &lo, &hi);
    pt_init(&p4, lo, hi);
    const struct adt_segment_ranges *seg = adt_segs("/arm-io/dcp/iop-dcp-nub", &n);
    for (int i = 0; i < n; i++) {
        u64 size = ALIGN_UP((u64)seg[i].size, SZ_16K), dva = seg[i].remap & ((1UL << 36) - 1);
        if (seg[i].unk == 0xa)
            continue;
        pt_map(&p5, dva, seg[i].phys, size);
        if (SID0_SEGS & (1u << i))
            pt_map(&p0, dva, seg[i].phys, size);
        if (SID4_SEGS & (1u << i))
            pt_map(&p4, dva, seg[i].phys, size);
    }
    if (o->stray_map) /* a display page outside every firmware segment */
        pt_map(&p0, 0x5000000, 0x10010000000UL, 0x4000);
    reg_set(DART_DCP + 0x1000 + 4 * 5, 1, R_PLAIN);
    reg_set(DART_DCP + 0x1400 + 4 * 5, ttbr_of(p5.lo), R_PLAIN);
    reg_set(DART_DISP0 + 0x1000, 1, R_PLAIN);
    reg_set(DART_DISP0 + 0x1400, ttbr_of(p0.lo), R_PLAIN);
    reg_set(DART_DISP0 + 0x1000 + 4 * 4, 1, R_PLAIN);
    reg_set(DART_DISP0 + 0x1400 + 4 * 4, ttbr_of(p4.lo), R_PLAIN);
}

/* ---------- checks ---------- */

static int failures;

static void check(bool ok, const char *fmt, ...)
{
    va_list ap;
    if (!ok)
        failures++;
    fputs(ok ? "ok   " : "FAIL ", stdout);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fputs("\n", stdout);
}

static int node_at(const void *dt, const char *path)
{
    return fdt_path_offset(dt, path);
}

static const char *status_of(const void *dt, const char *path)
{
    int node = node_at(dt, path);
    const char *s = node < 0 ? NULL : fdt_getprop(dt, node, "status", NULL);
    return s ? s : "(none)";
}

static const void *prop(const void *dt, const char *path, const char *name, int *len)
{
    int node = node_at(dt, path);
    return node < 0 ? NULL : fdt_getprop(dt, node, name, len);
}

static bool u32_is(const void *dt, const char *path, const char *name, u32 want)
{
    int len;
    const fdt32_t *v = prop(dt, path, name, &len);
    return v && len == 4 && fdt32_ld(v) == want;
}

static u32 u32_of(const void *dt, const char *path, const char *name)
{
    int len;
    const fdt32_t *v = prop(dt, path, name, &len);
    return v && len == 4 ? fdt32_ld(v) : 0xdeadbeef;
}

/* Every simulated register is as it was in `before`, except writes to `allowed` */
static int changed_registers(const struct reg *before, int nbefore, u64 allowed)
{
    int changed = 0;
    for (int i = 0; i < nbefore; i++) {
        struct reg *r = reg_find(before[i].addr);
        if (r->val != before[i].val && r->addr != allowed) {
            fprintf(stdout, "     register 0x%lx: 0x%x -> 0x%x\n", (unsigned long)r->addr,
                    before[i].val, r->val);
            changed++;
        }
    }
    return changed;
}

static int count_writes(u64 lo, u64 hi)
{
    int n = 0;
    for (int i = 0; i < nwrites; i++)
        n += writes[i].addr >= lo && writes[i].addr < hi;
    return n;
}

static bool log_has(const char *s)
{
    return strstr(log_buf, s) != NULL;
}

/* The five __OS_LOG buffers of the ADT, reserved no-map at their physical addresses */
static void check_oslogs(const void *dt, bool want)
{
    static const char *const owners[] = {"dcp", "dcpext0", "dcpext1", "dcpext2", "dcpext3"};
    int found = 0;
    for (int i = 0; i < 5; i++) {
        char nub[64], name[64];
        int n;
        snprintf(nub, sizeof(nub), "/arm-io/%s/iop-%s-nub", owners[i], owners[i]);
        const struct adt_segment_ranges *s = adt_segs(nub, &n);
        for (int k = 0; k < n; k++) {
            if (s[k].unk != 0xa)
                continue;
            snprintf(name, sizeof(name), "/reserved-memory/dcp-oslog@%lx",
                     (unsigned long)s[k].phys);
            int len;
            const fdt64_t *reg = prop(dt, name, "reg", &len);
            found += reg && len == 16 && fdt64_ld(reg) == s[k].phys &&
                     fdt64_ld(reg + 1) == ALIGN_UP((u64)s[k].size, SZ_16K) &&
                     prop(dt, name, "no-map", NULL) && prop(dt, name, "apple,dcp-os-log", NULL);
        }
    }
    if (want)
        check(found == 5, "the five dcp/dcpext0-3 __OS_LOG buffers are reserved no-map (%d)",
              found);
    else
        check(found == 0, "no __OS_LOG reservation (%d)", found);
}

static bool marked(const void *dt, const char *path)
{
    return u32_is(dt, path, "apple,t6031-handoff", 1);
}

#define DCP     "/soc/dcp@386c00000"
#define DISP    "/soc/display-subsystem"
#define PIODMA  DCP "/piodma"
#define GPU     "/soc/gpu@40a400000"
#define AGXMBOX "/soc/mbox@40a408000"
#define CHOSEN  "/chosen"
#define ID_NODE CHOSEN "/" T6031_GPU_IDENTITY
#define IMG     CHOSEN "/" T6031_GPU_IMAGE

/* The display handoff, as the kernel's gate and drm/apple read it */
static void check_display_published(const void *dt)
{
    check(marked(dt, DCP) && marked(dt, DISP) && marked(dt, PIODMA),
          "display: apple,t6031-handoff = <1> on dcp, display-subsystem and piodma");
    check(!strcmp(status_of(dt, DCP), "disabled") && !strcmp(status_of(dt, DISP), "disabled"),
          "display: dcp and display-subsystem are still disabled");
    const char *uuid = prop(dt, DCP, "apple,firmware-uuid", NULL);
    const char *adt_uuid = adt_str("/arm-io/dcp/iop-dcp-nub", "uuid");
    check(uuid && adt_uuid && !strcmp(uuid, adt_uuid),
          "display: dcp apple,firmware-uuid is the ADT's image (%s)", uuid ? uuid : "-");
    int len;
    const fdt32_t *compat = prop(dt, DCP, "apple,firmware-compat", &len);
    check(compat && len == 12 && fdt32_ld(compat) == 14 && fdt32_ld(compat + 1) == 7 &&
              !fdt32_ld(compat + 2),
          "display: dcp apple,firmware-compat is 14.7.0, as T6030's");
    int names = fdt_stringlist_count(dt, node_at(dt, DCP), "memory-region-names");
    check(names == 8, "display: dcp has the 8 dcp segments as memory regions (%d)", names);
    check(fdt_stringlist_count(dt, node_at(dt, DISP), "memory-region-names") == 2 &&
              fdt_stringlist_search(dt, node_at(dt, DISP), "memory-region-names", "framebuffer") >=
                  0,
          "display: display-subsystem has the stream 0 segments, the framebuffer among them");
    check(fdt_stringlist_count(dt, node_at(dt, PIODMA), "memory-region-names") == 1,
          "display: piodma has the stream 4 segment");
    check(reg_val(DART_DCP + 0x200) & 1, "display: dart-dcp locked (PROTECT 0x%x)",
          reg_val(DART_DCP + 0x200));
}

static void check_display_refused(const void *dt)
{
    check(!marked(dt, DCP) && !marked(dt, DISP) && !marked(dt, PIODMA) &&
              !prop(dt, DCP, "memory-region", NULL) && !prop(dt, DISP, "memory-region", NULL),
          "display refused: no marker and no memory regions on dcp, display or piodma");
    check(!(reg_val(DART_DCP + 0x200) & 1) && !count_writes(DART_DCP, DART_DCP + 0x4000),
          "display refused: dart-dcp not written, not locked");
}

/* The GPU setup, as drm/asahi's boot-loader admission reads it */
static void check_gpu_published(const void *dt)
{
    int len, sgx = adt_path_offset(adt, "/arm-io/sgx");
    check(!strcmp(status_of(dt, GPU), "okay") && !strcmp(status_of(dt, AGXMBOX), "okay"),
          "gpu: the gpu node and its mailbox are enabled");
    static const char *const fw[] = {"apple,firmware-version", "apple,firmware-abi",
                                     "apple,firmware-compat"};
    bool fw_ok = true;
    for (int i = 0; i < 3; i++) {
        const fdt32_t *v = prop(dt, GPU, fw[i], &len);
        fw_ok &=
            v && len == 12 && fdt32_ld(v) == 14 && fdt32_ld(v + 1) == 8 && fdt32_ld(v + 2) == 3;
    }
    check(fw_ok, "gpu: apple,firmware-version/-abi/-compat are 14.8.3");

    /* One OPP per ADT pstate, four voltages (the four ADT tables) and a power each */
    u32 plen;
    const struct {
        u32 hz, mv;
    } *ps = adt_getprop(adt, sgx, "perf-states", &plen);
    int opps = node_at(dt, "/opp-table-gpu"), opp, n = 0;
    bool opp_ok = ps && plen == 14 * 8 * 4;
    u32 top = 0;
    fdt_for_each_subnode(opp, dt, opps)
    {
        const fdt64_t *hz = fdt_getprop(dt, opp, "opp-hz", &len);
        opp_ok &= hz && len == 8 && fdt64_ld(hz) == ps[n].hz;
        const fdt32_t *uv = fdt_getprop(dt, opp, "opp-microvolt", &len);
        opp_ok &= uv && len == 16;
        for (int t = 0; uv && len == 16 && t < 4; t++)
            opp_ok &= fdt32_ld(uv + t) == (ps[t * 14 + n].mv ? ps[t * 14 + n].mv : ps[n].mv) * 1000;
        const fdt32_t *uw = fdt_getprop(dt, opp, "opp-microwatt", &len);
        opp_ok &= uw && len == 4;
        if (uw && len == 4)
            top = fdt32_ld(uw);
        n++;
    }
    check(opp_ok && n == 14 && top >= 1000,
          "gpu: 14 OPPs, each with the ADT frequency, 4 voltages (tables 0-3) and a power");

    /* Every power-management value as the ADT has it */
    static const char *const pm[][2] = {
        {"gpu-avg-power-ki-only", "apple,avg-power-ki-only"},
        {"gpu-avg-power-kp", "apple,avg-power-kp"},
        {"gpu-avg-power-filter-tc-ms", "apple,avg-power-filter-tc-ms"},
        {"gpu-fast-die0-integral-gain", "apple,fast-die0-integral-gain"},
        {"gpu-fast-die0-proportional-gain", "apple,fast-die0-proportional-gain"},
        {"gpu-ppm-ki", "apple,ppm-ki"},
        {"gpu-ppm-kp", "apple,ppm-kp"},
        {"gpu-ppm-filter-time-constant-ms", "apple,ppm-filter-time-constant-ms"},
        {"gpu-se-tgt", "apple,se-target"},
        {"gpu-perf-base-pstate", "apple,perf-base-pstate"},
        {"gpu-idleoff-standby-timer", "apple,idleoff-standby-timer"},
    };
    bool pm_ok = true;
    for (size_t i = 0; i < ARRAY_SIZE(pm); i++) {
        u32 want, l;
        const void *v = adt_getprop(adt, sgx, pm[i][0], &l);
        memcpy(&want, v, 4);
        pm_ok &= v && l == 4 && u32_is(dt, GPU, pm[i][1], want);
    }
    const fdt32_t *mask = prop(dt, GPU, "apple,fast-die0-sensor-mask", &len);
    check(pm_ok && mask && len == 8 && !fdt32_ld(mask) && fdt32_ld(mask + 1) == 0x8080 &&
              u32_is(dt, GPU, "apple,min-sram-microvolt", 815000),
          "gpu: this Mac's power coefficients, fast-die mask 0x8080 and SRAM floor 815 mV");

    float standin = 6412.0f, zero = 0.0f;
    u32 sb, zb;
    memcpy(&sb, &standin, 4);
    memcpy(&zb, &zero, 4);
    const fdt32_t *core = prop(dt, GPU, "apple,core-leak-coef", &len);
    bool leak_ok = core && len == 16;
    for (int i = 0; leak_ok && i < 4; i++)
        leak_ok &= fdt32_ld(core + i) == sb;
    const fdt32_t *sram = prop(dt, GPU, "apple,sram-leak-coef", &len);
    leak_ok &= sram && len == 16 && !fdt32_ld(sram) && !fdt32_ld(sram + 3);
    leak_ok &=
        u32_is(dt, GPU, "apple,cs-leak-coef", zb) && u32_is(dt, GPU, "apple,afr-leak-coef", zb);
    const char *standins = prop(dt, GPU, "asahi,t6031-gpu-standin", &len);
    check(leak_ok && standins &&
              fdt_stringlist_count(dt, node_at(dt, GPU), "asahi,t6031-gpu-standin") == 5,
          "gpu: stand-in leakage 6412 x 4 clusters, 0 for SRAM, CS and AFR, all listed in "
          "asahi,t6031-gpu-standin");

    /* CS and AFR: one OPP per ADT state, one voltage each */
    static const char *const aux[][2] = {{"cs-perf-states", "/opp-table-gpu-cs"},
                                         {"afr-perf-states", "/opp-table-gpu-afr"}};
    bool aux_ok = u32_is(dt, GPU, "apple,csafr-min-sram-microvolt", 815000);
    for (int a = 0; a < 2; a++) {
        const u8 *v = adt_getprop(adt, sgx, aux[a][0], &plen);
        int k = 0;
        fdt_for_each_subnode(opp, dt, node_at(dt, aux[a][1]))
        {
            u64 want_uv, want_hz;
            memcpy(&want_uv, v + 16 + 16 * k, 8);
            memcpy(&want_hz, v + 16 + 16 * k + 8, 8);
            const fdt64_t *hz = fdt_getprop(dt, opp, "opp-hz", &len);
            aux_ok &= hz && len == 8 && fdt64_ld(hz) == want_hz;
            const fdt32_t *uv = fdt_getprop(dt, opp, "opp-microvolt", &len);
            aux_ok &= uv && len == 4 && fdt32_ld(uv) == want_uv;
            k++;
        }
        aux_ok &= k == 7;
    }
    check(aux_ok, "gpu: CS and AFR OPPs (7 each) from the ADT, CS/AFR SRAM floor 815 mV");

    /* The UAT regions and the firmware segments of review 2.6 */
    static const char *const uat[][2] = {
        {"/reserved-memory/uat-ttbs", "gpu-region"},
        {"/reserved-memory/uat-pagetables", "gfx-shared-region"},
        {"/reserved-memory/uat-pagetables-l2", "gfx-shared-l2-region"},
        {"/reserved-memory/uat-handoff", "gfx-handoff"},
    };
    bool uat_ok = true;
    for (int i = 0; i < 4; i++) {
        char b[64], s[64];
        u64 base, size;
        u32 l;
        snprintf(b, sizeof(b), "%s-base", uat[i][1]);
        snprintf(s, sizeof(s), "%s-size", uat[i][1]);
        memcpy(&base, adt_getprop(adt, sgx, b, &l), 8);
        memcpy(&size, adt_getprop(adt, sgx, s, &l), 8);
        const fdt64_t *reg = prop(dt, uat[i][0], "reg", &len);
        uat_ok &= reg && fdt64_ld(reg) == base && fdt64_ld(reg + 1) == size &&
                  !strcmp(status_of(dt, uat[i][0]), "okay");
    }
    int n2;
    const struct adt_segment_ranges *seg = adt_segs("/arm-io/gfx-asc", &n2);
    char text[64], data[64];
    snprintf(text, sizeof(text), "/reserved-memory/gpu-fw-text@%lx", (unsigned long)seg[0].phys);
    snprintf(data, sizeof(data), "/reserved-memory/gpu-fw-data@%lx", (unsigned long)seg[1].phys);
    const fdt64_t *vas = prop(dt, GPU, "apple,firmware-segment-vas", &len);
    check(uat_ok && prop(dt, text, "no-map", NULL) && prop(dt, data, "no-map", NULL) && vas &&
              len == 16 && fdt64_ld(vas) == seg[0].iova && fdt64_ld(vas + 1) == seg[1].iova &&
              u32_is(dt, GPU, "apple,m3-handoff-version", 1),
          "gpu: uat-ttbs/-pagetables/-pagetables-l2/-handoff and no-map fw-text/fw-data from the "
          "ADT, segment VAs and flags");
}

static void check_gpu_refused(const void *dt)
{
    int len;
    const fdt32_t *compat = prop(dt, GPU, "apple,firmware-compat", &len);
    check(!strcmp(status_of(dt, GPU), "disabled") && !strcmp(status_of(dt, AGXMBOX), "disabled") &&
              (!compat || (len == 12 && !fdt32_ld(compat))) &&
              !strcmp(status_of(dt, "/reserved-memory/uat-ttbs"), "disabled") &&
              !prop(dt, GPU, "apple,m3-handoff-version", NULL),
          "gpu refused: gpu, mailbox and UAT regions disabled, firmware-compat zero, no segment "
          "description");
}

static void check_identity(const void *dt, u32 result, bool values)
{
    check(u32_is(dt, ID_NODE, "probe-result", result) && u32_is(dt, ID_NODE, "schema-version", 1),
          "identity: /chosen/" T6031_GPU_IDENTITY " probe-result %u (is %u)", result,
          u32_of(dt, ID_NODE, "probe-result"));
    if (values)
        check(u32_is(dt, ID_NODE, "id-version", sim_id_version) &&
                  u32_is(dt, ID_NODE, "id-counts-1", sim_counts1) &&
                  u32_is(dt, ID_NODE, "id-counts-2", sim_counts2) &&
                  u32_is(dt, ID_NODE, "core-mask-0", sim_mask[0]) &&
                  u32_is(dt, ID_NODE, "core-mask-1", sim_mask[1]) &&
                  u32_is(dt, ID_NODE, "core-mask-2", sim_mask[2]),
              "identity: the values read are published as read");
    else
        check(u32_is(dt, ID_NODE, "id-version", 0) && u32_is(dt, ID_NODE, "core-mask-0", 0),
              "identity: no value published after a faulted read");
    int len;
    const fdt64_t *addr = prop(dt, ID_NODE, "pmgr-addresses", &len);
    check(addr && len == 16 && fdt64_ld(addr) == PS_GFX && fdt64_ld(addr + 1) == PS_AFR,
          "identity: the power closure resolved from the ADT is GFX 0x%lx then its parent AFR "
          "0x%lx",
          PS_GFX, PS_AFR);
}

/* ---------- output ---------- */

static void dump_prop(const void *dt, int node, const char *indent)
{
    int p;
    fdt_for_each_property_offset(p, dt, node)
    {
        const char *name;
        int len;
        const u8 *v = fdt_getprop_by_offset(dt, p, &name, &len);
        printf("%s%s =", indent, name);
        bool text = len > 1 && !v[len - 1];
        for (int i = 0; text && i < len - 1; i++)
            text = v[i] ? (v[i] >= 0x20 && v[i] < 0x7f) : (i && v[i - 1]);
        if (text) {
            for (int i = 0; i < len - 1; i += strlen((const char *)v + i) + 1)
                printf(" \"%s\"", v + i);
        } else if (len % 4 == 0) {
            for (int i = 0; i < len; i += 4)
                printf(" 0x%08x", fdt32_ld((const fdt32_t *)(v + i)));
        } else {
            for (int i = 0; i < len; i++)
                printf(" %02x", v[i]);
        }
        printf("\n");
    }
}

static void dump_node(const void *dt, const char *path)
{
    int node = node_at(dt, path);
    if (node < 0)
        return;
    printf("  %s\n", path);
    dump_prop(dt, node, "    ");
    int child;
    fdt_for_each_subnode(child, dt, node)
    {
        char p[256];
        snprintf(p, sizeof(p), "%s/%s", path, fdt_get_name(dt, child, NULL));
        dump_node(dt, p);
    }
}

static void dump_published(const void *dt)
{
    printf("== published\n");
    int chosen = node_at(dt, CHOSEN), child;
    fdt_for_each_subnode(child, dt, chosen)
    {
        char p[128];
        snprintf(p, sizeof(p), CHOSEN "/%s", fdt_get_name(dt, child, NULL));
        dump_node(dt, p);
    }
    printf("  " CHOSEN " asahi,t6031-fw-uuids =");
    int len;
    const char *u = prop(dt, CHOSEN, "asahi,t6031-fw-uuids", &len);
    for (int i = 0; u && i < len; i += strlen(u + i) + 1)
        printf(" \"%s\"", u + i);
    printf("\n");
    int resv = node_at(dt, "/reserved-memory");
    fdt_for_each_subnode(child, dt, resv)
    {
        char p[128];
        snprintf(p, sizeof(p), "/reserved-memory/%s", fdt_get_name(dt, child, NULL));
        dump_node(dt, p);
    }
    static const char *const nodes[] = {
        DCP, DISP, GPU, AGXMBOX, "/opp-table-gpu", "/opp-table-gpu-cs", "/opp-table-gpu-afr"};
    for (size_t i = 0; i < ARRAY_SIZE(nodes); i++)
        dump_node(dt, nodes[i]);
    printf("== register writes\n");
    for (int i = 0; i < nwrites; i++)
        printf("  0x%lx: 0x%08x -> 0x%08x\n", (unsigned long)writes[i].addr, writes[i].old,
               writes[i].val);
}

/* ---------- scenarios ---------- */

static u8 *adt_blob;
static u32 adt_blob_size;
static u8 dt_in[1 << 20];

static void load_adt(void)
{
    adt = malloc(adt_blob_size);
    memcpy(adt, adt_blob, adt_blob_size);
    adt_size = adt_blob_size;
}

static void *load_dt(int room)
{
    static u8 dt[1 << 21];
    assert(!fdt_open_into(dt_in, dt, fdt_totalsize(dt_in) + room));
    assert(!fdt_setprop_string(dt, fdt_path_offset(dt, CHOSEN), "asahi,t6031-gpu", "1"));
    assert(!fdt_setprop_string(dt, fdt_path_offset(dt, CHOSEN), "asahi,t6031-dcp", "1"));
    return dt;
}

/* What kboot_prepare_dt() does on a T6031, for these parts and in this order */
static void boot(void *dt)
{
    log_len = 0;
    log_buf[0] = 0;
    nwrites = 0;
    assert(dt_set_display_handoff(dt) == 0);
    assert(dt_set_gpu(dt) == 0);
    host_pmp_t6031(dt);
    dt_t6031_fw_uuids(dt);
    dt_t6031_facts(dt);
}

static void chosen_set(void *dt, const char *name, const char *value)
{
    assert(!fdt_setprop_string(dt, node_at(dt, CHOSEN), name, value));
}

/* Run with the registers snapshotted after pmgr_init(), as kboot sees them */
static struct reg snap[1024];
static int nsnap;

static void run(void *dt)
{
    assert(pmgr_init() == 0);
    if (display_off_after_init)
        reg_set(PS_DISP_CPU, 0x0, R_PS);
    memcpy(snap, regs, sizeof(regs));
    nsnap = nregs;
    boot(dt);
    check(!unguarded, "no unguarded fault (%d)", unguarded);
    check(!gpu_reads_unpowered, "no GPU register read while GFX was off");
}

static void expect_power_restored(void)
{
    check(!changed_registers(snap, nsnap, DART_DCP + 0x200),
          "every register but the dart-dcp lock is as it was found (GFX 0x%x, AFR 0x%x)",
          reg_val(PS_GFX), reg_val(PS_AFR));
}

/* Structure and strings in use: what kboot_prepare_dt()'s buffer room has to hold */
static int dt_used(const void *dt)
{
    return fdt_off_dt_struct(dt) + fdt_size_dt_struct(dt) + fdt_size_dt_strings(dt);
}

static void sc_publish(void *dt, bool dump)
{
    world(&(struct opts){0});
    int used = dt_used(dt);
    run(dt);
    int grew = dt_used(dt) - used;
    check(grew < 64 * 1024, "the device tree grew by %d bytes, within the 64K kboot adds for T6031",
          grew);
    check_oslogs(dt, true);
    check_display_published(dt);
    check(prop(dt, CHOSEN "/asahi,t6031-pmp", "apple,tunable-uuid", NULL) &&
              u32_is(dt, CHOSEN "/asahi,t6031-pmp", "apple,board-id", 0x5a),
          "pmp: /chosen/asahi,t6031-pmp has board-id, dram-vendor-id and the tunables, "
          "tunable-uuid among them");
    int len;
    const char *u = prop(dt, CHOSEN, "asahi,t6031-fw-uuids", &len);
    check(u && fdt_stringlist_count(dt, node_at(dt, CHOSEN), "asahi,t6031-fw-uuids") == 7 &&
              !strncmp(u, "gfx=5E5E5E5E", 12),
          "fw-uuids: seven entries, gfx first");
    const fdt32_t *clk = prop(dt, CHOSEN "/asahi,t6031-facts", "disp0-clock-frequencies", &len);
    const fdt64_t *mcc = prop(dt, CHOSEN "/asahi,t6031-facts", "mcc-reg", &len);
    check(clk && fdt32_ld(clk) == 712000000 &&
              u32_is(dt, CHOSEN "/asahi,t6031-facts", "schema-version", 1) && mcc && len == 32 &&
              fdt64_ld(mcc) == 0x280000000UL && fdt64_ld(mcc + 2) == 0x280400000UL,
          "facts: disp0 clock rates and the mcc windows, translated through /arm-io ranges");
    check(!strcmp(prop(dt, IMG, "firmware-family", NULL), "14.8.3") &&
              !strcmp(prop(dt, IMG, "image-rtkit-version", NULL), sim_rtkit),
          "image: the 14.8.3 family, RTKit %s", sim_rtkit);
    check_identity(dt, T8122_ID_PASS, true);
    check_gpu_published(dt);
    expect_power_restored();
    check(gpu_reads == 6, "exactly the 6 identity registers were read (%d)", gpu_reads);
    if (dump)
        dump_published(dt);
}

static void sc_publish_quiet(void *dt)
{
    sc_publish(dt, false);
}

static void sc_publish_dump(void *dt)
{
    sc_publish(dt, true);
}

static void sc_t6034(void *dt)
{
    static u8 before[1 << 21];
    world(&(struct opts){0});
    chip_id = T6034;
    /* J514M and J516M are admitted; an unknown T6034 board must stay untouched */
    static const char compat[] = "apple,j999\0apple,t6034\0apple,arm-platform";
    assert(!fdt_setprop(dt, 0, "compatible", compat, sizeof(compat)));
    memcpy(before, dt, fdt_totalsize(dt));
    assert(pmgr_init() == 0);
    memcpy(snap, regs, sizeof(regs));
    nsnap = nregs;
    /* kboot.c sends no apple,t6034 tree to the handoff; it is called here to show it does nothing
     */
    boot(dt);
    check(!memcmp(before, dt, fdt_totalsize(before)), "T6034: the device tree is unchanged");
    check(!nwrites && !gpu_reads, "T6034: no register written, no GPU register read");
    check(!log_has("ADT: GPU: unsupported chip!") && !log_has("FDT: GPU: T6034"),
          "T6034: an unknown board returns quietly from the J514M/J516M GPU path");
}

static void sc_other_board(void *dt)
{
    static u8 before[1 << 21];
    world(&(struct opts){0});
    static const char compat[] = "apple,j999c\0apple,t6031\0apple,arm-platform";
    assert(!fdt_setprop(dt, 0, "compatible", compat, sizeof(compat)));
    memcpy(before, dt, fdt_totalsize(dt));
    run(dt);
    check(!memcmp(before, dt, fdt_totalsize(before)),
          "another T6031 board: the device tree is unchanged");
    check(!nwrites && !gpu_reads, "another T6031 board: no register written or GPU register read");
}

/* Only chosen.asahi,t6031-dcp=1 arms the display handoff; __OS_LOG is reserved regardless */
static void sc_dcp_not_armed(void *dt)
{
    static u8 armed[1 << 21];
    memcpy(armed, dt, fdt_totalsize(dt));
    static const char *const values[] = {NULL, "0", "", "10", "yes"};
    for (size_t i = 0; i < ARRAY_SIZE(values); i++) {
        memcpy(dt, armed, fdt_totalsize(armed));
        if (values[i])
            chosen_set(dt, "asahi,t6031-dcp", values[i]);
        else
            assert(!fdt_delprop(dt, node_at(dt, CHOSEN), "asahi,t6031-dcp"));
        world(&(struct opts){0});
        run(dt);
        check_oslogs(dt, true);
        check_display_refused(dt);
        check(!count_writes(DCP_COPROC, DCP_COPROC + 0x100000) &&
                  log_has("not armed (no chosen.asahi,t6031-dcp=1)"),
              "dcp not armed (%s): logged, nothing written to the display blocks",
              values[i] ? values[i] : "absent");
        check_identity(dt, T8122_ID_PASS, true);
        check_gpu_published(dt);
        expect_power_restored();
    }
}

static void sc_gpu_off(void *dt)
{
    world(&(struct opts){0});
    chosen_set(dt, "asahi,t6031-gpu", "0");
    run(dt);
    check(node_at(dt, ID_NODE) < 0, "gpu switched off: no identity node");
    check(node_at(dt, IMG) < 0, "gpu switched off: no image node");
    check(!count_writes(0x408e80000UL, 0x408e90000UL) && !gpu_reads,
          "gpu switched off: no PMGR write for the GPU, no GPU register read");
    check_gpu_refused(dt);
    check_display_published(dt);
}

static void sc_identity_fault(void *dt)
{
    world(&(struct opts){.gpu_fault = true});
    run(dt);
    check_identity(dt, T8122_ID_READ_FAULT, false);
    check_gpu_refused(dt);
    check_display_published(dt);
    expect_power_restored();
}

static void sc_identity_mismatch(void *dt)
{
    world(&(struct opts){0});
    sim_id_version = 0x07031100; /* a G15S */
    reg_set(SGX + 0xd04000, sim_id_version, R_GPU);
    run(dt);
    check_identity(dt, T8122_ID_MISMATCH, true);
    check_gpu_refused(dt);
    expect_power_restored();
    sim_id_version = 0x07041200;
}

static void sc_mask_beyond_40(void *dt)
{
    world(&(struct opts){0});
    sim_mask[1] = 0x1ff; /* a 41st core slot */
    reg_set(SGX + 0xe01504, sim_mask[1], R_GPU);
    run(dt);
    check_identity(dt, T8122_ID_MISMATCH, true);
    check_gpu_refused(dt);
    expect_power_restored();
    sim_mask[1] = 0xff;
}

static void sc_power_on_timeout(void *dt)
{
    world(&(struct opts){0});
    stuck_off = PS_GFX;
    run(dt);
    check_identity(dt, T8122_ID_POWER_FAILED, false);
    check(!gpu_reads, "power-on timed out: no GPU register read");
    check_gpu_refused(dt);
    expect_power_restored();
}

static void sc_power_off_fails(void *dt)
{
    world(&(struct opts){0});
    stuck_on = PS_GFX;
    run(dt);
    check_identity(dt, T8122_ID_POWER_OFF_FAILED, true);
    check(log_has("WARNING: the power closure is not as iBoot left it"),
          "power-off failed: logged loudly");
    check_gpu_refused(dt);
}

static void sc_unknown_firmware(void *dt)
{
    world(&(struct opts){0});
    int n;
    u64 text = adt_segs("/arm-io/gfx-asc", &n)[0].phys;
    strcpy((char *)dram_at(text + 0x5c100, 64), "RTKit-2600.1.1.release");
    run(dt);
    check(!strcmp(prop(dt, IMG, "firmware-family", NULL), "unknown"),
          "unknown firmware: the image node says so");
    check_identity(dt, T8122_ID_PASS, true);
    check_gpu_refused(dt);
    expect_power_restored();
}

static void sc_dcp_stopped(void *dt)
{
    world(&(struct opts){.dcp_stopped = true});
    run(dt);
    check_oslogs(dt, true);
    check_display_refused(dt);
    check_gpu_published(dt);
}

static void sc_stray_mapping(void *dt)
{
    world(&(struct opts){.stray_map = true});
    run(dt);
    check_oslogs(dt, true);
    check_display_refused(dt);
}

static void sc_display_power_off(void *dt)
{
    world(&(struct opts){.display_off = true});
    run(dt);
    check_oslogs(dt, true);
    check_display_refused(dt);
    check(log_has("display power domain not active"), "display off: no DCP or DART register read");
}

static void sc_no_dcp_uuid(void *dt)
{
    world(&(struct opts){0});
    adt_hide("/arm-io/dcp/iop-dcp-nub", "uuid");
    run(dt);
    check_display_refused(dt);
    const char *u = prop(dt, CHOSEN, "asahi,t6031-fw-uuids", NULL);
    check(u && !strcmp(u + strlen(u) + 1, "dcp=absent"), "fw-uuids: dcp=absent");
    expect_power_restored();
}

static void sc_no_facts(void *dt)
{
    world(&(struct opts){0});
    adt_hide("/arm-io", "clock-frequencies");
    adt_hide("/arm-io/mcc", "name");
    run(dt);
    int len;
    const fdt32_t *clk = prop(dt, CHOSEN "/asahi,t6031-facts", "disp0-clock-frequencies", &len);
    check(clk && len == 8 && !fdt32_ld(clk) &&
              !prop(dt, CHOSEN "/asahi,t6031-facts", "mcc-reg", NULL),
          "facts without clock-frequencies or mcc: the clock ids with rate 0, no mcc-reg");
    check_display_published(dt);
}

static void sc_dt_mismatch(void *dt)
{
    world(&(struct opts){0});
    int gpu = node_at(dt, GPU);
    fdt64_t reg[4] = {cpu_to_fdt64(0x40a400000UL), cpu_to_fdt64(0x6c000),
                      cpu_to_fdt64(0x290000000UL), cpu_to_fdt64(0x1000000)};
    assert(!fdt_setprop(dt, gpu, "reg", reg, sizeof(reg)));
    run(dt);
    check_identity(dt, T8122_ID_PASS, true);
    check_gpu_refused(dt);
    expect_power_restored();
}

static void sc_small_dt(void *dt)
{
    world(&(struct opts){0});
    (void)dt;
    void *small = load_dt(0x800);
    run(small);
    check(!marked(small, DCP) || (marked(small, DISP) && marked(small, PIODMA)),
          "small device tree: no partial display handoff");
    check(strcmp(status_of(small, GPU), "okay") ||
              u32_is(small, GPU, "apple,m3-handoff-version", 1),
          "small device tree: no partial GPU handoff");
    expect_power_restored();
}

static const struct {
    const char *name;
    void (*fn)(void *dt);
} scenarios[] = {
    {"publish", sc_publish_quiet},
    {"publish-dump", sc_publish_dump},
    {"t6034", sc_t6034},
    {"other-board", sc_other_board},
    {"dcp-not-armed", sc_dcp_not_armed},
    {"gpu-off", sc_gpu_off},
    {"identity-fault", sc_identity_fault},
    {"identity-mismatch", sc_identity_mismatch},
    {"mask-beyond-40", sc_mask_beyond_40},
    {"power-on-timeout", sc_power_on_timeout},
    {"power-off-fails", sc_power_off_fails},
    {"unknown-firmware", sc_unknown_firmware},
    {"dcp-stopped", sc_dcp_stopped},
    {"stray-mapping", sc_stray_mapping},
    {"display-power-off", sc_display_power_off},
    {"no-dcp-uuid", sc_no_dcp_uuid},
    {"no-facts", sc_no_facts},
    {"dt-mismatch", sc_dt_mismatch},
    {"small-dt", sc_small_dt},
};

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s <mac.adt> <in.dtb> <scenario|list>\n", argv[0]);
        return 2;
    }
    if (!strcmp(argv[3], "list")) {
        for (size_t i = 0; i < ARRAY_SIZE(scenarios); i++)
            printf("%s\n", scenarios[i].name);
        return 0;
    }
    FILE *f = fopen(argv[1], "rb");
    assert(f);
    adt_blob = malloc(1 << 22);
    adt_blob_size = fread(adt_blob, 1, 1 << 22, f);
    fclose(f);
    f = fopen(argv[2], "rb");
    assert(f && fread(dt_in, 1, sizeof(dt_in), f) > 0 && !fdt_check_header(dt_in));
    fclose(f);

    for (size_t i = 0; i < ARRAY_SIZE(scenarios); i++) {
        if (strcmp(scenarios[i].name, argv[3]))
            continue;
        load_adt();
        void *dt = load_dt(160 * 1024);
        printf("== %s\n", scenarios[i].name);
        scenarios[i].fn(dt);
        printf("== aura log (%zu bytes)\n%s", log_len, log_buf);
        printf("%s: %s\n", scenarios[i].name, failures ? "FAIL" : "pass");
        return failures ? 1 : 0;
    }
    fprintf(stderr, "unknown scenario %s\n", argv[3]);
    return 2;
}

/* These fixtures exercise G15/M3 only. Fail if dispatch unexpectedly reaches M4. */
int dt_set_gpu_m4(void *dt)
{
    (void)dt;
    assert(!"unexpected M4 dispatch in the G15/M3 host fixture");
    return -1;
}
