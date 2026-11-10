/* SPDX-License-Identifier: MIT */
/*
 * Host test of the G15 GPU setup in kboot_gpu.c (T6030 and T8122), with a synthetic ADT, a
 * simulated bus and real libfdt. run-kboot-gpu-g15.sh copies kboot_gpu.c next to this file and
 * the stand-in utils.h/memory.h from tests/g15_host, so the setup runs unchanged on the host.
 *
 * Usage: kboot-gpu-g15 <scenario>   prints the resulting DT, bus and PMGR traces and the log.
 *        kboot-gpu-g15 list         prints the scenario names.
 * T6030 scenarios also build against an older kboot_gpu.c, so the run script can check that the
 * T6030 results did not change. T8122 scenarios need G15_NEW (this tree) and assert on results;
 * the ones without the stand-in switch also build with G15_T8122_BASE, against an older tree with
 * the T8122 setup, so the run script can check that they did not change either.
 */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kboot_gpu.c"
#ifdef G15_FW26
#include "kboot_gpu26.c"
#endif

/* ---------- aura globals and stubs ---------- */

void *adt = (void *)1;
u32 board_id, chip_id;
volatile enum exc_guard_t exc_guard;
volatile int exc_count;
uint64_t ram_base;
u64 boot_flags, mem_size_actual;
size_t mcc_carveout_count;
struct mcc_carveout mcc_carveouts[4];

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

static char trace_buf[1 << 16];
static size_t trace_len;
static int pmgr_enables, pmgr_disables, mmio_reads, unguarded_faults;

static void trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void trace(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(trace_buf + trace_len, sizeof(trace_buf) - trace_len, fmt, ap);
    va_end(ap);
    if (n > 0)
        trace_len = min(trace_len + n, sizeof(trace_buf) - 1);
}

