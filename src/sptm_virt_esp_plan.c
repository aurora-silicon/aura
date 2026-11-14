/* SPDX-License-Identifier: MIT */
/*
 * The ESP macOS boot's handoff planner: a C port of plan_boot in
 * proxyclient/aura/sptm_virt_plan.py for the configuration boot_desktop.sh boots (native layout,
 * no /cpus pruning, no physBase gap). tests/esp_plan_host.c checks the two agree byte for byte
 * on a live J700 device tree. Keep them in step: a change to one is a change to both.
 */

#ifdef ESP_PLAN_HOST
#include <string.h>
#endif
#include "sptm_virt_esp_plan.h"
#ifndef ESP_PLAN_HOST
#include "string.h"
#endif

#define PAGE          0x4000UL
#define BLOCK         0x2000000UL
#define L2_INDEX_MASK ((1UL << 36) - 1)
#define MIN_SHIFT     0x2000000UL
#define ABSENT        0xffffffffffffffffUL

#define ESP_INLINE static inline __attribute__((always_inline))

ESP_INLINE u64 rp(u64 x)
{
    return (x + PAGE - 1) & ~(PAGE - 1);
}

#define FAIL(...)                                                                                  \
    do {                                                                                           \
        esp_fmt(err, errlen, __VA_ARGS__);                                                         \
        return -1;                                                                                 \
    } while (0)

/* A tiny formatter (no printf on the host-test side's terms): %s and %x only. */
AURA_XTEXT static void esp_fmt(char *out, u64 cap, const char *f, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    u64 n = 0;
    for (; *f && n + 1 < cap; f++) {
        if (*f != '%') {
            out[n++] = *f;
            continue;
        }
        f++;
        if (*f == 's') {
            for (const char *s = __builtin_va_arg(ap, const char *); *s && n + 1 < cap; s++)
                out[n++] = *s;
        } else if (*f == 'x') {
            u64 v = __builtin_va_arg(ap, u64);
            char d[17];
            int k = 0;
            do {
                d[k++] = "0123456789abcdef"[v & 15];
                v >>= 4;
            } while (v);
            if (n + 3 < cap) {
                out[n++] = '0';
                out[n++] = 'x';
            }
            while (k && n + 1 < cap)
                out[n++] = d[--k];
        } else if (!*f) {
            break;
        }
    }
    out[n] = 0;
    __builtin_va_end(ap);
}

/* ---- manifest ---- */

static const char *const esp_ids[ESP_NIMG] = {"sptm", "txm", "xnu", "tc", "cl4", "exic", "extc"};
static const char *const esp_cl4_names[4] = {"CL4-rx", "CL4-ro", "CL4-rw", "CL4-le"};
static const char *const esp_s2r_names[ESP_S2R_N] = {"dcp_pkt",  "cp_skip_ans", "cp_ansx",
                                                     "go_count", "wake_s",      "quiesce"};

AURA_XTEXT static int esp_id(const char *s)
{
    for (int i = 0; i < ESP_NIMG; i++)
        if (!strcmp(s, esp_ids[i]))
            return i;
    return -1;
}

AURA_XTEXT static bool esp_num(const char *s, u64 *v)
{
    u64 r = 0;
    int base = 10;
    if (!s || !*s)
        return false;
    if (s[0] == '0' && s[1] == 'x') {
        base = 16;
        s += 2;
        if (!*s)
            return false;
    }
    for (; *s; s++) {
        int d = (*s >= '0' && *s <= '9') ? *s - '0' : (*s >= 'a' && *s <= 'f') ? *s - 'a' + 10 : 99;
        if (d >= base)
            return false;
        r = r * base + d;
    }
    *v = r;
    return true;
}

AURA_XTEXT static bool esp_sha(const char *s, u8 out[32])
{
    if (!s || strlen(s) != 64)
        return false;
    for (int i = 0; i < 64; i++) {
        char c = s[i];
        int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
        if (d < 0)
            return false;
        out[i / 2] = (out[i / 2] << 4) | d;
    }
    return true;
}

