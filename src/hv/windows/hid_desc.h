/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef HID_DESC_H
#define HID_DESC_H

#include "types.h"

/*
 * A HID report descriptor walker (HID 1.11 section 6.2.2: short items, global and local
 * state, Push/Pop, report IDs, Input main items as variable or array fields).  Pure; written
 * from the specification.  Only Input fields are kept; Output and Feature items advance nothing
 * the carrier reads.
 */

#define HID_DESC_MAX      4096u
#define HID_MAX_FIELDS    64u
#define HID_MAX_USAGES    16u /* explicit Usage items kept per field */
#define HID_MAX_REPORT    256u /* bytes of one input report, after the ID */
#define HID_STACK_DEPTH   4u

struct hid_field {
    u8 report_id;    /* 0 when the descriptor uses no report IDs */
    u16 bit;         /* offset in the report, after the ID byte */
    u8 size;         /* bits per element, 1..32 */
    u16 count;       /* elements */
    bool variable;   /* Input bit 1 */
    bool constant;   /* Input bit 0 */
    s32 lmin, lmax;  /* logical range */
    /* Usages: explicit ones (32-bit, page in the high half), then a Min/Max range. */
    u8 n_usages;
    u32 usages[HID_MAX_USAGES];
    bool has_range;
    u32 umin, umax;
};

struct hid_desc {
    bool uses_ids;
    u16 n_fields;
    struct hid_field f[HID_MAX_FIELDS];
    u16 input_bits[256]; /* per report ID */
    u32 skipped_long;    /* long items skipped */
};

enum hid_desc_result {
    HID_OK = 0,
    HID_E_LENGTH = -1,   /* empty, over HID_DESC_MAX, or an item runs past the end */
    HID_E_FIELDS = -2,   /* more than HID_MAX_FIELDS input fields */
    HID_E_STACK = -3,    /* Push/Pop or collections unbalanced or too deep */
    HID_E_FIELD = -4,    /* a report size of 0 or over 32, or a report over HID_MAX_REPORT */
    HID_E_ID = -5,       /* report ID 0, or fields both before and after the first ID */
};

int hid_desc_parse(struct hid_desc *d, const u8 *p, u32 len);

/*
 * The usage of element i of a variable field (HID 1.11 6.2.2.8: explicit usages in order, the
 * last one repeating, or Usage Minimum + i up to Usage Maximum), page in the high 16 bits;
 * 0 when there is none.
 */
u32 hid_field_usage(const struct hid_field *f, u32 i);

/* The usage an array element's value selects (value - logical min indexes the usages), or 0. */
u32 hid_array_usage(const struct hid_field *f, s32 value);

/* Element i of a field, read from report data (after the ID byte), sign-extended if lmin < 0. */
s32 hid_field_value(const struct hid_field *f, const u8 *data, u32 data_len, u32 i);

#endif