static u64 ram_min = 0x10000000000, ram_size = 0x1e0000000;
u64 kboot_ram_base(void)
{
    return ram_min;
}
u64 kboot_ram_size(void)
{
    return ram_size;
}
u64 kboot_fb_base(void)
{
    return 0;
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
int32_t rust_fill_gpu_initdata(struct initdata_inputs *a, void *b, void *c, void *d)
{
    (void)a, (void)b, (void)c, (void)d;
    return -1;
}

/* PMGR: the GPU power closure. pmgr_off is the state seen by pmgr_adt_power_is_off(). */
static int pmgr_enable_ret, pmgr_off = 1, pmgr_off_after_release = 1;
int pmgr_adt_power_enable(const char *path)
{
    trace("pmgr enable %s\n", path);
    pmgr_enables++;
    if (!pmgr_enable_ret)
        pmgr_off = 0;
    return pmgr_enable_ret;
}
int pmgr_adt_power_enable_traced_saved(const char *path, struct pmgr_saved_modes *saved)
{
    (void)saved;
    return pmgr_adt_power_enable(path);
}
void pmgr_restore_modes(struct pmgr_saved_modes *saved)
{
    (void)saved;
}
int pmgr_adt_power_disable(const char *path)
{
    trace("pmgr disable %s\n", path);
    pmgr_disables++;
    return 0;
}
int pmgr_adt_power_disable_recursive(const char *path)
{
    trace("pmgr disable-recursive %s\n", path);
    pmgr_disables++;
    pmgr_off = pmgr_off_after_release;
    return 0;
}
int pmgr_adt_power_is_off(const char *path)
{
    trace("pmgr is-off %s = %d\n", path, pmgr_off);
    return pmgr_off;
}

/*
 * The T6031 code (kboot_gpu_t6031.c, kboot_t6031.c) links in with kboot_gpu.c from trees that have
 * it. No T6030 or T8122 scenario reaches it: a register write or PMGR lookup here fails the run.
 */
void host_write32(u64 addr, u32 val)
{
    trace("write32 0x%lx = 0x%x UNEXPECTED\n", addr, val);
    unguarded_faults++;
}
int pmgr_adt_gate(const char *path, const char *prop, u32 index, struct pmgr_gate *gate)
{
    (void)prop, (void)index, (void)gate;
    trace("pmgr gate %s UNEXPECTED\n", path);
    unguarded_faults++;
    return -1;
}
int pmgr_device_gate(u8 die, u16 id, struct pmgr_gate *gate)
{
    (void)die, (void)id, (void)gate;
    unguarded_faults++;
    return -1;
}
int pmgr_set_mode(uintptr_t addr, u8 target_mode)
{
    (void)addr, (void)target_mode;
    unguarded_faults++;
    return -1;
}

/* ---------- synthetic ADT ---------- */

struct hprop {
    const char *name;
    u32 len;
    u8 data[1024] __attribute__((aligned(8)));
};

struct hnode {
    const char *path;
    int nprops;
    struct hprop props[64];
};

static struct hnode hnodes[16];
static int n_hnodes;

static struct hnode *hnode(const char *path)
{
    for (int i = 0; i < n_hnodes; i++)
        if (!strcmp(hnodes[i].path, path))
            return &hnodes[i];
    assert(n_hnodes < (int)ARRAY_SIZE(hnodes));
    hnodes[n_hnodes].path = path;
    return &hnodes[n_hnodes++];
}

static void hset(const char *path, const char *name, const void *data, u32 len)
{
    struct hnode *n = hnode(path);
    struct hprop *p = NULL;
    for (int i = 0; i < n->nprops; i++)
        if (!strcmp(n->props[i].name, name))
            p = &n->props[i];
    if (!p) {
        assert(n->nprops < (int)ARRAY_SIZE(n->props));
        p = &n->props[n->nprops++];
    }
    assert(len <= sizeof(p->data));
    p->name = name;
    p->len = len;
    memcpy(p->data, data, len);
}

static void hdel(const char *path, const char *name)
{
    struct hnode *n = hnode(path);
    for (int i = 0; i < n->nprops; i++)
        if (!strcmp(n->props[i].name, name)) {
            n->props[i] = n->props[--n->nprops];
            return;
        }
}

static void hset_u32(const char *path, const char *name, u32 v)
{
    hset(path, name, &v, sizeof(v));
}

static void hset_u64(const char *path, const char *name, u64 v)
{
    hset(path, name, &v, sizeof(v));
}

int adt_path_offset(const void *tree, const char *path)
{
    (void)tree;
    for (int i = 0; i < n_hnodes; i++)
        if (!strcmp(hnodes[i].path, path))
            return i + 1;
    return -1;
}

int adt_path_offset_trace(const void *tree, const char *path, int *offsets)
{
    int off = adt_path_offset(tree, path);
    if (off > 0 && offsets) {
        offsets[0] = off;
        offsets[1] = 0;
    }
    return off;
}

const void *adt_getprop(const void *tree, int node, const char *name, u32 *lenp)
{
    (void)tree;
    if (node < 1 || node > n_hnodes)
        return NULL;
    struct hnode *n = &hnodes[node - 1];
    for (int i = 0; i < n->nprops; i++)
        if (!strcmp(n->props[i].name, name)) {
            if (lenp)
                *lenp = n->props[i].len;
            return n->props[i].data;
        }
    return NULL;
}

int adt_getprop_copy(const void *tree, int node, const char *name, void *out, size_t len)
{
    u32 plen;
    const void *p = adt_getprop(tree, node, name, &plen);
    if (!p || plen != len)
        return -1;
    memcpy(out, p, len);
    return len;
}

int adt_get_reg(const void *tree, int *path, const char *prop, int idx, u64 *addr, u64 *size)
{
    u32 len;
    const u64 *reg = adt_getprop(tree, path[0], prop, &len);
    if (!reg || len < (idx + 1) * 16u)
        return -1;
    if (addr)
        memcpy(addr, &reg[idx * 2], sizeof(*addr));
    if (size)
        memcpy(size, &reg[idx * 2 + 1], sizeof(*size));
    return 0;
}

/* ---------- simulated bus ---------- */

#define DRAM_BASE 0x10000000000UL
#define DRAM_SIZE 0x200000000UL
#define SGX_BASE  0x290000000UL
#define SGX_SIZE  0x1000000UL
#define ASC_BASE  0x292400000UL
#define ASC_SIZE  0x40000UL
#define FW_TEXT   0x101f8000000UL
#define FW_DATA   (FW_TEXT + 0x5c000)
#define FW_VA     0xfffffc0000000000UL
#define LEAK_FUSE 0x3642c81a8UL

static u32 fw_hdr[0x38 / 4];
static char fw_version_str[64] = "RTKit-test";
static u64 fw_text = FW_TEXT; /* where the bus serves the firmware text (header, version) */

struct mmio {
    u64 addr;
    u32 val;
    int fault;
};
static struct mmio mmio[16];
static int n_mmio;

static void mmio_set(u64 addr, u32 val, int fault)
{
    for (int i = 0; i < n_mmio; i++)
        if (mmio[i].addr == addr) {
            mmio[i].val = val;
            mmio[i].fault = fault;
            return;
        }
    mmio[n_mmio++] = (struct mmio){addr, val, fault};
}

static u32 bus_fault(u64 addr)
{
    trace("fault 0x%lx%s\n", addr, (exc_guard & GUARD_TYPE_MASK) == GUARD_MARK ? "" : " UNGUARDED");
    if ((exc_guard & GUARD_TYPE_MASK) == GUARD_MARK)
        exc_count++;
    else
        unguarded_faults++;
    return 0xffffffff;
}

u32 host_read32(u64 addr)
{
    if (addr >= fw_text + 0x4200 && addr < fw_text + 0x4200 + sizeof(fw_hdr)) {
        trace("read32 fw+0x%lx\n", addr - fw_text);
        return fw_hdr[(addr - fw_text - 0x4200) / 4];
    }
    for (int i = 0; i < n_mmio; i++)
        if (mmio[i].addr == addr) {
            trace("read32 0x%lx%s\n", addr, mmio[i].fault ? " (faults)" : "");
            mmio_reads++;
            if (mmio[i].fault)
                return bus_fault(addr);
            return mmio[i].val;
        }
    trace("read32 0x%lx UNMAPPED\n", addr);
    return bus_fault(addr);
}

u8 host_read8(u64 addr)
{
    u64 off = fw_hdr[3];
    if (addr >= fw_text + off && addr < fw_text + off + sizeof(fw_version_str))
        return fw_version_str[addr - fw_text - off];
    trace("read8 0x%lx UNMAPPED\n", addr);
    return bus_fault(addr);
}

void host_sysop(const char *op)
{
    (void)op;
}

/* ---------- fixtures ---------- */

/* The 14.8.3 "g15s build a0" image identity from src/firmware.c */
static const u8 g15s_a0_uuid[16] = {0x0c, 0xbb, 0x17, 0xaf, 0x78, 0x3d, 0x36, 0x7b,
                                    0x9c, 0xed, 0xd5, 0x91, 0x09, 0x44, 0x73, 0x1e};

struct fixture {
    u32 chip;
    const char *compatible;
    const char *optin;  /* chosen property set to "1", NULL for none */
    u32 clusters;       /* pstate tables, OPP microvolt cells */
    int mbox_disabled;  /* the gpu's mailbox node starts disabled */
    int dt_size;
};

#define U64PAIR(a, b) {(a), (b)}

static void setup_adt(const struct fixture *fx)
{
    n_hnodes = 0;
    memset(hnodes, 0, sizeof(hnodes));

    hset_u32("/arm-io", "chip-revision", 0x11);
    u64 sgx_reg[2] = U64PAIR(SGX_BASE, SGX_SIZE);
    hset("/arm-io/sgx", "reg", sgx_reg, sizeof(sgx_reg));

    /* 5 pstates (off + 4) per table; table 1 repeats table 0 with a zero to back-fill */
    struct perf_state ps[2][5] = {
        {{0, 125}, {338000000, 600}, {618000000, 700}, {796000000, 800}, {1380000000, 1000}},
        {{0, 125}, {338000000, 610}, {618000000, 0}, {796000000, 810}, {1380000000, 1010}},
    };
    hset_u32("/arm-io/sgx", "perf-state-count", 5);
    hset_u32("/arm-io/sgx", "gpu-num-perf-states", 4);
    hset_u32("/arm-io/sgx", "perf-state-table-count", fx->clusters);
    hset("/arm-io/sgx", "perf-states", ps, sizeof(ps[0]) * fx->clusters);
    hset("/arm-io/sgx", "perf-states-sram", ps, sizeof(ps[0]) * fx->clusters);

    static const char *const regions[] = {"gfx-handoff", "gfx-shared-region", "gpu-region",
                                          "gfx-shared-l2-region"};
    static char names[4][2][40];
    for (int i = 0; i < 4; i++) {
        snprintf(names[i][0], sizeof(names[i][0]), "%s-base", regions[i]);
        snprintf(names[i][1], sizeof(names[i][1]), "%s-size", regions[i]);
        hset_u64("/arm-io/sgx", names[i][0], 0x101f0000000UL + i * 0x1000000UL);
        hset_u64("/arm-io/sgx", names[i][1], 0x4000UL << i);
    }

    u64 asc_reg[2] = U64PAIR(ASC_BASE, ASC_SIZE);
    hset("/arm-io/gfx-asc", "reg", asc_reg, sizeof(asc_reg));
    hset("/arm-io/gfx-asc", "segment-names", "__TEXT;__DATA", sizeof("__TEXT;__DATA"));
    struct adt_segment_ranges seg[2] = {
        {FW_TEXT, FW_VA, FW_TEXT, 0x5c000, 1},
        {FW_DATA, FW_VA + 0x5c000, FW_DATA, 0x114000, 0},
    };
    hset("/arm-io/gfx-asc", "segment-ranges", seg, sizeof(seg));
    hset("/arm-io/gfx-asc/iop-gfx-nub", "segment-ranges", seg, sizeof(seg));
    hset_u32("/arm-io/gfx-asc/iop-gfx-nub", "pre-loaded", 1);

    hset_u64("/chosen", "dram-base", DRAM_BASE);
    hset_u64("/chosen", "dram-size", DRAM_SIZE);
}

static void setup_bus(const struct fixture *fx)
{
    n_mmio = 0;
    memset(fw_hdr, 0, sizeof(fw_hdr));
    fw_hdr[0] = 0x14000011;
    fw_hdr[1] = 0x64697575;
    fw_hdr[2] = 5;
    fw_hdr[3] = 0x4800;
    memcpy(&fw_hdr[5], g15s_a0_uuid, sizeof(g15s_a0_uuid));
    fw_hdr[11] = 0x58e88;
    fw_hdr[12] = 0x1e8;
    fw_hdr[13] = 0x5c000;

    if (fx->chip == T6030) {
        mmio_set(SGX_BASE + 0xd04000, 0x07031100, 0);
        mmio_set(SGX_BASE + 0xe01500, 0x000ffbff, 0); /* 9 + 10 cores */
        mmio_set(SGX_BASE + 0xe01504, 0, 0);
        mmio_set(SGX_BASE + 0xe01508, 0, 0);
        mmio_set(LEAK_FUSE, 0x12345678, 0);
    } else {
        mmio_set(SGX_BASE + 0xd04000, 0x07022000, 0);
        mmio_set(SGX_BASE + 0xd04010, 0x0001010a, 0); /* 1 die, 1 cluster, 10 cores */
        mmio_set(SGX_BASE + 0xd04014, 0x00040404, 0); /* further topology counts */
        mmio_set(SGX_BASE + 0xe01500, 0x000000ff, 0); /* 8 active cores */
        mmio_set(SGX_BASE + 0xe01504, 0, 0);
        mmio_set(SGX_BASE + 0xe01508, 0, 0);
    }

    ram_base = DRAM_BASE;
    mem_size_actual = DRAM_SIZE;
    mcc_carveout_count = 1;
    mcc_carveouts[0].base = 0x101fc000000UL;
    mcc_carveouts[0].size = 0x100000;
}

#define CHK(x) assert((x) >= 0)

static void add_resv(void *dt, int resv, const char *name, u32 phandle)
{
    int n = fdt_add_subnode(dt, resv, name);
    CHK(n);
    fdt64_t reg[2] = {0, 0};
    CHK(fdt_setprop(dt, n, "reg", reg, sizeof(reg)));
    CHK(fdt_setprop_string(dt, n, "status", "disabled"));
    CHK(fdt_setprop_u32(dt, n, "phandle", phandle));
}

static void setup_dt(void *dt, const struct fixture *fx)
{
    CHK(fdt_create_empty_tree(dt, fx->dt_size));
    CHK(fdt_setprop_u32(dt, 0, "#address-cells", 2));
    CHK(fdt_setprop_u32(dt, 0, "#size-cells", 2));

    int aliases = fdt_add_subnode(dt, 0, "aliases");
    CHK(aliases);
    CHK(fdt_setprop_string(dt, aliases, "gpu", "/soc/gpu@292400000"));

    int chosen = fdt_add_subnode(dt, 0, "chosen");
    CHK(chosen);
    CHK(fdt_setprop_string(dt, chosen, "bootargs", "quiet"));
    if (fx->optin)
        CHK(fdt_setprop_string(dt, chosen, fx->optin, "1"));

    int resv = fdt_add_subnode(dt, 0, "reserved-memory");
    CHK(resv);
    CHK(fdt_setprop_u32(dt, resv, "#address-cells", 2));
    CHK(fdt_setprop_u32(dt, resv, "#size-cells", 2));
    CHK(fdt_setprop_empty(dt, resv, "ranges"));
    /* fdt_add_subnode() inserts at the head: add in reverse */
    add_resv(dt, resv, "globals", 16);
    add_resv(dt, resv, "hw-cal-b", 15);
    add_resv(dt, resv, "hw-cal-a", 14);
    add_resv(dt, resv, "uat-pagetables-l2", 13);
    add_resv(dt, resv, "uat-ttbs", 12);
    add_resv(dt, resv, "uat-pagetables", 11);
    add_resv(dt, resv, "uat-handoff", 10);

    int opp = fdt_add_subnode(dt, 0, "opp-table-gpu");
    CHK(opp);
    CHK(fdt_setprop_u32(dt, opp, "phandle", 30));
    for (int i = 1; i >= 0; i--) {
        char name[8];
        snprintf(name, sizeof(name), "opp%02d", i);
        int o = fdt_add_subnode(dt, opp, name);
        CHK(o);
        fdt32_t uv[2] = {cpu_to_fdt32(400000), cpu_to_fdt32(400000)};
        CHK(fdt_setprop_u64(dt, o, "opp-hz", 0));
        CHK(fdt_setprop(dt, o, "opp-microvolt", uv, sizeof(fdt32_t) * fx->clusters));
        CHK(fdt_setprop_u32(dt, o, "opp-microwatt", 0));
    }

    int soc = fdt_add_subnode(dt, 0, "soc");
    CHK(soc);
    CHK(fdt_setprop_u32(dt, soc, "#address-cells", 2));
    CHK(fdt_setprop_u32(dt, soc, "#size-cells", 2));
    CHK(fdt_setprop_empty(dt, soc, "ranges"));

    int mbox = fdt_add_subnode(dt, soc, "mbox@292408000");
    CHK(mbox);
    CHK(fdt_setprop_string(dt, mbox, "compatible", "apple,asc-mailbox-v4"));
    CHK(fdt_setprop_u32(dt, mbox, "phandle", 20));
    if (fx->mbox_disabled)
        CHK(fdt_setprop_string(dt, mbox, "status", "disabled"));

    int gpu = fdt_add_subnode(dt, soc, "gpu@292400000");
    CHK(gpu);
    CHK(fdt_setprop_string(dt, gpu, "compatible", fx->compatible));
    fdt64_t reg[4] = {cpu_to_fdt64(ASC_BASE), cpu_to_fdt64(ASC_SIZE), cpu_to_fdt64(SGX_BASE),
                      cpu_to_fdt64(SGX_SIZE)};
    CHK(fdt_setprop(dt, gpu, "reg", reg, sizeof(reg)));
    CHK(fdt_setprop(dt, gpu, "reg-names", "asc\0sgx", sizeof("asc\0sgx")));
    CHK(fdt_setprop_u32(dt, gpu, "mboxes", 20));
    fdt32_t mr[6] = {cpu_to_fdt32(12), cpu_to_fdt32(11), cpu_to_fdt32(10),
                     cpu_to_fdt32(14), cpu_to_fdt32(15), cpu_to_fdt32(16)};
    CHK(fdt_setprop(dt, gpu, "memory-region", mr, sizeof(mr)));
    static const char mrn[] = "ttbs\0pagetables\0handoff\0hw-cal-a\0hw-cal-b\0globals";
    CHK(fdt_setprop(dt, gpu, "memory-region-names", mrn, sizeof(mrn)));
    fdt32_t zero3[3] = {0, 0, 0};
    CHK(fdt_setprop(dt, gpu, "apple,firmware-abi", zero3, sizeof(zero3)));
    CHK(fdt_setprop(dt, gpu, "apple,firmware-version", zero3, sizeof(zero3)));
    CHK(fdt_setprop(dt, gpu, "apple,firmware-compat", zero3, sizeof(zero3)));
    CHK(fdt_setprop_u32(dt, gpu, "operating-points-v2", 30));
    fdt32_t one[2] = {cpu_to_fdt32(0x3f800000), cpu_to_fdt32(0x3f800000)};
    CHK(fdt_setprop(dt, gpu, "apple,core-leak-coef", one, sizeof(fdt32_t) * fx->clusters));
    CHK(fdt_setprop(dt, gpu, "apple,sram-leak-coef", zero3, sizeof(fdt32_t) * fx->clusters));
    CHK(fdt_setprop_string(dt, gpu, "status", "disabled"));
}

/* ---------- output ---------- */

static void dump_node_to(FILE *out, const void *dt, int node, int depth)
{
    int prop;
    fprintf(out, "%*s%s {\n", depth * 2, "", depth ? fdt_get_name(dt, node, NULL) : "/");
    fdt_for_each_property_offset(prop, dt, node)
    {
        const char *name;
        int len;
        const u8 *v = fdt_getprop_by_offset(dt, prop, &name, &len);
        fprintf(out, "%*s%s =", depth * 2 + 2, "", name);
        for (int i = 0; i < len; i++)
            fprintf(out, " %02x", v[i]);
        fprintf(out, ";\n");
    }
    int child;
    fdt_for_each_subnode(child, dt, node) dump_node_to(out, dt, child, depth + 1);
    fprintf(out, "%*s}\n", depth * 2, "");
}

/* The dump of the subtree at `path`, in a malloc()ed string */
static char *dump_path(const void *dt, const char *path)
{
    char *buf = NULL;
    size_t size = 0;
    FILE *out = open_memstream(&buf, &size);
    assert(out);
    int node = fdt_path_offset(dt, path);
    assert(node >= 0);
    dump_node_to(out, dt, node, 0);
    fclose(out);
    return buf;
}

static void report(const void *dt)
{
    fprintf(stdout, "== dt\n");
    dump_node_to(stdout, dt, 0, 0);
    fprintf(stdout, "== trace\n%s", trace_buf);
    fprintf(stdout, "== log\n%s", log_buf);
}

/* ---------- helpers for assertions ---------- */

static const char *status_of(const void *dt, const char *path)
{
    int n = fdt_path_offset(dt, path);
    assert(n >= 0);
    const char *s = fdt_getprop(dt, n, "status", NULL);
    return s ? s : "(none)";
}

static int prop_len(const void *dt, const char *path, const char *name)
{
    int n = fdt_path_offset(dt, path), len = -1;
    assert(n >= 0);
    return fdt_getprop(dt, n, name, &len) ? len : -1;
}

static int log_has(const char *s)
{
    return strstr(log_buf, s) != NULL;
}

static void expect_gpu_off(const void *dt)
{
    assert(!strcmp(status_of(dt, "/soc/gpu@292400000"), "disabled"));
    static const char *const uat[] = {"uat-handoff", "uat-pagetables", "uat-ttbs",
                                      "uat-pagetables-l2"};
    for (int i = 0; i < 4; i++) {
        char p[64];
        snprintf(p, sizeof(p), "/reserved-memory/%s", uat[i]);
        assert(!strcmp(status_of(dt, p), "disabled"));
    }
    assert(prop_len(dt, "/soc/gpu@292400000", "apple,m3-handoff-version") < 0);
    assert(unguarded_faults == 0);
}

/* ---------- scenarios ---------- */

static const struct fixture t6030_fx = {T6030, "apple,agx-t6030", "asahi,t6030-gpu", 2, 0, 65536};
#if defined(G15_NEW) || defined(G15_T8122_BASE)
static const struct fixture t8122_fx = {T8122, "apple,agx-t8122", "asahi,t8122-gpu", 1, 1, 65536};
#endif

static struct fixture fx;
static u8 dtbuf[1 << 17];

static void prepare(const struct fixture *f)
{
    fx = *f;
    chip_id = fx.chip;
    log_len = trace_len = 0;
    log_buf[0] = trace_buf[0] = 0;
    pmgr_enables = pmgr_disables = mmio_reads = unguarded_faults = 0;
    pmgr_enable_ret = 0;
    pmgr_off = pmgr_off_after_release = 1;
    exc_guard = GUARD_OFF;
    exc_count = 0;
    os_firmware = fw_versions[V14_7];
    fw_text = FW_TEXT;
    ram_min = 0x10000000000;
    ram_size = 0x1e0000000;
    setup_adt(&fx);
    setup_bus(&fx);
    setup_dt(dtbuf, &fx);
}

static int run(void)
{
    return dt_set_gpu(dtbuf);
}

/* T6030: results must match the pre-refactor setup. */
static void t6030_dry(void)
{
    struct fixture f = t6030_fx;
    f.optin = NULL;
    prepare(&f);
    run();
}
static void t6030_armed(void)
{
    prepare(&t6030_fx);
    run();
}
static void t6030_unknown_fw(void)
{
    prepare(&t6030_fx);
    fw_hdr[5] ^= 1;
    run();
}
static void t6030_wrong_id(void)
{
    prepare(&t6030_fx);
    mmio_set(SGX_BASE + 0xd04000, 0x07021100, 0);
    run();
}
static void t6030_core_mask(void)
{
    prepare(&t6030_fx);
    mmio_set(SGX_BASE + 0xe01504, 0x10, 0);
    run();
}
static void t6030_fuse_fault(void)
{
    prepare(&t6030_fx);
    mmio_set(LEAK_FUSE, 0, 1);
    run();
}
static void t6030_id_fault(void)
{
    prepare(&t6030_fx);
    mmio_set(SGX_BASE + 0xd04000, 0, 1);
    run();
}
static void t6030_region_in_ram(void)
{
    prepare(&t6030_fx);
    hset_u64("/arm-io/sgx", "gpu-region-base", ram_min + 0x100000);
    run();
}
static void t6030_nub_mismatch(void)
{
    prepare(&t6030_fx);
    hset_u32("/arm-io/gfx-asc/iop-gfx-nub", "pre-loaded", 0);
    run();
}
static void t6030_dt_sgx_mismatch(void)
{
    prepare(&t6030_fx);
    u64 sgx_reg[2] = U64PAIR(SGX_BASE + 0x10000, SGX_SIZE);
    hset("/arm-io/sgx", "reg", sgx_reg, sizeof(sgx_reg));
    run();
}
static void t6030_bad_pstate(void)
{
    prepare(&t6030_fx);
    hset_u32("/arm-io/sgx", "gpu-num-perf-states", 5);
    run();
}
static void t6030_no_table_count(void)
{
    prepare(&t6030_fx);
    hdel("/arm-io/sgx", "perf-state-table-count");
    run();
}
static void t6030_power_fail(void)
{
    prepare(&t6030_fx);
    pmgr_enable_ret = -1;
    run();
}
static void t6030_small_dt(void)
{
    prepare(&t6030_fx);
    /* Leave a little room, so the OPP rebuild runs out of space part way */
    fdt_pack(dtbuf);
    CHK(fdt_open_into(dtbuf, dtbuf, fdt_totalsize(dtbuf) + 0x200));
    run();
}
static void t6030_tiny_dt(void)
{
    prepare(&t6030_fx);
    /* Too little room to rebuild the OPP table */
    fdt_pack(dtbuf);
    CHK(fdt_open_into(dtbuf, dtbuf, fdt_totalsize(dtbuf) + 0x40));
    run();
}
static void t6030_no_alias(void)
{
    prepare(&t6030_fx);
    fdt_delprop(dtbuf, fdt_path_offset(dtbuf, "/aliases"), "gpu");
    run();
}
static void t6030_wrong_compat(void)
{
    struct fixture f = t6030_fx;
    f.compatible = "apple,agx-t6031";
    prepare(&f);
    run();
}

/*
 * T6030 with the T8122 stand-in switch set too: the same log, bus and PMGR traces and GPU, OPP and
 * reserved-memory nodes as without it (and, through the run script, as an older tree).
 */
static void t6030_standin_switch(void)
{
    prepare(&t6030_fx);
    run();
    static char log0[sizeof(log_buf)], trace0[sizeof(trace_buf)];
    memcpy(log0, log_buf, sizeof(log_buf));
    memcpy(trace0, trace_buf, sizeof(trace_buf));
    static const char *const paths[] = {"/soc", "/opp-table-gpu", "/reserved-memory"};
    char *nodes0[3];
    for (int i = 0; i < 3; i++)
        nodes0[i] = dump_path(dtbuf, paths[i]);

    prepare(&t6030_fx);
    CHK(fdt_setprop_string(dtbuf, fdt_path_offset(dtbuf, "/chosen"),
                           "asahi,t8122-gpu-power-standin", "1"));
    run();
    assert(!strcmp(log0, log_buf) && !strcmp(trace0, trace_buf));
    for (int i = 0; i < 3; i++) {
        char *nodes = dump_path(dtbuf, paths[i]);
        assert(!strcmp(nodes0[i], nodes));
        free(nodes);
        free(nodes0[i]);
    }
    assert(!strcmp(status_of(dtbuf, "/soc/gpu@292400000"), "okay"));
}

#if defined(G15_NEW) || defined(G15_T8122_BASE)
/*
 * T8122 without the stand-in switch. The setup must refuse before any GPU access: the G15G image,
 * leakage fuse and power model are not known. These also build against an older tree with the
 * T8122 setup (G15_T8122_BASE), and their results must not have changed.
 */
static void t8122_default(void)
{
    struct fixture f = t8122_fx;
    f.optin = NULL;
    prepare(&f);
    static u8 before[sizeof(dtbuf)];
    memcpy(before, dtbuf, sizeof(dtbuf));
    run();
    assert(!memcmp(before, dtbuf, sizeof(dtbuf)));
    assert(!trace_len && !pmgr_enables && !mmio_reads);
}

static void t8122_armed_refuses(void)
{
    prepare(&t8122_fx);
    run();
    expect_gpu_off(dtbuf);
    assert(!strcmp(status_of(dtbuf, "/soc/mbox@292408000"), "disabled"));
    assert(!pmgr_enables && !mmio_reads);
    assert(log_has("FW: GFX: image uuid"));
    assert(log_has("T8122 GPU setup failed"));
}

/*
 * A 10-core J613 (M3 Air, 16 GB): its ADT pstates, power-management values, UAT regions, firmware
 * segments and G15G firmware image, and its powered identity, with `core_mask` 0x3ff (10 cores) or
 * 0x37e (8 cores). Armed with chosen.asahi,t8122-gpu=1, plus the stand-in switch if `standin`.
 */
static const u8 g15g_c0_uuid[16] = {0xdf, 0x69, 0x7f, 0x05, 0xf6, 0xb5, 0x33, 0xef,
                                    0xa1, 0x37, 0x61, 0xc4, 0xa7, 0x3d, 0x66, 0x6a};

static const struct {
    const char *name;
    u32 val;
} j613_power[] = {
    {"gpu-perf-base-pstate", 0x00000001},
    {"gpu-avg-power-filter-tc-ms", 0x00000028},
    {"gpu-avg-power-ki-only", 0x428c0000},
    {"gpu-avg-power-kp", 0x3ff9999a},
    {"gpu-avg-power-min-duty-cycle", 0x0000001e},
    {"gpu-avg-power-target-filter-tc", 0x00000001},
    {"gpu-fast-die0-integral-gain", 0x43e10000},
    {"gpu-fast-die0-proportional-gain", 0x42080000},
    {"gpu-perf-boost-ce-step", 0x00000032},
    {"gpu-perf-boost-min-util", 0x0000005a},
    {"gpu-perf-filter-drop-threshold", 0x00000000},
    {"gpu-perf-filter-time-constant", 0x00000005},
    {"gpu-perf-filter-time-constant2", 0x000000c8},
    {"gpu-perf-integral-gain", 0x3f4c28f6},
    {"gpu-perf-integral-gain2", 0x3f4c28f6},
    {"gpu-perf-integral-min-clamp", 0x00000000},
    {"gpu-perf-proportional-gain", 0x40accccd},
    {"gpu-perf-proportional-gain2", 0x40accccd},
    {"gpu-perf-tgt-utilization", 0x00000055},
    {"gpu-power-sample-period", 0x00000008},
    {"gpu-ppm-filter-time-constant-ms", 0x00000064},
    {"gpu-ppm-ki", 0x42c23333},
    {"gpu-ppm-kp", 0x4039999a},
    {"gpu-pwr-filter-time-constant", 0x00000139},
    {"gpu-pwr-integral-gain", 0x3ca59586},
    {"gpu-pwr-integral-min-clamp", 0x00000000},
    {"gpu-pwr-min-duty-cycle", 0x0000001e},
    {"gpu-pwr-proportional-gain", 0x40a90fdb},
    {"gpu-pwr-sample-period-aic-clks", 0x00030d40},
    {"gpu-se-engagement-criteria", 0x00000258},
    {"gpu-se-filter-time-constant", 0x00000009},
    {"gpu-se-filter-time-constant-1", 0x00000003},
    {"gpu-se-inactive-threshold", 0x000009c4},
    {"gpu-se-ki", 0xc2480000},
    {"gpu-se-ki-1", 0xc2c80000},
    {"gpu-se-kp", 0xc0a00000},
    {"gpu-se-kp-1", 0xc1200000},
    {"gpu-se-reset-criteria", 0x00000032},
    {"gpu-se-tgt", 0x00000578},
    {"gpu-fast-die0-sensor-mask", 0x00000002},
};

#define J613_FW_TEXT 0x1000084c000UL
#define J613_FW_DATA 0x10001888000UL

static void prepare_j613_setup(u32 core_mask, int standin)
{
    prepare(&t8122_fx);

    hset_u32("/arm-io", "chip-revision", 0x20);
    static const struct perf_state ps[14] = {
        {0, 125},          {338000000, 630},  {618000000, 690},  {796000000, 720},
        {836000000, 760},  {928000000, 760},  {952000000, 810},  {1056000000, 810},
        {1053000000, 870}, {1170000000, 870}, {1152000000, 910}, {1278000000, 910},
        {1204000000, 935}, {1338000000, 935},
    };
    static const struct perf_state sram[14] = {
        {0, 790},          {338000000, 790},  {618000000, 790},  {796000000, 790},
        {836000000, 790},  {928000000, 790},  {952000000, 810},  {1056000000, 810},
        {1053000000, 870}, {1170000000, 870}, {1152000000, 910}, {1278000000, 910},
        {1204000000, 935}, {1338000000, 935},
    };
    hset_u32("/arm-io/sgx", "perf-state-count", 14);
    hset_u32("/arm-io/sgx", "gpu-num-perf-states", 13);
    hset_u32("/arm-io/sgx", "perf-state-table-count", 1);
    hset("/arm-io/sgx", "perf-states", ps, sizeof(ps));
    hset("/arm-io/sgx", "perf-states-sram", sram, sizeof(sram));
    hset_u64("/arm-io/sgx", "gfx-handoff-base", 0x103fff70000UL);
    hset_u64("/arm-io/sgx", "gfx-handoff-size", 0x4000);
    hset_u64("/arm-io/sgx", "gfx-shared-region-base", 0x103fff78000UL);
    hset_u64("/arm-io/sgx", "gfx-shared-region-size", 0x40000);
    hset_u64("/arm-io/sgx", "gpu-region-base", 0x103fffb8000UL);
    hset_u64("/arm-io/sgx", "gpu-region-size", 0x4000);
    hset_u64("/arm-io/sgx", "gfx-shared-l2-region-base", 0x103fff74000UL);
    hset_u64("/arm-io/sgx", "gfx-shared-l2-region-size", 0x4000);
    for (size_t i = 0; i < ARRAY_SIZE(j613_power); i++)
        hset_u32("/arm-io/sgx", j613_power[i].name, j613_power[i].val);

    struct adt_segment_ranges seg[2] = {
        {J613_FW_TEXT, FW_VA, J613_FW_TEXT, 0x64000, 1},
        {J613_FW_DATA, FW_VA + 0x64000, J613_FW_DATA, 0xb0000, 0},
    };
    hset("/arm-io/gfx-asc", "segment-ranges", seg, sizeof(seg));
    hset("/arm-io/gfx-asc/iop-gfx-nub", "segment-ranges", seg, sizeof(seg));
    hset_u64("/chosen", "dram-base", DRAM_BASE);
    hset_u64("/chosen", "dram-size", 0x400000000UL);
    ram_base = DRAM_BASE;
    mem_size_actual = 0x400000000UL;
    ram_min = 0x10001f74000UL;
    ram_size = 0x3dc4ac000UL;

    fw_text = J613_FW_TEXT;
    memcpy(&fw_hdr[5], g15g_c0_uuid, sizeof(g15g_c0_uuid));
    fw_hdr[11] = 0x5d54a;
    fw_hdr[12] = 0x1e8;
    fw_hdr[13] = 0x64000;

    mmio_set(SGX_BASE + 0xd04010, 0x0011010a, 0);
    mmio_set(SGX_BASE + 0xe01500, core_mask, 0);

    /* The T8122 DT's four memory regions, and no leakage placeholders */
    int gpu = fdt_path_offset(dtbuf, "/soc/gpu@292400000");
    fdt32_t mr[4] = {cpu_to_fdt32(12), cpu_to_fdt32(11), cpu_to_fdt32(10), cpu_to_fdt32(13)};
    CHK(fdt_setprop(dtbuf, gpu, "memory-region", mr, sizeof(mr)));
    static const char mrn[] = "ttbs\0pagetables\0handoff\0pagetables-l2";
    CHK(fdt_setprop(dtbuf, gpu, "memory-region-names", mrn, sizeof(mrn)));
    CHK(fdt_delprop(dtbuf, gpu, "apple,core-leak-coef"));
    CHK(fdt_delprop(dtbuf, gpu, "apple,sram-leak-coef"));
    if (standin)
        CHK(fdt_setprop_string(dtbuf, fdt_path_offset(dtbuf, "/chosen"),
                               "asahi,t8122-gpu-power-standin", "1"));
}

static void t8122_j613_switch_off(void)
{
    /* The J613's own G15G image, without the stand-in switch: refused as before */
    prepare_j613_setup(0x3ff, 0);
    run();
    expect_gpu_off(dtbuf);
    assert(!strcmp(status_of(dtbuf, "/soc/mbox@292408000"), "disabled"));
    assert(!pmgr_enables && !mmio_reads);
    assert(log_has("the G15G leakage fuse and power model are not known yet"));
    assert(log_has("FW: GFX: image uuid df697f05-f6b5-33ef-a137-61c4a73d666a"));
    assert(log_has("FW: GFX: unknown GPU firmware image, not identified"));
    assert(log_has("loaded GPU firmware is not a known ABI"));
    assert(log_has("T8122 GPU setup failed"));
    assert(prop_len(dtbuf, "/soc/gpu@292400000", "apple,se-target") < 0);
}
#endif

#ifdef G15_NEW

/*
 * The T8122 pipeline with the data that is still missing supplied by the test: the G15S image
 * stands in for the G15G one, and the leakage and power model are fixed test values.
 */
static int test_read_leak(const struct g15_soc *soc, float *leak)
{
    (void)soc;
    *leak = 2.0f;
    return 0;
}

static void test_calc_power(u32 count, const struct perf_state *core, float leak, u32 cores,
                            u32 *max_pwr)
{
    (void)leak;
    (void)cores;
    max_pwr[0] = 0;
    for (u32 i = 1; i < count; i++)
        max_pwr[i] = core[i].volt * (core[i].freq / 1000000);
}

static struct g15_soc test_soc;

static int run_t8122_test_soc(void)
{
    test_soc = g15_soc_t8122;
    test_soc.chip = T6030; /* the G15S images stand in for the G15G ones */
    test_soc.read_leak = test_read_leak;
    test_soc.calc_power = test_calc_power;
    return dt_set_gpu_g15(dtbuf, &test_soc);
}

static void t8122_pipeline(void)
{
    prepare(&t8122_fx);
    run_t8122_test_soc();
    const char *gpu = "/soc/gpu@292400000";
    assert(!strcmp(status_of(dtbuf, gpu), "okay"));
    assert(!strcmp(status_of(dtbuf, "/soc/mbox@292408000"), "okay"));
    assert(!strcmp(status_of(dtbuf, "/reserved-memory/uat-pagetables-l2"), "okay"));
    assert(prop_len(dtbuf, gpu, "apple,core-leak-coef") == 4);
    assert(prop_len(dtbuf, gpu, "apple,sram-leak-coef") == 4);
    assert(prop_len(dtbuf, gpu, "apple,m3-handoff-version") == 4);
    assert(prop_len(dtbuf, "/opp-table-gpu/opp04", "opp-microvolt") == 4);
    assert(prop_len(dtbuf, "/opp-table-gpu/opp00", "opp-microvolt") == 4);
    int gpu_node = fdt_path_offset(dtbuf, gpu);
    const fdt32_t *compat = fdt_getprop(dtbuf, gpu_node, "apple,firmware-compat", NULL);
    assert(fdt32_to_cpu(compat[0]) == 14 && fdt32_to_cpu(compat[1]) == 8 &&
           fdt32_to_cpu(compat[2]) == 3);
    assert(pmgr_enables == 1 && pmgr_disables == 0 && unguarded_faults == 0);
    assert(log_has("1 cluster(s) x 10 core slots"));
    assert(log_has("T8122 GPU enabled"));
}

static void t8122_closure_on(void)
{
    prepare(&t8122_fx);
    pmgr_off = 0;
    run_t8122_test_soc();
    expect_gpu_off(dtbuf);
    assert(!pmgr_enables && !mmio_reads);
    assert(log_has("power closure is not off"));
}

static void t8122_wrong_id(void)
{
    prepare(&t8122_fx);
    mmio_set(SGX_BASE + 0xd04000, 0x07032000, 0);
    run_t8122_test_soc();
    expect_gpu_off(dtbuf);
    assert(!strcmp(status_of(dtbuf, "/soc/mbox@292408000"), "disabled"));
    assert(pmgr_enables == 1 && pmgr_disables == 1 && pmgr_off == 1);
    assert(strstr(trace_buf, "pmgr disable-recursive /arm-io/sgx"));
}

static void t8122_two_dies(void)
{
    prepare(&t8122_fx);
    mmio_set(SGX_BASE + 0xd04010, 0x0002010a, 0);
    run_t8122_test_soc();
    expect_gpu_off(dtbuf);
    assert(pmgr_disables == 1 && pmgr_off == 1);
    assert(log_has("dies"));
}

static void t8122_mask_beyond_slots(void)
{
    prepare(&t8122_fx);
    mmio_set(SGX_BASE + 0xe01500, 0x000004ff, 0);
    run_t8122_test_soc();
    expect_gpu_off(dtbuf);
    assert(pmgr_disables == 1);
}

static void t8122_tables_exceed_clusters(void)
{
    struct fixture f = t8122_fx;
    f.clusters = 2;
    prepare(&f);
    run_t8122_test_soc();
    expect_gpu_off(dtbuf);
    assert(pmgr_disables == 1);
    assert(log_has("perf-state tables"));
}

static void t8122_release_fails(void)
{
    prepare(&t8122_fx);
    mmio_set(SGX_BASE + 0xd04000, 0, 1);
    pmgr_off_after_release = 0;
    run_t8122_test_soc();
    expect_gpu_off(dtbuf);
    assert(log_has("failed to power the GPU back off"));
}

static void t8122_small_dt(void)
{
    /* Too little room to rebuild the OPP table: everything is undone and the power released */
    prepare(&t8122_fx);
    fdt_pack(dtbuf);
    CHK(fdt_open_into(dtbuf, dtbuf, fdt_totalsize(dtbuf) + 0x40));
    run_t8122_test_soc();
    expect_gpu_off(dtbuf);
    assert(!strcmp(status_of(dtbuf, "/soc/mbox@292408000"), "disabled"));
    assert(pmgr_disables == 1 && pmgr_off == 1);
}

/*
 * The reservation-only diagnostic with the powered identity probe: the probe reads ID_VERSION,
 * ID_COUNTS_1 and the core masks under the guard and publishes them, and releases the power.
 */
static u32 identity_u32(const char *name)
{
    int n = fdt_path_offset(dtbuf, "/chosen/asahi,t8122-gpu-powered-identity"), len = 0;
    assert(n >= 0);
    const fdt32_t *v = fdt_getprop(dtbuf, n, name, &len);
    assert(v && len == 4);
    return fdt32_to_cpu(*v);
}

static void prepare_powered_identity(void)
{
    struct fixture f = t8122_fx;
    f.optin = NULL;
    prepare(&f);
    CHK(fdt_setprop_string(dtbuf, fdt_path_offset(dtbuf, "/chosen"),
                           "asahi,t8122-gpu-handoff-diag", "1"));
    CHK(fdt_setprop_string(dtbuf, fdt_path_offset(dtbuf, "/chosen"), "asahi,t8122-gpu-power-diag",
                           "1"));
}

static void t8122_powered_identity(void)
{
    prepare_powered_identity();
    run();
    assert(log_has("T8122 UAT and firmware reservations supplied from ADT"));
    assert(identity_u32("schema-version") == 1);
    assert(identity_u32("probe-result") == T8122_ID_PASS);
    assert(identity_u32("id-version") == 0x07022000);
    assert(identity_u32("id-counts-1") == 0x0001010a);
    assert(identity_u32("id-counts-2") == 0x00040404);
    assert(identity_u32("core-mask-0") == 0xff);
    assert(identity_u32("core-mask-1") == 0 && identity_u32("core-mask-2") == 0);
    assert(log_has("ID_VERSION 0x07022000 ID_COUNTS_1 0x0001010a ID_COUNTS_2 0x00040404 "
                   "core masks 0xff 0x0 0x0"));
    assert(strstr(trace_buf,
                  "pmgr enable /arm-io/sgx\nread32 0x290d04000\nread32 0x290d04010\n"
                  "read32 0x290d04014\n"));
    assert(pmgr_enables == 1 && pmgr_disables == 1 && pmgr_off == 1);
    assert(!strcmp(status_of(dtbuf, "/soc/gpu@292400000"), "disabled"));
    assert(!strcmp(status_of(dtbuf, "/soc/mbox@292408000"), "disabled"));
    assert(unguarded_faults == 0);
}

static void t8122_powered_identity_counts(void)
{
    /* Any ID_COUNTS_1 is published as read; it does not change the result */
    prepare_powered_identity();
    mmio_set(SGX_BASE + 0xd04010, 0x00020203, 0);
    run();
    assert(identity_u32("probe-result") == T8122_ID_PASS);
    assert(identity_u32("id-counts-1") == 0x00020203);
    assert(identity_u32("id-counts-2") == 0x00040404);
    assert(pmgr_disables == 1 && pmgr_off == 1 && unguarded_faults == 0);
}

static void t8122_powered_identity_counts2(void)
{
    /* Any ID_COUNTS_2 is published as read; it does not change the result */
    prepare_powered_identity();
    mmio_set(SGX_BASE + 0xd04014, 0x00050607, 0);
    run();
    assert(identity_u32("probe-result") == T8122_ID_PASS);
    assert(identity_u32("id-counts-2") == 0x00050607);
    assert(pmgr_disables == 1 && pmgr_off == 1 && unguarded_faults == 0);
}

static void t8122_powered_identity_counts_fault(void)
{
    /* A fault on ID_COUNTS_1 is a read fault, caught by the guard; the power is released */
    prepare_powered_identity();
    mmio_set(SGX_BASE + 0xd04010, 0, 1);
    run();
    assert(identity_u32("probe-result") == T8122_ID_READ_FAULT);
    assert(pmgr_enables == 1 && pmgr_disables == 1 && pmgr_off == 1);
    assert(unguarded_faults == 0);
}

static void t8122_powered_identity_counts2_fault(void)
{
    /* A fault on ID_COUNTS_2 is a read fault, caught by the guard; the power is released */
    prepare_powered_identity();
    mmio_set(SGX_BASE + 0xd04014, 0, 1);
    run();
    assert(identity_u32("probe-result") == T8122_ID_READ_FAULT);
    assert(pmgr_enables == 1 && pmgr_disables == 1 && pmgr_off == 1);
    assert(unguarded_faults == 0);
}

/*
 * A J613 G15G match: the reservation diagnostic and powered identity probe
 * opted in, the root marked apple,j613, the SGX registers reading this J613's
 * exact identity, and the owner-supplied image metadata strings in /chosen.
 */
#define J613_UUID "0cbb17af783d367b9cedd5910944731e"
static void prepare_j613(void)
{
    prepare_powered_identity();
    CHK(fdt_setprop_string(dtbuf, 0, "compatible", "apple,j613"));
    mmio_set(SGX_BASE + 0xd04010, 0x0011010a, 0);
    mmio_set(SGX_BASE + 0xe01500, 0x37e, 0);
    int chosen = fdt_path_offset(dtbuf, "/chosen");
    CHK(fdt_setprop_string(dtbuf, chosen, "asahi,t8122-gpu-abi-ack", "14.8.3"));
    CHK(fdt_setprop_string(dtbuf, chosen, "asahi,t8122-gpu-expected-uuid", J613_UUID));
    CHK(fdt_setprop_string(dtbuf, chosen, "asahi,t8122-gpu-expected-rtkit", "RTKit-test"));
}

static void t8122_j613_image_admitted(void)
{
    /* Image admission publishes apple,firmware-compat but enables nothing */
    prepare_j613();
    run();
    assert(identity_u32("probe-result") == T8122_ID_PASS);
    assert(identity_u32("id-counts-1") == 0x0011010a);
    assert(identity_u32("id-counts-2") == 0x00040404);
    assert(identity_u32("core-mask-0") == 0x37e);
    assert(log_has("J613 exact image metadata admitted"));
    int gpu = fdt_path_offset(dtbuf, "/soc/gpu@292400000"), len = 0;
    const fdt32_t *compat = fdt_getprop(dtbuf, gpu, "apple,firmware-compat", &len);
    assert(compat && len == 12 && fdt32_to_cpu(compat[0]) == 14 &&
           fdt32_to_cpu(compat[1]) == 8 && fdt32_to_cpu(compat[2]) == 3);
    assert(!strcmp(status_of(dtbuf, "/soc/gpu@292400000"), "disabled"));
    assert(!strcmp(status_of(dtbuf, "/soc/mbox@292408000"), "disabled"));
    assert(unguarded_faults == 0);
}

static void t8122_asc_preflight_enables(void)
{
    /* With only asahi,t8122-gpu-asc-preflight=1, the preflight enables both nodes */
    prepare_j613();
    CHK(fdt_setprop_string(dtbuf, fdt_path_offset(dtbuf, "/chosen"),
                           "asahi,t8122-gpu-asc-preflight", "1"));
    run();
    assert(log_has("J613 exact image metadata admitted"));
    assert(log_has("one-boot ASC register preflight enabled"));
    assert(!strcmp(status_of(dtbuf, "/soc/gpu@292400000"), "okay"));
    assert(!strcmp(status_of(dtbuf, "/soc/mbox@292408000"), "okay"));
    assert(unguarded_faults == 0);
}

static void t8122_asc_preflight_full_optin_noop(void)
{
    /* With the full GPU opt-in present, the preflight no-ops and leaves the two
     * nodes to the full setup. Driven directly on an admitted-but-disabled DT:
     * the full opt-in otherwise dispatches away from this diagnostic entirely.
     */
    prepare_j613();
    run(); /* admits image metadata, sets apple,firmware-compat; nodes stay disabled */
    assert(log_has("J613 exact image metadata admitted"));
    assert(!strcmp(status_of(dtbuf, "/soc/gpu@292400000"), "disabled"));
    int chosen = fdt_path_offset(dtbuf, "/chosen");
    CHK(fdt_setprop_string(dtbuf, chosen, "asahi,t8122-gpu", "1"));
    CHK(fdt_setprop_string(dtbuf, chosen, "asahi,t8122-gpu-asc-preflight", "1"));
    log_len = 0;
    log_buf[0] = 0;
    t8122_enable_asc_preflight(dtbuf);
    assert(log_has("ASC preflight skipped"));
    assert(!log_has("one-boot ASC register preflight enabled"));
    assert(!strcmp(status_of(dtbuf, "/soc/gpu@292400000"), "disabled"));
    assert(!strcmp(status_of(dtbuf, "/soc/mbox@292408000"), "disabled"));
}

static void t8122_with_diag_optin(void)
{
    /* The full setup takes precedence over the reservation-only diagnostic */
    prepare(&t8122_fx);
    fdt_setprop_string(dtbuf, fdt_path_offset(dtbuf, "/chosen"), "asahi,t8122-gpu-handoff-diag",
                       "1");
    run();
    expect_gpu_off(dtbuf);
    assert(log_has("handoff-diag ignored"));
}

/*
 * The stand-in experiment on the J613 inputs: chosen.asahi,t8122-gpu=1 and
 * chosen.asahi,t8122-gpu-power-standin=1.
 */
static const fdt32_t *gpu_prop(const char *name, int *len)
{
    int gpu = fdt_path_offset(dtbuf, "/soc/gpu@292400000");
    assert(gpu >= 0);
    return fdt_getprop(dtbuf, gpu, name, len);
}

static u32 gpu_u32(const char *name)
{
    int len = 0;
    const fdt32_t *v = gpu_prop(name, &len);
    assert(v && len == 4);
    return fdt32_to_cpu(*v);
}

static void expect_standin_handoff(const u32 *max_pwr)
{
    const char *gpu = "/soc/gpu@292400000";
    assert(!strcmp(status_of(dtbuf, gpu), "okay"));
    assert(!strcmp(status_of(dtbuf, "/soc/mbox@292408000"), "okay"));
    static const char *const uat[] = {"uat-handoff", "uat-pagetables", "uat-ttbs",
                                      "uat-pagetables-l2"};
    for (int i = 0; i < 4; i++) {
        char p[64];
        snprintf(p, sizeof(p), "/reserved-memory/%s", uat[i]);
        assert(!strcmp(status_of(dtbuf, p), "okay"));
    }
    assert(log_has("T8122 experiment armed"));
    assert(log_has("FW: GFX: identified image g15g build c0, firmware 14.8.3"));
    assert(log_has("FDT: GPU: T8122 power model: stand-in (not measured; does not limit clocks)"));
    assert(!log_has("perf-states-sram present but unused"));
    /* Not a J613 root here: no fuse read, and the stand-in leakage */
    assert(log_has("FDT: GPU: T8122 leakage: the stand-in\n"));
    assert(!strstr(trace_buf, "0x2e42c812"));
    assert(prop_len(dtbuf, "/chosen", "asahi,t8122-gpu-leak-fuse") < 0);
    assert(log_has("FDT: GPU: Leakage: 6412.000"));
    assert(log_has("FDT: GPU: T8122 GPU enabled (firmware 14.8.3)"));
    assert(log_has("ADT: GPU: 39 power-management values, fast-die sensor mask 0x2, SRAM floor "
                   "790 mV, idle-off standby timer absent (kernel default)"));
    assert(log_has("FDT: GPU: 41 power-management properties added from the ADT"));

    /* firmware version, ABI and compat: 14.8.3 */
    static const char *const fw[] = {"apple,firmware-version", "apple,firmware-abi",
                                     "apple,firmware-compat"};
    for (int i = 0; i < 3; i++) {
        int len = 0;
        const fdt32_t *v = gpu_prop(fw[i], &len);
        assert(v && len == 12 && fdt32_to_cpu(v[0]) == 14 && fdt32_to_cpu(v[1]) == 8 &&
               fdt32_to_cpu(v[2]) == 3);
    }
    assert(prop_len(dtbuf, gpu, "apple,m3-handoff-version") == 4);
    assert(fdt_stringlist_search(dtbuf, fdt_path_offset(dtbuf, gpu), "memory-region-names",
                                 "fw-text") == 4);
    assert(fdt_stringlist_search(dtbuf, fdt_path_offset(dtbuf, gpu), "memory-region-names",
                                 "fw-data") == 5);

    /* The leakage: one cell per cluster, the stand-in for the core and 0 for SRAM */
    float leak = 6412.0f;
    u32 leak_bits;
    memcpy(&leak_bits, &leak, sizeof(leak_bits));
    assert(gpu_u32("apple,core-leak-coef") == leak_bits);
    assert(gpu_u32("apple,sram-leak-coef") == 0);

    /* Every ADT power-management value under its apple,* name, as it is */
    for (size_t i = 0; i < ARRAY_SIZE(g15_power_props); i++) {
        u32 want = 0;
        for (size_t j = 0; j < ARRAY_SIZE(j613_power); j++)
            if (!strcmp(j613_power[j].name, g15_power_props[i].adt))
                want = j613_power[j].val;
        assert(gpu_u32(g15_power_props[i].dt) == want);
    }
    assert(gpu_u32("apple,se-target") == 1400);
    assert(gpu_u32("apple,perf-base-pstate") == 1);
    assert(gpu_u32("apple,min-sram-microvolt") == 790000);
    int len = 0;
    const fdt32_t *mask = gpu_prop("apple,fast-die0-sensor-mask", &len);
    assert(mask && len == 8 && fdt32_to_cpu(mask[0]) == 0 && fdt32_to_cpu(mask[1]) == 2);
    assert(prop_len(dtbuf, gpu, "apple,idleoff-standby-timer") < 0);

    /* One OPP per ADT pstate, in ADT order, one voltage cell, the stand-in power */
    static const u32 hz[14] = {0,          338000000,  618000000,  796000000,  836000000,
                               928000000,  952000000,  1056000000, 1053000000, 1170000000,
                               1152000000, 1278000000, 1204000000, 1338000000};
    static const u32 mv[14] = {125, 630, 690, 720, 760, 760, 810,
                               810, 870, 870, 910, 910, 935, 935};
    int opps = fdt_path_offset(dtbuf, "/opp-table-gpu"), n = 0, opp;
    fdt_for_each_subnode(opp, dtbuf, opps)
    {
        char name[8];
        snprintf(name, sizeof(name), "opp%02d", n);
        assert(!strcmp(fdt_get_name(dtbuf, opp, NULL), name));
        const fdt64_t *f = fdt_getprop(dtbuf, opp, "opp-hz", &len);
        assert(f && len == 8 && fdt64_ld(f) == hz[n]);
        const fdt32_t *uv = fdt_getprop(dtbuf, opp, "opp-microvolt", &len);
        assert(uv && len == 4 && fdt32_to_cpu(*uv) == mv[n] * 1000);
        const fdt32_t *uw = fdt_getprop(dtbuf, opp, "opp-microwatt", &len);
        assert(uw && len == 4 && fdt32_to_cpu(*uw) == max_pwr[n]);
        n++;
    }
    assert(n == 14);

    /* Powered for the identity reads and left on for the kernel, as on T6030 */
    assert(pmgr_enables == 1 && pmgr_disables == 0 && unguarded_faults == 0);
}

/*
 * The stand-in max power table (uW) on the host's expf()/powf(); aura's own may differ in the last
 * digits
 */
static void t8122_j613_standin_10(void)
{
    prepare_j613_setup(0x3ff, 1);
    run();
    static const u32 max_pwr[14] = {0,        7370636,  10950020, 12234028, 14239188,
                                    15044005, 17748106, 18794802, 22305218, 23683214,
                                    25489264, 27127520, 28513048, 30362362};
    expect_standin_handoff(max_pwr);
    assert(log_has("1 cluster(s) x 10 core slots"));
    assert(log_has("Core counts: 10 \n"));
    assert(log_has("T8122 stand-in: G15S model, dynamic power for 10 of 20 G15S cores"));
}

static void t8122_j613_standin_8(void)
{
    prepare_j613_setup(0x37e, 1);
    run();
    static const u32 max_pwr[14] = {0,        6979241,  10075828, 10997526, 12776519,
                                    13420372, 15831849, 16669205, 19824824, 20927220,
                                    22493598, 23804204, 25189806, 26669256};
    expect_standin_handoff(max_pwr);
    assert(log_has("Core counts: 8 \n"));
    assert(log_has("T8122 stand-in: G15S model, dynamic power for 8 of 20 G15S cores"));
}

static void t8122_j613_standin_only(void)
{
    /* The stand-in switch alone arms nothing: logged, and the boot is the default one */
    prepare_j613_setup(0x3ff, 1);
    fdt_delprop(dtbuf, fdt_path_offset(dtbuf, "/chosen"), "asahi,t8122-gpu");
    static u8 before[sizeof(dtbuf)];
    memcpy(before, dtbuf, sizeof(dtbuf));
    run();
    assert(!memcmp(before, dtbuf, sizeof(dtbuf)));
    assert(!trace_len && !pmgr_enables && !mmio_reads);
    assert(log_has("asahi,t8122-gpu-power-standin ignored: it needs chosen.asahi,t8122-gpu=1"));
}

static void t8122_j613_standin_missing_value(void)
{
    /* A missing ADT power-management value stops the setup before the GPU is powered */
    prepare_j613_setup(0x3ff, 1);
    hdel("/arm-io/sgx", "gpu-se-tgt");
    run();
    expect_gpu_off(dtbuf);
    assert(!strcmp(status_of(dtbuf, "/soc/mbox@292408000"), "disabled"));
    assert(!pmgr_enables && !mmio_reads);
    assert(log_has("ADT: GPU: missing or invalid gpu-se-tgt"));
    assert(log_has("T8122 GPU setup failed"));
    assert(prop_len(dtbuf, "/soc/gpu@292400000", "apple,se-ki") < 0);
}

static void t8122_j613_standin_bad_mask_size(void)
{
    /* A fast-die sensor mask that is not 4 bytes is refused the same way */
    prepare_j613_setup(0x3ff, 1);
    u64 mask = 2;
    hset("/arm-io/sgx", "gpu-fast-die0-sensor-mask", &mask, sizeof(mask));
    run();
    expect_gpu_off(dtbuf);
    assert(!pmgr_enables && !mmio_reads);
    assert(log_has("missing or invalid gpu-fast-die0-sensor-mask"));
}

static void t8122_j613_standin_bad_sram(void)
{
    /* SRAM pstates that do not match the pstate table are refused before the GPU is powered */
    prepare_j613_setup(0x3ff, 1);
    struct perf_state sram[13] = {{0, 790}};
    hset("/arm-io/sgx", "perf-states-sram", sram, sizeof(sram));
    run();
    expect_gpu_off(dtbuf);
    assert(!pmgr_enables && !mmio_reads);
    assert(log_has("invalid or missing perf-states-sram"));
}

static void t8122_j613_standin_idleoff_malformed(void)
{
    /* An ADT idle-off standby timer that is not one cell is refused before the GPU is powered */
    prepare_j613_setup(0x3ff, 1);
    u64 timer = 700;
    hset("/arm-io/sgx", "gpu-idleoff-standby-timer", &timer, sizeof(timer));
    run();
    expect_gpu_off(dtbuf);
    assert(!pmgr_enables && !mmio_reads);
    assert(log_has("ADT: GPU: malformed gpu-idleoff-standby-timer (len 8)"));
    assert(prop_len(dtbuf, "/soc/gpu@292400000", "apple,idleoff-standby-timer") < 0);
}

static void set_j613_sram(u32 pstate, u32 mv)
{
    int node = adt_path_offset(adt, "/arm-io/sgx");
    u32 len = 0;
    struct perf_state *sram = (struct perf_state *)adt_getprop(adt, node, "perf-states-sram", &len);
    assert(sram && len == 14 * sizeof(*sram));
    sram[pstate].volt = mv;
}

static void t8122_j613_standin_sram_above(void)
{
    /*
     * SRAM above max(core, floor) at any pstate cannot be passed on (the kernel would give it
     * less): refused before the GPU is powered. Here the top pstate asks for core + 25 mV.
     */
    prepare_j613_setup(0x3ff, 1);
    set_j613_sram(13, 960);
    run();
    expect_gpu_off(dtbuf);
    assert(!strcmp(status_of(dtbuf, "/soc/mbox@292408000"), "disabled"));
    assert(!pmgr_enables && !mmio_reads);
    assert(log_has("ADT: GPU: perf state 0/13 SRAM 960 mV is above max(core 935 mV, floor 790 mV); "
                   "not supported"));
    assert(log_has("T8122 GPU setup failed"));
    assert(prop_len(dtbuf, "/soc/gpu@292400000", "apple,min-sram-microvolt") < 0);
}

static void t8122_j613_standin_sram_above_floor(void)
{
    /* The same below the core voltage: pstate 1 (630 mV core) asks for 800 mV, above the floor */
    prepare_j613_setup(0x3ff, 1);
    set_j613_sram(1, 800);
    run();
    expect_gpu_off(dtbuf);
    assert(!pmgr_enables && !mmio_reads);
    assert(log_has("ADT: GPU: perf state 0/1 SRAM 800 mV is above max(core 630 mV, floor 790 mV); "
                   "not supported"));
}

static void t8122_j613_standin_sram_below(void)
{
    /* SRAM below max(core, floor) is fine: the kernel then gives it more, not less */
    prepare_j613_setup(0x3ff, 1);
    set_j613_sram(13, 930);
    run();
    assert(!strcmp(status_of(dtbuf, "/soc/gpu@292400000"), "okay"));
    assert(gpu_u32("apple,min-sram-microvolt") == 790000);
    assert(log_has("T8122 GPU enabled"));
}

static void t8122_j613_standin_idleoff(void)
{
    /* An ADT idle-off standby timer is added too */
    prepare_j613_setup(0x3ff, 1);
    hset_u32("/arm-io/sgx", "gpu-idleoff-standby-timer", 700);
    run();
    assert(!strcmp(status_of(dtbuf, "/soc/gpu@292400000"), "okay"));
    assert(gpu_u32("apple,idleoff-standby-timer") == 700);
    assert(log_has("FDT: GPU: 42 power-management properties added from the ADT"));
}

static void t8122_j613_standin_g15s_image(void)
{
    /* The experiment accepts only the listed G15G image: a G15S image is not identified */
    prepare_j613_setup(0x3ff, 1);
    memcpy(&fw_hdr[5], g15s_a0_uuid, sizeof(g15s_a0_uuid));
    fw_hdr[11] = 0x58e88;
    fw_hdr[13] = 0x5c000;
    run();
    expect_gpu_off(dtbuf);
    assert(!pmgr_enables && !mmio_reads);
    assert(log_has("FW: GFX: unknown GPU firmware image, not identified"));
    assert(log_has("loaded GPU firmware is not a known ABI"));
}

static void t8122_j613_standin_no_room(void)
{
    /*
     * Room for the OPP table but not for the power-management properties: everything is undone
     * and the GPU power released
     */
    prepare_j613_setup(0x3ff, 1);
    fdt_pack(dtbuf);
    CHK(fdt_open_into(dtbuf, dtbuf, fdt_totalsize(dtbuf) + 0x700));
    run();
    expect_gpu_off(dtbuf);
    assert(!strcmp(status_of(dtbuf, "/soc/mbox@292408000"), "disabled"));
    assert(log_has("failed to add the power-management properties"));
    assert(pmgr_enables == 1 && pmgr_disables == 1 && pmgr_off == 1);
    assert(unguarded_faults == 0);
}

static void t8122_j613_standin_wrong_id(void)
{
    /* A failure after the power-on (here the GPU ID) releases the power, as without the switch */
    prepare_j613_setup(0x3ff, 1);
    mmio_set(SGX_BASE + 0xd04000, 0x07032000, 0);
    run();
    expect_gpu_off(dtbuf);
    assert(pmgr_enables == 1 && pmgr_disables == 1 && pmgr_off == 1);
    assert(log_has("not a G15G"));
}

/*
 * The T8122 leakage fuse (t8122_leak_fuse_decode(), t8122_leak_fuse_export()). Every fuse word
 * here is synthetic: no unit's values.
 */
static u32 float_bits(float f)
{
    u32 v;
    memcpy(&v, &f, sizeof(v));
    return v;
}

static void t8122_fuse_decode(void)
{
    static const struct {
        u32 word0, word1, a, b, v0, v1;
    } cases[] = {
        /* nothing set */
        {0x00000000, 0x00000000, 0x0000, 0x000, 2, 1},
        /* only the unused bits set: word 0 bits 28:0, word 1 bits 31:21 */
        {0x1fffffff, 0xffe00000, 0x0000, 0x000, 2, 1},
        /* each field's lowest and highest bit on its own */
        {0x20000000, 0x00000000, 0x0001, 0x000, 4, 1},          /* word 0 bit 29: A bit 0 */
        {0x80000000, 0x00000000, 0x0004, 0x000, 10, 1},         /* word 0 bit 31: A bit 2 */
        {0x00000000, 0x00000001, 0x0008, 0x000, 18, 1},         /* word 1 bit 0: A bit 3 */
        {0x00000000, 0x00000400, 0x2000, 0x000, 16386, 1},      /* word 1 bit 10: A bit 13 */
        {0x00000000, 0x00000800, 0x0000, 0x001, 2, 2},          /* word 1 bit 11: B bit 0 */
        {0x00000000, 0x00100000, 0x0000, 0x200, 2, 513},        /* word 1 bit 20: B bit 9 */
        /* every field bit set, and every bit set */
        {0xe0000000, 0x001fffff, 0x3fff, 0x3ff, 32768, 1024},
        {0xffffffff, 0xffffffff, 0x3fff, 0x3ff, 32768, 1024},
        /* a mixed pattern */
        {0xa0000000, 0x000aaaaa, 0x1555, 0x155, 10924, 342},
    };
    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        struct t8122_leak_fuse f;
        t8122_leak_fuse_decode(cases[i].word0, cases[i].word1, &f);
        if (f.field_a != cases[i].a || f.field_b != cases[i].b ||
            float_bits(f.value[0]) != float_bits((float)cases[i].v0) ||
            float_bits(f.value[1]) != float_bits((float)cases[i].v1)) {
            fprintf(stderr, "case %zu: 0x%08x 0x%08x -> A 0x%x B 0x%x %f %f\n", i, cases[i].word0,
                    cases[i].word1, f.field_a, f.field_b, f.value[0], f.value[1]);
            assert(0);
        }
    }
}

