/* SPDX-License-Identifier: MIT */
/*
 * auractl: the SPTM-virt guest controls aura's stage-2 MMIO trace (AURACTL=1 builds only).
 *
 * The guest's kernel, at virtual EL2, issues `hvc #AURACTL_HVC_IMM` with w0 = the magic and x1 =
 * the address of a struct auractl_req in the process it acts for (src/auractl_abi.h). aura then:
 *
 *  - translates every caller page with AT S1E0R/S1E0W in the caller's own translation regime, so
 *    only memory the calling process can read or write is touched;
 *  - refuses a page outside the RAM SPTM manages (boot_args physBase/memSize, read once at
 *    setup), inside aura's image, or of Device memory;
 *  - maps one physical page at a time at a private aura address, never aura's identity alias;
 *  - moves at most AURACTL_MAX_BUF bytes and changes at most AURACTL_MAX_PAGES hooks per call;
 *  - hooks only MMIO pages in 32 MiB blocks it split at setup that no other aura hook owns, so
 *    the guest running on other cores only ever sees one L3 leaf change;
 *  - takes the changed stage-2 tree as the one each core checks at its XNU entry, under the lock
 *    that check takes;
 *  - serializes calls with one lock and returns a Darwin errno in x0.
 *
 * Two transports reach the same handler. The hvc one above needs a byte of guest kernel code (an
 * entry) to make the call. The second (sptm_virt_ctl_dcc) needs none: an unprivileged process
 * writes the request's own address to the debug-communications-channel register DBGDTR_EL0, which
 * aura traps to EL2 (MDCR_EL2.TDCC, src/nv2.c). Same ABI, same handler; the request is read and
 * run identically. This matters where no kext can load (SIP on, or a host-fed BootKC).
 *
 * The catalogue of named ranges comes from this machine's ADT: the PMGR, PMS and AMCC blocks the
 * sleep path found (sptm_virt_s2r_soc.c), the coprocessor mailboxes, GPIO, SPMI and the DCP DART.
 */

#ifdef CTL_HOST
#include "ctl_host.h" /* tests/ctl_host: this file against a fake guest */
#else
#include "../build/build_cfg.h"

#include "adt.h"
#include "arm_cpu_regs.h"
#include "hv.h"
#include "memory.h"
#include "smp.h"
#include "soc.h"
#include "sptm_virt_priv.h"
#include "sptm_virt_s2r_soc.h"
#include "string.h"
#include "utils.h"
#include "xnuboot.h"

extern char con_buf[];
extern size_t con_wp;
extern const char *const aura_version;
#endif
#include "auractl_abi.h"

#ifdef AURACTL

#define CTL_WIN 0xb000000000UL /* aura VA of the one-page window onto a caller's page */
#define PG      0x4000UL
#define BLK     (1UL << 25) /* stage-2 L2 block at 16 KiB granule */

#define CON_BYTES 8192 /* iodev.c CONSOLE_BUFFER_SIZE */

/* ---- the catalogue ---- */

#define CTL_MAX_CAT 112

struct ctl_ent {
    /* "c:<name>": the tracer's page name. The prefix keeps every name-keyed sleep rule of
     * sptm_virt_s2r.c off the pages auractl arms. */
    char what[2 + sizeof(((struct auractl_range *)0)->name)];
    u64 base, size; /* size 0: this machine's ADT has no such block */
};

/*
 * T8140 pages aura never hooks or reads for auractl: words whose read faults the SoC. A chip
 * without such a list gets no PEEK at all. TODO: the lists of the other SPTM-virt chips.
 */
static const u64 ctl_never_t8140[] = {0x3082d0000UL, 0x300780000UL, 0x3082b8000UL, 0x3082c8000UL,
                                      0x30819c000UL, 0x300464000UL, 0x389608000UL};

static struct {
    struct ctl_ent cat[CTL_MAX_CAT];
    u32 ncat;
    u64 blocks[128]; /* 32 MiB blocks split at setup */
    u32 nblocks;
    u64 deny[4][2]; /* aic, dockchannel */
    u32 ndeny;
    u64 ram_base, ram_end; /* guest RAM (SPTM's managed window) */
    bool ready;
} ctl;

static spinlock_t ctl_lock = SPINLOCK_INIT;
static u32 ctl_errs;

#define ctl_log(...)                                                                               \
    do {                                                                                           \
        if (!sptm_virt_s2r_quiet)                                                                  \
            printf(__VA_ARGS__);                                                                   \
    } while (0)

/* ---- the caller's memory ---- */

static u64 ctl_mapped; /* physical page in the window, or 0 */

AURA_XTEXT static void ctl_unmap(void)
{
    if (!ctl_mapped)
        return;
    mmu_rm_mapping(CTL_WIN, PG);
    sysop("dsb ish");
    sysop("tlbi alle2is");
    sysop("dsb ish");
    sysop("isb");
    ctl_mapped = 0;
}

