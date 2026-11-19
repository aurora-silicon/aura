/* SPDX-License-Identifier: MIT */

#ifndef INPUT_MODEL_H
#define INPUT_MODEL_H

#include "types.h"

/*
 * I0: the guest input device of the J804 Windows carrier, pure model (no system registers, no
 * locks, no I/O).  hv_input.c is its carrier glue; tests/e2e/windows_input_host.c replays it.
 *
 * The guest sees one 16 KiB register page (every access trapped, never mapped) and one level
 * SPI.  It holds a queue of HID input reports in the fixed layout of the Windows driver's own
 * report descriptor (windows-m6/INPUT-DESIGN.md section 1.5, from the HID 1.11 specification
 * and the HID Usage Tables; ours, not taken from any other driver):
 *
 *   ID 1 keyboard  22 bytes: id, modifier bits (usages 0xE0-0xE7), 160-bit bitmap of the
 *                  Keyboard/Keypad page usages 0x00-0x9F (bit n = usage n)
 *   ID 2 absolute   8 bytes: id, buttons 1-5, X u16, Y u16 (0..32767), wheel s8, AC Pan s8
 *   ID 3 relative   8 bytes: id, buttons 1-5, dX s16, dY s16 (-32767..32767), wheel, pan
 *   ID 4 consumer   3 bytes: id, one Consumer page usage u16 (0..0x3FF, 0 = none)
 *
 * Every report carries the full state of its collection (motion and wheel are deltas), so a
 * report that is dropped or coalesced never leaves a key or button down.
 *
 * Sources: the host feed (frames from this Mac on the vUART CDC pipe's RX direction, parsed
 * here), the internal keyboard (I1, translated by input_kbd.c) and the trackpad (I2).  Each
 * source keeps its own state.  The keyboard report is the bitwise OR of all sources; relative
 * pointer buttons are the OR of the sources' buttons, and each source's motion passes through;
 * the absolute pointer is the host's alone; the consumer usage is the newest non-zero one.
 */

/* ---- the register page (INPUT-DESIGN.md 1.3) ---------------------------------------------- */

#define INPUT_PAGE_SIZE 0x4000u
#define INPUT_ID_VALUE  0x4e49364du /* "M6IN" */
#define INPUT_VERSION   0x00010000u /* major 1, minor 0 */

#define INPUT_REG_ID         0x000u /* RO */
#define INPUT_REG_VERSION    0x004u /* RO */
#define INPUT_REG_FEATURES   0x008u /* RO */
#define INPUT_REG_CTRL       0x00cu /* RW */
#define INPUT_REG_STATUS     0x010u /* RO */
#define INPUT_REG_IRQ_STATUS 0x014u /* RO */
#define INPUT_REG_SEQ        0x018u /* RO */
#define INPUT_REG_DROPPED    0x01cu /* RO */
#define INPUT_REG_POP        0x020u /* WO */
#define INPUT_REG_REPORT_LEN 0x024u /* RO */
#define INPUT_REG_OUT_LEN    0x040u /* WO */
#define INPUT_REG_OUT_DATA   0x080u /* WO, 64 bytes */
#define INPUT_REG_REPORT     0x100u /* RO, 64 bytes */
#define INPUT_REG_STATS      0x180u /* RO, 16 x u32 */
#define INPUT_REG_END        0x200u /* 0x200..0x3FFF: RAZ/WI */

#define INPUT_FEAT_KEYBOARD  BIT(0)
#define INPUT_FEAT_ABSOLUTE  BIT(1)
#define INPUT_FEAT_RELATIVE  BIT(2)
#define INPUT_FEAT_CONSUMER  BIT(3)
#define INPUT_FEAT_PTP       BIT(4) /* I2; never set by this model */
#define INPUT_FEAT_SRC_HOST  BIT(16)
#define INPUT_FEAT_SRC_KBD   BIT(17)
#define INPUT_FEAT_SRC_TP    BIT(18)
#define INPUT_FEAT_REPORTS   (INPUT_FEAT_KEYBOARD | INPUT_FEAT_ABSOLUTE | INPUT_FEAT_RELATIVE | \
                            INPUT_FEAT_CONSUMER)

#define INPUT_CTRL_ENABLE   BIT(0)
#define INPUT_CTRL_IRQ_EN   BIT(1)
#define INPUT_CTRL_PTP_MODE BIT(2)
#define INPUT_CTRL_RESET    BIT(31) /* write-only action, reads 0 */
#define INPUT_CTRL_RW       (INPUT_CTRL_ENABLE | INPUT_CTRL_IRQ_EN | INPUT_CTRL_PTP_MODE)

#define INPUT_STATUS_PENDING     BIT(0)
#define INPUT_STATUS_OVERFLOW    BIT(1)
#define INPUT_STATUS_DEPTH_SHIFT 16

#define INPUT_OUT_FEATURE BIT(31)

