/* SPDX-License-Identifier: MIT */
/*
 * src/sptm_virt_ctl.c against a fake guest. Guest RAM is one file; the "page tables" are a table
 * here; aura's window is a real mapping of that file at CTL_WIN, present only while aura maps it,
 * so any copy past what ctl_copy mapped is a real SIGSEGV and any mapping outside guest RAM aborts.
 * The SoC is a synthetic layout, not any real chip's. Needs a host with 16 KiB pages.
 * Build and run: tests/python/test_auractl_ctl_host.py
 */
#include <assert.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ctl_host.h"

#include "../../src/auractl_abi.h"

#define PG        0x4000UL
#define RAM       0x10000000000UL
#define RAM_PAGES 256
#define WIN       0xb000000000UL
#define UVA       0x200000000UL
#define KVA       0xfffffe0010000000UL

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE MAP_FIXED
#endif

u64 host_PAR_EL1, host_CNTFRQ_EL0 = 24000000;
void *adt;
u32 chip_id = T8140;
__typeof__(cur_boot_args) cur_boot_args = {RAM + 0x3000000}; /* physBase: DRAM base + 48 MiB */

struct map {
    u64 va, pa;
    bool r, w;
    u8 attr;
};
static struct map vm[64];
static int nvm;
static int ram_fd;
static u8 *guest;
static int win_maps, win_live, win_stale, lock_depth, s2_lock_depth, splits, resums, s2_tlbis;
static u64 split_blocks[256];

void spin_lock(spinlock_t *l)
{
    (void)l;
    assert(++lock_depth == 1);
}

void spin_unlock(spinlock_t *l)
{
    (void)l;
    assert(--lock_depth == 0);
}

void ctl_host_sysop(const char *op)
{
    if (!strcmp(op, "tlbi alle2is"))
        win_stale = 0; /* aura's own TLBs: the window's old entry is gone */
    if (!strcmp(op, "tlbi vmalls12e1is")) {
        assert(s2_lock_depth == 1); /* a stage-2 change is invalidated inside the entry lock */
        s2_tlbis++;
    }
}

static void map(u64 va, u64 pa, bool r, bool w, u8 attr)
{
    vm[nvm++] = (struct map){va, pa, r, w, attr};
}

static u8 *gptr(u64 pa)
{
    assert(pa >= RAM && pa < RAM + RAM_PAGES * PG);
    return guest + (pa - RAM);
}

void ctl_host_at(u64 va, bool write)
{
    assert(lock_depth == 1);
    for (int i = 0; i < nvm; i++)
        if ((va & ~(PG - 1)) == vm[i].va && (write ? vm[i].w : vm[i].r)) {
            host_PAR_EL1 = (u64)vm[i].attr << 56 | ((vm[i].pa | (va & 0x3000)) & PAR_PA);
            return;
        }
    host_PAR_EL1 = PAR_F | 0x800;
}

void mmu_add_mapping(u64 from, u64 to, size_t size, u8 attr, u64 perms)
{
    (void)attr;
    (void)perms;
    assert(from == WIN && size == PG && !(to & (PG - 1)));
    if (to < RAM || to + size > RAM + RAM_PAGES * PG) {
        fprintf(stderr, "aura mapped 0x%llx, outside guest RAM\n", (unsigned long long)to);
        abort();
    }
    assert(!win_live);
    if (win_stale) {
        fprintf(stderr, "aura re-mapped its window with the old entry maybe still in its TLB\n");
        abort();
    }
    void *p = mmap((void *)WIN, PG, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED_NOREPLACE,
                   ram_fd, to - RAM);
    assert(p == (void *)WIN);
    win_live = 1;
    win_maps++;
}

void mmu_rm_mapping(u64 from, size_t size)
{
    assert(from == WIN && size == PG && win_live);
    assert(!munmap((void *)WIN, PG));
    win_live = 0;
    win_stale = 1;
}

/* ---- ADT, stage 2 and the tracer ---- */

int adt_path_offset_trace(const void *dt, const char *path, int *offs)
{
    (void)dt;
    /* Everything but the GPU's ASCs exists; they show up as absent in the list. */
    if (!strcmp(path, "/arm-io/gfx-asc") || !strcmp(path, "/arm-io/gfx1-asc"))
        return -1;
    /* /arm-io/dart-dcp stands for a catalogue entry that resolves into RAM by mistake. */
    offs[0] = strcmp(path, "/arm-io/dart-dcp") ? path[8] * 256 + path[9] : -1;
    return 0;
}

int adt_get_reg(const void *dt, const int *path, const char *prop, int idx, u64 *pa, u64 *sz)
{
    (void)dt;
    (void)prop;
    if (path[0] == -1) {
        *pa = RAM + 0x4000000;
        *sz = PG;
        return 0;
    }
    *pa = 0x300000000UL + (u64)path[0] * 0x100 * 0x4000 + (u64)idx * 0x100000;
    *sz = 0x10000;
    return 0;
}