AURA_XTEXT static const u64 *ctl_never(u32 *n)
{
    if (chip_id == T8140) {
        *n = ARRAY_SIZE(ctl_never_t8140);
        return ctl_never_t8140;
    }
    /*
     * Every other chip, the T6040/T6041 (the J616s) included: PEEK is not enabled yet. There is no
     * branch for them above, so they fall through here and get the same EACCES as any unlisted
     * chip. Enabling PEEK on a chip means confirming its never-read list from our own measured ADT,
     * the same way ctl_never_t8140[] was (no Apple firmware bytes), and adding a branch above.
     * Returning NULL until then is fail-safe: an unconfirmed page could fault the SoC when read.
     * The other commands (ARM/START/STOP/READ/MARK/CONSOLE) do not need it. See docs/auractl.md.
     */
    *n = 0;
    return NULL;
}

AURA_XTEXT static u64 ctl_dram_base(void)
{
    return ALIGN_DOWN(cur_boot_args.phys_base, BIT(32));
}

AURA_XTEXT static void ctl_split(u64 base, u64 size)
{
    if (base + size > ctl_dram_base())
        return; /* RAM is never hookable: nothing to split */
    for (u64 b = base & ~(BLK - 1); b < base + size; b += BLK) {
        bool have = false;
        for (u32 i = 0; i < ctl.nblocks; i++)
            have |= ctl.blocks[i] == b;
        if (have || ctl.nblocks >= ARRAY_SIZE(ctl.blocks))
            continue;
        hv_pt_split_block(b);
        ctl.blocks[ctl.nblocks++] = b;
    }
}

AURA_XTEXT static void ctl_add(const char *name, u64 base, u64 size)
{
    if (ctl.ncat >= ARRAY_SIZE(ctl.cat))
        return;
    struct ctl_ent *e = &ctl.cat[ctl.ncat++];
    memset(e, 0, sizeof(*e));
    e->what[0] = 'c';
    e->what[1] = ':';
    strncpy(e->what + 2, name, sizeof(e->what) - 3);
    if (!base || !size)
        return;
    e->base = base;
    e->size = size;
    ctl_split(base, size);
}

AURA_XTEXT static void ctl_add_adt(const char *path, const char *name, u32 idx, u64 off, u64 size)
{
    int p[8];
    u64 pa = 0, sz = 0;
    if (adt_path_offset_trace(adt, path, p) < 0 || adt_get_reg(adt, p, "reg", idx, &pa, &sz) < 0)
        pa = sz = 0;
    else if (size && off + size > sz)
        pa = sz = 0; /* the window is not inside this node's reg */
    ctl_add(name, pa ? pa + off : 0, size ? size : sz);
}

AURA_XTEXT static void ctl_add_blk(const char *name, const struct s2r_blk *b)
{
    ctl_add(name, b->base, b->size);
}

AURA_XTEXT static void ctl_catalogue(void)
{
    const struct s2r_soc *soc = sptm_virt_s2r_ctl_soc();
    static const struct s2r_blk none;

#define SOC_BLK(f) (soc ? &soc->f : &none)
    /* The PMGR blocks the sleep path found from the ADT (sptm_virt_s2r_soc.c). */
    ctl_add_blk("pmgr-soc-ps", SOC_BLK(soc_ps));
    ctl_add_blk("pmgr-pwrgate", SOC_BLK(pwrgate));
    ctl_add_blk("pmgr-soc-misc", SOC_BLK(soc_misc));
    ctl_add_blk("pmgr-cllt", SOC_BLK(cllt));
    ctl_add_blk("pmgr-cluster-ctl", SOC_BLK(cluster_ctl));
    ctl_add_blk("pmgr-aon", SOC_BLK(aon));
    ctl_add_blk("pmgr-aon-pst", SOC_BLK(aon_pst));
    ctl_add_blk("nub-ap-ctrl", SOC_BLK(nub_ap));
    ctl_add_blk("clkgen", SOC_BLK(clkgen));
#undef SOC_BLK
    for (u32 i = 0; i < 2; i++)
        ctl_add("pms-pmc", soc ? soc->pms_pmc[i] : 0, PG);
    u32 planes = soc && soc->amcc ? min(soc->amcc_planes, 8) : 0;
    if (!planes)
        ctl_add("amcc", 0, 0);
    for (u32 i = 0; i < planes; i++)
        ctl_add("amcc", ALIGN_DOWN(soc->amcc, PG) + ((u64)i << 18), PG);
    /* A sleep that is the S2R timer's start alone: its PS groups and the cores' windows. */
    for (int i = 0; soc && soc->timer_only && i < soc->n_trace; i++)
        ctl_add(soc->trace[i].what, soc->trace[i].base, soc->trace[i].size);
    if (soc && soc->timer_only)
        ctl_add("s2r-timer", ALIGN_DOWN(soc->stop_timer, PG), PG);

    /* The coprocessors' mailboxes (the ASC's at +0x8000), GPIO, SPMI and the DCP DART. */
    static const struct {
        const char *path, *name;
    } mbox[] = {
        {"/arm-io/pmp", "pmp-mbox"},     {"/arm-io/smc", "smc-mbox"},
        {"/arm-io/aop", "aop-mbox"},     {"/arm-io/aop2", "aop2-mbox"},
        {"/arm-io/sep", "sep-mbox"},     {"/arm-io/sio", "sio-mbox"},
        {"/arm-io/mtp", "mtp-mbox"},     {"/arm-io/ans", "ans-mbox"},
        {"/arm-io/dcp", "dcp-mbox"},     {"/arm-io/dcpext", "dcpext-mbox"},
        {"/arm-io/gfx-asc", "gfx-mbox"}, {"/arm-io/gfx1-asc", "gfx1-mbox"},
    };
    for (u32 i = 0; i < ARRAY_SIZE(mbox); i++)
        ctl_add_adt(mbox[i].path, mbox[i].name, 0, 0x8000, PG);
    ctl_add_adt("/arm-io/gpio0", "gpio0", 0, 0, PG);
    ctl_add_adt("/arm-io/nub-spmi0", "spmi0", 0, 0, PG);
    ctl_add_adt("/arm-io/nub-spmi-a0", "spmi-a0", 0, 0, PG);
    ctl_add_adt("/arm-io/dart-dcp", "dart-dcp", 0, 0, 0);
}

