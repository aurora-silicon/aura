/* SPDX-License-Identifier: MIT */

/*
 * I1 keyboard translation.  See input_kbd.h.  Written on this branch from the HID 1.11
 * specification, the HID Usage Tables and the build plan's Fn table; no third-party source.
 */

#include "input_kbd.h"

static void zero(void *p, u32 n)
{
    u8 *b = p;
    for (u32 i = 0; i < n; i++)
        b[i] = 0;
}

static bool on_page(u32 usage, u16 page)
{
    return usage >> 16 == page;
}

static bool is_key_field(const struct hid_field *f)
{
    if (f->constant)
        return false;
    if (f->variable)
        return f->size == 1 && (on_page(hid_field_usage(f, 0), KBD_PAGE_KEYS) ||
                                on_page(hid_field_usage(f, f->count - 1u), KBD_PAGE_KEYS));
    u32 first = f->has_range ? f->umin : (f->n_usages ? f->usages[0] : 0);
    return on_page(first, KBD_PAGE_KEYS);
}

int kbd_layout_from_desc(struct kbd_layout *l, const u8 *desc, u32 len)
{
    static struct hid_desc d; /* 4 KiB+: not on the carrier's stack */
    zero(l, sizeof(*l));
    if (hid_desc_parse(&d, desc, len) != HID_OK)
        return KBD_E_DESC;
    int rid = -1;
    for (u32 i = 0; i < d.n_fields && rid < 0; i++)
        if (is_key_field(&d.f[i]))
            rid = d.f[i].report_id;
    if (rid < 0)
        return KBD_E_NO_KEYS;
    l->report_id = (u8)rid;
    l->data_bytes = (u16)((d.input_bits[rid] + 7u) / 8u);
    for (u32 i = 0; i < d.n_fields; i++) {
        const struct hid_field *f = &d.f[i];
        if (f->report_id != rid)
            continue;
        if (is_key_field(f)) {
            if (l->n_fields >= KBD_MAX_KEYFIELDS)
                return KBD_E_FIELDS;
            l->keys[l->n_fields++] = *f;
            continue;
        }
        bool fn = false;
        if (f->variable && f->size == 1 && !f->constant) {
            for (u32 e = 0; e < f->count && !fn; e++) {
                if (hid_field_usage(f, e) == KBD_USAGE_FN && !l->has_fn) {
                    l->has_fn = true;
                    l->fn_bit = (u16)(f->bit + e);
                    fn = true;
                }
            }
        }
        if (!fn && !f->constant)
            l->ignored_fields++;
    }
    l->valid = true;
    return KBD_OK;
}

void kbd_state_init(struct kbd_state *s)
{
    zero(s, sizeof(*s));
}

void kbd_release_all(struct kbd_state *s)
{
    zero(s->phys, sizeof(s->phys));
    zero(s->out, sizeof(s->out));
    s->fn = false;
    s->n_cons = 0;
}

/* The F-row's consumer usages in "media" (INPUT-I1I2-BUILD.md 2.5); 0: the key stays a key. */
static u16 media_usage(u8 key)
{
    switch (key) {
        case 0x3a: /* F1: brightness down */
            return 0x70;
        case 0x3b: /* F2: brightness up */
            return 0x6f;
        case 0x3d: /* F4: AC Search */
            return 0x221;
        case 0x40: /* F7: previous track */
            return 0xb6;
        case 0x41: /* F8: play/pause */
            return 0xcd;
        case 0x42: /* F9: next track */
            return 0xb5;
        case 0x43: /* F10: mute */
            return 0xe2;
        case 0x44: /* F11: volume down */
            return 0xea;
        case 0x45: /* F12: volume up */
            return 0xe9;
        default:
            return 0;
    }
}

/* Fn + key on the navigation cluster; 0: no Fn meaning. */
static u8 fn_nav(u8 key)
{
    switch (key) {
        case 0x4f: /* Right -> End */
            return 0x4d;
        case 0x50: /* Left -> Home */
            return 0x4a;
        case 0x51: /* Down -> Page Down */
            return 0x4e;
        case 0x52: /* Up -> Page Up */
            return 0x4b;
        case 0x2a: /* Backspace -> Delete */
            return 0x4c;
        case 0x28: /* Return -> Insert */
            return 0x49;
        default:
            return 0;
    }
}

static u16 translate(u8 key, bool fn, enum kbd_fn_mode mode)
{
    u16 media = media_usage(key);
    if (media && fn == (mode == KBD_FN_FKEYS)) /* media mode: without Fn */
        return KBD_OUT_CONSUMER | media;
    if (fn && fn_nav(key))
        return fn_nav(key);
    return key;
}