/* The blocks the sleep path would have found: a synthetic PMGR, PMS and AMCC. pwrgate is absent;
 * cllt is one page, a page on the T8140 never-read list. */
static struct s2r_soc fake_soc = {
    .soc_ps = {0x300400000UL, 0x18000},
    .soc_misc = {0x300430000UL, 0x14000},
    .cllt = {0x300464000UL, PG},
    .cluster_ctl = {0x300480000UL, PG},
    .nub_ap = {0x300484000UL, 0x1000},
    .aon = {0x300700000UL, 0x80000},
    .aon_pst = {0x300750000UL, 0x8000},
    .clkgen = {0x2ff900000UL, 0x120000}, /* 72 pages: more than one call may change */
    .pms_pmc = {0x301800000UL, 0x3018a8000UL},
    .amcc = 0x2a0025000UL,
    .amcc_planes = 4,
};

const struct s2r_soc *sptm_virt_s2r_ctl_soc(void)
{
    return &fake_soc;
}

/* PEEK's register file: a pattern. */
static int peeks;
u32 ctl_host_read32(u64 pa)
{
    peeks++;
    return (u32)(pa * 3);
}

void hv_pt_split_block(u64 addr)
{
    assert(!(addr & ((1UL << 25) - 1)));
    split_blocks[splits++] = addr;
}

void sptm_virt_ctl_guest_ram(u64 *base, u64 *size)
{
    *base = RAM;
    *size = RAM_PAGES * PG;
}

void sptm_virt_s2_ctl_begin(void)
{
    assert(lock_depth == 1 && ++s2_lock_depth == 1);
}

void sptm_virt_s2_ctl_end(bool changed)
{
    assert(s2_lock_depth-- == 1);
    resums += changed;
}

struct s2r_rec {
    u64 t, meta, val, pc;
};
struct s2r_rec *sptm_virt_s2r_ring;
u64 sptm_virt_s2r_nrec;
volatile u64 sptm_virt_s2r_head;
volatile bool sptm_virt_s2r_quiet = true;
volatile bool sptm_virt_xnu_live = true;
volatile bool sptm_virt_s2r_ctl_paused;
char con_buf[8192];
size_t con_wp;
const char *const aura_version = "host-test";

static struct {
    u64 page;
    bool ctl;
} hooks[512];
static int nhooks;

int sptm_virt_s2r_ctl_page(const char *what, u64 page, bool arm)
{
    (void)what;
    assert(lock_depth == 1 && s2_lock_depth == 1 && !(page & (PG - 1)));
    for (int i = 0; i < nhooks; i++)
        if (hooks[i].page == page) {
            if (arm)
                return 0;
            if (!hooks[i].ctl)
                return -AURACTL_EBUSY;
            hooks[i] = hooks[--nhooks];
            return 1;
        }
    if (!arm)
        return 0;
    hooks[nhooks++].page = page;
    hooks[nhooks - 1].ctl = true;
    return 1;
}

u32 sptm_virt_s2r_ctl_release_all(void)
{
    assert(lock_depth == 1 && s2_lock_depth == 1);
    u32 n = 0;
    for (int i = nhooks - 1; i >= 0; i--)
        if (hooks[i].ctl) {
            hooks[i] = hooks[--nhooks];
            n++;
        }
    return n;
}

u32 sptm_virt_s2r_ctl_count(u64 base, u64 size, u32 *ctl_pages)
{
    u32 n = 0, c = 0;
    for (int i = 0; i < nhooks; i++)
        if (hooks[i].page + PG > base && hooks[i].page < base + size) {
            n++;
            c += hooks[i].ctl;
        }
    if (ctl_pages)
        *ctl_pages = c;
    return n;
}

const void *sptm_virt_s2r_ctl_rec(u64 seq)
{
    return &sptm_virt_s2r_ring[seq % sptm_virt_s2r_nrec];
}

static char last_tag[9];
static u64 last_val;

void sptm_virt_s2r_marker(const char *tag, u64 arg)
{
    strncpy(last_tag, tag, 8);
    last_val = arg;
    sptm_virt_s2r_ring[sptm_virt_s2r_head++ % sptm_virt_s2r_nrec].val = arg;
}

/* ---- the tests ---- */

static int tests;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        tests++;                                                                                   \
        if (!(c)) {                                                                                \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c);                                   \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

#define PA_REQ    (RAM + 2 * PG)
#define VA_REQ    (UVA)
#define PA_BUF    (RAM + 8 * PG)
#define VA_BUF    (UVA + 8 * PG)
#define BUF_PAGES 32

#define SOC_PS 0x300400000UL /* a pmgr- range PEEK may read */

static struct auractl_req *req_ptr(void)
{
    return (struct auractl_req *)gptr(PA_REQ);
}