AURA_XTEXT static void ctl_deny_adt(const char *path)
{
    int p[8];
    u64 pa, sz;
    if (ctl.ndeny < ARRAY_SIZE(ctl.deny) && adt_path_offset_trace(adt, path, p) >= 0 &&
        adt_get_reg(adt, p, "reg", 0, &pa, &sz) >= 0) {
        ctl.deny[ctl.ndeny][0] = pa & ~(PG - 1);
        ctl.deny[ctl.ndeny++][1] = ALIGN_UP(pa + sz, PG);
    }
}

/* From sptm_virt_s2_setup, after the sleep path's setup and before the stage-2 tree is summed and
 * the guest runs. */
AURA_XTEXT void sptm_virt_ctl_s2_setup(void)
{
    ctl_catalogue();
    ctl_deny_adt("/arm-io/aic");
    ctl_deny_adt("/arm-io/dockchannel-uart");
    /* The window's tables, allocated now so a call never allocates. */
    u64 size = 0;
    sptm_virt_ctl_guest_ram(&ctl.ram_base, &size);
    ctl.ram_end = ctl.ram_base + size;
    if (ctl.ram_base && size) {
        mmu_add_mapping(CTL_WIN, ctl.ram_base, PG, MAIR_IDX_NORMAL, PERM_RW);
        ctl_mapped = ctl.ram_base;
        ctl_unmap(); /* with its TLB invalidate: a call never finds this mapping cached */
        ctl.ready = true;
    }
    printf("auractl: ABI %d at hvc #0x%x: %u catalogue ranges in %u split blocks, guest RAM "
           "[0x%lx, 0x%lx)%s\n",
           AURACTL_VERSION, AURACTL_HVC_IMM, ctl.ncat, ctl.nblocks, ctl.ram_base, ctl.ram_end,
           ctl.ready ? "" : "; off");
}

/* The caller's VA, translated as its own EL0 access would be. */
AURA_XTEXT static int ctl_at(u64 va, bool write, u64 *pa)
{
    u64 save = mrs(PAR_EL1);
#ifdef CTL_HOST
    ctl_host_at(va, write);
#else
    if (write)
        __asm__ volatile("at s1e0w, %0" ::"r"(va) : "memory");
    else
        __asm__ volatile("at s1e0r, %0" ::"r"(va) : "memory");
#endif
    sysop("isb");
    u64 par = mrs(PAR_EL1);
    msr(PAR_EL1, save);
    if (par & PAR_F)
        return AURACTL_EFAULT;
    if (!((par >> 60) & 0xf)) /* PAR.ATTR[7:4] = 0: Device memory */
        return AURACTL_EFAULT;
    u64 p = (par & PAR_PA) | (va & 0xfff);
    if (p < ctl.ram_base || p >= ctl.ram_end || (p >= (u64)_base && p < (u64)_end))
        return AURACTL_EFAULT;
    *pa = p;
    return 0;
}

