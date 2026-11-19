/* SPDX-License-Identifier: MIT */
/*
 * Host replay of src/hv/windows/hid_desc.c and input_kbd.c.  Synthetic descriptors written for
 * this test from the HID 1.11 specification and the HID Usage Tables (no device capture exists
 * on this Mac; the real descriptor is logged by the carrier's PROBE on the M6).
 *
 * Failure list (written before the code):
 *   H1  items: short items of 0/1/2/4 bytes, long items skipped, an item past the end refused
 *   H2  global state: Push/Pop restore page, sizes and ranges; unbalanced Pop refused
 *   H3  report IDs: per-ID bit offsets; ID 0 refused; fields before the first ID refused
 *   H4  local state is cleared after every main item; extended (4-byte) usages keep their page
 *   H5  logical max 0xFF with min 0 is 255 (unsigned), with a negative min it is -1
 *   H6  over 64 fields, a report size 0 or > 32, a report over 256 bytes: refused
 *   K1  a boot keyboard with a report ID maps (modifiers, array); one with an Fn bit too; an
 *       NKRO bitmap without IDs; a mouse is refused (no keys)
 *   K2  letters, Ctrl+Alt+Del, keypad and JIS usages (0x87/0x89/0x90/0x91) reach the bitmap
 *   K3  the Fn layer in both modes: F-row to consumer usages, Fn+arrows, Fn+Backspace/Return;
 *       F3/F5/F6 stay keys; Fn alone sends nothing
 *   K4  sticky release: a key keeps the translation it was pressed with when Fn changes
 *   K5  overlapping media keys: the newest wins, a release falls back to the one still held
 *   K6  ErrorRollOver (0x01..0x03 in the array) keeps the previous state
 *   K7  usages above 0x9F are dropped and counted; other report IDs and short reports change
 *       nothing
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hid_desc.h"
#include "input_kbd.h"

static int failures;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                    \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static void case_result(const char *name, int before)
{
    printf("{\"case\":\"%s\",\"result\":\"%s\"}\n", name, failures == before ? "passed" : "failed");
}

/* Boot keyboard, report ID 1: modifiers, reserved byte, 6-key array; optional Fn bit + pad. */
static const u8 boot_kbd[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01,                         /* ID 1 */
    0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, /* modifiers */
    0x95, 0x08, 0x81, 0x02,                                                 /* */
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,                                     /* reserved */
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0xff, 0x05, 0x07, 0x19, 0x00, /* keys */
    0x29, 0xff, 0x81, 0x00,                                                 /* */
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x95, 0x05, 0x75, 0x01, 0x91, 0x02, /* LEDs (output) */
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,                                     /* */
    0xc0,
};

static const u8 fn_kbd[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01,                         /* ID 1 */
    0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, /* modifiers */
    0x95, 0x08, 0x81, 0x02,                                                 /* */
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,                                     /* reserved */
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x26, 0xff, 0x00, 0x05, 0x07, 0x19, /* keys */
    0x00, 0x29, 0xff, 0x81, 0x00,                                           /* */
    0x06, 0xff, 0x00, 0x09, 0x03, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, /* Fn */
    0x01, 0x81, 0x02, 0x75, 0x07, 0x81, 0x01,                               /* pad */
    0xc0,
    /* A second collection with another report ID (vendor), to be ignored. */
    0x06, 0x00, 0xff, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x02, 0x15, 0x00, 0x26, 0xff, 0x00,
    0x75, 0x08, 0x95, 0x04, 0x09, 0x01, 0x81, 0x02, 0xc0,
};

/* NKRO bitmap, no report IDs: modifiers, then usages 0x00-0x9F as bits. */
static const u8 nkro_kbd[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15,
    0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x19, 0x00, 0x29, 0x9f,
    0x95, 0xa0, 0x81, 0x02, 0xc0,
};

static const u8 mouse[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19, 0x01,
    0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01,
    0x75, 0x05, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f,
    0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xc0, 0xc0,
};

