/* SPDX-License-Identifier: MIT */

#ifndef INPUT_TP_H
#define INPUT_TP_H

#include "types.h"

/*
 * I2a: the J804 (T8152) trackpad as a relative mouse, pure (no MMIO, no locks, no floating
 * point).  mtp_bringup.c feeds it the multi-touch interface's reports; its output goes to the
 * guest device's report ID 3 (buttons 1-5, dX, dY, wheel, AC Pan) through input_model.c.
 * tests/e2e/windows_input_tp_host.c replays synthetic finger records.
 *
 * Fact ledger (windows-m6/INPUT-I1I2-BUILD.md 1 and 12; every offset is [?] until the S2 probe
 * logs real frames, and the parser refuses what does not fit):
 *   report 0x75, a 38-byte header: byte 0 the report ID, byte 22 the record count, byte 23
 *   bit 0 the physical click (the firmware gives the haptic); then N 30-byte records of
 *   little-endian 16-bit fields: +4 X, +6 Y (signed; Y grows upwards), +12 tool major,
 *   +18 touch major.  A record with touch major 0 is not touching.  Records carry no contact
 *   ID this file relies on: contacts are matched to the previous frame by position.
 *   Geometry: the feature report 0xD9 answers {u32 width, u32 height (0.01 mm), s16 min X,
 *   min Y, max X, max Y}; without it the defaults below are used.
 *
 * Gestures (our own rules):
 *   one contact, or any while the button is down: pointer motion of the contact that moved
 *     most, scaled by the gain and the acceleration curve, remainders carried;
 *   physical click: left, or right when it starts with two or more contacts; latched until
 *     the click releases; motion is ignored for click_settle_ms after it starts;
 *   tap (one contact down and up within tap_ms, travel below tap_travel_um, no click, no
 *     scroll, no internal key pressed in the last tap_key_quiet_ms): left down, left up;
 *     the same with two contacts: right;
 *   two contacts moving the same way: wheel and AC Pan detents (scroll_um of travel each,
 *     the dominant axis only, traditional direction unless `natural`);
 *   three or more contacts: nothing (PTP gestures are a later step);
 *   a change in the number of contacts moves nothing in that frame (no jump).
 * Silence: after TP_STALE_US without a frame, held contacts and the button are dropped.
 */

#define TP_REPORT_ID     0x75u
#define TP_HDR_LEN       38u
#define TP_HDR_COUNT     22u
#define TP_HDR_BUTTON    23u
#define TP_REC_LEN       30u
#define TP_REC_X         4u
#define TP_REC_Y         6u
#define TP_REC_TOOL      12u
#define TP_REC_TOUCH     18u
#define TP_MAX_CONTACTS  16u
#define TP_FEATURE_GEOMETRY 0xd9u
#define TP_GEOMETRY_LEN  17u /* report ID + 16 bytes */

#define TP_BTN_LEFT  0x01u
#define TP_BTN_RIGHT 0x02u

#define TP_DT_MIN_US   4000u
#define TP_DT_MAX_US   50000u
#define TP_STALE_US    500000u
#define TP_MATCH_MM    20u /* farther than this from every previous contact: a new contact */

struct tp_geometry {
    u32 width, height; /* 0.01 mm */
    s16 min_x, min_y, max_x, max_y;
};

/* Tunables (the launcher's parameter page; the defaults are tp_config_default()). */
struct tp_config {
    u16 gain_q8;          /* pointer counts per mm at low speed, Q8.8 */
    u16 accel_q8;         /* extra factor reached at accel_hi, Q8.8 (0: no acceleration) */
    u16 accel_lo, accel_hi; /* mm/s: the curve rises linearly between them */
    u16 scroll_um;        /* finger travel per wheel or pan detent */
    u16 tap_ms;
    u16 tap_travel_um;
    u16 tap_key_quiet_ms;
    u16 click_settle_ms;
    u16 palm_major;       /* touch major above this is a palm and ignored; 0: off */
    bool tap;
    bool natural;
};

struct tp_report {
    u8 buttons;
    s16 dx, dy;
    s8 wheel, pan;
};

#define TP_MAX_REPORTS 3u

struct tp_contact {
    s32 x, y;   /* raw units, y downwards */
    s32 x0, y0; /* where it touched down */
    u8 slot;
};

struct tp_state {
    struct tp_config cfg;
    struct tp_geometry geo;
    u32 res_x, res_y; /* raw units per mm */

    struct tp_contact prev[TP_MAX_CONTACTS];
    u32 n_prev;
    u8 next_slot;
    u64 t_prev;
    bool have_prev;

    /* the touch session (first contact down to the last one up) */
    bool session;
    u64 t_session;
    u32 max_contacts;
    u32 max_travel_um;
    bool clicked, scrolled;

    bool phys;
    u8 buttons;       /* the latched physical-click button */
    u8 last_buttons;  /* the last buttons reported */
    u64 settle_until;
    u64 last_key;
    bool key_seen;

    s32 rem_x, rem_y;   /* Q8 counts carried */
    s32 acc_v, acc_h;   /* scroll travel carried, um */

    u32 frames, bad, ignored, taps, clicks, detents, stale, palms;
};

void tp_config_default(struct tp_config *c);
/* Accept a geometry (0xD9 or the defaults).  False, and nothing changed, when it is not sane. */
bool tp_set_geometry(struct tp_state *s, const struct tp_geometry *g);
void tp_geometry_default(struct tp_geometry *g);
/* Parse a 0xD9 feature report (report ID first). */
bool tp_parse_geometry(const u8 *p, u32 len, struct tp_geometry *g);

void tp_init(struct tp_state *s, const struct tp_config *cfg);

/* An internal key went down at `now` (taps are suppressed for a while). */
void tp_note_key(struct tp_state *s, u64 now);

/*
 * One multi-touch report (report ID first) at `now` (us).  Fills `out` with up to
 * TP_MAX_REPORTS relative reports and returns how many; < 0 for a report that is not a
 * well-formed 0x75 frame (counted, state unchanged).
 */
int tp_frame(struct tp_state *s, const u8 *p, u32 len, u64 now, struct tp_report *out);

/*
 * Time passes without frames: after TP_STALE_US with a contact or the button held, everything
 * is dropped (no tap).  Returns 1 and fills `out` with an all-up report when it did.
 */
int tp_idle(struct tp_state *s, u64 now, struct tp_report *out);

/* Forget every contact and the button (a latch, guest entry or exit); returns the all-up report. */
void tp_release(struct tp_state *s, struct tp_report *out);

#endif