/* Copy between the caller and aura, 4 KiB of VA at a time (a 4 KiB-page process is allowed). */
AURA_XTEXT static int ctl_copy(u64 uva, void *k, u64 len, bool to_user)
{
    u8 *kp = k;
    while (len) {
        u64 pa;
        int err = ctl_at(uva, to_user, &pa);
        if (err)
            return err;
        u64 n = min(len, 0x1000 - (uva & 0xfff));
        u64 page = pa & ~(PG - 1);
        if (page != ctl_mapped) {
            ctl_unmap();
            mmu_add_mapping(CTL_WIN, page, PG, MAIR_IDX_NORMAL, PERM_RW);
            ctl_mapped = page;
        }
        void *w = (void *)(CTL_WIN + (pa & (PG - 1)));
        if (to_user)
            memcpy(w, kp, n);
        else
            memcpy(kp, w, n);
        uva += n;
        kp += n;
        len -= n;
    }
    return 0;
}

/* ---- hooks ---- */

AURA_XTEXT static bool ctl_hookable(u64 page)
{
    if (page >= ctl_dram_base())
        return false; /* MMIO only */
    u32 nnever;
    const u64 *never = ctl_never(&nnever);
    for (u32 i = 0; i < nnever; i++)
        if (page == never[i])
            return false;
    for (u32 i = 0; i < ctl.ndeny; i++)
        if (page >= ctl.deny[i][0] && page < ctl.deny[i][1])
            return false;
    for (u32 i = 0; i < ctl.nblocks; i++)
        if ((page & ~(BLK - 1)) == ctl.blocks[i])
            return true;
    return false; /* its block is a 32 MiB block: changing it with the guest running is not safe */
}

/* (Dis)arm [base, base + size): 0 or the errno of the first refused page. *changed counts the
 * pages that did change, refusal or not: the caller owes them a TLB invalidate either way. */
AURA_XTEXT static int ctl_hook_range(const char *what, u64 base, u64 size, bool arm, u32 *budget,
                                     int *changed)
{
    for (u64 p = base & ~(PG - 1); p < base + size; p += PG) {
        if (!*budget)
            return AURACTL_E2BIG;
        if (arm && !ctl_hookable(p))
            return AURACTL_EACCES;
        int r = sptm_virt_s2r_ctl_page(what, p, arm);
        if (r < 0)
            return -r;
        *changed += r;
        *budget -= r;
    }
    return 0;
}

/* The tree changed with the guest running (inside sptm_virt_s2_ctl_begin/end). */
AURA_XTEXT static void ctl_s2_invalidate(void)
{
    sysop("dsb ishst");
    sysop("tlbi vmalls12e1is");
    sysop("dsb ish");
    sysop("isb");
}

/* ---- commands ---- */

AURA_XTEXT static int ctl_arm(struct auractl_req *r, bool arm)
{
    u32 budget = AURACTL_MAX_PAGES;
    int total = 0, err = 0;

    if (r->arg0 == AURACTL_SEL_ALL) {
        if (arm)
            return AURACTL_EINVAL;
    } else if (r->arg0 == AURACTL_SEL_RANGE) {
        u64 base = r->arg1, size = r->arg2;
        if (!size || size > AURACTL_MAX_PAGES * PG || base + size < base)
            return AURACTL_EINVAL;
    } else if (r->arg0 != AURACTL_SEL_NAME) {
        return AURACTL_EINVAL;
    }
    char name[sizeof(((struct auractl_range *)0)->name)] = {0};
    if (r->arg0 == AURACTL_SEL_NAME) {
        if (!r->buf || !r->buf_len)
            return AURACTL_EINVAL;
        int e = ctl_copy(r->buf, name, min(r->buf_len, sizeof(name) - 1), false);
        if (e)
            return e;
        bool found = false;
        for (u32 i = 0; i < ctl.ncat; i++)
            found |= ctl.cat[i].size && !strcmp(ctl.cat[i].what + 2, name);
        if (!found)
            return AURACTL_ENOENT;
    }

    sptm_virt_s2_ctl_begin();
    if (r->arg0 == AURACTL_SEL_ALL) {
        total = sptm_virt_s2r_ctl_release_all();
    } else if (r->arg0 == AURACTL_SEL_RANGE) {
        err = ctl_hook_range("c:range", r->arg1, r->arg2, arm, &budget, &total);
    } else {
        for (u32 i = 0; i < ctl.ncat && !err; i++) {
            const struct ctl_ent *e = &ctl.cat[i];
            if (e->size && !strcmp(e->what + 2, name))
                err = ctl_hook_range(e->what, e->base, e->size, arm, &budget, &total);
        }
    }
    if (total)
        ctl_s2_invalidate();
    sptm_virt_s2_ctl_end(total != 0);
    r->ret0 = total;
    ctl_log("auractl: %s %d pages (sel %lu 0x%lx+0x%lx)%s\n", arm ? "armed" : "released", total,
            r->arg0, r->arg1, r->arg2, err ? ", then refused" : "");
    return err;
}