static void h_items(void)
{
    int before = failures;
    static struct hid_desc d;
    CHECK(hid_desc_parse(&d, boot_kbd, sizeof(boot_kbd)) == HID_OK);
    CHECK(d.uses_ids && d.n_fields == 3 && d.input_bits[1] == 64);
    CHECK(d.f[0].size == 1 && d.f[0].count == 8 && d.f[0].variable && d.f[0].bit == 0 &&
          hid_field_usage(&d.f[0], 0) == 0x000700e0 && hid_field_usage(&d.f[0], 7) == 0x000700e7);
    CHECK(d.f[1].constant && d.f[1].bit == 8);
    CHECK(!d.f[2].variable && d.f[2].bit == 16 && d.f[2].count == 6 && d.f[2].lmax == 255 &&
          hid_array_usage(&d.f[2], 0x04) == 0x00070004 &&
          hid_array_usage(&d.f[2], 0) == 0x00070000);
    /* H5: 0x25 0xff with a negative minimum is -1. */
    u8 neg[] = {0x05, 0x01, 0x09, 0x30, 0x15, 0x80, 0x25, 0xff, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02};
    CHECK(hid_desc_parse(&d, neg, sizeof(neg)) == HID_OK && d.f[0].lmin == -128 &&
          d.f[0].lmax == -1);
    /* H1: a long item is skipped; an item past the end is refused. */
    u8 longi[] = {0xfe, 0x02, 0x10, 0xaa, 0xbb, 0x05, 0x07, 0x09, 0x04, 0x75, 0x01, 0x95, 0x01,
                  0x81, 0x02};
    CHECK(hid_desc_parse(&d, longi, sizeof(longi)) == HID_OK && d.skipped_long == 1 &&
          d.n_fields == 1);
    CHECK(hid_desc_parse(&d, longi, 3) == HID_E_LENGTH);
    u8 cut[] = {0x05, 0x07, 0x27, 0x01, 0x02};
    CHECK(hid_desc_parse(&d, cut, sizeof(cut)) == HID_E_LENGTH);
    CHECK(hid_desc_parse(&d, cut, 0) == HID_E_LENGTH);
    /* H4: an extended usage keeps its own page; local state is cleared after a main item. */
    u8 ext[] = {0x05, 0x01, 0x0b, 0x03, 0x00, 0xff, 0x00, 0x75, 0x01, 0x95, 0x02, 0x81, 0x02,
                0x95, 0x01, 0x81, 0x02};
    CHECK(hid_desc_parse(&d, ext, sizeof(ext)) == HID_OK &&
          hid_field_usage(&d.f[0], 0) == 0x00ff0003 && hid_field_usage(&d.f[0], 1) == 0x00ff0003 &&
          hid_field_usage(&d.f[1], 0) == 0 && d.f[1].bit == 2);
    case_result("items", before);
}