/* A T8122 default boot (no GPU switch), with a J613 root if `j613` and these fuse words */
static void prepare_fuse(int j613, u32 word0, u32 word1, int fault)
{
    struct fixture f = t8122_fx;
    f.optin = NULL;
    prepare(&f);
    if (j613)
        CHK(fdt_setprop_string(dtbuf, 0, "compatible", "apple,j613"));
    mmio_set(T8122_LEAK_FUSE_WORD0, word0, fault);
    mmio_set(T8122_LEAK_FUSE_WORD1, word1, 0);
}

static void expect_fuse_prop(u32 v0, u32 v1)
{
    int len = 0;
    const fdt32_t *p = fdt_getprop(dtbuf, fdt_path_offset(dtbuf, "/chosen"),
                                   "asahi,t8122-gpu-leak-fuse", &len);
    assert(p && len == 8);
    assert(fdt32_to_cpu(p[0]) == float_bits((float)v0) && fdt32_to_cpu(p[1]) == float_bits((float)v1));
}

static void t8122_j613_fuse_export(void)
{
    /* A default J613 boot: the two reads, the log and the property, and nothing else */
    prepare_fuse(1, 0xa0000000, 0x000aaaaa, 0);
    char *before = dump_path(dtbuf, "/");
    run();
    expect_fuse_prop(10924, 342);
    assert(log_has("FDT: GPU: T8122 leakage fuse: field A 0x1555, field B 0x155; values 10924 and 342\n"));
    assert(log_has("FDT: GPU: T8122 leakage fuse values in /chosen/asahi,t8122-gpu-leak-fuse; "
                   "the GPU setup does not use them\n"));
    assert(!strcmp(trace_buf, "read32 0x2e42c8128\nread32 0x2e42c812c\n"));
    assert(mmio_reads == 2 && !pmgr_enables && !exc_count && !unguarded_faults);
    CHK(fdt_delprop(dtbuf, fdt_path_offset(dtbuf, "/chosen"), "asahi,t8122-gpu-leak-fuse"));
    char *after = dump_path(dtbuf, "/");
    assert(!strcmp(before, after));
    free(before);
    free(after);
}