AURA_XTEXT static int ctl_list(struct auractl_req *r)
{
    u64 max = r->buf_len / sizeof(struct auractl_range), n = 0;
    for (u32 i = 0; i < ctl.ncat && n < max; i++, n++) {
        const struct ctl_ent *c = &ctl.cat[i];
        struct auractl_range e;
        memset(&e, 0, sizeof(e));
        strncpy(e.name, c->what + 2, sizeof(e.name) - 1);
        e.base = c->base;
        e.size = c->size;
        u32 pages = c->size ? (ALIGN_UP(e.base + e.size, PG) - (e.base & ~(PG - 1))) / PG : 0;
        u32 hooked = c->size ? sptm_virt_s2r_ctl_count(e.base & ~(PG - 1), pages * PG, NULL) : 0;
        e.pages_hooked = hooked;
        e.flags = (c->size ? AURACTL_RANGE_FOUND : 0) |
                  (hooked && hooked == pages ? AURACTL_RANGE_ARMED : 0) |
                  (hooked && hooked < pages ? AURACTL_RANGE_PARTIAL : 0);
        int err = ctl_copy(r->buf + n * sizeof(e), &e, sizeof(e), true);
        if (err)
            return err;
    }
    r->ret0 = ctl.ncat;
    r->buf_used = n * sizeof(struct auractl_range);
    return 0;
}

AURA_XTEXT static int ctl_info(struct auractl_req *r)
{
    struct auractl_info in;
    memset(&in, 0, sizeof(in));
    in.version = AURACTL_VERSION;
    in.flags = (sptm_virt_s2r_ring ? AURACTL_INFO_TRACE : 0) |
               (sptm_virt_s2r_ring && !sptm_virt_s2r_ctl_paused ? AURACTL_INFO_RECORDING : 0);
#ifdef SPTM_VIRT_ESP
    in.flags |= AURACTL_INFO_ESP;
#endif
    in.cntfrq = mrs(CNTFRQ_EL0);
    in.nrec = sptm_virt_s2r_ring ? sptm_virt_s2r_nrec : 0;
    in.head = sptm_virt_s2r_head;
    in.rec_size = sizeof(struct auractl_rec);
    in.pages = sptm_virt_s2r_ctl_count(0, ~0UL >> 1, &in.ctl_pages);
    in.ncatalog = ctl.ncat;
    strncpy(in.build, aura_version, sizeof(in.build) - 1);
    r->ret0 = AURACTL_VERSION;
    if (!r->buf)
        return 0;
    if (r->buf_len < sizeof(in))
        return AURACTL_E2BIG;
    r->buf_used = sizeof(in);
    return ctl_copy(r->buf, &in, sizeof(in), true);
}

AURA_XTEXT static int ctl_read(struct auractl_req *r)
{
    if (!sptm_virt_s2r_ring)
        return AURACTL_ENODEV;
    u64 head = sptm_virt_s2r_head, nrec = sptm_virt_s2r_nrec;
    u64 oldest = head > nrec ? head - nrec : 0;
    u64 first = max(r->arg0, oldest);
    if (first > head)
        first = head;
    u64 n = min(min(r->arg1, head - first),
                min(r->buf_len, (u64)AURACTL_MAX_BUF) / sizeof(struct auractl_rec));
    for (u64 i = 0; i < n;) {
        u64 seq = first + i;
        u64 run = min(n - i, nrec - seq % nrec); /* up to the ring's end */
        int err =
            ctl_copy(r->buf + i * sizeof(struct auractl_rec), (void *)sptm_virt_s2r_ctl_rec(seq),
                     run * sizeof(struct auractl_rec), true);
        if (err)
            return err;
        i += run;
    }
    r->ret0 = first;
    r->ret1 = n;
    r->ret2 = head;
    r->buf_used = n * sizeof(struct auractl_rec);
    return 0;
}

AURA_XTEXT static int ctl_mark(struct auractl_req *r)
{
    char text[AURACTL_MAX_MARK + 1] = {0};
    if (!sptm_virt_s2r_ring)
        return AURACTL_ENODEV;
    if (sptm_virt_s2r_ctl_paused)
        return AURACTL_EBUSY;
    if (r->buf && r->buf_len) {
        int err = ctl_copy(r->buf, text, min(r->buf_len, (u64)AURACTL_MAX_MARK), false);
        if (err)
            return err;
    }
    for (char *c = text; *c; c++)
        if (*c < 0x20 || *c > 0x7e)
            *c = '.';
    char tag[9] = {0};
    memcpy(tag, text, 8);
    sptm_virt_s2r_marker(tag[0] ? tag : "MARK", r->arg0);
    ctl_log("auractl: mark \"%s\" 0x%lx\n", text, r->arg0);
    r->ret0 = sptm_virt_s2r_head;
    return 0;
}