static void h_state(void)
{
    int before = failures;
    static struct hid_desc d;
    /* H2: Push/Pop. */
    u8 push[] = {0x05, 0x07, 0x75, 0x08, 0xa4, 0x05, 0x01, 0x75, 0x01, 0xb4, 0x09, 0x04, 0x95,
                 0x01, 0x81, 0x02};
    CHECK(hid_desc_parse(&d, push, sizeof(push)) == HID_OK && d.f[0].size == 8 &&
          hid_field_usage(&d.f[0], 0) == 0x00070004);
    u8 pop[] = {0xb4};
    CHECK(hid_desc_parse(&d, pop, 1) == HID_E_STACK);
    u8 deep[] = {0xa4, 0xa4, 0xa4, 0xa4, 0xa4};
    CHECK(hid_desc_parse(&d, deep, sizeof(deep)) == HID_E_STACK);
    u8 endc[] = {0xc0};
    CHECK(hid_desc_parse(&d, endc, 1) == HID_E_STACK);
    /* H3: IDs. */
    u8 id0[] = {0x85, 0x00};
    CHECK(hid_desc_parse(&d, id0, 2) == HID_E_ID);
    u8 mixed[] = {0x75, 0x08, 0x95, 0x01, 0x81, 0x02, 0x85, 0x01, 0x81, 0x02};
    CHECK(hid_desc_parse(&d, mixed, sizeof(mixed)) == HID_E_ID);
    u8 two[] = {0x75, 0x08, 0x95, 0x02, 0x85, 0x01, 0x81, 0x02, 0x85, 0x02, 0x95, 0x03, 0x81, 0x02,
                0x85, 0x01, 0x95, 0x01, 0x81, 0x02};
    CHECK(hid_desc_parse(&d, two, sizeof(two)) == HID_OK && d.input_bits[1] == 24 &&
          d.input_bits[2] == 24 && d.f[2].bit == 16 && d.f[2].report_id == 1);
    /* H6 */
    u8 zero[] = {0x75, 0x00, 0x95, 0x01, 0x81, 0x02};
    CHECK(hid_desc_parse(&d, zero, sizeof(zero)) == HID_E_FIELD);
    u8 wide[] = {0x75, 0x21, 0x95, 0x01, 0x81, 0x02};
    CHECK(hid_desc_parse(&d, wide, sizeof(wide)) == HID_E_FIELD);
    u8 big[] = {0x75, 0x08, 0x96, 0x01, 0x01, 0x81, 0x02};
    CHECK(hid_desc_parse(&d, big, sizeof(big)) == HID_E_FIELD);
    static u8 many[4 + 65 * 2];
    many[0] = 0x75;
    many[1] = 0x01;
    many[2] = 0x95;
    many[3] = 0x01;
    for (int i = 0; i < 65; i++) {
        many[4 + 2 * i] = 0x81;
        many[5 + 2 * i] = 0x02;
    }
    CHECK(hid_desc_parse(&d, many, sizeof(many)) == HID_E_FIELDS);
    case_result("state", before);
}

static u32 bit(const u8 *keys, u32 u)
{
    return (keys[u / 8] >> (u % 8)) & 1;
}

static int nkeys(const u8 *keys)
{
    int n = 0;
    for (u32 u = 0; u < 160; u++)
        n += bit(keys, u);
    return n;
}

/* Send one fn_kbd report: modifiers, up to 6 keys, Fn. */
static int send(const struct kbd_layout *l, struct kbd_state *s, enum kbd_fn_mode mode, u8 m,
                const u8 *k, int n, bool fn, u8 *mods, u8 *keys, u16 *cons)
{
    u8 r[10] = {1, m, 0, 0, 0, 0, 0, 0, 0, (u8)(fn ? 1 : 0)};
    for (int i = 0; i < n; i++)
        r[3 + i] = k[i];
    return kbd_translate(l, s, mode, r, sizeof(r), mods, keys, cons);
}

static void k_layouts(void)
{
    int before = failures;
    static struct kbd_layout l;
    CHECK(kbd_layout_from_desc(&l, boot_kbd, sizeof(boot_kbd)) == KBD_OK && l.valid &&
          l.report_id == 1 && !l.has_fn && l.data_bytes == 8);
    CHECK(kbd_layout_from_desc(&l, fn_kbd, sizeof(fn_kbd)) == KBD_OK && l.has_fn &&
          l.fn_bit == 64 && l.data_bytes == 9);
    /* Fn as vendor usage 3 on the 0xFF00 and 0xFF0C pages too; another usage on 0x00FF is not. */
    {
        static u8 v[sizeof(fn_kbd)];
        const u32 at = 47; /* fn_kbd's Fn usage page item: 0x06, lo, hi */
        static const u8 pages[3][2] = {{0x00, 0xff}, {0x0c, 0xff}, {0xff, 0x00}};
        for (u32 i = 0; i < 3; i++) {
            memcpy(v, fn_kbd, sizeof(v));
            CHECK(v[at] == 0x06 && v[at + 1] == 0xff && v[at + 2] == 0x00 && v[at + 4] == 0x03);
            v[at + 1] = pages[i][0];
            v[at + 2] = pages[i][1];
            if (i == 2)
                v[at + 4] = 0x04;
            CHECK(kbd_layout_from_desc(&l, v, sizeof(v)) == KBD_OK && l.has_fn == (i < 2) &&
                  (i == 2 || l.fn_bit == 64));
        }
    }
    CHECK(kbd_layout_from_desc(&l, nkro_kbd, sizeof(nkro_kbd)) == KBD_OK && l.report_id == 0 &&
          l.data_bytes == 21);
    CHECK(kbd_layout_from_desc(&l, mouse, sizeof(mouse)) == KBD_E_NO_KEYS && !l.valid);
    CHECK(kbd_layout_from_desc(&l, mouse, 5) == KBD_E_DESC && !l.valid);
    case_result("layouts", before);
}