static int hvc_at(u64 ureq)
{
    u64 regs[31] = {0};
    regs[0] = AURACTL_HVC_MAGIC;
    regs[1] = ureq;
    host_PAR_EL1 = 0x1234567;
    CHECK(sptm_virt_ctl_hvc(regs, 0x58000000 | AURACTL_HVC_IMM));
    CHECK(host_PAR_EL1 == 0x1234567); /* the guest's PAR_EL1 comes back */
    CHECK(!win_live);                 /* no alias left behind */
    CHECK(!win_stale);                /* nor in aura's TLB */
    CHECK(lock_depth == 0 && s2_lock_depth == 0);
    return regs[0];
}

static int call(u16 cmd, u64 a0, u64 a1, u64 a2, u64 buf, u64 len)
{
    struct auractl_req *r = req_ptr();
    memset(r, 0x5a, sizeof(*r));
    r->magic = AURACTL_REQ_MAGIC;
    r->version = AURACTL_VERSION;
    r->cmd = cmd;
    r->arg0 = a0;
    r->arg1 = a1;
    r->arg2 = a2;
    r->buf = buf;
    r->buf_len = len;
    return hvc_at(VA_REQ);
}

static void standard_map(void)
{
    nvm = 0;
    map(VA_REQ, PA_REQ, true, true, 0xff);
    for (int i = 0; i < BUF_PAGES; i++)
        map(VA_BUF + i * PG, PA_BUF + i * PG, true, true, 0xff);
}

static void test_setup(void)
{
    CHECK(splits > 10);
    for (int i = 0; i < splits; i++) {
        CHECK(split_blocks[i] < RAM); /* guest RAM's stage 2 is never touched */
        for (int k = 0; k < i; k++)
            CHECK(split_blocks[i] != split_blocks[k]);
    }
    CHECK(!win_live && !win_stale);
}

static void test_dispatch(void)
{
    u64 regs[31] = {0};
    /* Not ours: goes back to nv2_inject_self untouched. */
    regs[0] = AURACTL_HVC_MAGIC;
    CHECK(!sptm_virt_ctl_hvc(regs, 0x58000000 | 0x1234));
    regs[0] = 0xdeadbeef;
    CHECK(!sptm_virt_ctl_hvc(regs, 0x58000000 | AURACTL_HVC_IMM));
    CHECK(regs[0] == 0xdeadbeef);
    /* The upper half of x0 is not part of the magic: the caller writes w0. */
    regs[0] = (0x77UL << 32) | AURACTL_HVC_MAGIC;
    regs[1] = 0;
    CHECK(sptm_virt_ctl_hvc(regs, 0x58000000 | AURACTL_HVC_IMM));
    CHECK(regs[0] == AURACTL_EFAULT);
}

static void test_request_checks(void)
{
    standard_map();
    /* Unmapped, kernel, and misplaced requests */
    CHECK(hvc_at(0) == AURACTL_EFAULT);
    CHECK(hvc_at(KVA) == AURACTL_EFAULT);
    CHECK(hvc_at(UVA + 100 * PG) == AURACTL_EFAULT);
    /* Bad magic, bad version, unknown command: the block is left alone */
    struct auractl_req *r = req_ptr();
    memset(r, 0, sizeof(*r));
    CHECK(hvc_at(VA_REQ) == AURACTL_EINVAL);
    r->version = AURACTL_VERSION;
    CHECK(hvc_at(VA_REQ) == AURACTL_EINVAL); /* the magic alone is wrong */
    r->magic = AURACTL_REQ_MAGIC;
    r->version = 2;
    r->ret0 = 0x77;
    CHECK(hvc_at(VA_REQ) == AURACTL_EINVAL);
    CHECK(r->ret0 == 0x77);
    r->version = AURACTL_VERSION;
    r->cmd = AURACTL_NCMDS;
    CHECK(hvc_at(VA_REQ) == AURACTL_ENOSYS);
    r->cmd = 0xffff;
    CHECK(hvc_at(VA_REQ) == AURACTL_ENOSYS);
    /* Read-only request block: read in, the write-back fails */
    nvm = 0;
    map(VA_REQ, PA_REQ, true, false, 0xff);
    CHECK(call(AURACTL_START, 0, 0, 0, 0, 0) == AURACTL_EFAULT);
    /* Device memory, and a page outside the RAM SPTM manages */
    nvm = 0;
    map(VA_REQ, PA_REQ, true, true, 0x00);
    CHECK(call(AURACTL_INFO, 0, 0, 0, 0, 0) == AURACTL_EFAULT);
    nvm = 0;
    map(VA_REQ, 0x300000000UL, true, true, 0xff); /* MMIO */
    CHECK(hvc_at(VA_REQ) == AURACTL_EFAULT);
    nvm = 0;
    map(VA_REQ, RAM - PG, true, true, 0xff);
    CHECK(hvc_at(VA_REQ) == AURACTL_EFAULT);
    nvm = 0;
    map(VA_REQ, RAM + RAM_PAGES * PG, true, true, 0xff);
    CHECK(hvc_at(VA_REQ) == AURACTL_EFAULT);
    /* aura's own image */
    nvm = 0;
    map(VA_REQ, 0x10000f00000UL, true, true, 0xff);
    CHECK(hvc_at(VA_REQ) == AURACTL_EFAULT);
    /* A block that spans two pages */
    standard_map();
    map(VA_REQ + PG, RAM + 3 * PG, true, true, 0xff);
    u8 *a = gptr(PA_REQ + PG - 40), *b = gptr(RAM + 3 * PG);
    struct auractl_req q = {
        .magic = AURACTL_REQ_MAGIC, .version = AURACTL_VERSION, .cmd = AURACTL_STOP};
    memcpy(a, &q, 40);
    memcpy(b, (u8 *)&q + 40, sizeof(q) - 40);
    CHECK(hvc_at(VA_REQ + PG - 40) == 0);
    CHECK(sptm_virt_s2r_ctl_paused);
    q.cmd = AURACTL_START;
    memcpy(a, &q, 40);
    CHECK(hvc_at(VA_REQ + PG - 40) == 0);
    CHECK(!sptm_virt_s2r_ctl_paused);
}