AURA_XTEXT static int ctl_console(struct auractl_req *r)
{
    u64 wp = con_wp, n = min(min(wp, (u64)CON_BYTES), min(r->buf_len, (u64)AURACTL_MAX_BUF));
    u64 from = wp - n, done = 0;
    while (done < n) {
        u64 at = (from + done) % CON_BYTES, run = min(n - done, CON_BYTES - at);
        int err = ctl_copy(r->buf + done, con_buf + at, run, true);
        if (err)
            return err;
        done += run;
    }
    r->ret0 = wp;
    r->buf_used = n;
    return 0;
}

/* A word of a powered register block, read at EL2 for PEEK. */
AURA_XTEXT static u32 ctl_read32(u64 pa)
{
#ifdef CTL_HOST
    return ctl_host_read32(pa);
#else
    return read32(pa);
#endif
}

/* PEEK: words from one catalogue range of plain PMGR registers (the power states, the clock
 * generator, the AP nub), never a mailbox, a FIFO or a page aura will not hook, and only on a
 * chip whose never-read words aura knows. */
AURA_XTEXT static int ctl_peek(struct auractl_req *r)
{
    static const char *const ok[] = {"pmgr-", "clkgen", "nub-ap-ctrl"};
    u64 pa = r->arg0, n = r->arg1 ? r->arg1 : 1;
    if ((pa & 3) || n > AURACTL_MAX_PEEK || !r->buf || r->buf_len < n * 4)
        return AURACTL_EINVAL;
    u32 nnever;
    if (!ctl_never(&nnever))
        return AURACTL_EACCES;
    for (u32 i = 0; i < ctl.ncat; i++) {
        const struct ctl_ent *c = &ctl.cat[i];
        const char *nm = c->what + 2;
        if (!c->size || pa < c->base || pa + n * 4 > c->base + c->size)
            continue;
        bool allowed = false;
        for (u32 k = 0; k < ARRAY_SIZE(ok); k++)
            allowed |= !strncmp(nm, ok[k], strlen(ok[k]));
        if (!allowed || !ctl_hookable(pa & ~(PG - 1)) ||
            !ctl_hookable((pa + n * 4 - 1) & ~(PG - 1)))
            return AURACTL_EACCES;
        u32 v[AURACTL_MAX_PEEK];
        for (u64 k = 0; k < n; k++)
            v[k] = ctl_read32(pa + 4 * k);
        r->ret0 = v[0];
        r->buf_used = n * 4;
        return ctl_copy(r->buf, v, n * 4, true);
    }
    return AURACTL_EACCES;
}

AURA_XTEXT static int ctl_run(struct auractl_req *r)
{
    switch (r->cmd) {
        case AURACTL_INFO:
            return ctl_info(r);
        case AURACTL_LIST:
            return ctl_list(r);
        case AURACTL_ARM:
        case AURACTL_DISARM:
            if (!sptm_virt_s2r_ring)
                return AURACTL_ENODEV;
            return ctl_arm(r, r->cmd == AURACTL_ARM);
        case AURACTL_START:
        case AURACTL_STOP:
            if (!sptm_virt_s2r_ring)
                return AURACTL_ENODEV;
            sptm_virt_s2r_ctl_paused = r->cmd == AURACTL_STOP;
            ctl_log("auractl: recording %s at record %lu\n",
                    r->cmd == AURACTL_STOP ? "stopped" : "started", sptm_virt_s2r_head);
            return 0;
        case AURACTL_CLEAR: {
            if (!sptm_virt_s2r_ring)
                return AURACTL_ENODEV;
            bool paused = sptm_virt_s2r_ctl_paused;
            sptm_virt_s2r_ctl_paused = true;
            sysop("dsb ish");
            r->ret0 = sptm_virt_s2r_head;
            memset(sptm_virt_s2r_ring, 0, sptm_virt_s2r_nrec * sizeof(struct auractl_rec));
            sptm_virt_s2r_head = 0;
            sysop("dsb ish");
            sptm_virt_s2r_ctl_paused = paused;
            ctl_log("auractl: ring cleared (%lu records)\n", r->ret0);
            return 0;
        }
        case AURACTL_READ:
            return ctl_read(r);
        case AURACTL_MARK:
            return ctl_mark(r);
        case AURACTL_CONSOLE:
            return ctl_console(r);
        case AURACTL_PEEK:
            return ctl_peek(r);
        default:
            return AURACTL_ENOSYS;
    }
}

/*
 * A command reachable without the per-boot cookie: only the read-only status (INFO). Everything
 * else -- listing, arming, recording, reading records, peeking -- is gated. The hvc transports
 * (iokit/xnu) are already behind the guest kernel's own root check and always pass authed; only
 * the kext-free DCC transport, which any EL0 process can ring, presents a cookie
 * (sptm_virt_ctl_dcc).
 */
