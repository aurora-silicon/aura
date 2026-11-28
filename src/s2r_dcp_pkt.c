/* SPDX-License-Identifier: MIT */

/*
 * The virtualized SPTM's sleep trace on T6050: the display coprocessors' IOMFB packets.
 * Kept out of the sptm_virt objects, which share a full 64 KiB slot in the image.
 */

#include "s2r_dcp_pkt.h"
#include "adt.h"
#include "dart.h"
#include "sptm_virt_priv.h"
#include "string.h"
#include "types.h"
#include "utils.h"
#include "xnuboot.h"

/*
 * The display coprocessors' IOMFB calls (endpoint 0x37) live in shared memory, not in the mailbox
 * words: with DCP_PKT each call's packet head (tag, sizes, the first input bytes) and, at its
 * reply, its output are copied into the ring as the message passes. The shared memory's address
 * comes from the AP's init word ((DVA << 16) | 0x40); its pages are found once by a read-only
 * walk of that DCP's DART. Packet records carry S2R_PKT_TAG in their pc field.
 */
#define S2R_DCP_N     5
#define S2R_DCP_RPC   0xc0000UL
#define S2R_DCP_PAGES (S2R_DCP_RPC / 0x4000 + 1)
#define S2R_DCP_EP    0x37
#define S2R_PKT_TAG   0x544b5043UL /* "CPKT" */
#define S2R_PKT_MAX   128
#define S2R_DCP_OPEN  4
#define S2R_PKT_NFULL 4
#define S2R_PKT_FULL  BIT(63) /* in open[]: the packet was copied whole, so is its output */
/*
 * dart_walk_locked_t8110() visits 2048 L1 entries of 32 MiB: 36-bit IOVAs. A DVA is the DART's
 * vm-base (its ADT node's property, as dart.c reads it) plus such an IOVA: J613's RPC memory is
 * at 0x1000003c000. Without a vm-base only the walk's width is compared, as before.
 */
#define S2R_DCP_T8110_IOVA_BITS 36

static struct s2r_dcp {
    u64 mbox, dart, dart_size;
    u64 vm_base;
    bool have_vm_base;
    u64 dva, lo;
    u64 pa[S2R_DCP_PAGES];
    int resolved; /* 0 not yet, 1 found, -1 not found */
    u64 m0[2];    /* message word 0 of a pair not yet complete: 0 A2I, 1 I2A */
    bool have_m0[2];
    u64 open[2][S2R_DCP_OPEN]; /* outstanding packets per direction, for their replies */
    int nopen[2];
    u8 busy; /* one core at a time; a contended access is not followed */
} s2r_dcp[S2R_DCP_N];
static int s2r_n_dcp;
u32 sptm_virt_s2r_dcp_pkt; /* host: copy the DCPs' IOMFB packets into the ring */
/*
 * Host, debug only (AURA_S2R_DCP_PKT_*): the input bytes copied per packet (head), and up to
 * S2R_PKT_NFULL packet tags (the header's first word, e.g. "A407" stored as '7' '0' '4' 'A')
 * whose input and output are copied up to pkt_fullmax bytes each, one in every pkt_every of each
 * tag (the rest as any other packet). A swap at 120 Hz copied whole would fill the ring in seconds.
 */
u32 sptm_virt_s2r_dcp_pkt_max = S2R_PKT_MAX;
u32 sptm_virt_s2r_dcp_pkt_full[S2R_PKT_NFULL];
u32 sptm_virt_s2r_dcp_pkt_fullmax = 0x1000;
u32 sptm_virt_s2r_dcp_pkt_every = 1;
static u32 s2r_pkt_seen[S2R_PKT_NFULL];

struct s2r_dcp_walk {
    struct s2r_dcp *d;
    u64 lo; /* the DVA within the walk's IOVA width: J613's DVA carries bit 40 above it */
    int found;
};

static int s2r_dcp_visit(u64 iova, u64 pa, void *opaque)
{
    struct s2r_dcp_walk *w = opaque;
    if (iova >= w->lo && iova < w->lo + S2R_DCP_PAGES * 0x4000UL) {
        w->d->pa[(iova - w->lo) / 0x4000] = pa;
        if (++w->found == S2R_DCP_PAGES)
            return 1;
    }
    return 0;
}