static void test_info(void)
{
    standard_map();
    struct auractl_info *in = (void *)gptr(PA_BUF);
    memset(in, 0xee, sizeof(*in));
    CHECK(call(AURACTL_INFO, 0, 0, 0, VA_BUF, sizeof(*in)) == 0);
    CHECK(in->version == AURACTL_VERSION && in->rec_size == 32);
    CHECK(in->cntfrq == 24000000 && in->nrec == sptm_virt_s2r_nrec);
    CHECK((in->flags & AURACTL_INFO_TRACE) && (in->flags & AURACTL_INFO_RECORDING));
    CHECK(!strcmp(in->build, "host-test"));
    CHECK(req_ptr()->buf_used == sizeof(*in) && req_ptr()->ret0 == AURACTL_VERSION);
    CHECK(call(AURACTL_INFO, 0, 0, 0, VA_BUF, sizeof(*in) - 1) == AURACTL_E2BIG);
    CHECK(call(AURACTL_INFO, 0, 0, 0, 0, 0) == 0);
    /* A buffer that runs off its mapping */
    CHECK(call(AURACTL_INFO, 0, 0, 0, VA_BUF + BUF_PAGES * PG - 8, sizeof(*in)) == AURACTL_EFAULT);
    /* Read-only buffer */
    nvm = 0;
    map(VA_REQ, PA_REQ, true, true, 0xff);
    map(VA_BUF, PA_BUF, true, false, 0xff);
    CHECK(call(AURACTL_INFO, 0, 0, 0, VA_BUF, sizeof(*in)) == AURACTL_EFAULT);
}

static void fill_ring(u64 total)
{
    sptm_virt_s2r_head = total;
    for (u64 s = total > sptm_virt_s2r_nrec ? total - sptm_virt_s2r_nrec : 0; s < total; s++) {
        struct s2r_rec *r = &sptm_virt_s2r_ring[s % sptm_virt_s2r_nrec];
        r->t = s;
        r->val = ~s;
        r->meta = r->pc = s * 3;
    }
}