static void k_keys(void)
{
    int before = failures;
    static struct kbd_layout l;
    static struct kbd_state s;
    u8 mods, keys[20];
    u16 cons;
    CHECK(kbd_layout_from_desc(&l, fn_kbd, sizeof(fn_kbd)) == KBD_OK);
    kbd_state_init(&s);
    u8 a[] = {0x04};
    CHECK(send(&l, &s, KBD_FN_MEDIA, 0, a, 1, false, &mods, keys, &cons) == KBD_T_UPDATE &&
          bit(keys, 4) && nkeys(keys) == 1 && !mods && !cons);
    /* Ctrl+Alt+Del (left Ctrl 0x01, left Alt 0x04, Delete 0x4C). */
    u8 del[] = {0x4c};
    CHECK(send(&l, &s, KBD_FN_MEDIA, 0x05, del, 1, false, &mods, keys, &cons) == KBD_T_UPDATE &&
          mods == 0x05 && bit(keys, 0x4c) && nkeys(keys) == 1);
    /* Keypad and JIS usages. */
    u8 jis[] = {0x87, 0x89, 0x90, 0x91, 0x59, 0x62};
    CHECK(send(&l, &s, KBD_FN_MEDIA, 0, jis, 6, false, &mods, keys, &cons) == KBD_T_UPDATE &&
          nkeys(keys) == 6 && bit(keys, 0x87) && bit(keys, 0x91) && bit(keys, 0x62));
    CHECK(send(&l, &s, KBD_FN_MEDIA, 0, NULL, 0, false, &mods, keys, &cons) == KBD_T_UPDATE &&
          !nkeys(keys) && !mods);
    /* NKRO bitmap. */
    static struct kbd_layout n;
    static struct kbd_state ns;
    CHECK(kbd_layout_from_desc(&n, nkro_kbd, sizeof(nkro_kbd)) == KBD_OK);
    kbd_state_init(&ns);
    u8 r[21] = {0x02};
    r[1 + 0x04 / 8] |= 1 << (0x04 % 8);
    r[1 + 0x9f / 8] |= 1 << (0x9f % 8);
    r[1 + 0x16 / 8] |= 1 << (0x16 % 8);
    CHECK(kbd_translate(&n, &ns, KBD_FN_MEDIA, r, sizeof(r), &mods, keys, &cons) == KBD_T_UPDATE &&
          mods == 0x02 && nkeys(keys) == 3 && bit(keys, 0x9f) && bit(keys, 0x16));
    case_result("keys", before);
}