static int s2r_dcp_resolve(struct s2r_dcp *d)
{
    u64 ram = ALIGN_DOWN(cur_boot_args.phys_base, BIT(32));
    memset(d->pa, 0, sizeof(d->pa));
    d->lo = d->dva & ~0x3fffUL;
    u64 iova = d->lo & GENMASK(S2R_DCP_T8110_IOVA_BITS - 1, 0);
    /*
     * With a vm-base that the DVA sits above, the IOVA is the difference, bounded by the walk's
     * width. A DVA outside it keeps the width-only compare (and says so): the J613 value this was
     * written against has not been read from its ADT yet.
     */
    if (d->have_vm_base) {
        if (d->lo >= d->vm_base && d->lo - d->vm_base < BIT(S2R_DCP_T8110_IOVA_BITS))
            iova = d->lo - d->vm_base;
        else
            printf("SPTM-virt[s2r]: dcp-pkt dva 0x%lx not within vm-base 0x%lx + 2^%u; "
                   "comparing the low %u bits\n",
                   d->dva, d->vm_base, S2R_DCP_T8110_IOVA_BITS, S2R_DCP_T8110_IOVA_BITS);
    }
    u32 walked = 0;
    for (u8 sid = 0; sid < 16; sid++) {
        struct s2r_dcp_walk w = {d, iova, 0};
        int ret = dart_walk_locked_t8110(d->dart, d->dart_size, sid, ram, mem_size_actual,
                                         s2r_dcp_visit, &w);
        if (ret >= 0)
            walked |= BIT(sid);
        if (ret >= 0 && w.found) {
            printf("SPTM-virt[s2r]: dcp-pkt dva 0x%lx: sid %u, %d of %lu pages\n", d->dva, sid,
                   w.found, S2R_DCP_PAGES);
            for (unsigned i = 0; i < S2R_DCP_PAGES; i++)
                if (!d->pa[i])
                    printf("SPTM-virt[s2r]: dcp-pkt dva 0x%lx: page %u (+0x%x) not mapped; its "
                           "records are dropped\n",
                           d->dva, i, i * 0x4000);
            return 1;
        }
    }
    printf("SPTM-virt[s2r]: dcp-pkt dva 0x%lx not found (walkable sids 0x%x)\n", d->dva, walked);
    return -1;
}

static u64 s2r_dcp_pa(struct s2r_dcp *d, u64 off)
{
    u64 a = d->dva + off;
    u64 i = (a - d->lo) / 0x4000;
    return i < S2R_DCP_PAGES && d->pa[i] ? d->pa[i] + (a & 0x3fff) : 0;
}

static void s2r_pkt_rec(int idx, int kind, int word, u64 pa, u64 v, bool a2i)
{
    sptm_virt_s2r_rec_raw(
        pa, v, S2R_PKT_TAG | ((u64)idx << 32) | ((u64)kind << 36) | ((u64)word << 40), a2i);
}

/* A packet whose tag the host named, on its turn (one in pkt_every): copied whole. */
static bool s2r_dcp_full(u32 tag)
{
    for (int i = 0; i < S2R_PKT_NFULL; i++)
        if (sptm_virt_s2r_dcp_pkt_full[i] && sptm_virt_s2r_dcp_pkt_full[i] == tag) {
            u32 every = sptm_virt_s2r_dcp_pkt_every ? sptm_virt_s2r_dcp_pkt_every : 1;
            return s2r_pkt_seen[i]++ % every == 0;
        }
    return false;
}

/*
 * kind 0: an AP call, 1: a DCP callback, 2: an AP call's output, 3: a callback's output.
 * Returns whether the packet was copied whole (a call's output then is too).
 */
static bool s2r_dcp_copy(int idx, struct s2r_dcp *d, u64 off, int kind, bool a2i, bool full)
{
    u64 h0, h1, pa0 = s2r_dcp_pa(d, off), pa1 = s2r_dcp_pa(d, off + 8);
    if (!pa0 || !pa1 || !sptm_virt_peek64(pa0, &h0) || !sptm_virt_peek64(pa1, &h1))
        return false;
    if (kind < 2)
        full = s2r_dcp_full(h0);
    s2r_pkt_rec(idx, kind, 0, pa0, h0, a2i);
    s2r_pkt_rec(idx, kind, 1, pa1, h1, a2i);
    u64 in = h0 >> 32, out = h1 & 0xffffffff;
    u64 imax = full ? sptm_virt_s2r_dcp_pkt_fullmax : sptm_virt_s2r_dcp_pkt_max;
    u64 omax = full ? sptm_virt_s2r_dcp_pkt_fullmax : 56;
    u64 from = kind < 2 ? off + 16 : ALIGN_DOWN(off + 12 + min(in, (u64)S2R_DCP_RPC), 8);
    u64 to = kind < 2 ? off + 12 + min(in, imax) : from + min(out + 8, omax + 8);
    /* the record's word number has 24 bits (pc 63:40) */
    for (u64 o = from, w = 2; o < to && o + 8 <= S2R_DCP_RPC && w < BIT(24); o += 8, w++) {
        u64 v, pa = s2r_dcp_pa(d, o);
        if (!pa || !sptm_virt_peek64(pa, &v))
            return full;
        s2r_pkt_rec(idx, kind, w, pa, v, a2i);
    }
    return full;
}

