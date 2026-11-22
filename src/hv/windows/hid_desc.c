/* SPDX-License-Identifier: GPL-2.0-only */

/*
 * HID report descriptor walker.  See hid_desc.h.  Written on this branch from the HID 1.11
 * specification (section 6.2.2); no third-party source.
 */

#include "hid_desc.h"

struct hid_global {
    u16 page;
    s32 lmin;
    u32 lmax_raw;
    u8 lmax_size;
    u32 size, count;
    u8 id;
};

struct hid_local {
    u8 n;
    u32 usages[HID_MAX_USAGES];
    bool usage_full[HID_MAX_USAGES]; /* given with 4 bytes: carries its own page */
    bool has_min, has_max, min_full, max_full;
    u32 umin, umax;
};

static void zero(void *p, u32 n)
{
    u8 *b = p;
    for (u32 i = 0; i < n; i++)
        b[i] = 0;
}

static u32 item_u(const u8 *p, u32 n)
{
    u32 v = 0;
    for (u32 i = 0; i < n; i++)
        v |= (u32)p[i] << (8 * i);
    return v;
}

static s32 item_s(const u8 *p, u32 n)
{
    u32 v = item_u(p, n);
    if (n == 1)
        return (s8)v;
    if (n == 2)
        return (s16)v;
    return (s32)v;
}

static u32 full_usage(u16 page, u32 u, bool full)
{
    return full ? u : ((u32)page << 16 | (u & 0xffff));
}

/* Logical Maximum: unsigned when the minimum is not negative (the common reading). */
static s32 lmax_of(const struct hid_global *g)
{
    if (g->lmin >= 0 || g->lmax_size == 4)
        return (s32)g->lmax_raw;
    if (g->lmax_size == 1)
        return (s8)g->lmax_raw;
    if (g->lmax_size == 2)
        return (s16)g->lmax_raw;
    return 0;
}

static int input_item(struct hid_desc *d, const struct hid_global *g, const struct hid_local *l,
                      u32 flags, bool *seen_unnumbered)
{
    if (!g->size || g->size > 32)
        return HID_E_FIELD;
    if (g->count > HID_MAX_REPORT * 8)
        return HID_E_FIELD;
    u32 bits = g->size * g->count;
    if (d->input_bits[g->id] + bits > HID_MAX_REPORT * 8)
        return HID_E_FIELD;
    if (!g->id)
        *seen_unnumbered = true;
    if (d->uses_ids && !g->id)
        return HID_E_ID;
    if (!bits)
        return HID_OK;
    if (d->n_fields >= HID_MAX_FIELDS)
        return HID_E_FIELDS;
    struct hid_field *f = &d->f[d->n_fields++];
    zero(f, sizeof(*f));
    f->report_id = g->id;
    f->bit = d->input_bits[g->id];
    f->size = (u8)g->size;
    f->count = (u16)g->count;
    f->constant = flags & 1;
    f->variable = flags & 2;
    f->lmin = g->lmin;
    f->lmax = lmax_of(g);
    f->n_usages = l->n;
    for (u32 i = 0; i < l->n; i++)
        f->usages[i] = full_usage(g->page, l->usages[i], l->usage_full[i]);
    if (l->has_min && l->has_max) {
        f->has_range = true;
        f->umin = full_usage(g->page, l->umin, l->min_full);
        f->umax = full_usage(g->page, l->umax, l->max_full);
    }
    d->input_bits[g->id] += (u16)bits;
    return HID_OK;
}

