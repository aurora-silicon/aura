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
#define S2R_DCP_SIDS  64

static struct s2r_dcp {
    u64 mbox, dart, dart_size;
    u64 dva, lo;
    u64 pa[S2R_DCP_PAGES];
    int resolved; /* 0 not yet, 1 found, -1 not found: tried again after 256 more messages */
    u32 miss;
    u64 m0[2];    /* message word 0 of a pair not yet complete: 0 A2I, 1 I2A */
    bool have_m0[2];
    u64 open[2][S2R_DCP_OPEN]; /* outstanding packets per direction, for their replies */
    int nopen[2];
    u8 busy; /* one core at a time; a contended access is not followed */
} s2r_dcp[S2R_DCP_N];
static int s2r_n_dcp;
u32 sptm_virt_s2r_dcp_pkt; /* host: copy the DCPs' IOMFB packets into the ring */

struct s2r_dcp_walk {
    struct s2r_dcp *d;
    int found;
};

/* The walk gives an IOVA below 2^36; the AP's DVA carries the stream's VM base above that. */
#define S2R_DCP_VM_LOW BIT(36)

static int s2r_dcp_visit(u64 iova, u64 pa, void *opaque)
{
    struct s2r_dcp_walk *w = opaque;
    u64 full = iova | (w->d->lo & ~(S2R_DCP_VM_LOW - 1));
    if (full >= w->d->lo && full < w->d->lo + S2R_DCP_PAGES * 0x4000UL) {
        w->d->pa[(full - w->d->lo) / 0x4000] = pa;
        if (++w->found == S2R_DCP_PAGES)
            return 1;
    }
    return 0;
}

static void s2r_pkt_rec(int idx, int kind, int word, u64 pa, u64 v, bool a2i);

/* The IOMFB shared memory is on whichever of the DART's streams (T6050: 23 and 24 carry it, not
 * stream 0..15) maps most of its pages: walk every stream and keep the best. One ring record per
 * attempt (kind 7): found pages, the stream, the streams the walk refused. */
static int s2r_dcp_resolve(struct s2r_dcp *d)
{
    u64 ram = ALIGN_DOWN(cur_boot_args.phys_base, BIT(32)), best[S2R_DCP_PAGES] = {0};
    int best_found = 0, best_sid = -1, refused = 0;

    d->lo = d->dva & ~0x3fffUL;
    for (u8 sid = 0; sid < S2R_DCP_SIDS; sid++) {
        struct s2r_dcp_walk w = {d, 0};
        memset(d->pa, 0, sizeof(d->pa));
        int rc = dart_walk_locked_t8110(d->dart, d->dart_size, sid, ram, mem_size_actual,
                                        s2r_dcp_visit, &w);
        refused += rc < 0;
        if (rc >= 0 && w.found > best_found) {
            best_found = w.found;
            best_sid = sid;
            memcpy(best, d->pa, sizeof(best));
        }
    }
    memcpy(d->pa, best, sizeof(best));
    s2r_pkt_rec(d - s2r_dcp, 7, best_sid < 0 ? 0 : best_sid, d->dva,
                ((u64)best_found << 32) | (u32)refused, false);
    return best_found ? 1 : -1;
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

/* kind 0: an AP call, 1: a DCP callback, 2: an AP call's output, 3: a callback's output */
static void s2r_dcp_copy(int idx, struct s2r_dcp *d, u64 off, int kind, bool a2i)
{
    u64 h0, h1, pa0 = s2r_dcp_pa(d, off), pa1 = s2r_dcp_pa(d, off + 8);
    if (!pa0 || !pa1 || !sptm_virt_peek64(pa0, &h0) || !sptm_virt_peek64(pa1, &h1))
        return;
    s2r_pkt_rec(idx, kind, 0, pa0, h0, a2i);
    s2r_pkt_rec(idx, kind, 1, pa1, h1, a2i);
    u64 in = h0 >> 32, out = h1 & 0xffffffff;
    u64 from = kind < 2 ? off + 16 : ALIGN_DOWN(off + 12 + min(in, (u64)S2R_DCP_RPC), 8);
    u64 to = kind < 2 ? off + 12 + min(in, (u64)S2R_PKT_MAX) : from + min(out + 8, 64UL);
    for (u64 o = from, w = 2; o < to && o + 8 <= S2R_DCP_RPC; o += 8, w++) {
        u64 v, pa = s2r_dcp_pa(d, o);
        if (!pa || !sptm_virt_peek64(pa, &v))
            return;
        s2r_pkt_rec(idx, kind, w, pa, v, a2i);
    }
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
    if (d->resolved < 0 && ++d->miss % 256 == 0)
        d->resolved = 0;
    if (!d->resolved)
        d->resolved = s2r_dcp_resolve(d);
    if (d->resolved < 0)
        return;
    if (ctl & 0x40) {
        /* A reply closes the newest outstanding packet of the other side. */
        int o = !dir;
        if (d->nopen[o] > 0 && d->nopen[o] <= S2R_DCP_OPEN)
            s2r_dcp_copy(idx, d, d->open[o][--d->nopen[o]], 2 + o, dir == 0);
        return;
    }
    u64 off = (dir == 0 ? ((ctl & 0x100) ? 0x20000 : 0) : ((ctl & 0x100) ? 0x40000 : 0x60000)) +
              ((ctl >> 10) & 3) * 0x8000UL + (u16)(m0 >> 16);
    if (off >= S2R_DCP_RPC)
        return;
    s2r_dcp_copy(idx, d, off, dir, dir == 0);
    if (d->nopen[dir] >= 0 && d->nopen[dir] < S2R_DCP_OPEN)
        d->open[dir][d->nopen[dir]++] = off;
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
    snprintf(path, sizeof(path), "/arm-io/dart-%s", name);
    if (adt_path_offset_trace(adt, path, p) < 0 || adt_get_reg(adt, p, "reg", 0, &base, &size) < 0)
        return;
    s2r_dcp[s2r_n_dcp] = (struct s2r_dcp){.mbox = mbox, .dart = base, .dart_size = size};
    printf("SPTM-virt[s2r]: dcp-pkt %d %s mbox 0x%lx dart 0x%lx\n", s2r_n_dcp, name, mbox, base);
    s2r_n_dcp++;
}