static void s2r_dcp_msg(int idx, u64 m0, u64 m1, int dir)
{
    struct s2r_dcp *d = &s2r_dcp[idx];
    u16 ctl = m0;

    if ((m1 & 0xff) != S2R_DCP_EP)
        return;
    if (dir == 0 && ctl == 0x40 && (m0 >> 16)) {
        d->dva = m0 >> 16;
        d->resolved = 0;
        d->nopen[0] = d->nopen[1] = 0;
        return;
    }
    if ((ctl & 3) != 2 || !d->dva)
        return;
    if (!d->resolved)
        d->resolved = s2r_dcp_resolve(d);
    if (d->resolved < 0)
        return;
    if (ctl & 0x40) {
        /* A reply closes the newest outstanding packet of the other side. */
        int o = !dir;
        if (d->nopen[o] > 0 && d->nopen[o] <= S2R_DCP_OPEN) {
            u64 e = d->open[o][--d->nopen[o]];
            s2r_dcp_copy(idx, d, e & ~S2R_PKT_FULL, 2 + o, dir == 0, e & S2R_PKT_FULL);
        }
        return;
    }
    u64 off = (dir == 0 ? ((ctl & 0x100) ? 0x20000 : 0) : ((ctl & 0x100) ? 0x40000 : 0x60000)) +
              ((ctl >> 10) & 3) * 0x8000UL + (u16)(m0 >> 16);
    if (off >= S2R_DCP_RPC)
        return;
    bool full = s2r_dcp_copy(idx, d, off, dir, dir == 0, false);
    if (d->nopen[dir] >= 0 && d->nopen[dir] < S2R_DCP_OPEN)
        d->open[dir][d->nopen[dir]++] = off | (full ? S2R_PKT_FULL : 0);
}

static void s2r_dcp_watch_one(int i, u64 off, const u64 *val, int width);

/* Called for every access to a hooked mailbox page; assembles the message pairs of the DCPs. */
void s2r_dcp_watch(u64 addr, const u64 *val, int width)
{
    if (!sptm_virt_s2r_dcp_pkt)
        return;
    u64 page = addr & ~0x3fffUL, off = addr & 0x3fff;
    for (int i = 0; i < s2r_n_dcp; i++) {
        if (s2r_dcp[i].mbox != page)
            continue;
        if (__atomic_exchange_n(&s2r_dcp[i].busy, 1, __ATOMIC_ACQUIRE))
            return;
        s2r_dcp_watch_one(i, off, val, width);
        __atomic_store_n(&s2r_dcp[i].busy, 0, __ATOMIC_RELEASE);
        return;
    }
}

static void s2r_dcp_watch_one(int i, u64 off, const u64 *val, int width)
{
    int dir = off >= 0x830 ? 1 : 0;
    u64 base = dir ? 0x830 : 0x800;

    if (off < 0x800 || off >= 0x840 || (off > 0x808 && off < 0x830))
        return;
    if (width == 4 && off == base) {
        s2r_dcp_msg(i, val[0], val[1], dir);
    } else if (width == 3 && off == base) {
        s2r_dcp[i].m0[dir] = val[0];
        s2r_dcp[i].have_m0[dir] = true;
    } else if (width == 3 && off == base + 8 && s2r_dcp[i].have_m0[dir]) {
        s2r_dcp[i].have_m0[dir] = false;
        s2r_dcp_msg(i, s2r_dcp[i].m0[dir], val[0], dir);
    }
}

/* A DCP or DCPEXT and its mailbox page: its DART is /arm-io/dart-<name>. */
void s2r_dcp_add(const char *name, u64 mbox)
{
    char path[48];
    int p[8];
    u64 base, size;

    if (s2r_n_dcp >= S2R_DCP_N)
        return;
    for (int i = 0; i < s2r_n_dcp; i++)
        if (s2r_dcp[i].mbox == mbox)
            return; /* both the coprocessor trace and s2r_hook's mailbox list can name it */
    snprintf(path, sizeof(path), "/arm-io/dart-%s", name);
    int node = adt_path_offset_trace(adt, path, p);
    if (node < 0 || adt_get_reg(adt, p, "reg", 0, &base, &size) < 0)
        return;
    s2r_dcp[s2r_n_dcp] = (struct s2r_dcp){.mbox = mbox, .dart = base, .dart_size = size};
    u32 len = 0;
    const void *vb = adt_getprop(adt, node, "vm-base", &len);
    if (vb && (len == 4 || len == 8)) {
        u64 v = 0;
        memcpy(&v, vb, len);
        s2r_dcp[s2r_n_dcp].vm_base = v;
        s2r_dcp[s2r_n_dcp].have_vm_base = true;
    }
    printf("SPTM-virt[s2r]: dcp-pkt %d %s mbox 0x%lx dart 0x%lx vm-base %s0x%lx\n", s2r_n_dcp, name,
           mbox, base, s2r_dcp[s2r_n_dcp].have_vm_base ? "" : "(none) ",
           s2r_dcp[s2r_n_dcp].vm_base);
    s2r_n_dcp++;
}