static void test_read(void)
{
    standard_map();
    u64 nrec = sptm_virt_s2r_nrec;
    struct auractl_rec *out = (void *)gptr(PA_BUF);
    /* Not yet wrapped */
    fill_ring(100);
    CHECK(call(AURACTL_READ, 0, 1000, 0, VA_BUF, 1000 * 32) == 0);
    CHECK(req_ptr()->ret0 == 0 && req_ptr()->ret1 == 100 && req_ptr()->ret2 == 100);
    CHECK(out[0].t == 0 && out[99].t == 99 && out[99].val == ~99UL);
    CHECK(req_ptr()->buf_used == 100 * 32);
    /* Past the head: nothing, no error */
    CHECK(call(AURACTL_READ, 500, 10, 0, VA_BUF, 1000) == 0 && req_ptr()->ret1 == 0);
    CHECK(req_ptr()->ret0 == 100);
    /* Wrapped: the oldest are gone, a run across the ring's end comes back in order */
    fill_ring(nrec + 50);
    CHECK(call(AURACTL_READ, 0, 200, 0, VA_BUF, 200 * 32) == 0);
    CHECK(req_ptr()->ret0 == 50 && req_ptr()->ret1 == 200);
    CHECK(out[0].t == 50);
    CHECK(call(AURACTL_READ, nrec - 10, 40, 0, VA_BUF, 40 * 32) == 0);
    CHECK(req_ptr()->ret0 == nrec - 10 && req_ptr()->ret1 == 40);
    for (int i = 0; i < 40; i++)
        CHECK(out[i].t == nrec - 10 + i);
    /* Bounds: the smaller of what was asked, what the buffer holds and the per-call limit */
    CHECK(call(AURACTL_READ, nrec, 40, 0, VA_BUF, 3 * 32 + 31) == 0 && req_ptr()->ret1 == 3);
    CHECK(call(AURACTL_READ, 50, nrec * 2, 0, VA_BUF, BUF_PAGES * PG) == 0);
    CHECK(req_ptr()->ret1 == AURACTL_MAX_BUF / 32 && req_ptr()->buf_used == AURACTL_MAX_BUF);
    /* A huge claimed length and a short mapping: refused at the first unmapped page */
    nvm = 0;
    map(VA_REQ, PA_REQ, true, true, 0xff);
    map(VA_BUF, PA_BUF, true, true, 0xff);
    CHECK(call(AURACTL_READ, 50, nrec, 0, VA_BUF, ~0UL) == AURACTL_EFAULT);
    /* Overflowing arguments do not wrap the bounds */
    standard_map();
    CHECK(call(AURACTL_READ, ~0UL, ~0UL, 0, VA_BUF, 1024) == 0 && req_ptr()->ret1 == 0);
    CHECK(call(AURACTL_READ, 50, ~0UL, 0, VA_BUF, 320) == 0 && req_ptr()->ret1 == 10);
    /* A 4 KiB process: a run of pages at scattered physical pages */
    nvm = 0;
    map(VA_REQ, PA_REQ, true, true, 0xff);
    map(VA_BUF, RAM + 40 * PG, true, true, 0xff);
    map(VA_BUF + PG, RAM + 20 * PG, true, true, 0xff);
    fill_ring(5000);
    CHECK(call(AURACTL_READ, 100, 1000, 0, VA_BUF, 1000 * 32) == 0);
    struct auractl_rec *p1 = (void *)gptr(RAM + 40 * PG), *p2 = (void *)gptr(RAM + 20 * PG);
    CHECK(p1[0].t == 100 && p1[511].t == 611 && p2[0].t == 612);
    fill_ring(100);
}

static void test_ring_control(void)
{
    standard_map();
    fill_ring(100);
    CHECK(call(AURACTL_STOP, 0, 0, 0, 0, 0) == 0 && sptm_virt_s2r_ctl_paused);
    CHECK(call(AURACTL_MARK, 1, 0, 0, 0, 0) == AURACTL_EBUSY);
    CHECK(call(AURACTL_START, 0, 0, 0, 0, 0) == 0 && !sptm_virt_s2r_ctl_paused);
    /* Clear keeps the paused state it found, and empties the ring */
    CHECK(call(AURACTL_CLEAR, 0, 0, 0, 0, 0) == 0 && req_ptr()->ret0 == 100);
    CHECK(sptm_virt_s2r_head == 0 && !sptm_virt_s2r_ctl_paused);
    CHECK(sptm_virt_s2r_ring[5].t == 0);
    CHECK(call(AURACTL_STOP, 0, 0, 0, 0, 0) == 0);
    CHECK(call(AURACTL_CLEAR, 0, 0, 0, 0, 0) == 0 && sptm_virt_s2r_ctl_paused);
    CHECK(call(AURACTL_START, 0, 0, 0, 0, 0) == 0);
    /* Marks: the first eight bytes are the tag, the rest of the text is bounded and cleaned */
    char *t = (char *)gptr(PA_BUF);
    strcpy(t, "boot-done and a long tail");
    CHECK(call(AURACTL_MARK, 0x55, 0, 0, VA_BUF, strlen(t) + 1) == 0);
    CHECK(!strcmp(last_tag, "boot-don") && last_val == 0x55 && req_ptr()->ret0 == 1);
    memset(t, 'x', 4096);
    t[1] = 0x07;
    CHECK(call(AURACTL_MARK, 1, 0, 0, VA_BUF, 4096) == 0);
    CHECK(last_tag[1] == '.');
    CHECK(call(AURACTL_MARK, 2, 0, 0, 0, 0) == 0 && !strcmp(last_tag, "MARK"));
    CHECK(call(AURACTL_MARK, 3, 0, 0, UVA + 200 * PG, 8) == AURACTL_EFAULT);
}