AURA_XTEXT static void esp_copy(char *dst, const char *src, u64 cap)
{
    u64 i = 0;
    for (; src && src[i] && i + 1 < cap; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

AURA_XTEXT int esp_manifest_parse(struct esp_manifest *m, char *text, u64 len, char *err,
                                  u64 errlen)
{
    bool head = false, end = false;
    memset(m, 0, sizeof(*m));
    for (u64 at = 0; at < len && !end;) {
        char *line = text + at, *nl = line;
        while (nl < text + len && *nl != '\n')
            nl++;
        at = nl - text + 1;
        *nl = 0;
        if (!*line || *line == '#')
            continue;
        char *tok[8] = {0}, *rest = NULL;
        int nt = 0;
        for (char *c = line; *c && nt < 8;) {
            while (*c == ' ')
                *c++ = 0;
            if (!*c)
                break;
            if (nt == 1 && !strcmp(tok[0], "bootargs")) {
                rest = c;
                break;
            }
            tok[nt++] = c;
            while (*c && *c != ' ')
                c++;
        }
        if (!nt)
            continue;
        const char *k = tok[0];
        u64 v[6] = {0};
        int id = nt > 1 ? esp_id(tok[1]) : -1;
        if (!head) {
            if (strcmp(k, "aura-macos-esp") || nt < 2 || strcmp(tok[1], "2"))
                FAIL("manifest: not an aura-macos-esp 2 manifest");
            head = true;
        } else if (!strcmp(k, "build") && nt == 2) {
            esp_copy(m->build, tok[1], sizeof(m->build));
        } else if (!strcmp(k, "file") && id >= 0 && (nt == 5 || nt == 8)) {
            struct esp_file *f = &m->file[id];
            esp_copy(f->name, tok[2], sizeof(f->name));
            if (!esp_num(tok[3], &f->size) || !esp_sha(tok[4], f->sha))
                FAIL("manifest: bad file line for %s", tok[1]);
            f->raw_size = f->size;
            memcpy(f->raw_sha, f->sha, 32);
            if (nt == 8) {
                if (strcmp(tok[5], "gzip") || !esp_num(tok[6], &f->raw_size) ||
                    !esp_sha(tok[7], f->raw_sha))
                    FAIL("manifest: bad encoding for %s", tok[1]);
                f->gzip = true;
            }
            for (const char *c = f->name; *c; c++)
                if (*c == '/' || *c == ';' || *c == '\\')
                    FAIL("manifest: file %s is not a plain name", f->name);
            f->present = true;
        } else if (!strcmp(k, "image") && id >= 0 && id <= ESP_XNU && nt == 5) {
            struct esp_image *im = &m->img[id];
            if (!esp_num(tok[2], &im->virtlo) || !esp_num(tok[3], &im->virthi) ||
                !esp_num(tok[4], &im->entry))
                FAIL("manifest: bad image line");
        } else if (!strcmp(k, "seg") && id >= 0 && id <= ESP_XNU && nt == 7) {
            struct esp_image *im = &m->img[id];
            if (im->nseg >= ESP_MAX_SEGS)
                FAIL("manifest: too many segments");
            struct esp_seg *s = &im->seg[im->nseg++];
            esp_copy(s->name, tok[2], sizeof(s->name));
            if (!esp_num(tok[3], &s->vmaddr) || !esp_num(tok[4], &s->vmsize) ||
                !esp_num(tok[5], &s->fileoff) || !esp_num(tok[6], &s->filesize))
                FAIL("manifest: bad seg line");
        } else if (!strcmp(k, "cl4") && nt == 3) {
            if (!esp_num(tok[1], &m->cl4_entry) || !esp_num(tok[2], &m->cl4_virt))
                FAIL("manifest: bad cl4 line");
            m->cl4 = true;
        } else if (!strcmp(k, "cl4region") && nt == 4) {
            int r = -1;
            for (int i = 0; i < 4; i++)
                if (!strcmp(tok[1], esp_cl4_names[i]))
                    r = i;
            if (r < 0 || !esp_num(tok[2], &m->cl4r[r][0]) || !esp_num(tok[3], &m->cl4r[r][1]))
                FAIL("manifest: bad cl4region line");
        } else if (!strcmp(k, "patch") && (id == ESP_SPTM || id == ESP_XNU) && nt == 6) {
            if (m->npatch >= ESP_MAX_PATCHES)
                FAIL("manifest: too many patches");
            struct esp_patch *p = &m->patch[m->npatch++];
            p->img = id;
            if (!esp_num(tok[2], &p->off) || !esp_num(tok[3], &v[0]) || !esp_num(tok[4], &v[1]) ||
                (p->off & 3) || (v[0] >> 32) || (v[1] >> 32))
                FAIL("manifest: bad patch line");
            p->old = v[0];
            p->new = v[1];
            esp_copy(p->label, tok[5], sizeof(p->label));
        } else if (!strcmp(k, "hide") && nt == 2) {
            if (m->nhide >= ESP_MAX_HIDE || tok[1][0] != '/')
                FAIL("manifest: bad hide line");
            esp_copy(m->hide[m->nhide++], tok[1], ESP_PATH);
        } else if (!strcmp(k, "drop") && nt == 3) {
            if (m->ndrop >= ESP_MAX_DROP || tok[1][0] != '/')
                FAIL("manifest: bad drop line");
            esp_copy(m->drop[m->ndrop][0], tok[1], ESP_PATH);
            esp_copy(m->drop[m->ndrop++][1], tok[2], ESP_PATH);
        } else if (!strcmp(k, "serial") && nt == 2) {
            esp_copy(m->serial, tok[1], sizeof(m->serial));
        } else if (!strcmp(k, "trace") && nt == 2) {
            if (!esp_num(tok[1], &m->trace))
                FAIL("manifest: bad trace line");
        } else if (!strcmp(k, "s2r") && nt == 3) {
            int i = 0;
            while (i < ESP_S2R_N && strcmp(tok[1], esp_s2r_names[i]))
                i++;
            if (i == ESP_S2R_N || !esp_num(tok[2], &v[0]) || v[0] >> 32)
                FAIL("manifest: bad s2r line");
            m->s2r[i] = v[0];
            m->s2r_set[i] = true;
        } else if (!strcmp(k, "ctrr-stash") && nt == 2) {
            if (!esp_num(tok[1], &m->ctrr_stash))
                FAIL("manifest: bad ctrr-stash line");
        } else if (!strcmp(k, "link-phys") && nt >= 2 && nt <= 3) {
            for (int i = 1; i < nt; i++) {
                if (!esp_num(tok[i], &v[0]) || v[0] >> 32)
                    FAIL("manifest: bad link-phys line");
                m->link_phys[i - 1] = v[0];
            }
        } else if (!strcmp(k, "love") && nt == 2) {
            if (strlen(tok[1]) >= sizeof(m->love))
                FAIL("manifest: love too long");
            esp_copy(m->love, tok[1], sizeof(m->love));
        } else if (!strcmp(k, "video") && nt == 2) {
            m->video = !strcmp(tok[1], "1");
        } else if (!strcmp(k, "bootargs")) {
            if (rest && strlen(rest) >= sizeof(m->bootargs))
                FAIL("manifest: bootargs too long");
            esp_copy(m->bootargs, rest ? rest : "", sizeof(m->bootargs));
        } else if (!strcmp(k, "end") && nt == 1) {
            end = true;
        } else {
            FAIL("manifest: unknown line '%s'", k);
        }
    }
    if (!end)
        FAIL("manifest: no end line (truncated?)");
    for (int i = ESP_SPTM; i <= ESP_TC; i++)
        if (!m->file[i].present)
            FAIL("manifest: no %s file", esp_ids[i]);
    for (int i = ESP_SPTM; i <= ESP_XNU; i++)
        if (!m->img[i].nseg || !m->img[i].virthi)
            FAIL("manifest: no %s image", esp_ids[i]);
    if (m->cl4 &&
        (!m->file[ESP_CL4].present || !m->file[ESP_EXIC].present || !m->file[ESP_EXTC].present ||
         !m->cl4r[0][1] || !m->cl4r[1][1] || !m->cl4r[2][1] || !m->cl4r[3][1]))
        FAIL("manifest: cl4 needs the bundle, both exclave files and four regions");
    return 0;
}

/* ---- layout ---- */

AURA_XTEXT static const struct esp_seg *esp_seg(const struct esp_image *im, const char *name)
{
    for (u32 i = 0; i < im->nseg; i++)
        if (!strcmp(im->seg[i].name, name))
            return &im->seg[i];
    return NULL;
}

AURA_XTEXT static u64 esp_seglen(const struct esp_image *im, const char *name)
{
    const struct esp_seg *s = esp_seg(im, name);
    return s ? s->vmsize : 0;
}

/* The regions dict of plan_boot: first assignment fixes the order, later ones update. */
AURA_XTEXT static int esp_reg(struct esp_plan *p, const char *name, u64 pa, u64 size)
{
    for (u32 i = 0; i < p->nregion; i++)
        if (!strcmp(p->region[i].name, name)) {
            p->region[i].pa = pa;
            p->region[i].size = size;
            return 0;
        }
    if (p->nregion >= ESP_MAX_REGIONS)
        return -1;
    esp_copy(p->region[p->nregion].name, name, sizeof(p->region[0].name));
    p->region[p->nregion].pa = pa;
    p->region[p->nregion++].size = size;
    return 0;
}

AURA_XTEXT static int esp_load(struct esp_plan *p, const char *what, u64 pa, int img, u64 off,
                               u64 size, u64 total)
{
    if (p->nload >= ESP_MAX_LOADS)
        return -1;
    struct esp_load *l = &p->load[p->nload++];
    memset(l, 0, sizeof(*l));
    esp_copy(l->what, what, sizeof(l->what));
    l->pa = pa;
    l->img = img;
    l->fileoff = off;
    l->size = size;
    l->total = total;
    return 0;
}

struct esp_cursor {
    u64 head, tail;
};

AURA_XTEXT static int esp_push(struct esp_plan *p, struct esp_cursor *c,
                               const struct esp_manifest *m, int img, const char *seg)
{
    const struct esp_seg *s = esp_seg(&m->img[img], seg);
    if (!s)
        return 0; /* plan_boot: absent, zero length */
    if (esp_load(p, seg, c->head, img, s->fileoff, s->filesize, s->vmsize) < 0)
        return -1;
    c->head += s->vmsize;
    return 0;
}

AURA_XTEXT static int esp_end(struct esp_plan *p, struct esp_cursor *c, const char *name)
{
    if (c->head % PAGE)
        return -1;
    if (esp_reg(p, name, c->tail, c->head - c->tail) < 0)
        return -1;
    c->tail = c->head;
    return 0;
}

/* ---- device tree ---- */

struct aw {
    const struct esp_plan_in *in;
    const struct esp_plan *pl;
    const u8 *a;
    u32 alen;
    u8 *o;
    u64 cap, n;
    bool bad;
    bool hide_atc[2];
    bool have_mm, have_chosen, have_carve, have24;
    bool chosen_has[3]; /* hibernation, exclave-memory-map, exclave-misc */
    u8 ph[4];
    bool have_ph;
    u64 old24[2];
    u64 used[8]; /* region-id-N in use, N < 512 */
    u64 oslog_base, rtb_dst, rtb_size;
    bool oslog;
    char err[96];
};

ESP_INLINE u32 rd32(const u8 *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24;
}

ESP_INLINE u64 rd64(const u8 *p)
{
    return rd32(p) | (u64)rd32(p + 4) << 32;
}

ESP_INLINE void wr64(u8 *p, u64 v)
{
    for (int i = 0; i < 8; i++)
        p[i] = v >> (8 * i);
}

/* Offset just past the node at off, or 0 if the tree is malformed. */
AURA_XTEXT static u32 adt_end(struct aw *w, u32 off, int depth)
{
    if (depth > 16 || off + 8 > w->alen)
        return 0;
    u32 np = rd32(w->a + off), nk = rd32(w->a + off + 4);
    off += 8;
    for (u32 i = 0; i < np; i++) {
        if (off + 36 > w->alen)
            return 0;
        u32 len = rd32(w->a + off + 32) & 0x7fffffff;
        off += 36 + ((len + 3) & ~3);
        if (off > w->alen)
            return 0;
    }
    for (u32 i = 0; i < nk && off; i++)
        off = adt_end(w, off, depth + 1);
    return off;
}

/* The node's name property, as plan_boot's RawNode.name() reads it. */
AURA_XTEXT static void adt_name(struct aw *w, u32 off, char *out, u32 cap)
{
    u32 np = rd32(w->a + off);
    out[0] = 0;
    off += 8;
    for (u32 i = 0; i < np; i++) {
        u32 len = rd32(w->a + off + 32) & 0x7fffffff;
        if (!strncmp((const char *)w->a + off, "name", 32)) {
            u32 k = 0;
            for (; k < len && k + 1 < cap && w->a[off + 36 + k]; k++)
                out[k] = w->a[off + 36 + k];
            out[k] = 0;
            return;
        }
        off += 36 + ((len + 3) & ~3);
    }
}

AURA_XTEXT static bool aw_hidden(struct aw *w, const char *path)
{
    const struct esp_manifest *m = w->in->m;
    for (u32 i = 0; i < m->nhide; i++)
        if (!strcmp(m->hide[i], path))
            return true;
    return false;
}

AURA_XTEXT static bool aw_dropped(struct aw *w, const char *path, const char *prop)
{
    const struct esp_manifest *m = w->in->m;
    for (u32 i = 0; i < m->ndrop; i++)
        if (!strcmp(m->drop[i][0], path) && !strcmp(m->drop[i][1], prop))
            return true;
    return false;
}

AURA_XTEXT static int rid_of(const char *name)
{
    if (strncmp(name, "region-id-", 10) || !name[10])
        return -1;
    int v = 0;
    for (const char *c = name + 10; *c; c++) {
        if (*c < '0' || *c > '9' || v > 1000)
            return -1;
        v = v * 10 + (*c - '0');
    }
    return v;
}

AURA_XTEXT static int rid_free(struct aw *w)
{
    for (int i = 26; i < 512; i++)
        if (!(w->used[i / 64] & (1UL << (i % 64)))) {
            w->used[i / 64] |= 1UL << (i % 64);
            return i;
        }
    return -1;
}

/* Pass 1: what the edits depend on (hidden subtrees excluded, as plan_boot removes them first). */
AURA_XTEXT static int aw_scan(struct aw *w, u32 off, char *path, u32 plen, int depth)
{
    u32 np = rd32(w->a + off), nk = rd32(w->a + off + 4);
    u32 p = off + 8;
    bool carve = !strcmp(path, "/chosen/carveout-memory-map");
    w->have_mm |= !strcmp(path, "/chosen/memory-map");
    w->have_chosen |= !strcmp(path, "/chosen");
    w->have_carve |= carve;
    for (u32 i = 0; i < np; i++) {
        const char *name = (const char *)w->a + p;
        u32 len = rd32(w->a + p + 32) & 0x7fffffff;
        const u8 *v = w->a + p + 36;
        if (w->in->m->serial[0] && !strcmp(path, w->in->m->serial) &&
            !strncmp(name, "AAPL,phandle", 32) && len == 4) {
            memcpy(w->ph, v, 4);
            w->have_ph = true;
        }
        if (carve) {
            char nm[33] = {0};
            memcpy(nm, name, 32);
            int r = rid_of(nm);
            if (r >= 0 && r < 512)
                w->used[r / 64] |= 1UL << (r % 64);
            if (r == 24 && !w->have24) {
                if (len != 16)
                    return -1;
                w->old24[0] = rd64(v);
                w->old24[1] = rd64(v + 8);
                w->have24 = true;
            }
        }
        if (w->rtb_size && !strncmp(name, "segment-ranges", 32) && len % 32 == 0)
            for (u32 e = 0; e < len; e += 32) {
                u64 ph = rd64(v + e);
                if (rd32(v + e + 28) == 0xa && ph < w->in->phys_base &&
                    (!w->oslog || ph < w->oslog_base)) {
                    w->oslog_base = ph;
                    w->oslog = true;
                }
            }
        p += 36 + ((len + 3) & ~3);
    }
    for (u32 i = 0; i < nk; i++) {
        u32 end = adt_end(w, p, depth + 1);
        if (!end)
            return -1;
        char nm[64];
        adt_name(w, p, nm, sizeof(nm));
        u32 l = strlen(nm);
        if (plen + 1 + l + 1 > 256)
            return -1;
        path[plen] = '/';
        memcpy(path + plen + 1, nm, l + 1);
        if (!aw_hidden(w, path)) {
            if (!strcmp(path, "/chosen/hibernation"))
                w->chosen_has[0] = true;
            if (!strcmp(path, "/chosen/exclave-memory-map"))
                w->chosen_has[1] = true;
            if (!strcmp(path, "/chosen/exclave-misc"))
                w->chosen_has[2] = true;
            if (aw_scan(w, p, path, plen + 1 + l, depth + 1) < 0)
                return -1;
        }
        path[plen] = 0;
        p = end;
    }
    return 0;
}

AURA_XTEXT static void aw_emit(struct aw *w, const void *src, u64 len)
{
    if (w->n + len > w->cap) {
        w->bad = true;
        return;
    }
    memcpy(w->o + w->n, src, len);
    w->n += len;
}

AURA_XTEXT static void aw_prop(struct aw *w, const char *name, const void *val, u32 len, u32 flags)
{
    u8 nm[32] = {0}, pad[4] = {0};
    u32 lf = len | flags << 31;
    for (int i = 0; i < 32 && name[i]; i++)
        nm[i] = name[i];
    aw_emit(w, nm, 32);
    aw_emit(w, &lf, 4);
    aw_emit(w, val, len);
    aw_emit(w, pad, (4 - len % 4) % 4);
}

AURA_XTEXT static void aw_region(struct aw *w, const char *name, u64 pa, u64 size)
{
    u8 v[16];
    wr64(v, pa);
    wr64(v + 8, size);
    aw_prop(w, name, v, 16, 0);
}

AURA_XTEXT static void aw_rid(struct aw *w, int rid, u64 pa, u64 size)
{
    char nm[24] = "region-id-";
    int k = 10, d = 100;
    for (bool lead = true; d; d /= 10) {
        int digit = rid / d % 10;
        if (lead && !digit && d > 1)
            continue;
        lead = false;
        nm[k++] = '0' + digit;
    }
    nm[k] = 0;
    aw_region(w, nm, pa, size);
}

static const u8 esp_hib_keys[4] = {0x5a, 0xa5, 0x3c, 0xc3};
static const char *const esp_hib_names[4] = {"key-sptm-ctrr", "key-xnu-ctrr", "key-exclave",
                                             "key-exclave-entropy"};

AURA_XTEXT static void aw_hdr(struct aw *w, u32 np, u32 nk)
{
    aw_emit(w, &np, 4);
    aw_emit(w, &nk, 4);
}

/* /chosen's injected children, as plan_boot appends them. */
AURA_XTEXT static u32 aw_chosen_kids(struct aw *w)
{
    u32 n = 0;
    if (!w->chosen_has[0]) {
        aw_hdr(w, 5, 0);
        aw_prop(w, "name", "hibernation", 12, 0);
        for (int i = 0; i < 4; i++) {
            u8 key[48];
            memset(key, esp_hib_keys[i], sizeof(key));
            aw_prop(w, esp_hib_names[i], key, i < 2 ? 48 : 32, 0);
        }
        n++;
    }
    const struct esp_plan *pl = w->pl;
    const struct esp_region *ic = NULL, *tc = NULL;
    for (u32 i = 0; i < pl->nregion; i++) {
        if (!strcmp(pl->region[i].name, "ExclaveOSIntegrityCatalog"))
            ic = &pl->region[i];
        if (!strcmp(pl->region[i].name, "ExclaveOSTrustCache"))
            tc = &pl->region[i];
    }
    if (w->in->m->cl4 && ic && tc) {
        if (!w->chosen_has[1]) {
            aw_hdr(w, 1, 2);
            aw_prop(w, "name", "exclave-memory-map", 19, 0);
            const struct esp_region *r[2] = {ic, tc};
            for (int i = 0; i < 2; i++) {
                u32 flags = 0;
                aw_hdr(w, 3, 0);
                aw_prop(w, "name", r[i]->name, strlen(r[i]->name) + 1, 0);
                aw_region(w, "range", r[i]->pa, r[i]->size);
                aw_prop(w, "flags", &flags, 4, 0);
            }
            n++;
        }
        if (!w->chosen_has[2]) {
            aw_hdr(w, 2, 0);
            aw_prop(w, "name", "exclave-misc", 13, 0);
            aw_prop(w, "bootargstring", "", 1, 0);
            n++;
        }
    }
    return n;
}

static u8 aw_val[0x800]; /* one rewritten property value */

/* Pass 2: copy the node at off with every edit applied. Returns the offset past it. */
AURA_XTEXT static u32 aw_node(struct aw *w, u32 off, char *path, u32 plen, int depth)
{
    const struct esp_plan_in *in = w->in;
    const struct esp_plan *pl = w->pl;
    u32 np = rd32(w->a + off), nk = rd32(w->a + off + 4);
    u64 hdr = w->n;
    u32 onp = 0, onk = 0;
    bool mm = !strcmp(path, "/chosen/memory-map");
    bool carve = !strcmp(path, "/chosen/carveout-memory-map");
    bool xbar = !strncmp(path, "/arm-io/display-crossbar", 24) && !strchr(path + 8, '/');
    bool seen[ESP_MAX_REGIONS] = {0}, seen24 = false;
    bool mp = in->m->love[0] && !strcmp(path, "/chosen/manifest-properties"), love = false;

    aw_hdr(w, 0, 0);
    u32 p = off + 8;
    for (u32 i = 0; i < np; i++) {
        char name[33] = {0};
        memcpy(name, w->a + p, 32);
        u32 lf = rd32(w->a + p + 32), len = lf & 0x7fffffff, fl = lf >> 31;
        const u8 *v = w->a + p + 36;
        p += 36 + ((len + 3) & ~3);
        if (aw_dropped(w, path, name))
            continue;
        onp++;
        if (mm) {
            int r = -1;
            for (u32 k = 0; k < pl->nregion; k++)
                if (!strcmp(pl->region[k].name, name))
                    r = k;
            if (r >= 0) {
                if (len != 16) {
                    esp_fmt(w->err, sizeof(w->err), "memory-map %s is not 16 bytes", name);
                    w->bad = true;
                }
                seen[r] = true;
                aw_region(w, name, pl->region[r].pa, pl->region[r].size);
                continue;
            }
            if (in->ncopies && !strcmp(name, "Kernel-mach_header") && len == 16) {
                aw_region(w, name, ABSENT, ABSENT);
                continue;
            }
        }
        if (carve && !strcmp(name, "region-id-24")) {
            seen24 = true;
            aw_region(w, name, in->phys_base, in->mem_size);
            continue;
        }
        if (mp && !strcmp(name, "love")) {
            /* The guest's own OS long version, as plan_boot sets it: the same, NUL-terminated */
            u32 n = strlen(in->m->love), k = len;
            while (k && !v[k - 1])
                k--;
            if (k != n || memcmp(v, in->m->love, n)) {
                esp_fmt(w->err, sizeof(w->err),
                        "the live OS long version differs from the guest's");
                w->bad = true;
            }
            aw_prop(w, name, in->m->love, n + 1, fl);
            love = true;
            continue;
        }
        if (w->have_ph && !strcmp(path, "/defaults") && !strcmp(name, "serial-device")) {
            aw_prop(w, name, w->ph, 4, fl);
            continue;
        }
        bool big = len >= sizeof(aw_val);
        if (xbar && !strcmp(name, "dfp-endpoints") && (w->hide_atc[0] || w->hide_atc[1]) && big)
            w->bad = true;
        if (w->oslog && !strcmp(name, "segment-ranges") && len % 32 == 0 && big)
            w->bad = true;
        if (xbar && !strcmp(name, "dfp-endpoints") && (w->hide_atc[0] || w->hide_atc[1]) && !big) {
            u32 n = 0, kept = 0, all = 0;
            for (u32 s = 0; s < len;) {
                u32 e = s;
                while (e < len && v[e])
                    e++;
                if (e > s) {
                    all++;
                    bool atc = e - s == 4 && !memcmp(v + s, "atc", 3) &&
                               (v[s + 3] == '0' || v[s + 3] == '1') && w->hide_atc[v[s + 3] - '0'];
                    if (!atc) {
                        if (kept++)
                            aw_val[n++] = 0;
                        memcpy(aw_val + n, v + s, e - s);
                        n += e - s;
                    }
                }
                s = e + 1;
            }
            if (kept != all) {
                aw_val[n++] = 0;
                aw_prop(w, name, aw_val, n, fl);
                continue;
            }
        }
        if (w->oslog && !strcmp(name, "segment-ranges") && len % 32 == 0 && !big) {
            memcpy(aw_val, v, len);
            for (u32 e = 0; e < len; e += 32) {
                u64 ph = rd64(aw_val + e), sz = rd32(aw_val + e + 24);
                if (rd32(aw_val + e + 28) == 0xa && w->oslog_base <= ph &&
                    ph + sz <= w->oslog_base + w->rtb_size)
                    wr64(aw_val + e, w->rtb_dst + (ph - w->oslog_base));
            }
            aw_prop(w, name, aw_val, len, fl);
            continue;
        }
        aw_prop(w, name, v, len, fl);
    }
    if (mm)
        for (u32 k = 0; k < pl->nregion; k++)
            if (!seen[k]) {
                aw_region(w, pl->region[k].name, pl->region[k].pa, pl->region[k].size);
                onp++;
            }
    if (mp && !love) {
        aw_prop(w, "love", in->m->love, strlen(in->m->love) + 1, 0);
        onp++;
    }
    if (carve) {
        if (!seen24) {
            aw_region(w, "region-id-24", in->phys_base, in->mem_size);
            onp++;
        }
        u64 aura_base = in->phys_base + in->mem_size, aura_end = w->old24[0] + w->old24[1];
        if (w->have24 && aura_end > aura_base) {
            aw_rid(w, rid_free(w), aura_base, aura_end - aura_base);
            onp++;
        }
        for (u32 r = 0; r < in->nreserved; r++) {
            aw_rid(w, rid_free(w), in->reserved[r][0], in->reserved[r][1]);
            onp++;
        }
    }
    for (u32 i = 0; i < nk; i++) {
        u32 end = adt_end(w, p, depth + 1);
        char nm[64];
        adt_name(w, p, nm, sizeof(nm));
        u32 l = strlen(nm);
        path[plen] = '/';
        memcpy(path + plen + 1, nm, l + 1);
        if (!aw_hidden(w, path)) {
            aw_node(w, p, path, plen + 1 + l, depth + 1);
            onk++;
        }
        path[plen] = 0;
        p = end;
    }
    if (!strcmp(path, "/chosen"))
        onk += aw_chosen_kids(w);
    if (hdr + 8 <= w->cap) {
        memcpy(w->o + hdr, &onp, 4);
        memcpy(w->o + hdr + 4, &onk, 4);
    }
    return p;
}

/* ---- the plan ---- */

AURA_XTEXT int esp_plan(const struct esp_plan_in *in, struct esp_plan *out, u8 *adt_out,
                        u64 adt_cap, char *err, u64 errlen)
{
    const struct esp_manifest *m = in->m;
    const struct esp_image *si = &m->img[ESP_SPTM], *ti = &m->img[ESP_TXM], *xi = &m->img[ESP_XNU];
    struct esp_cursor c = {in->dram_base, in->dram_base};
    memset(out, 0, sizeof(*out));

    u64 tc_len = in->file_len[ESP_TC] + (in->tc_container ? 0 : 8);
    u64 tc_size = rp(tc_len);
    u64 adt_space = rp(in->adt_len + 0x2000);
    u64 cl4_total = m->cl4 ? rp(m->cl4r[0][1]) + rp(m->cl4r[1][1]) : 0;
    u64 before = esp_seglen(ti, "__TEXT") + esp_seglen(ti, "__DATA_CONST") +
                 esp_seglen(ti, "__TEXT_EXEC") + esp_seglen(ti, "__TEXT_BOOT_EXEC") +
                 esp_seglen(xi, "__TEXT_EXEC") + esp_seglen(xi, "__TEXT_BOOT_EXEC") +
                 esp_seglen(xi, "__TEXT") + esp_seglen(xi, "__PRELINK_TEXT") +
                 esp_seglen(xi, "__DATA_CONST") + esp_seglen(xi, "__DATA_SPTM") + cl4_total +
                 adt_space + tc_size + PAGE;
    if ((xi->virtlo & L2_INDEX_MASK) <= before)
        FAIL("plan: the BootKC L2 index leaves no room below SPTM");
    c.head += (xi->virtlo & L2_INDEX_MASK) - before;
    c.tail = c.head;
    u64 first_mapped = (in->phys_base + BLOCK) & ~(BLOCK - 1);
    if (in->phys_base > c.head) {
        u64 shift = rp(first_mapped - c.head);
        c.head += shift > MIN_SHIFT ? shift : MIN_SHIFT;
        c.tail = c.head;
    }
    if (c.head < first_mapped)
        FAIL("plan: blob would start at %x, below SPTM's first block %x", c.head, first_mapped);

    int bad = 0;
    bad |= esp_push(out, &c, m, ESP_TXM, "__TEXT") | esp_push(out, &c, m, ESP_TXM, "__DATA_CONST") |
           esp_end(out, &c, "TXM-ro");
    out->txm_pa = out->region[0].pa;
    bad |= esp_push(out, &c, m, ESP_TXM, "__TEXT_EXEC") | esp_end(out, &c, "TXM-rx");
    bad |= esp_push(out, &c, m, ESP_TXM, "__TEXT_BOOT_EXEC") | esp_end(out, &c, "TXM-bx");
    bad |= esp_load(out, "TrustCache", c.head, ESP_TC, 0, in->file_len[ESP_TC], tc_size);
    out->load[out->nload - 1].tc_header = !in->tc_container;
    c.head += tc_size;
    bad |= esp_end(out, &c, "TrustCache");
    bad |= esp_push(out, &c, m, ESP_XNU, "__TEXT_EXEC") | esp_end(out, &c, "BootKC-rx");
    bad |= esp_push(out, &c, m, ESP_XNU, "__TEXT_BOOT_EXEC") | esp_end(out, &c, "BootKC-bx");
    bad |= esp_push(out, &c, m, ESP_XNU, "__TEXT") |
           esp_push(out, &c, m, ESP_XNU, "__PRELINK_TEXT") |
           esp_push(out, &c, m, ESP_XNU, "__DATA_CONST") | esp_end(out, &c, "BootKC-ro");
    bad |= esp_push(out, &c, m, ESP_XNU, "__DATA_SPTM") | esp_end(out, &c, "BootKC-rs");

    u64 cl4pa[4] = {0}, cl4sz[4] = {0};
    for (int r = 0; m->cl4 && r < 2; r++) {
        bad |= esp_load(out, esp_cl4_names[r], c.head, ESP_CL4, m->cl4r[r][0], m->cl4r[r][1],
                        rp(m->cl4r[r][1]));
        cl4pa[r] = c.head;
        cl4sz[r] = m->cl4r[r][1];
        c.head += rp(m->cl4r[r][1]);
        c.tail = c.head;
    }

    out->adt_pa = c.head;
    out->adt_space = adt_space + PAGE;
    c.head += adt_space + PAGE;
    bad |= esp_end(out, &c, "DeviceTree");

    out->sptm_pa = c.head;
    bad |=
        esp_load(out, "SPTM", c.head, ESP_SPTM, 0, in->file_len[ESP_SPTM], si->virthi - si->virtlo);
    static const char *const sl[4][3] = {{"__TEXT", "__DATA_CONST", "__LATE_CONST"},
                                         {"__TEXT_EXEC", "__LAST", NULL},
                                         {"__DATA", "__BOOTDATA", NULL},
                                         {"__LINKEDIT", NULL, NULL}};
    static const char *const sn[4] = {"SPTM-ro", "SPTM-rx", "SPTM-rw", "SPTM-le"};
    for (int r = 0; r < 4; r++) {
        const struct esp_seg *first = esp_seg(si, sl[r][0]);
        u64 size = 0;
        for (int k = 0; k < 3 && sl[r][k]; k++) {
            const struct esp_seg *s = esp_seg(si, sl[r][k]);
            if (!s)
                FAIL("plan: SPTM has no %s", sl[r][k]);
            size += s->vmsize;
        }
        bad |= esp_reg(out, sn[r], out->sptm_pa + (first->vmaddr - si->virtlo), size);
    }
    bad |= esp_reg(out, "SPTM-rm", out->region[out->nregion - 3].pa, 0);
    c.head += si->virthi - si->virtlo;
    c.tail = c.head;

    bad |= esp_push(out, &c, m, ESP_TXM, "__DATA") | esp_end(out, &c, "TXM-rw");
    bad |= esp_push(out, &c, m, ESP_TXM, "__LINKEDIT") | esp_end(out, &c, "TXM-le");
    bad |= esp_push(out, &c, m, ESP_XNU, "__PRELINK_INFO") |
           esp_push(out, &c, m, ESP_XNU, "__DATA") | esp_end(out, &c, "BootKC-rw");
    bad |= esp_push(out, &c, m, ESP_XNU, "__LINKEDIT") | esp_end(out, &c, "BootKC-le");
    if (m->cl4) {
        for (int r = 2; r < 4; r++) {
            bad |= esp_load(out, esp_cl4_names[r], c.head, ESP_CL4, m->cl4r[r][0], m->cl4r[r][1],
                            rp(m->cl4r[r][1]));
            cl4pa[r] = c.head;
            cl4sz[r] = m->cl4r[r][1];
            c.head += rp(m->cl4r[r][1]);
            c.tail = c.head;
        }
        if (c.head != cl4pa[2] + (m->cl4r[1][0] - m->cl4r[2][0]))
            FAIL("plan: CL4-rw/CL4-le are not linear by bundle offset");
        bad |= esp_load(out, "CL4-ro (data-window copy)", c.head, ESP_CL4, m->cl4r[1][0],
                        m->cl4r[1][1], rp(m->cl4r[1][1]));
        cl4sz[3] += rp(m->cl4r[1][1]);
        c.head += rp(m->cl4r[1][1]);
        c.tail = c.head;
    }
    c.head += PAGE;
    bad |= esp_end(out, &c, "CL4-dummypage");
    out->boot_args_pa = c.head;
    c.head += rp(0x488);
    bad |= esp_end(out, &c, "BootArgs");
    u64 rd_pa = c.head;
    bad |= esp_end(out, &c, "RAMDisk");
    u64 xpa[2] = {0};
    for (int r = 0; m->cl4 && r < 2; r++) {
        int img = r ? ESP_EXTC : ESP_EXIC;
        if (!in->file_len[img])
            continue;
        bad |= esp_load(out, r ? "ExclaveOSTrustCache" : "ExclaveOSIntegrityCatalog", c.head, img,
                        0, in->file_len[img], rp(in->file_len[img]));
        xpa[r] = c.head;
        c.head += rp(in->file_len[img]);
    }
    c.tail = c.head;
    struct aw *w = NULL;
    u64 rtb_dst = 0, rtb_size = 0;
    for (u32 i = 0; i < in->ncopies; i++) {
        bad |= esp_load(out, in->copies[i].name, c.head, ESP_NIMG, in->copies[i].pa,
                        in->copies[i].size, rp(in->copies[i].size));
        bad |= esp_reg(out, in->copies[i].name, c.head, in->copies[i].size);
        if (!strcmp(in->copies[i].name, "RTBuddySeg")) {
            rtb_dst = c.head;
            rtb_size = in->copies[i].size;
        }
        c.head += rp(in->copies[i].size);
    }
    c.tail = c.head;
    out->blob_end = c.head;

    bad |= esp_reg(out, "DeviceTree", out->adt_pa, adt_space + PAGE);
    bad |= esp_reg(out, "RAMDisk", rd_pa, 0);
    bad |= esp_reg(out, "TXM-virt", ti->virtlo, 0) | esp_reg(out, "TXM-entry", ti->entry, 0);
    bad |= esp_reg(out, "BootKC-virt", xi->virtlo, 0) | esp_reg(out, "BootKC-entry", xi->entry, 0);
    bad |= esp_reg(out, "SPTM-virt", si->virtlo, 0) | esp_reg(out, "SPTM-entry", si->entry, 0);
    bad |= esp_reg(out, "slide", 0, 0);
    if (m->cl4) {
        bad |=
            esp_reg(out, "CL4-entry", m->cl4_entry, 0) | esp_reg(out, "CL4-virt", m->cl4_virt, 0);
        bad |=
            esp_reg(out, "CL4-rx", cl4pa[0], cl4sz[0]) | esp_reg(out, "CL4-rw", cl4pa[2], cl4sz[2]);
        bad |=
            esp_reg(out, "CL4-le", cl4pa[3], cl4sz[3]) | esp_reg(out, "CL4-ro", cl4pa[1], cl4sz[1]);
        if (xpa[0])
            bad |= esp_reg(out, "ExclaveOSIntegrityCatalog", xpa[0], in->file_len[ESP_EXIC]);
        if (xpa[1])
            bad |= esp_reg(out, "ExclaveOSTrustCache", xpa[1], in->file_len[ESP_EXTC]);
    } else {
        bad |= esp_reg(out, "CL4-entry", 0, 0) | esp_reg(out, "CL4-virt", 0, 0);
        bad |= esp_reg(out, "CL4-rx", ABSENT, ABSENT) | esp_reg(out, "CL4-ro", ABSENT, ABSENT);
    }
    if (bad)
        FAIL("plan: a segment or table overflowed");

    /* The device tree */
    static struct aw aw;
    w = &aw;
    memset(w, 0, sizeof(*w));
    w->in = in;
    w->pl = out;
    w->a = in->adt;
    w->alen = in->adt_len;
    w->o = adt_out;
    w->cap = adt_cap;
    w->rtb_dst = rtb_dst;
    w->rtb_size = rtb_size;
    for (u32 i = 0; i < m->nhide; i++) {
        const char *leaf = strchr(m->hide[i], 0);
        while (leaf > m->hide[i] && leaf[-1] != '/')
            leaf--;
        for (int k = 0; k < 2; k++)
            if ((!strncmp(leaf, "atc-phy", 7) && leaf[7] == '0' + k && !leaf[8]) ||
                (!strncmp(leaf, "atc", 3) && leaf[3] == '0' + k && leaf[4] == '-'))
                w->hide_atc[k] = true;
    }
    char path[256] = {0};
    if (!adt_end(w, 0, 0) || aw_scan(w, 0, path, 0, 0) < 0)
        FAIL("adt: malformed tree or carveout region-id-24");
    if (!w->have_mm || !w->have_chosen)
        FAIL("adt: no /chosen/memory-map");
    aw_node(w, 0, path, 0, 0);
    if (w->bad)
        FAIL("adt: %s", w->err[0] ? w->err : "edited tree outgrew its space");
    u64 len = rp(w->n);
    if (len > adt_space || len > adt_cap)
        FAIL("adt: edited tree outgrew its reserved space");
    memset(adt_out + w->n, 0, len - w->n);
    out->adt_len = len;

    /* Virtual placement and boot_args (plan_boot's build_boot_args) */
    out->virt_base = (xi->virtlo & ~L2_INDEX_MASK) + (in->phys_base - in->dram_base);
    u64 adt_va = out->adt_pa - in->phys_base + out->virt_base;
    if (!(in->phys_base <= out->txm_pa && out->blob_end <= in->phys_base + in->mem_size))
        FAIL("plan: the blob does not fit managed RAM");
    out->sptm_entry_pa = out->sptm_pa + (si->entry - si->virtlo);
    u8 *b = out->boot_args;
    memset(b, 0, sizeof(out->boot_args));
    b[0] = 2;
    b[2] = 2;
    wr64(b + 0x08, out->virt_base);
    wr64(b + 0x10, in->phys_base);
    wr64(b + 0x18, in->mem_size);
    wr64(b + 0x20, out->blob_end);
    for (int i = 0; m->video && i < 6; i++)
        wr64(b + 0x28 + 8 * i, in->video[i]);
    wr64(b + 0x60, adt_va);
    u32 alen32 = len;
    memcpy(b + 0x68, &alen32, 4);
    u64 cl = strlen(m->bootargs);
    if (cl >= 1024)
        FAIL("plan: boot-args too long");
    memcpy(b + 0x6c, m->bootargs, cl);
    wr64(b + 0x478, in->mem_size_actual);
    return 0;
}