static void t8122_fuse_not_j613(void)
{
    /* Another T8122 root: the fuse is not read */
    prepare_fuse(0, 0xa0000000, 0x000aaaaa, 0);
    static u8 before[sizeof(dtbuf)];
    memcpy(before, dtbuf, sizeof(dtbuf));
    run();
    assert(!memcmp(before, dtbuf, sizeof(dtbuf)));
    assert(!trace_len && !mmio_reads && !log_has("leakage fuse"));
}

static void t8122_j613_fuse_fault(void)
{
    /* A faulting read is caught, logged, and the boot goes on without the property */
    prepare_fuse(1, 0, 0, 1);
    run();
    assert(prop_len(dtbuf, "/chosen", "asahi,t8122-gpu-leak-fuse") < 0);
    assert(log_has("FDT: GPU: T8122 leakage fuse: the read faulted; nothing exported\n"));
    assert(exc_count == 1 && !unguarded_faults && !pmgr_enables);
}

/* The stand-in experiment on a J613 root with fuse words; `use` sets the fuse switch */
static void prepare_j613_standin_fuse(int use, int fault)
{
    prepare_j613_setup(0x3ff, 1);
    CHK(fdt_setprop_string(dtbuf, 0, "compatible", "apple,j613"));
    mmio_set(T8122_LEAK_FUSE_WORD0, 0xa0000000, fault);
    mmio_set(T8122_LEAK_FUSE_WORD1, 0x000aaaaa, 0);
    if (use)
        CHK(fdt_setprop_string(dtbuf, fdt_path_offset(dtbuf, "/chosen"),
                               "asahi,t8122-gpu-fuse-leakage", "1"));
}

