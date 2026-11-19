/* SPDX-License-Identifier: MIT */

#ifndef INPUT_KBD_H
#define INPUT_KBD_H

#include "hid_desc.h"
#include "types.h"

/*
 * I1: the M6's internal keyboard (MTP interface "keyboard") translated into the guest device's
 * keyboard (ID 1: modifiers + 160-bit bitmap) and consumer (ID 4) reports.  Pure.
 *
 * The layout is found at run time from the HID descriptor the MTP sends in INIT (nothing is
 * assumed from other machines): the modifier usages 0xE0-0xE7 as one-bit variables, the keys
 * as an array and/or one-bit variables on the Keyboard/Keypad page, and the Fn key (vendor page
 * 0x00FF usage 0x0003, the convention Linux hid-apple follows) when present.  A descriptor with
 * no keyboard report is refused: the keyboard is then left disabled.
 *
 * Fn layer (ours, windows-m6/INPUT-I1I2-BUILD.md 2.5).  Default "media": the top row sends
 * consumer usages and Fn gives F1-F12; "fkeys" swaps that for the F-row.  Fn + arrows give
 * Home/End/PgUp/PgDn, Fn + Backspace Delete, Fn + Return Insert.  A key's translation is chosen
 * when it is pressed and kept until it is released, whatever Fn does in between.  Fn alone sends
 * nothing.  Usages above 0x9F do not fit the guest bitmap and are dropped (counted).
 */

#define KBD_BITMAP_BYTES 20u /* usages 0x00-0x9F */
#define KBD_MAX_KEYFIELDS 4u
#define KBD_CONSUMER_STACK 8u

#define KBD_PAGE_KEYS     0x0007u
#define KBD_USAGE_FN      0x00ff0003u
#define KBD_ERR_ROLLOVER  0x01u /* ..0x03: the keyboard's error codes */

enum kbd_fn_mode {
    KBD_FN_MEDIA = 0,
    KBD_FN_FKEYS = 1,
};

struct kbd_layout {
    bool valid;
    u8 report_id;   /* 0: no ID byte */
    u16 data_bytes; /* the report after the ID byte */
    u8 n_fields;
    struct hid_field keys[KBD_MAX_KEYFIELDS]; /* page-7 arrays and one-bit variables */
    bool has_fn;
    u16 fn_bit;
    u32 ignored_fields; /* other fields of the keyboard report */
};

enum kbd_layout_result {
    KBD_OK = 0,
    KBD_E_DESC = -1,    /* the descriptor did not parse */
    KBD_E_NO_KEYS = -2, /* no report with Keyboard-page keys */
    KBD_E_FIELDS = -3,  /* more key fields than this side keeps */
};

int kbd_layout_from_desc(struct kbd_layout *l, const u8 *desc, u32 len);

/* What a held key sends: a keyboard usage, a consumer usage, or nothing. */
#define KBD_OUT_NONE     0x0000u
#define KBD_OUT_CONSUMER 0x8000u /* | consumer usage (<= 0x3FF) */

struct kbd_state {
    u8 phys[32];      /* usages 0x00-0xFF held now */
    bool fn;
    u16 out[256];     /* per held usage, what it was translated to at press */
    u16 cons[KBD_CONSUMER_STACK]; /* held consumer usages, oldest first */
    u8 n_cons;
    u32 reports, ignored, rollover, dropped_usages, short_reports;
};

void kbd_state_init(struct kbd_state *s);

enum kbd_translate_result {
    KBD_T_UPDATE = 1,   /* *mods, keys and *consumer hold the new state */
    KBD_T_IGNORED = 0,  /* another report ID, or a rollover/error report: state unchanged */
    KBD_T_SHORT = -1,   /* shorter than the layout: state unchanged */
};

/* One report as the MTP sent it (report ID byte first when the layout has IDs). */
int kbd_translate(const struct kbd_layout *l, struct kbd_state *s, enum kbd_fn_mode mode,
                  const u8 *report, u32 len, u8 *mods, u8 keys[KBD_BITMAP_BYTES], u16 *consumer);

/* All up: what a released source looks like (mods 0, no keys, consumer 0). */
void kbd_release_all(struct kbd_state *s);

#endif