int hid_desc_parse(struct hid_desc *d, const u8 *p, u32 len)
{
    zero(d, sizeof(*d));
    if (!len || len > HID_DESC_MAX)
        return HID_E_LENGTH;
    struct hid_global g, stack[HID_STACK_DEPTH];
    struct hid_local l;
    u32 depth = 0, collections = 0;
    bool unnumbered = false;
    zero(&g, sizeof(g));
    zero(&l, sizeof(l));
    for (u32 at = 0; at < len;) {
        u8 prefix = p[at];
        if (prefix == 0xfe) { /* long item: size, tag, data */
            if (at + 3 > len || at + 3 + p[at + 1] > len)
                return HID_E_LENGTH;
            at += 3 + p[at + 1];
            d->skipped_long++;
            continue;
        }
        u32 n = prefix & 3;
        if (n == 3)
            n = 4;
        if (at + 1 + n > len)
            return HID_E_LENGTH;
        const u8 *data = p + at + 1;
        u8 type = (prefix >> 2) & 3, tag = prefix >> 4;
        at += 1 + n;
        if (type == 0) { /* main */
            int rc = HID_OK;
            switch (tag) {
                case 0x8: /* Input */
                    rc = input_item(d, &g, &l, item_u(data, n), &unnumbered);
                    break;
                case 0xa: /* Collection */
                    if (++collections > 16)
                        return HID_E_STACK;
                    break;
                case 0xc: /* End Collection */
                    if (!collections)
                        return HID_E_STACK;
                    collections--;
                    break;
                default: /* Output, Feature */
                    break;
            }
            if (rc)
                return rc;
            zero(&l, sizeof(l));
        } else if (type == 1) { /* global */
            switch (tag) {
                case 0x0:
                    g.page = (u16)item_u(data, n);
                    break;
                case 0x1:
                    g.lmin = item_s(data, n);
                    break;
                case 0x2:
                    g.lmax_raw = item_u(data, n);
                    g.lmax_size = (u8)n;
                    break;
                case 0x7:
                    g.size = item_u(data, n);
                    break;
                case 0x8: {
                    u32 id = item_u(data, n);
                    if (!id || id > 255 || unnumbered)
                        return HID_E_ID;
                    g.id = (u8)id;
                    d->uses_ids = true;
                    break;
                }
                case 0x9:
                    g.count = item_u(data, n);
                    break;
                case 0xa: /* Push */
                    if (depth >= HID_STACK_DEPTH)
                        return HID_E_STACK;
                    stack[depth++] = g;
                    break;
                case 0xb: /* Pop */
                    if (!depth)
                        return HID_E_STACK;
                    g = stack[--depth];
                    break;
                default: /* physical range, unit, exponent */
                    break;
            }
        } else if (type == 2) { /* local */
            u32 v = item_u(data, n);
            bool full = n == 4;
            switch (tag) {
                case 0x0:
                    if (l.n < HID_MAX_USAGES) {
                        l.usage_full[l.n] = full;
                        l.usages[l.n++] = v;
                    }
                    break;
                case 0x1:
                    l.has_min = true;
                    l.min_full = full;
                    l.umin = v;
                    break;
                case 0x2:
                    l.has_max = true;
                    l.max_full = full;
                    l.umax = v;
                    break;
                default:
                    break;
            }
        }
    }
    return HID_OK;
}

u32 hid_field_usage(const struct hid_field *f, u32 i)
{
    if (f->n_usages)
        return f->usages[i < f->n_usages ? i : f->n_usages - 1u];
    if (f->has_range) {
        u32 u = f->umin + i;
        return u > f->umax ? f->umax : u;
    }
    return 0;
}

u32 hid_array_usage(const struct hid_field *f, s32 value)
{
    if (value < f->lmin || value > f->lmax)
        return 0;
    u32 k = (u32)(value - f->lmin);
    if (f->has_range) {
        u32 u = f->umin + k;
        return u > f->umax ? 0 : u;
    }
    return k < f->n_usages ? f->usages[k] : 0;
}

s32 hid_field_value(const struct hid_field *f, const u8 *data, u32 data_len, u32 i)
{
    u32 start = f->bit + i * f->size, v = 0;
    for (u32 b = 0; b < f->size; b++) {
        u32 at = start + b;
        if (at / 8 >= data_len)
            return 0;
        v |= (u32)((data[at / 8] >> (at % 8)) & 1) << b;
    }
    if (f->lmin < 0 && f->size < 32 && (v >> (f->size - 1)) & 1)
        v |= ~0u << f->size;
    return (s32)v;
}