static void k_fn_layer(void)
{
    int before = failures;
    static struct kbd_layout l;
    static struct kbd_state s;
    u8 mods, keys[20];
    u16 cons;
    CHECK(kbd_layout_from_desc(&l, fn_kbd, sizeof(fn_kbd)) == KBD_OK);
    static const struct {
        u8 key;
        u16 media; /* consumer usage without Fn in media mode, 0 = stays a key */
    } row[] = {{0x3a, 0x70}, {0x3b, 0x6f}, {0x3c, 0},     {0x3d, 0x221}, {0x3e, 0},
               {0x3f, 0},    {0x40, 0xb6}, {0x41, 0xcd}, {0x42, 0xb5},  {0x43, 0xe2},
               {0x44, 0xea}, {0x45, 0xe9}};
    for (int mode = 0; mode < 2; mode++) {
        for (unsigned i = 0; i < sizeof(row) / sizeof(row[0]); i++) {
            for (int fn = 0; fn < 2; fn++) {
                kbd_state_init(&s);
                u8 k = row[i].key;
                CHECK(send(&l, &s, mode, 0, &k, 1, fn, &mods, keys, &cons) == KBD_T_UPDATE);
                bool media = row[i].media && (fn == (mode == KBD_FN_FKEYS)); /* media mode: without Fn */
                if (media)
                    CHECK(cons == row[i].media && !nkeys(keys));
                else
                    CHECK(!cons && bit(keys, k) && nkeys(keys) == 1);
                CHECK(send(&l, &s, mode, 0, NULL, 0, fn, &mods, keys, &cons) == KBD_T_UPDATE &&
                      !cons && !nkeys(keys));
            }
        }
    }
    /* Fn + arrows / Backspace / Return; without Fn they stay. */
    static const u8 nav[][2] = {{0x4f, 0x4d}, {0x50, 0x4a}, {0x51, 0x4e},
                                {0x52, 0x4b}, {0x2a, 0x4c}, {0x28, 0x49}};
    for (unsigned i = 0; i < 6; i++) {
        kbd_state_init(&s);
        u8 k = nav[i][0];
        CHECK(send(&l, &s, KBD_FN_MEDIA, 0, &k, 1, true, &mods, keys, &cons) == KBD_T_UPDATE &&
              bit(keys, nav[i][1]) && nkeys(keys) == 1);
        kbd_state_init(&s);
        CHECK(send(&l, &s, KBD_FN_MEDIA, 0, &k, 1, false, &mods, keys, &cons) == KBD_T_UPDATE &&
              bit(keys, k) && nkeys(keys) == 1);
    }
    /* Fn alone. */
    kbd_state_init(&s);
    CHECK(send(&l, &s, KBD_FN_MEDIA, 0, NULL, 0, true, &mods, keys, &cons) == KBD_T_UPDATE &&
          !mods && !nkeys(keys) && !cons);
    case_result("fn_layer", before);
}

static void k_sticky(void)
{
    int before = failures;
    static struct kbd_layout l;
    static struct kbd_state s;
    u8 mods, keys[20];
    u16 cons;
    CHECK(kbd_layout_from_desc(&l, fn_kbd, sizeof(fn_kbd)) == KBD_OK);
    kbd_state_init(&s);
    u8 down = 0x51, f12 = 0x45;
    /* Fn+Down = PgDn; Fn released first: still PgDn until Down is released. */
    send(&l, &s, KBD_FN_MEDIA, 0, &down, 1, true, &mods, keys, &cons);
    CHECK(bit(keys, 0x4e) && !bit(keys, 0x51));
    send(&l, &s, KBD_FN_MEDIA, 0, &down, 1, false, &mods, keys, &cons);
    CHECK(bit(keys, 0x4e) && !bit(keys, 0x51));
    send(&l, &s, KBD_FN_MEDIA, 0, NULL, 0, false, &mods, keys, &cons);
    CHECK(!nkeys(keys));
    /* F12 pressed as volume up, then Fn pressed: still volume up; release: nothing stuck. */
    send(&l, &s, KBD_FN_MEDIA, 0, &f12, 1, false, &mods, keys, &cons);
    CHECK(cons == 0xe9 && !nkeys(keys));
    send(&l, &s, KBD_FN_MEDIA, 0, &f12, 1, true, &mods, keys, &cons);
    CHECK(cons == 0xe9 && !nkeys(keys));
    send(&l, &s, KBD_FN_MEDIA, 0, NULL, 0, true, &mods, keys, &cons);
    CHECK(!cons && !nkeys(keys));
    case_result("sticky", before);
}