static void test_console(void)
{
    standard_map();
    for (int i = 0; i < 8192; i++)
        con_buf[i] = 'a' + i % 26;
    con_wp = 100;
    char *o = (char *)gptr(PA_BUF);
    CHECK(call(AURACTL_CONSOLE, 0, 0, 0, VA_BUF, 1000) == 0);
    CHECK(req_ptr()->buf_used == 100 && req_ptr()->ret0 == 100 && !memcmp(o, con_buf, 100));
    CHECK(call(AURACTL_CONSOLE, 0, 0, 0, VA_BUF, 10) == 0);
    CHECK(req_ptr()->buf_used == 10 && !memcmp(o, con_buf + 90, 10));
    /* Wrapped: oldest first, 8 KiB at most */
    con_wp = 8192 * 3 + 7;
    CHECK(call(AURACTL_CONSOLE, 0, 0, 0, VA_BUF, 100000) == 0);
    CHECK(req_ptr()->buf_used == 8192 && req_ptr()->ret0 == con_wp);
    CHECK(o[0] == con_buf[7] && o[8191] == con_buf[6]);
    CHECK(call(AURACTL_CONSOLE, 0, 0, 0, UVA + 200 * PG, 100) == AURACTL_EFAULT);
}

static void test_peek(void)
{
    standard_map();
    const u32 *v = (const u32 *)gptr(PA_BUF);
    CHECK(call(AURACTL_PEEK, SOC_PS, 1, 0, VA_BUF, 4) == 0);
    CHECK(req_ptr()->ret0 == (u32)(SOC_PS * 3) && req_ptr()->buf_used == 4);
    CHECK(call(AURACTL_PEEK, SOC_PS + 0x10, 4, 0, VA_BUF, 16) == 0);
    CHECK(v[3] == (u32)((SOC_PS + 0x1c) * 3) && req_ptr()->buf_used == 16);
    CHECK(call(AURACTL_PEEK, 0x2ff900000UL, 1, 0, VA_BUF, 4) == 0); /* clkgen */
    /* Bounds: alignment, count, a buffer too small, past the range, not in the catalogue */
    CHECK(call(AURACTL_PEEK, SOC_PS + 2, 1, 0, VA_BUF, 4) == AURACTL_EINVAL);
    CHECK(call(AURACTL_PEEK, SOC_PS, AURACTL_MAX_PEEK + 1, 0, VA_BUF, PG) == AURACTL_EINVAL);
    CHECK(call(AURACTL_PEEK, SOC_PS, 4, 0, VA_BUF, 12) == AURACTL_EINVAL);
    CHECK(call(AURACTL_PEEK, SOC_PS + 0x18000 - 4, 2, 0, VA_BUF, 8) == AURACTL_EACCES);
    CHECK(call(AURACTL_PEEK, RAM, 1, 0, VA_BUF, 4) == AURACTL_EACCES);
    /* Never the PMS, an AMCC, a mailbox or a never-read page, and none of them is read */
    int p0 = peeks;
    CHECK(call(AURACTL_PEEK, 0x3018a8000UL, 1, 0, VA_BUF, 4) == AURACTL_EACCES); /* pms-pmc */
    CHECK(call(AURACTL_PEEK, 0x2a0064000UL, 1, 0, VA_BUF, 4) == AURACTL_EACCES); /* amcc */
    CHECK(call(AURACTL_PEEK, 0x300000000UL + 0x7365UL * 0x400000 + 0x8000, 1, 0, VA_BUF, 4) ==
          AURACTL_EACCES); /* sep-mbox */
    CHECK(call(AURACTL_PEEK, 0x300464000UL, 1, 0, VA_BUF, 4) == AURACTL_EACCES); /* pmgr-cllt */
    CHECK(peeks == p0);
    /* A chip whose never-read words aura does not know: no PEEK at all */
    chip_id = 0x6050;
    CHECK(call(AURACTL_PEEK, SOC_PS, 1, 0, VA_BUF, 4) == AURACTL_EACCES);
    CHECK(peeks == p0);
    chip_id = T8140;
    /* The caller's buffer still goes through its own translation */
    CHECK(call(AURACTL_PEEK, SOC_PS, 1, 0, UVA + 200 * PG, 4) == AURACTL_EFAULT);
}