/* STATS at 0x180 + 4 * index (also the glue's STATUS proxy op). */
enum input_stat {
    INPUT_ST_FRAMES_OK = 0,  /* host frames accepted (CRC good, payload valid) */
    INPUT_ST_BAD_CRC = 1,    /* host frames with a CRC mismatch */
    INPUT_ST_BAD_HEADER = 2, /* sync pair followed by an unknown type or len > 64 */
    INPUT_ST_BAD_PAYLOAD = 3,/* CRC good, payload invalid for its type: ignored */
    INPUT_ST_SKIPPED = 4,    /* bytes skipped while looking for a sync pair */
    INPUT_ST_SEQ_GAPS = 5,   /* host frames whose seq was not the previous + 1 */
    INPUT_ST_WATCHDOG = 6,   /* host keys/buttons released by the 2 s watchdog */
    INPUT_ST_REPORTS = 7,    /* reports queued */
    INPUT_ST_HIGH_WATER = 8, /* deepest queue */
    INPUT_ST_MTP = 9,        /* internal-source reports (I1/I2; 0 here) */
    INPUT_ST_OUT_OK = 10,    /* output reports taken (keyboard LEDs) */
    INPUT_ST_OUT_BAD = 11,   /* output/feature reports refused */
    INPUT_ST_HELLOS = 12,
    INPUT_ST_BAD_ACCESS = 13,/* MMIO accesses of a bad width or alignment (RAZ/WI) */
    INPUT_ST_RX_BYTES = 14,  /* host bytes fed to the parser */
    INPUT_ST_DROPPED = 15,   /* reports dropped by overflow, all time */
    INPUT_ST_COUNT = 16,
};

/* ---- reports ------------------------------------------------------------------------------- */

#define INPUT_RID_KEYBOARD 1u
#define INPUT_RID_ABSOLUTE 2u
#define INPUT_RID_RELATIVE 3u
#define INPUT_RID_CONSUMER 4u
#define INPUT_LEN_KEYBOARD 22u
#define INPUT_LEN_ABSOLUTE 8u
#define INPUT_LEN_RELATIVE 8u
#define INPUT_LEN_CONSUMER 3u
#define INPUT_REPORT_MAX   64u
#define INPUT_KBD_BITMAP   20u    /* 160 bits: usages 0x00-0x9F */
#define INPUT_ABS_MAX      32767u
#define INPUT_REL_MAX      32767
#define INPUT_WHEEL_MAX    127
#define INPUT_CONSUMER_MAX 0x3ffu
#define INPUT_BUTTONS_MASK 0x1fu
#define INPUT_LED_MASK     0x1fu  /* Num, Caps, Scroll, Compose, Kana */
#define INPUT_QUEUE_LEN    256u

/* ---- the host feed (INPUT-DESIGN.md 2.2), little-endian ------------------------------------ */
/*
 *  +0 u8 'M'  +1 u8 '6'  +2 u8 type  +3 u8 len (0..64)  +4 u16 seq  +6 payload[len]
 *  +6+len u32 CRC-32 (zlib) of bytes +2 .. +6+len-1
 */
#define INPUT_HOST_SYNC0       0x4du /* 'M' */
#define INPUT_HOST_SYNC1       0x36u /* '6' */
#define INPUT_HOST_HDR         6u
#define INPUT_HOST_CRC         4u
#define INPUT_HOST_MAX_PAYLOAD 64u
#define INPUT_HOST_MAX_FRAME   (INPUT_HOST_HDR + INPUT_HOST_MAX_PAYLOAD + INPUT_HOST_CRC)
#define INPUT_HOST_PROTO       1u
#define INPUT_HOST_MAX_KEYS    16u
#define INPUT_HOST_USAGE_MIN   0x04u
#define INPUT_HOST_USAGE_MAX   0x9fu

enum input_host_type {
    INPUT_HOST_HELLO = 0x01,       /* u16 version (1), u16 flags (0): host state all up */
    INPUT_HOST_KEYS = 0x02,        /* u8 modifiers, u8 n <= 16, u8 usage[n] (0x04..0x9F) */
    INPUT_HOST_ABS = 0x03,         /* u8 buttons, u16 x, u16 y, s8 wheel, s8 pan */
    INPUT_HOST_REL = 0x04,         /* u8 buttons, s16 dx, s16 dy, s8 wheel, s8 pan */
    INPUT_HOST_CONSUMER = 0x05,    /* u16 usage (0 = released) */
    INPUT_HOST_KEEPALIVE = 0x06,   /* refreshes the held-key watchdog */
    INPUT_HOST_RELEASE_ALL = 0x07, /* host keys and buttons up */
};

/* Sources.  Only the host source has the held-key watchdog: the others send full state. */
enum input_source {
    INPUT_SRC_HOST = 0,
    INPUT_SRC_INTERNAL = 1, /* the M6's own keyboard (I1) */
    INPUT_SRC_TRACKPAD = 2, /* the M6's own trackpad (I2) */
    INPUT_SRC_COUNT = 3,
};