static void t8122_j613_standin_fuse_off(void)
{
    /*
     * Without the switch the GPU handoff is byte for byte the stand-in experiment's: the same gpu
     * node, OPP table and reserved memory as a run with no fuse at all.
     */
    prepare_j613_setup(0x3ff, 1);
    run();
    char *gpu0 = dump_path(dtbuf, "/soc/gpu@292400000");
    char *opp0 = dump_path(dtbuf, "/opp-table-gpu");
    char *resv0 = dump_path(dtbuf, "/reserved-memory");

    prepare_j613_standin_fuse(0, 0);
    run();
    char *gpu1 = dump_path(dtbuf, "/soc/gpu@292400000");
    char *opp1 = dump_path(dtbuf, "/opp-table-gpu");
    char *resv1 = dump_path(dtbuf, "/reserved-memory");
    assert(!strcmp(gpu0, gpu1) && !strcmp(opp0, opp1) && !strcmp(resv0, resv1));
    assert(gpu_u32("apple,core-leak-coef") == float_bits(6412.0f));
    assert(log_has("FDT: GPU: T8122 leakage: the stand-in (the fuse values are exported only)\n"));
    assert(log_has("FDT: GPU: T8122 GPU enabled (firmware 14.8.3)"));
    expect_fuse_prop(10924, 342);
    free(gpu0), free(opp0), free(resv0), free(gpu1), free(opp1), free(resv1);
}