AURA_XTEXT static bool ctl_cmd_public(u16 cmd)
{
    return cmd == AURACTL_INFO;
}

/*
 * Run one request whose block is at the caller's own EL0 address req_va, read and written through
 * that caller's translation (AT S1E0R/S1E0W). The transport-agnostic core: both the hvc transport
 * (below) and the DCC transport call it with nothing but that address. authed is the transport's
 * verdict on whether this caller may run a gated command. Returns a Darwin errno.
 */
AURA_XTEXT static int sptm_virt_ctl_call(u64 req_va, bool authed)
{
    struct auractl_req r = {0};
    spin_lock(&ctl_lock);
    int err = ctl_copy(req_va, &r, sizeof(r), false);
    if (!err && (r.magic != AURACTL_REQ_MAGIC || r.version != AURACTL_VERSION))
        err = AURACTL_EINVAL;
    if (!err && !authed && !ctl_cmd_public(r.cmd))
        err = AURACTL_EPERM;
    if (!err && r.buf_len > AURACTL_MAX_BUF)
        r.buf_len = AURACTL_MAX_BUF;
    if (!err) {
        r.ret0 = r.ret1 = r.ret2 = r.buf_used = 0;
        err = ctl_run(&r);
        r.version = AURACTL_VERSION;
        int werr = ctl_copy(req_va, &r, sizeof(r), true);
        if (!err)
            err = werr;
    }
    ctl_unmap();
    spin_unlock(&ctl_lock);
    if (err && ctl_errs++ < 16)
        ctl_log("auractl: cmd %u -> errno %d\n", r.cmd, err);
    return err;
}

/*
 * nv2_handle_sync, for an HVC from the guest's kernel at virtual EL2 (not its user space, not a
 * nested guest). Ours only with the immediate and the magic; anything else goes back to the
 * guest as before.
 */
AURA_XTEXT bool sptm_virt_ctl_hvc(u64 *regs, u64 esr)
{
    if ((esr & 0xffff) != AURACTL_HVC_IMM || (u32)regs[0] != AURACTL_HVC_MAGIC)
        return false;
    if (!ctl.ready || !sptm_virt_xnu_live)
        regs[0] = AURACTL_ENODEV;
    else
        regs[0] =
            sptm_virt_ctl_call(regs[1], true); /* the guest kernel already vetted the caller */
    return true;
}

#ifdef CTL_HOST
/* The host harness drives the cookie gate the DCC transport applies: a call with authed=false must
 * refuse everything past the read-only status (tests/ctl_host/ctl_host.c). */
int ctl_host_call_unauth(u64 req_va)
{
    return sptm_virt_ctl_call(req_va, false);
}
#endif

#ifndef CTL_HOST
/*
 * The second transport: a guest process with no kernel help at all. With MDCR_EL2.TDCC set
 * (src/nv2.c, only when the boot's flags include SPTM_VIRT_CTL_DCC) every EL0 access to a
 * debug-communications-channel register traps here as EC 0x18. The doorbell is a write to
 * DBGDTR_EL0 of the request's own EL0 address, tagged in the top 16 bits (a user VA is 47-bit) so
 * stray DCC traffic is told from a doorbell; aura runs it exactly as the hvc transport does, then
 * leaves the errno to be read back from DBGDTR_EL0. No kext, no entitlement: a plain MSR/MRS to an
 * EL0-accessible debug register.
 *
 * Because any EL0 process can ring the doorbell, a gated command (anything past the read-only
 * status) also needs the per-boot cookie: a random value the host or the ESP manifest put in the
 * guest's boot-args, which only root can read (sysctl kern.bootargs). A caller presents it by
 * first writing it to DBGDTRTX_EL0; aura latches it per core and the very next doorbell consumes
 * it, so the authorisation is one request wide. A process that cannot read the cookie (anyone but
 * root) is left with the status command alone. The gate (the boot flag) and the cookie are
 * independent: the flag decides the transport exists at all; the cookie decides who may act.
 *
 * The errno is kept per core between the doorbell write and its read-back. A thread that migrates
 * cores in that window reads a stale errno, but the command already ran and wrote its outputs
 * into the request block, so only the summary errno, never the trace, is affected.
 */
#define CTL_DCC_TAG 0xAC71UL /* top 16 bits of a doorbell write, and of its errno read-back */

static int ctl_dcc_errno[32]; /* per core, indexed by smp_id() */
static u64 ctl_dcc_cookie;    /* the boot's cookie; 0 (unset) refuses every gated command */
static u64
    ctl_dcc_presented[32]; /* per core, the cookie last written to DBGDTRTX before a doorbell */