struct input_report {
    u32 seq;
    u8 id;
    u8 len;
    u8 data[INPUT_REPORT_MAX]; /* data[0] = id */
};

struct input_source_state {
    u8 mods;
    u8 keys[INPUT_KBD_BITMAP];
    u8 abs_buttons;
    u16 abs_x, abs_y;
    bool abs_valid; /* an ABS state was ever given (a release keeps the position) */
    u8 rel_buttons;
    u16 consumer;
    u32 consumer_stamp; /* when this source's non-zero usage began (the newest one wins) */
};

struct input_model {
    u32 features;
    u64 watchdog_ticks;

    /* Registers. */
    u32 ctrl;
    bool overflow;
    u32 dropped; /* since ENABLE */
    u8 out_data[INPUT_REPORT_MAX];
    u8 leds;

    /*
     * Queue: a ring of slot numbers (q_order, a permutation of 0..255) over the report slots,
     * so dropping a report from the middle moves at most 255 bytes, never the reports.  The
     * head is the latched report.
     */
    struct input_report q[INPUT_QUEUE_LEN];
    u8 q_order[INPUT_QUEUE_LEN];
    u32 q_head, q_count;
    u32 next_seq;

    /* Sources, and the last state of each collection that was queued. */
    struct input_source_state src[INPUT_SRC_COUNT];
    u8 last_kbd[INPUT_LEN_KEYBOARD];
    u8 last_abs[INPUT_LEN_ABSOLUTE];
    u8 last_rel_buttons;
    u16 last_consumer;
    u32 consumer_clock;
    bool last_valid; /* last_* hold what the queue's end state is */

    /* Host parser. */
    u8 rx[INPUT_HOST_MAX_FRAME];
    u32 rx_len;
    u64 rx_last; /* time of the last host byte */
    u16 host_seq;
    bool host_seq_valid;
    u64 host_last; /* time of the last accepted host frame */

    u32 stats[INPUT_ST_COUNT];
};

/*
 * Reset everything.  `sources` is the INPUT_FEAT_SRC_* mask the carrier serves (FEATURES bits
 * 16-23); `watchdog_ticks` the held-key watchdog in the units of the `now` arguments.
 */
void input_model_init(struct input_model *m, u32 sources, u64 watchdog_ticks);

/* Feed host bytes (any split; frames may straddle calls).  `now` stamps accepted frames. */
void input_model_host_rx(struct input_model *m, const u8 *data, u32 len, u64 now);

/*
 * Time passes: a partial frame that has waited watchdog / 40 (50 ms) since the last byte is
 * given up byte by byte and re-scanned (a damaged length must not hold the frames behind it),
 * then the held-key watchdog.  Call at least every few ms while the guest runs.
 */
void input_model_poll(struct input_model *m, u64 now);

/*
 * One trapped access at `off` (< INPUT_PAGE_SIZE) of 1 << width bytes.  Only aligned 32- and
 * 64-bit accesses act; anything else reads 0, writes nothing, counts INPUT_ST_BAD_ACCESS and
 * returns false (the glue logs it once).  Offsets with no register are RAZ/WI (true).
 */
bool input_model_mmio(struct input_model *m, u64 off, int width, u64 *val, bool write);

/* The SPI level: ENABLE && IRQ_EN && a report is latched. */
bool input_model_line(const struct input_model *m);

/* Set one source's keyboard state (modifier bits, bitmap of usages 0x00-0x9F). */
void input_model_set_keyboard(struct input_model *m, u32 src, u8 mods,
                              const u8 keys[INPUT_KBD_BITMAP]);

/* Set one source's consumer usage (0 = released, else <= 0x3FF). */
void input_model_set_consumer(struct input_model *m, u32 src, u16 usage);

/*
 * One source's relative pointer: buttons (bits 0-4) are state, the rest are deltas
 * (-32767..32767, wheel and pan -127..127; out-of-range values are clamped).
 */
void input_model_set_relative(struct input_model *m, u32 src, u8 buttons, s16 dx, s16 dy,
                              s8 wheel, s8 pan);

/* All of one source's keys and buttons up (watchdog, RELEASE_ALL, carrier exit). */
void input_model_release(struct input_model *m, u32 src);

/*
 * Guest entry: the host parser forgets any partial frame and the last sequence number, and the
 * host source is released (state only before ENABLE).  The glue discards pre-entry bytes first.
 */
void input_model_host_restart(struct input_model *m);

/* Queue depth and the counters, for the glue's report and STATUS op. */
u32 input_model_depth(const struct input_model *m);

/* CRC-32 as zlib's crc32() (reflected 0xEDB88320, init and final XOR ~0). */
u32 input_model_crc32(u32 crc, const void *data, u32 len);

#endif