static void t8122_j613_standin_fuse_on(void)
{
    /* chosen.asahi,t8122-gpu-fuse-leakage=1: the first value takes the stand-in's place */
    prepare_j613_standin_fuse(1, 0);
    run();
    assert(gpu_u32("apple,core-leak-coef") == float_bits(10924.0f));
    assert(gpu_u32("apple,sram-leak-coef") == 0);
    assert(log_has("FDT: GPU: T8122 leakage: 10924, from this Mac's fuse "
                   "(chosen.asahi,t8122-gpu-fuse-leakage=1)\n"));
    assert(log_has("the GPU setup uses the first (chosen.asahi,t8122-gpu-fuse-leakage=1)"));
    assert(log_has("FDT: GPU: Leakage: 10924.000"));
    assert(log_has("FDT: GPU: T8122 GPU enabled (firmware 14.8.3)"));
    expect_fuse_prop(10924, 342);
    /* The power table follows the leakage: every OPP above the off state has more power */
    int opps = fdt_path_offset(dtbuf, "/opp-table-gpu"), opp, n = 0, len;
    fdt_for_each_subnode(opp, dtbuf, opps)
    {
        const fdt32_t *uw = fdt_getprop(dtbuf, opp, "opp-microwatt", &len);
        assert(uw && len == 4);
        if (n++)
            assert(fdt32_to_cpu(*uw) > 0);
    }
    assert(n == 14);
}