/* The per-boot cookie, from the host (run_guest_sptm.py, over the proxy) or extracted from the
 * ESP manifest's boot-args (src/sptm_virt_esp.c). used+retain: a host-called entry, like
 * sptm_virt_config, so it is never dropped even when nothing in-tree references it. */
__attribute__((used, retain)) void sptm_virt_ctl_set_cookie(u64 cookie)
{
    ctl_dcc_cookie = cookie;
    ctl_log("auractl: DCC transport cookie set (%s)\n", cookie ? "gated" : "none; gated off");
}

/*
 * The self-boot (ESP) path has no host to call sptm_virt_ctl_set_cookie: the cookie travels only in
 * the guest's boot-args, which aura itself lays down from the manifest. Read it back from there, so
 * the one source of truth -- what root sees in kern.bootargs -- is also what aura checks. The token
 * is "aura-actl-ck=<hex>", ended by a space or the end of the line. An absent or malformed token
 * leaves the cookie 0 (every gated command refused).
 */
AURA_XTEXT void sptm_virt_ctl_cookie_from_cmdline(const char *cmdline)
{
    const char *p = cmdline ? strstr(cmdline, "aura-actl-ck=") : NULL;
    if (!p)
        return;
    p += sizeof("aura-actl-ck=") - 1;
    u64 c = 0;
    int n = 0;
    for (; *p && *p != ' '; p++, n++) {
        unsigned d;
        if (*p >= '0' && *p <= '9')
            d = *p - '0';
        else if (*p >= 'a' && *p <= 'f')
            d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F')
            d = *p - 'A' + 10;
        else {
            n = 0; /* not a clean hex token: refuse rather than take a half-cookie */
            break;
        }
        c = (c << 4) | d;
    }
    if (n >= 1 && n <= 16)
        sptm_virt_ctl_set_cookie(c);
}

AURA_XTEXT bool sptm_virt_ctl_dcc(u64 *regs, u64 esr)
{
    /* Off unless enabled at boot: with the flag clear the trap is never armed (nv2.c), but guard
     * here too so a stray EC 0x18 is left for L1 rather than consumed. */
    if (!(sptm_virt_flags & SPTM_VIRT_CTL_DCC))
        return false;

    /* EC 0x18 ISS: op0[21:20] op2[19:17] op1[16:14] CRn[13:10] Rt[9:5] CRm[4:1] Dir[0]. */
    unsigned op0 = (esr >> 20) & 3, op2 = (esr >> 17) & 7, op1 = (esr >> 14) & 7,
             crn = (esr >> 10) & 15, crm = (esr >> 1) & 15, rt = (esr >> 5) & 31;
    bool read = esr & 1;

    /* The DCC registers (op0=2 op1=3 CRn=0 op2=0): CRm 1 = MDCCSR_EL0, 4 = DBGDTR_EL0,
     * 5 = DBGDTRRX/TX_EL0. Anything else is not ours; let L1 take its own trap. */
    if (op0 != 2 || op1 != 3 || crn != 0 || op2 != 0)
        return false;

    unsigned cpu = smp_id();
    if (cpu >= ARRAY_SIZE(ctl_dcc_errno))
        cpu = ARRAY_SIZE(ctl_dcc_errno) - 1;

    if (crm == 4) { /* DBGDTR_EL0: the doorbell (write) and the errno read-back (read) */
        if (read) {
            if (rt != 31)
                regs[rt] = (CTL_DCC_TAG << 48) | (u32)ctl_dcc_errno[cpu];
        } else {
            u64 v = rt == 31 ? 0 : regs[rt];
            u64 presented = ctl_dcc_presented[cpu];
            ctl_dcc_presented[cpu] = 0;     /* one doorbell wide: consume the presented cookie */
            if ((v >> 48) == CTL_DCC_TAG) { /* a doorbell, not stray DCC traffic */
                bool authed = ctl_dcc_cookie && presented == ctl_dcc_cookie;
                ctl_dcc_errno[cpu] = (ctl.ready && sptm_virt_xnu_live)
                                         ? sptm_virt_ctl_call(v & 0xffffffffffffUL, authed)
                                         : AURACTL_ENODEV;
            }
        }
    } else if (crm == 1) { /* MDCCSR_EL0 read: TXfull=1, RXfull=0, so a poller backs off cleanly */
        if (read && rt != 31)
            regs[rt] = 1u << 29;
    } else if (crm == 5) { /* DBGDTRRX/TX_EL0: the cookie inbox (write) ahead of a doorbell */
        if (read) {
            if (rt != 31)
                regs[rt] = 0;
        } else {
            ctl_dcc_presented[cpu] = rt == 31 ? 0 : regs[rt];
        }
    } else {
        return false;
    }

    sysop("isb");
    msr(ELR_EL2, mrs(ELR_EL2) + 4);
    return true;
}
#endif /* !CTL_HOST */

#endif