static void test_hooks(void)
{
    standard_map();
    int r0 = resums, t0 = s2_tlbis;
    char *t = (char *)gptr(PA_BUF);
    /* List: everything comes back, the missing blocks marked */
    CHECK(call(AURACTL_LIST, 0, 0, 0, VA_BUF, BUF_PAGES * PG) == 0);
    u64 total = req_ptr()->ret0;
    CHECK(total == 9 + 2 + 4 + 12 + 4); /* PMGR blocks, PMS pages, AMCC planes, mailboxes, rest */
    CHECK(req_ptr()->buf_used == total * sizeof(struct auractl_range));
    const struct auractl_range *e = (void *)gptr(PA_BUF);
    int absent = 0;
    for (u64 i = 0; i < total; i++)
        if (!strcmp(e[i].name, "gfx-mbox") || !strcmp(e[i].name, "gfx1-mbox") ||
            !strcmp(e[i].name, "pmgr-pwrgate"))
            absent += !(e[i].flags & AURACTL_RANGE_FOUND) && !e[i].size && !e[i].base;
    CHECK(absent == 3);
    CHECK(call(AURACTL_LIST, 0, 0, 0, VA_BUF, sizeof(*e) * 3) == 0 && req_ptr()->ret0 == total);
    CHECK(req_ptr()->buf_used == 3 * sizeof(*e));
    /* By name: two ranges of one name, then the same again (nothing changes) */
    strcpy(t, "pms-pmc");
    CHECK(call(AURACTL_ARM, AURACTL_SEL_NAME, 0, 0, VA_BUF, 8) == 0 && req_ptr()->ret0 == 2);
    CHECK(resums == r0 + 1 && s2_tlbis == t0 + 1);
    CHECK(call(AURACTL_ARM, AURACTL_SEL_NAME, 0, 0, VA_BUF, 8) == 0 && req_ptr()->ret0 == 0);
    CHECK(resums == r0 + 1 && s2_tlbis == t0 + 1); /* no change, no TLB work, no new reference */
    CHECK(call(AURACTL_LIST, 0, 0, 0, VA_BUF, BUF_PAGES * PG) == 0);
    for (u64 i = 0; i < total; i++)
        if (!strcmp(e[i].name, "pms-pmc"))
            CHECK(e[i].flags & AURACTL_RANGE_ARMED);
    strcpy(t, "pms-pmc");
    CHECK(call(AURACTL_DISARM, AURACTL_SEL_NAME, 0, 0, VA_BUF, 8) == 0 && req_ptr()->ret0 == 2);
    strcpy(t, "no-such-range");
    CHECK(call(AURACTL_ARM, AURACTL_SEL_NAME, 0, 0, VA_BUF, 14) == AURACTL_ENOENT);
    strcpy(t, "gfx-mbox"); /* in the catalogue, absent on this machine */
    CHECK(call(AURACTL_ARM, AURACTL_SEL_NAME, 0, 0, VA_BUF, 9) == AURACTL_ENOENT);
    memset(t, 'n', 4096); /* an unterminated name is cut, and simply not found */
    CHECK(call(AURACTL_ARM, AURACTL_SEL_NAME, 0, 0, VA_BUF, 4096) == AURACTL_ENOENT);
    CHECK(call(AURACTL_ARM, AURACTL_SEL_NAME, 0, 0, 0, 0) == AURACTL_EINVAL);
    /* A range of more pages than one call may change */
    strcpy(t, "pmgr-aon");
    CHECK(call(AURACTL_ARM, AURACTL_SEL_NAME, 0, 0, VA_BUF, 9) == 0 && req_ptr()->ret0 == 32);
    CHECK(call(AURACTL_DISARM, AURACTL_SEL_NAME, 0, 0, VA_BUF, 9) == 0 && req_ptr()->ret0 == 32);
    strcpy(t, "clkgen");
    CHECK(call(AURACTL_ARM, AURACTL_SEL_NAME, 0, 0, VA_BUF, 7) == AURACTL_E2BIG);
    CHECK(req_ptr()->ret0 == AURACTL_MAX_PAGES);
    CHECK(call(AURACTL_DISARM, AURACTL_SEL_ALL, 0, 0, 0, 0) == 0 &&
          req_ptr()->ret0 == AURACTL_MAX_PAGES);
    /* By range: inside a split block */
    CHECK(call(AURACTL_ARM, AURACTL_SEL_RANGE, SOC_PS + 0x100, 8, 0, 0) == 0 &&
          req_ptr()->ret0 == 1);
    CHECK(sptm_virt_s2r_ctl_count(SOC_PS, PG, NULL) == 1);
    /* RAM, a block never split, the never-read list and the deny list are all refused */
    CHECK(call(AURACTL_ARM, AURACTL_SEL_RANGE, RAM, PG, 0, 0) == AURACTL_EACCES);
    CHECK(call(AURACTL_ARM, AURACTL_SEL_RANGE, RAM + 0x1000000, PG, 0, 0) == AURACTL_EACCES);
    CHECK(call(AURACTL_ARM, AURACTL_SEL_RANGE, 0x2c0000000UL, PG, 0, 0) == AURACTL_EACCES);
    CHECK(call(AURACTL_ARM, AURACTL_SEL_RANGE, 0x300780000UL, PG, 0, 0) == AURACTL_EACCES);
    strcpy(t, "pmgr-cllt"); /* its one page is on the never-read list */
    CHECK(call(AURACTL_ARM, AURACTL_SEL_NAME, 0, 0, VA_BUF, 10) == AURACTL_EACCES);
    CHECK(req_ptr()->ret0 == 0);
    /* Bad ranges */
    CHECK(call(AURACTL_ARM, AURACTL_SEL_RANGE, SOC_PS, 0, 0, 0) == AURACTL_EINVAL);
    CHECK(call(AURACTL_ARM, AURACTL_SEL_RANGE, ~0UL - 10, 100, 0, 0) == AURACTL_EINVAL);
    CHECK(call(AURACTL_ARM, AURACTL_SEL_RANGE, SOC_PS, (AURACTL_MAX_PAGES + 1) * PG, 0, 0) ==
          AURACTL_EINVAL);
    CHECK(call(AURACTL_ARM, 99, 0, 0, 0, 0) == AURACTL_EINVAL);
    CHECK(call(AURACTL_ARM, AURACTL_SEL_ALL, 0, 0, 0, 0) == AURACTL_EINVAL);
    /* A partial range that hits a refused page reports what changed before it */
    int r1 = resums, t1 = s2_tlbis;
    CHECK(call(AURACTL_ARM, AURACTL_SEL_RANGE, 0x300780000UL - PG, 2 * PG, 0, 0) == AURACTL_EACCES);
    CHECK(req_ptr()->ret0 == 1);
    CHECK(resums == r1 + 1 && s2_tlbis == t1 + 1); /* the page that did change still gets both */
    /* A catalogue entry in RAM is refused, and its block was never split */
    strcpy(t, "dart-dcp");
    CHECK(call(AURACTL_ARM, AURACTL_SEL_NAME, 0, 0, VA_BUF, 9) == AURACTL_EACCES);
    CHECK(call(AURACTL_ARM, AURACTL_SEL_RANGE, RAM + 0x4000000, PG, 0, 0) == AURACTL_EACCES);
    /* A trace-setup page: arming is a no-op, and auractl may not release it */
    hooks[nhooks].page = SOC_PS + PG;
    hooks[nhooks++].ctl = false;
    CHECK(call(AURACTL_ARM, AURACTL_SEL_RANGE, SOC_PS + PG, PG, 0, 0) == 0 &&
          req_ptr()->ret0 == 0);
    CHECK(call(AURACTL_DISARM, AURACTL_SEL_RANGE, SOC_PS + PG, PG, 0, 0) == AURACTL_EBUSY);
    /* Disarm everything auractl armed, and only that */
    CHECK(call(AURACTL_DISARM, AURACTL_SEL_ALL, 0, 0, 0, 0) == 0 && req_ptr()->ret0 == 2);
    CHECK(sptm_virt_s2r_ctl_count(0, ~0UL >> 1, NULL) == 1);
    nhooks = 0;
    /* The never-read list is T8140's: on another chip that page is an ordinary one */
    chip_id = 0x6050;
    CHECK(call(AURACTL_ARM, AURACTL_SEL_RANGE, 0x300780000UL, PG, 0, 0) == 0 &&
          req_ptr()->ret0 == 1);
    CHECK(call(AURACTL_DISARM, AURACTL_SEL_ALL, 0, 0, 0, 0) == 0 && req_ptr()->ret0 == 1);
    chip_id = T8140;
    /* No ring: hooks and reads say so */
    struct s2r_rec *ring = sptm_virt_s2r_ring;
    sptm_virt_s2r_ring = NULL;
    CHECK(call(AURACTL_ARM, AURACTL_SEL_RANGE, SOC_PS, PG, 0, 0) == AURACTL_ENODEV);
    CHECK(call(AURACTL_READ, 0, 1, 0, VA_BUF, 32) == AURACTL_ENODEV);
    CHECK(call(AURACTL_MARK, 0, 0, 0, 0, 0) == AURACTL_ENODEV);
    CHECK(call(AURACTL_STOP, 0, 0, 0, 0, 0) == AURACTL_ENODEV);
    CHECK(call(AURACTL_CLEAR, 0, 0, 0, 0, 0) == AURACTL_ENODEV);
    CHECK(call(AURACTL_INFO, 0, 0, 0, 0, 0) == 0);
    sptm_virt_s2r_ring = ring;
}