static void t8122_j613_standin_fuse_on_fault(void)
{
    /* The switch with no fuse values: the setup refuses, it does not fall back to the stand-in */
    prepare_j613_standin_fuse(1, 1);
    run();
    expect_gpu_off(dtbuf);
    assert(log_has("FDT: GPU: chosen.asahi,t8122-gpu-fuse-leakage=1, but this boot has no T8122 "
                   "leakage fuse values\n"));
    assert(log_has("T8122 GPU setup failed"));
    assert(prop_len(dtbuf, "/chosen", "asahi,t8122-gpu-leak-fuse") < 0);
    assert(pmgr_enables == 1 && pmgr_disables == 1 && pmgr_off == 1);
}

static void t8122_j613_standin_fuse_not_j613(void)
{
    /* The switch on another T8122 root: no fuse values, so the setup refuses */
    prepare_j613_setup(0x3ff, 1);
    CHK(fdt_setprop_string(dtbuf, fdt_path_offset(dtbuf, "/chosen"), "asahi,t8122-gpu-fuse-leakage",
                           "1"));
    run();
    expect_gpu_off(dtbuf);
    assert(!strstr(trace_buf, "0x2e42c812"));
    assert(log_has("but this boot has no T8122 leakage fuse values"));
}

/* Each Air hands over its own fuse, clock and power values. */
static void prepare_air_current14(bool j615, u32 mask, unsigned unit)
{
    prepare_j613_setup(mask, 1);
    static const char air13[] = "apple,j613\0apple,t8122";
    static const char air15[] = "apple,j615\0apple,t8122";
    CHK(fdt_setprop(dtbuf, 0, "compatible", j615 ? air15 : air13,
                   j615 ? sizeof(air15) : sizeof(air13)));
    mmio_set(T8122_LEAK_FUSE_WORD0, unit ? 0 : 0x80000000, 0);
    mmio_set(T8122_LEAK_FUSE_WORD1, unit ? 0x70 : 0x46, 0);
    CHK(fdt_setprop_string(dtbuf, fdt_path_offset(dtbuf, "/chosen"),
                           "asahi,t8122-gpu-fuse-leakage", "1"));
    hset_u32("/arm-io/sgx", "gpu-se-tgt", unit ? 1421 : 1411);
    if (unit) {
        int sgx = adt_path_offset(adt, "/arm-io/sgx");
        u32 len;
        struct perf_state *ps = (struct perf_state *)adt_getprop(adt, sgx, "perf-states", &len);
        assert(ps && len == 14 * sizeof(*ps));
        for (unsigned i = 1; i < 14; i++)
            ps[i].freq += 1000000;
    }
}

static void check_air_current14(u32 mask, unsigned unit)
{
    assert(!strcmp(status_of(dtbuf, "/soc/gpu@292400000"), "okay"));
    assert(identity_u32("probe-result") == T8122_ID_PASS);
    assert(identity_u32("core-mask-0") == mask);
    assert(gpu_u32("apple,core-leak-coef") == float_bits(unit ? 1794.0f : 1130.0f));
    assert(gpu_u32("apple,se-target") == (unit ? 1421u : 1411u));
    assert(pmgr_enables == 1 && !pmgr_disables && !unguarded_faults);
    int opps = fdt_path_offset(dtbuf, "/opp-table-gpu"), opp, len, points = 0;
    u64 top = 0;
    fdt_for_each_subnode(opp, dtbuf, opps)
    {
        const fdt64_t *hz = fdt_getprop(dtbuf, opp, "opp-hz", &len);
        assert(hz && len == 8);
        top = max(top, fdt64_ld(hz));
        points++;
    }
    assert(points == 14 && top == (unit ? 1339000000u : 1338000000u));
    assert(prop_len(dtbuf, "/soc/gpu@292400000", "apple,j613-25g83-gpu-handoff") < 0);
}

static void t8122_j615_full14(void)
{
    prepare_air_current14(true, 0x3ff, 0);
    run();
    check_air_current14(0x3ff, 0);
}

static void t8122_air_per_mac_power(void)
{
    prepare_air_current14(false, 0x3ff, 0);
    run();
    check_air_current14(0x3ff, 0);
    char *first_opps = dump_path(dtbuf, "/opp-table-gpu");
    prepare_air_current14(true, 0x37e, 1);
    run();
    check_air_current14(0x37e, 1);
    char *second_opps = dump_path(dtbuf, "/opp-table-gpu");
    assert(strcmp(first_opps, second_opps));
    free(first_opps);
    free(second_opps);
}

static void t8122_j615_full14_unknown_image(void)
{
    prepare_air_current14(true, 0x3ff, 0);
    fw_hdr[5] ^= 1;
    run();
    expect_gpu_off(dtbuf);
    assert(!pmgr_enables);
}

static void t8122_j615_full14_missing_power(void)
{
    prepare_air_current14(true, 0x3ff, 0);
    hdel("/arm-io/sgx", "gpu-se-tgt");
    run();
    expect_gpu_off(dtbuf);
    assert(!pmgr_enables);
}

static void t8122_j615_full14_fuse_fault(void)
{
    prepare_air_current14(true, 0x3ff, 0);
    mmio_set(T8122_LEAK_FUSE_WORD0, 0, 1);
    run();
    expect_gpu_off(dtbuf);
    assert(pmgr_enables == 1 && pmgr_disables == 1 && pmgr_off == 1);
    assert(identity_u32("probe-result") == T8122_ID_NOT_RUN);
}
#endif

#ifdef G15_FW26
static void prepare_25(int intent, int fault)
{
    prepare_j613_standin_fuse(1, fault);
    os_firmware = fw_versions[V26_6_2];
    hset("/arm-io/sgx", "compatible", "gpu,t8122", sizeof("gpu,t8122"));
    struct adt_segment_ranges seg[2] = {
        {J613_FW_TEXT, FW_VA, J613_FW_TEXT, 0x60000, 1},
        {J613_FW_DATA, FW_VA + 0x60000, J613_FW_DATA, 0x12c000, 0},
    };
    hset("/arm-io/gfx-asc", "segment-ranges", seg, sizeof(seg));
    hset("/arm-io/gfx-asc/iop-gfx-nub", "segment-ranges", seg, sizeof(seg));
    int dcp = fdt_add_subnode(dtbuf, fdt_path_offset(dtbuf, "/soc"), "dcp");
    CHK(dcp);
    CHK(fdt_setprop_u32(dtbuf, dcp, "apple,j613-25g83-profile", 1));
    fdt32_t compat[] = {cpu_to_fdt32(26), cpu_to_fdt32(6), cpu_to_fdt32(2)};
    CHK(fdt_setprop(dtbuf, dcp, "apple,firmware-compat", compat, sizeof(compat)));
    CHK(fdt_setprop_string(dtbuf, fdt_path_offset(dtbuf, "/aliases"), "dcp", "/soc/dcp"));
    if (!intent)
        CHK(fdt_delprop(dtbuf, fdt_path_offset(dtbuf, "/chosen"), "asahi,t8122-gpu"));
}
static void t8122_25_calibrated(void)
{
    prepare_25(1, 0);
    run();
    assert(!strcmp(status_of(dtbuf, "/soc/gpu@292400000"), "okay"));
    assert(gpu_u32("apple,j613-25g83-gpu-handoff") == 1);
    assert(gpu_u32("apple,gpu-power-model") == 1);
    int len;
    const fdt32_t *compat = gpu_prop("apple,firmware-compat", &len);
    assert(compat && len == 12 && fdt32_ld(compat) == 26 && fdt32_ld(compat+1) == 6 && fdt32_ld(compat+2) == 2);
    assert(gpu_u32("apple,m3-handoff-version") == 1);
    assert(fdt_stringlist_search(dtbuf, fdt_path_offset(dtbuf, "/soc/gpu@292400000"), "memory-region-names", "fw-data") >= 0);
    int opp = fdt_path_offset(dtbuf, "/opp-table-gpu/opp01");
    const fdt32_t *sram = fdt_getprop(dtbuf, opp, "opp-microvolt-sram", &len);
    assert(sram && len == 4 && fdt32_ld(sram) == 790000);
    const fdt32_t *power = fdt_getprop(dtbuf, opp, "opp-microwatt", &len);
    assert(power && len == 4 && fdt32_ld(power) > 0);
    assert(!strcmp(status_of(dtbuf, "/soc/mbox@292408000"), "okay"));
    assert(pmgr_enables == 1 && pmgr_disables == 1 && pmgr_off == 1);
}
static void t8122_25_no_intent(void)
{
    prepare_25(0, 0); run(); assert(!strcmp(status_of(dtbuf, "/soc/gpu@292400000"), "disabled"));
    assert(!pmgr_enables);
}
static void t8122_25_no_fuse(void)
{
    prepare_25(1, 1); run(); assert(!strcmp(status_of(dtbuf, "/soc/gpu@292400000"), "disabled"));
    assert(!pmgr_enables && prop_len(dtbuf, "/soc/gpu@292400000", "apple,j613-25g83-gpu-handoff") < 0);
}
static void t8122_25_wrong_os(void)
{
    prepare_25(1, 0); os_firmware = fw_versions[V14_7]; run(); assert(!strcmp(status_of(dtbuf, "/soc/gpu@292400000"), "disabled"));
    assert(!pmgr_enables && prop_len(dtbuf, "/soc/gpu@292400000", "apple,j613-25g83-gpu-handoff") < 0);
}
static void t8122_25_j615_refused(void)
{
    prepare_25(1, 0);
    static const char air15[] = "apple,j615\0apple,t8122";
    CHK(fdt_setprop(dtbuf, 0, "compatible", air15, sizeof(air15)));
    run();
    assert(!strcmp(status_of(dtbuf, "/soc/gpu@292400000"), "disabled"));
    assert(!pmgr_enables && prop_len(dtbuf, "/soc/gpu@292400000", "apple,j613-25g83-gpu-handoff") < 0);
}
static void t8122_25_wrong_input_compat(void)
{
    prepare_25(1, 0);
    fdt32_t compat[] = {cpu_to_fdt32(14), cpu_to_fdt32(8), cpu_to_fdt32(3)};
    CHK(fdt_setprop(dtbuf, fdt_path_offset(dtbuf, "dcp"), "apple,firmware-compat", compat, sizeof(compat)));
    run(); assert(!strcmp(status_of(dtbuf, "/soc/gpu@292400000"), "disabled"));
    assert(!pmgr_enables && prop_len(dtbuf, "/soc/gpu@292400000", "apple,j613-25g83-gpu-handoff") < 0);
}
static void t8122_25_missing_calibration(void)
{
    prepare_25(1, 0); hdel("/arm-io/sgx", "perf-states-sram"); run(); assert(!strcmp(status_of(dtbuf, "/soc/gpu@292400000"), "disabled"));
    assert(!pmgr_enables && prop_len(dtbuf, "/soc/gpu@292400000", "apple,j613-25g83-gpu-handoff") < 0);
}
static void t8122_25_short_calibration(void)
{
    prepare_25(1, 0); u32 short_sram = 790;
    hset("/arm-io/sgx", "perf-states-sram", &short_sram, sizeof(short_sram));
    run(); assert(!strcmp(status_of(dtbuf, "/soc/gpu@292400000"), "disabled"));
    assert(!pmgr_enables);
}
static void t8122_25_overflow_voltage(void)
{
    prepare_25(1, 0); struct perf_state sram[14] = {{0}};
    for (int i=1; i<14; i++) sram[i].volt = ~0u;
    hset("/arm-io/sgx", "perf-states-sram", sram, sizeof(sram));
    run(); assert(!strcmp(status_of(dtbuf, "/soc/gpu@292400000"), "disabled"));
    assert(!pmgr_enables);
}
static void t8122_25_identity_failure(void)
{
    prepare_25(1, 0); mmio_set(SGX_BASE + G15_SGX_ID_VERSION, 0, 1);
    run(); assert(!strcmp(status_of(dtbuf, "/soc/gpu@292400000"), "disabled"));
    assert(pmgr_enables == 1 && pmgr_disables == 1 && pmgr_off == 1);
}
#endif

#ifdef G15_NEW
/* ---------- T8142 (M5, G17): the reserved regions of the firmware's data ---------- */

/* The J813's sgx regions on macOS 27.0 (26A428), from its ADT. */
#define T8142_GFX_DATA  0x10002444000UL
#define T8142_GFX1_DATA 0x1000250c000UL
#define T8142_DT_FW_DATA 0x10002444000UL /* the kernel DT's own default */