static void k_media_overlap(void)
{
    int before = failures;
    static struct kbd_layout l;
    static struct kbd_state s;
    u8 mods, keys[20];
    u16 cons;
    CHECK(kbd_layout_from_desc(&l, fn_kbd, sizeof(fn_kbd)) == KBD_OK);
    kbd_state_init(&s);
    u8 both[] = {0x44, 0x45}, f11 = 0x44, f12 = 0x45;
    send(&l, &s, KBD_FN_MEDIA, 0, &f11, 1, false, &mods, keys, &cons);
    CHECK(cons == 0xea);
    send(&l, &s, KBD_FN_MEDIA, 0, both, 2, false, &mods, keys, &cons);
    CHECK(cons == 0xe9);
    send(&l, &s, KBD_FN_MEDIA, 0, &f11, 1, false, &mods, keys, &cons);
    CHECK(cons == 0xea);
    send(&l, &s, KBD_FN_MEDIA, 0, both, 2, false, &mods, keys, &cons);
    CHECK(cons == 0xe9);
    send(&l, &s, KBD_FN_MEDIA, 0, &f12, 1, false, &mods, keys, &cons);
    CHECK(cons == 0xe9);
    send(&l, &s, KBD_FN_MEDIA, 0, NULL, 0, false, &mods, keys, &cons);
    CHECK(cons == 0);
    case_result("media_overlap", before);
}

static void k_errors(void)
{
    int before = failures;
    static struct kbd_layout l;
    static struct kbd_state s;
    u8 mods, keys[20];
    u16 cons;
    CHECK(kbd_layout_from_desc(&l, fn_kbd, sizeof(fn_kbd)) == KBD_OK);
    kbd_state_init(&s);
    u8 a = 0x04;
    send(&l, &s, KBD_FN_MEDIA, 0x02, &a, 1, false, &mods, keys, &cons);
    CHECK(bit(keys, 4) && mods == 0x02);
    u8 roll[6] = {1, 1, 1, 1, 1, 1};
    CHECK(send(&l, &s, KBD_FN_MEDIA, 0, roll, 6, false, &mods, keys, &cons) == KBD_T_IGNORED &&
          s.rollover == 1);
    /* State kept: the next real report releases 'a' normally. */
    CHECK(send(&l, &s, KBD_FN_MEDIA, 0, NULL, 0, false, &mods, keys, &cons) == KBD_T_UPDATE &&
          !nkeys(keys) && !mods);
    /* Above 0x9F: dropped, counted. */
    u8 hi[] = {0xb0, 0x05};
    CHECK(send(&l, &s, KBD_FN_MEDIA, 0, hi, 2, false, &mods, keys, &cons) == KBD_T_UPDATE &&
          nkeys(keys) == 1 && bit(keys, 5) && s.dropped_usages == 1);
    /* Another report ID and a short report. */
    u8 other[5] = {2, 1, 2, 3, 4};
    CHECK(kbd_translate(&l, &s, KBD_FN_MEDIA, other, 5, &mods, keys, &cons) == KBD_T_IGNORED);
    u8 shortr[4] = {1, 0, 0, 4};
    CHECK(kbd_translate(&l, &s, KBD_FN_MEDIA, shortr, 4, &mods, keys, &cons) == KBD_T_SHORT &&
          s.short_reports == 1);
    kbd_release_all(&s);
    CHECK(send(&l, &s, KBD_FN_MEDIA, 0, NULL, 0, false, &mods, keys, &cons) == KBD_T_UPDATE &&
          !nkeys(keys) && !cons && !mods);
    case_result("errors", before);
}

int main(void)
{
    h_items();
    h_state();
    k_layouts();
    k_keys();
    k_fn_layer();
    k_sticky();
    k_media_overlap();
    k_errors();
    return failures ? 1 : 0;
}