static void test_not_live(void)
{
    standard_map();
    sptm_virt_xnu_live = false;
    CHECK(call(AURACTL_INFO, 0, 0, 0, 0, 0) == AURACTL_ENODEV);
    sptm_virt_xnu_live = true;
}

int main(void)
{
    char path[] = "/tmp/ctl_host.XXXXXX";
    ram_fd = mkstemp(path);
    assert(ram_fd >= 0);
    unlink(path);
    assert(!ftruncate(ram_fd, RAM_PAGES * PG));
    guest = mmap(NULL, RAM_PAGES * PG, PROT_READ | PROT_WRITE, MAP_SHARED, ram_fd, 0);
    assert(guest != MAP_FAILED);
    assert(sysconf(_SC_PAGESIZE) == (long)PG);

    sptm_virt_s2r_nrec = 8192;
    sptm_virt_s2r_ring = calloc(sptm_virt_s2r_nrec, sizeof(struct s2r_rec));
    sptm_virt_ctl_s2_setup();

    test_setup();
    test_dispatch();
    test_request_checks();
    test_info();
    test_read();
    test_ring_control();
    test_console();
    test_peek();
    test_hooks();
    test_not_live();
    printf("ok: %d checks, %d splits, %d window mappings\n", tests, splits, win_maps);
    return 0;
}