static void cons_push(struct kbd_state *s, u16 usage)
{
    if (s->n_cons == KBD_CONSUMER_STACK) {
        for (u32 i = 1; i < KBD_CONSUMER_STACK; i++)
            s->cons[i - 1] = s->cons[i];
        s->n_cons--;
    }
    s->cons[s->n_cons++] = usage;
}

static void cons_remove(struct kbd_state *s, u16 usage)
{
    for (u32 i = s->n_cons; i-- > 0;) {
        if (s->cons[i] != usage)
            continue;
        for (u32 k = i + 1; k < s->n_cons; k++)
            s->cons[k - 1] = s->cons[k];
        s->n_cons--;
        return;
    }
}

static void set_bit(u8 *bits, u32 n)
{
    bits[n / 8] |= (u8)(1u << (n % 8));
}

static bool get_bit(const u8 *bits, u32 n)
{
    return (bits[n / 8] >> (n % 8)) & 1;
}

/* One Keyboard-page usage into the new state.  False for an error code (rollover). */
static bool take(u32 usage, u8 *phys, u8 *mods, bool array)
{
    if (!on_page(usage, KBD_PAGE_KEYS))
        return true;
    u32 id = usage & 0xffff;
    if (id >= 0xe0 && id <= 0xe7) {
        *mods |= (u8)(1u << (id - 0xe0));
        return true;
    }
    if (id <= 3)
        return !(array && id >= KBD_ERR_ROLLOVER);
    if (id <= 0xff)
        set_bit(phys, id);
    return true;
}

int kbd_translate(const struct kbd_layout *l, struct kbd_state *s, enum kbd_fn_mode mode,
                  const u8 *report, u32 len, u8 *mods, u8 keys[KBD_BITMAP_BYTES], u16 *consumer)
{
    if (!l->valid)
        return KBD_T_IGNORED;
    const u8 *data = report;
    u32 dlen = len;
    if (l->report_id) {
        if (!len || report[0] != l->report_id) {
            s->ignored++;
            return KBD_T_IGNORED;
        }
        data++;
        dlen--;
    }
    if (dlen < l->data_bytes) {
        s->short_reports++;
        return KBD_T_SHORT;
    }
    u8 phys[32], m = 0;
    zero(phys, sizeof(phys));
    for (u32 i = 0; i < l->n_fields; i++) {
        const struct hid_field *f = &l->keys[i];
        for (u32 e = 0; e < f->count; e++) {
            s32 v = hid_field_value(f, data, dlen, e);
            u32 usage;
            if (f->variable) {
                if (!v)
                    continue;
                usage = hid_field_usage(f, e);
            } else {
                usage = hid_array_usage(f, v);
                if (!usage)
                    continue;
            }
            if (!take(usage, phys, &m, !f->variable)) {
                s->rollover++;
                return KBD_T_IGNORED;
            }
        }
    }
    bool fn = l->has_fn && ((data[l->fn_bit / 8] >> (l->fn_bit % 8)) & 1);

    for (u32 u = 0; u < 256; u++) {
        bool was = get_bit(s->phys, u), now = get_bit(phys, u);
        if (now && !was) {
            u16 out = translate((u8)u, fn, mode);
            if (!(out & KBD_OUT_CONSUMER) && out >= KBD_BITMAP_BYTES * 8) {
                s->dropped_usages++;
                out = KBD_OUT_NONE;
            }
            if (out & KBD_OUT_CONSUMER)
                cons_push(s, out & ~KBD_OUT_CONSUMER);
            s->out[u] = out;
        } else if (!now && was) {
            if (s->out[u] & KBD_OUT_CONSUMER)
                cons_remove(s, s->out[u] & ~KBD_OUT_CONSUMER);
            s->out[u] = KBD_OUT_NONE;
        }
    }
    for (u32 i = 0; i < sizeof(phys); i++)
        s->phys[i] = phys[i];
    s->fn = fn;
    s->reports++;

    zero(keys, KBD_BITMAP_BYTES);
    for (u32 u = 0; u < 256; u++) {
        u16 out = s->out[u];
        if (get_bit(phys, u) && out && !(out & KBD_OUT_CONSUMER))
            set_bit(keys, out);
    }
    *mods = m;
    *consumer = s->n_cons ? s->cons[s->n_cons - 1] : 0;
    return KBD_T_UPDATE;
}