static void t8142_setup(bool shared_ro, u64 ro0_base, bool fw_data_node)
{
    chip_id = 0x8142;
    log_len = trace_len = 0;
    log_buf[0] = trace_buf[0] = 0;
    exc_guard = GUARD_OFF;
    exc_count = 0;
    n_hnodes = 0;
    memset(hnodes, 0, sizeof(hnodes));

    const char *sgx = "/arm-io/sgx";
    struct perf_state states[13];
    for (int i = 0; i < 13; i++)
        states[i] = (struct perf_state){i ? 300000000u + 100000000u * i : 0, i ? 600 + 20 * i : 0};
    hset_u32(sgx, "perf-state-count", 13);
    hset_u32(sgx, "gpu-num-perf-states", 12);
    hset_u32(sgx, "perf-state-table-count", 1);
    hset(sgx, "perf-states", states, sizeof(states));
    hset(sgx, "perf-states-sram", states, sizeof(states));
    static const struct {
        const char *name;
        u64 base, size;
    } regions[] = {
        {"gfx-handoff", 0x103fff30000UL, 0x4000},
        {"gfx-shared-l2-region", 0x103fff34000UL, 0x4000},
        {"gfx-shared-region", 0x103fff38000UL, 0x80000},
        {"gpu-region", 0x103fffb8000UL, 0x4000},
        {"gfx-data", T8142_GFX_DATA, 0xc0000},
        {"gfx1-data", T8142_GFX1_DATA, 0xb4000},
    };
    for (size_t i = 0; i < ARRAY_SIZE(regions); i++) {
        char prop[64];
        snprintf(prop, sizeof(prop), "%s-base", regions[i].name);
        hset_u64(sgx, strdup(prop), regions[i].base);
        snprintf(prop, sizeof(prop), "%s-size", regions[i].name);
        hset_u64(sgx, strdup(prop), regions[i].size);
    }
    if (shared_ro) {
        hset_u64(sgx, "gfx-data-shared-ro-base", ro0_base);
        hset_u64(sgx, "gfx-data-shared-ro-size", 0x8000);
        hset_u64(sgx, "gfx1-data-shared-ro-base", 0x100025c0000UL);
        hset_u64(sgx, "gfx1-data-shared-ro-size", 0x8000);
    }

    void *dt = dtbuf;
    CHK(fdt_create_empty_tree(dt, sizeof(dtbuf)));
    CHK(fdt_setprop_u32(dt, 0, "#address-cells", 2));
    CHK(fdt_setprop_u32(dt, 0, "#size-cells", 2));
    int aliases = fdt_add_subnode(dt, 0, "aliases");
    CHK(aliases);
    CHK(fdt_setprop_string(dt, aliases, "gpu", "/soc/gpu@582600000"));
    CHK(fdt_add_subnode(dt, 0, "chosen"));
    int opps = fdt_add_subnode(dt, 0, "opp-table");
    CHK(opps);
    CHK(fdt_setprop_u32(dt, opps, "phandle", 0x40));
    for (int i = 12; i >= 0; i--) {
        char name[16];
        snprintf(name, sizeof(name), "opp%02d", i);
        int opp = fdt_add_subnode(dt, opps, name);
        CHK(opp);
        CHK(fdt_setprop_u64(dt, opp, "opp-hz", 0));
        CHK(fdt_setprop_u32(dt, opp, "opp-microvolt", 0));
    }
    int resv = fdt_add_subnode(dt, 0, "reserved-memory");
    CHK(resv);
    static const char *const nodes[] = {"uat-handoff", "uat-l2-pagetables", "uat-pagetables",
                                        "uat-ttbs", "gfx-data", "gfx1-data"};
    for (size_t i = 0; i < ARRAY_SIZE(nodes); i++)
        add_resv(dt, resv, nodes[i], 0x50 + i);
    if (fw_data_node) {
        int n = fdt_add_subnode(dt, resv, "fw-data");
        CHK(n);
        fdt64_t reg[2];
        fdt64_st(&reg[0], T8142_DT_FW_DATA);
        fdt64_st(&reg[1], 0x184000);
        CHK(fdt_setprop(dt, n, "reg", reg, sizeof(reg)));
    }
    int soc = fdt_add_subnode(dt, 0, "soc");
    CHK(soc);
    int gpu = fdt_add_subnode(dt, soc, "gpu@582600000");
    CHK(gpu);
    CHK(fdt_setprop_string(dt, gpu, "compatible", "apple,agx-t8142"));
    CHK(fdt_setprop_u32(dt, gpu, "operating-points-v2", 0x40));
}

static void t8142_reg(const char *path, u64 *base, u64 *size)
{
    int n = fdt_path_offset(dtbuf, path), len;
    assert(n >= 0);
    const fdt64_t *reg = fdt_getprop(dtbuf, n, "reg", &len);
    assert(reg && len == 16);
    *base = fdt64_ld(&reg[0]);
    *size = fdt64_ld(&reg[1]);
}

static void t8142_expect_gpu_on(void)
{
    assert(strcmp(status_of(dtbuf, "/soc/gpu@582600000"), "disabled"));
    u64 base, size;
    t8142_reg("/reserved-memory/gfx-data", &base, &size);
    assert(base == T8142_GFX_DATA && size == 0xc0000);
    t8142_reg("/reserved-memory/gfx1-data", &base, &size);
    assert(base == T8142_GFX1_DATA && size == 0xb4000);
}

/* macOS 27.0: both roles' __DATA and __DATA_SHARED_RO, contiguous, become fw-data. */
static void t8142_fw_data(void)
{
    t8142_setup(true, T8142_GFX_DATA + 0xc0000, true);
    CHK(dt_set_gpu_g17(dtbuf, &g17_soc_t8142));
    t8142_expect_gpu_on();
    u64 base, size;
    t8142_reg("/reserved-memory/fw-data", &base, &size);
    assert(base == T8142_GFX_DATA && size == 0x184000);
    assert(!strcmp(status_of(dtbuf, "/reserved-memory/fw-data"), "okay"));
}

/* Firmware without __DATA_SHARED_RO leaves fw-data as the DT has it. */
static void t8142_fw_data_no_shared_ro(void)
{
    t8142_setup(false, 0, true);
    CHK(dt_set_gpu_g17(dtbuf, &g17_soc_t8142));
    t8142_expect_gpu_on();
    assert(prop_len(dtbuf, "/reserved-memory/fw-data", "status") < 0);
}

/* A __DATA_SHARED_RO placed apart from its __DATA is not covered by one region. */
static void t8142_fw_data_apart(void)
{
    t8142_setup(true, T8142_GFX_DATA + 0xc4000, true);
    CHK(dt_set_gpu_g17(dtbuf, &g17_soc_t8142));
    t8142_expect_gpu_on();
    assert(prop_len(dtbuf, "/reserved-memory/fw-data", "status") < 0);
    assert(log_has("firmware data segments are not contiguous"));
}

/* A DT without the node is filled as before. */
static void t8142_no_fw_data_node(void)
{
    t8142_setup(true, T8142_GFX_DATA + 0xc0000, false);
    CHK(dt_set_gpu_g17(dtbuf, &g17_soc_t8142));
    t8142_expect_gpu_on();
    assert(fdt_path_offset(dtbuf, "/reserved-memory/fw-data") < 0);
}
#endif

static const struct {
    const char *name;
    void (*fn)(void);
} scenarios[] = {
#ifdef G15_FW26
    {"t8122-25-calibrated", t8122_25_calibrated},
    {"t8122-25-wrong-input-compat", t8122_25_wrong_input_compat},
    {"t8122-25-short-calibration", t8122_25_short_calibration},
    {"t8122-25-overflow-voltage", t8122_25_overflow_voltage},
    {"t8122-25-identity-failure", t8122_25_identity_failure},
    {"t8122-25-no-intent", t8122_25_no_intent},
    {"t8122-25-no-fuse", t8122_25_no_fuse},
    {"t8122-25-wrong-os", t8122_25_wrong_os},
    {"t8122-25-j615-refused", t8122_25_j615_refused},
    {"t8122-25-missing-calibration", t8122_25_missing_calibration},
#endif
    {"t6030-dry", t6030_dry},
    {"t6030-armed", t6030_armed},
    {"t6030-unknown-fw", t6030_unknown_fw},
    {"t6030-wrong-id", t6030_wrong_id},
    {"t6030-core-mask", t6030_core_mask},
    {"t6030-fuse-fault", t6030_fuse_fault},
    {"t6030-id-fault", t6030_id_fault},
    {"t6030-region-in-ram", t6030_region_in_ram},
    {"t6030-nub-mismatch", t6030_nub_mismatch},
    {"t6030-dt-sgx-mismatch", t6030_dt_sgx_mismatch},
    {"t6030-bad-pstate", t6030_bad_pstate},
    {"t6030-no-table-count", t6030_no_table_count},
    {"t6030-power-fail", t6030_power_fail},
    {"t6030-small-dt", t6030_small_dt},
    {"t6030-tiny-dt", t6030_tiny_dt},
    {"t6030-no-alias", t6030_no_alias},
    {"t6030-wrong-compat", t6030_wrong_compat},
    {"t6030-standin-switch", t6030_standin_switch},
#if defined(G15_NEW) || defined(G15_T8122_BASE)
    {"t8122-default", t8122_default},
    {"t8122-armed-refuses", t8122_armed_refuses},
    {"t8122-j613-switch-off", t8122_j613_switch_off},
#endif
#ifdef G15_NEW
    {"t8122-pipeline", t8122_pipeline},
    {"t8122-closure-on", t8122_closure_on},
    {"t8122-wrong-id", t8122_wrong_id},
    {"t8122-two-dies", t8122_two_dies},
    {"t8122-mask-beyond-slots", t8122_mask_beyond_slots},
    {"t8122-tables-exceed-clusters", t8122_tables_exceed_clusters},
    {"t8122-release-fails", t8122_release_fails},
    {"t8122-small-dt", t8122_small_dt},
    {"t8122-with-diag-optin", t8122_with_diag_optin},
    {"t8122-powered-identity", t8122_powered_identity},
    {"t8122-powered-identity-counts", t8122_powered_identity_counts},
    {"t8122-powered-identity-counts2", t8122_powered_identity_counts2},
    {"t8122-powered-identity-counts-fault", t8122_powered_identity_counts_fault},
    {"t8122-powered-identity-counts2-fault", t8122_powered_identity_counts2_fault},
    {"t8122-j613-image-admitted", t8122_j613_image_admitted},
    {"t8122-asc-preflight-enables", t8122_asc_preflight_enables},
    {"t8122-asc-preflight-full-optin-noop", t8122_asc_preflight_full_optin_noop},
    {"t8122-j613-standin-10", t8122_j613_standin_10},
    {"t8122-j613-standin-8", t8122_j613_standin_8},
    {"t8122-j613-standin-only", t8122_j613_standin_only},
    {"t8122-j613-standin-missing-value", t8122_j613_standin_missing_value},
    {"t8122-j613-standin-bad-mask-size", t8122_j613_standin_bad_mask_size},
    {"t8122-j613-standin-bad-sram", t8122_j613_standin_bad_sram},
    {"t8122-j613-standin-idleoff", t8122_j613_standin_idleoff},
    {"t8122-j613-standin-idleoff-malformed", t8122_j613_standin_idleoff_malformed},
    {"t8122-j613-standin-sram-above", t8122_j613_standin_sram_above},
    {"t8122-j613-standin-sram-above-floor", t8122_j613_standin_sram_above_floor},
    {"t8122-j613-standin-sram-below", t8122_j613_standin_sram_below},
    {"t8122-j613-standin-g15s-image", t8122_j613_standin_g15s_image},
    {"t8122-j613-standin-no-room", t8122_j613_standin_no_room},
    {"t8122-j613-standin-wrong-id", t8122_j613_standin_wrong_id},
    {"t8122-fuse-decode", t8122_fuse_decode},
    {"t8122-j613-fuse-export", t8122_j613_fuse_export},
    {"t8122-fuse-not-j613", t8122_fuse_not_j613},
    {"t8122-j613-fuse-fault", t8122_j613_fuse_fault},
    {"t8122-j613-standin-fuse-off", t8122_j613_standin_fuse_off},
    {"t8122-j613-standin-fuse-on", t8122_j613_standin_fuse_on},
    {"t8122-j613-standin-fuse-on-fault", t8122_j613_standin_fuse_on_fault},
    {"t8122-j613-standin-fuse-not-j613", t8122_j613_standin_fuse_not_j613},
    {"t8122-j615-full14", t8122_j615_full14},
    {"t8122-air-per-mac-power", t8122_air_per_mac_power},
    {"t8122-j615-full14-unknown-image", t8122_j615_full14_unknown_image},
    {"t8122-j615-full14-missing-power", t8122_j615_full14_missing_power},
    {"t8122-j615-full14-fuse-fault", t8122_j615_full14_fuse_fault},
    {"t8142-fw-data", t8142_fw_data},
    {"t8142-fw-data-no-shared-ro", t8142_fw_data_no_shared_ro},
    {"t8142-fw-data-apart", t8142_fw_data_apart},
    {"t8142-no-fw-data-node", t8142_no_fw_data_node},
#endif
};

#undef printf
int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    for (size_t i = 0; i < ARRAY_SIZE(scenarios); i++) {
        if (!strcmp(argv[1], "list")) {
            puts(scenarios[i].name);
            continue;
        }
        if (strcmp(argv[1], scenarios[i].name))
            continue;
        scenarios[i].fn();
        report(dtbuf);
        return 0;
    }
    return strcmp(argv[1], "list") ? 2 : 0;
}

/* No fixture here is a T8152. */
int dt_set_gpu_t8152(void *dt)
{
    (void)dt;
    return 0;
}

/* These fixtures exercise G15/M3 only. Fail if dispatch unexpectedly reaches M4. */
int dt_set_gpu_m4(void *dt)
{
    (void)dt;
    assert(!"unexpected M4 dispatch in the G15/M3 host fixture");
    return -1;
}
