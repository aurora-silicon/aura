/* SPDX-License-Identifier: MIT */

#ifndef MTP_BRINGUP_H
#define MTP_BRINGUP_H

#include "input_kbd.h"
#include "input_tp.h"
#include "mtp_dart_t8152.h"
#include "mtp_scm.h"
#include "types.h"

/*
 * I1/I2: bring the T8152 MTP up to its keyboard and trackpad, then service it.  Pure: every hardware access goes
 * through struct mtp_ops, which hv_mtp_t8152.c implements with allow-listed MMIO and
 * tests/e2e/windows_mtp_bringup_host.c with a fake MTP.  Sequence (windows-m6/INPUT-I1I2-BUILD.md
 * 2.3, the order our Linux driver measured):
 *
 *   power and  the 13 PMGR group-1 MTP states read first.  All on: ASC_CPU_CONTROL is read
 *   RUN rule   and bit 4 (RUN) set is refused with nothing written.  Any off: the MTP cannot be
 *              running, so those states are switched on (parent first, each polled to "on" for
 *              at most 10 ms) and RUN is read after them; set then, it is still refused.
 *              ASC_CPU_CONTROL is never read with a domain off (an unpowered ASC faults).
 *              Only a reboot through iBoot gives a fresh MTP.
 *   DART       TTBR[4] valid, TCR[4] == 9, the root accepted by the glue; the rings and the
 *              RTKit pool mapped under that root (mtp_dart_t8152.c), checked by a software walk
 *   RTKit      the glue boots it (RUN, HELLO, endpoints, power) with the premapped pool and
 *              starts endpoint 0x20
 *   SCM        power 2, wait for 0xfe, register the TX, RX and wake rings, 0xff
 *   INIT       the interface table; the "keyboard" interface's HID descriptor gives the layout
 *              (an interface is complete with its last packet and a descriptor: one whose last
 *              packet carried none is waited for, as Linux does on ring transports)
 *   keyboard   enable it ({0xb4, iface}), wait for the acknowledgement (return code 0) and READY
 *   trackpad   (I2, only when asked and the launcher's parameter page and firmware check out)
 *              enable "multi-touch"; patch the firmware's interface byte; AFE reset asserted
 *              (ops->afe_reset, the SMC's pcIO key: the glue's only SMC write), 50 ms; the
 *              firmware (0x95, its buffer mapped in stream 4 with the rings); power method 2
 *              "on" announced; AFE reset released, 50 ms; "on" confirmed; READY; then the
 *              optional 0xD9 geometry
 *
 * Every wait is bounded; a failure stops the bring-up with a step and a reason, and touches
 * nothing more.  A trackpad failure leaves the keyboard up (its own reason, tp_reason); after
 * the assert it leaves the AFE in reset (the safe state: a release during an upload crashed the
 * MTP on Linux) and makes no further SMC call.  Run time: mtp_bringup_poll() services RTKit (at
 * most 4 messages) and the rings (at most `max_frames`), never waits, and latches a failure
 * (RTKit, ring indices, 3 checksum errors in a row) after reporting the keyboard and the
 * pointer all up once.  The run time never waits: a doorbell that finds the mailbox full is
 * retried on the next poll (MTP_E_SCM_SEND once it has been owed MTP_T_SEND_MS).
 */

/* The reserved MTP area (carved by the launcher off the top of the guest RAM window). */
#define MTP_AREA_SIZE      0x800000ull
#define MTP_AREA_ALIGN     0x200000ull
#define MTP_AREA_PARAMS    0x000000u /* launcher parameter page (I2) */
#define MTP_AREA_STATE     0x004000u /* carrier state record (diagnostics) */
#define MTP_AREA_TABLES    0x010000u /* 16 DART table pages */
#define MTP_AREA_TX        0x100000u
#define MTP_AREA_RX        0x300000u
#define MTP_AREA_WAKE      0x500000u
#define MTP_AREA_POOL      0x504000u
#define MTP_AREA_POOL_SIZE 0x100000u
#define MTP_AREA_FW        0x700000u /* I2: the multi-touch firmware payload */
#define MTP_AREA_FW_SIZE   0x20000u  /* 128 KiB, mapped whole */
#define MTP_AREA_RINGS_END 0x504000u /* [TX, RINGS_END) is the device-written span */

/* T8152 / J804 facts (ADT /arm-io/mtp, /arm-io/dart-sbr, mapper-mtp reg 4; Linux DT). */
#define MTP_T8152_ASC           0x314600000ull
#define MTP_T8152_ASC_CPU_CTRL  (MTP_T8152_ASC + 0x44)
#define MTP_T8152_RUN           0x10u
#define MTP_T8152_DART          0x318100000ull
#define MTP_T8152_SID           4u
#define MTP_T8152_DART_TCR      (MTP_T8152_DART + 0x1000 + 4 * MTP_T8152_SID)
#define MTP_T8152_DART_TTBR     (MTP_T8152_DART + 0x1400 + 4 * MTP_T8152_SID)
#define MTP_T8152_TCR           9u
#define MTP_T8152_TTBR_VALID    1u
#define MTP_T8152_TTBR_ADDR     0x3ffffffcu /* bits 29:2: the root's PA >> 12 */
#define MTP_T8152_PMGR_PS       0x30828c000ull
#define MTP_T8152_PMGR_COUNT    13u
#define MTP_T8152_PMGR_STRIDE   0x20u
#define MTP_T8152_DVA_BASE      0x10040000000ull
#define MTP_T8152_DVA_WINDOW    0x10000000ull /* what this side hands out (256 MiB) */

/* PMGR power-state register fields (aura's pmgr.c). */
#define MTP_PS_TARGET      0xfu
#define MTP_PS_ACTUAL(v)   (((v) >> 4) & 0xfu)
#define MTP_PS_ON          0xfu
#define MTP_PS_KEEP        (~(0x10000000u | 0x200u | 0x100u | 0xfu)) /* auto, was-gated, target */

/*
 * The launcher's parameter page (area + MTP_AREA_PARAMS, little-endian; I2 only):
 *   +0  "AWMTPP01"     +8  u32 version 1      +12 u32 length 64
 *   +16 u32 firmware offset in the area (MTP_AREA_FW)   +20 u32 firmware size (payload bytes)
 *   +24 u32 CRC-32 (zlib) of the payload as written     +28 u32 interface-byte offset (0: none)
 *   +32 u32 flags: bit 0 tap-to-click, bit 1 natural scrolling
 *   +36 u16 gain (Q8.8 counts/mm)  +38 u16 acceleration (Q8.8)  +40 u16 accel low (mm/s)
 *   +42 u16 accel high (mm/s)      +44 u16 scroll travel per detent (um)
 *   +46 u16 tap time (ms)          +48 u16 tap travel (um)      +50 u16 tap quiet after a key (ms)
 *   +52 u16 click settle (ms)      +54 u16 palm touch major (0: off)
 *   +56 u32 reserved (0)           +60 u32 CRC-32 (zlib) of bytes 0..59
 */
#define MTP_PARAMS_MAGIC   "AWMTPP01"
#define MTP_PARAMS_VERSION 1u
#define MTP_PARAMS_LEN     64u
#define MTP_PARAMS_TAP     BIT(0)
#define MTP_PARAMS_NATURAL BIT(1)

struct mtp_params {
    u32 fw_size, fw_crc, fw_iface_off;
    struct tp_config tp;
};

/* zlib's CRC-32. */
u32 mtp_crc32(u32 crc, const u8 *p, u32 len);
/* Check the page (and nothing else); MTP_E_NONE or MTP_E_TP_PARAMS. */
int mtp_params_parse(const u8 *page, struct mtp_params *out);

/* Bounds (ms). */
#define MTP_T_PMGR_MS   10u
#define MTP_T_RTKIT_MS  500u
#define MTP_T_SCM_MS    200u
#define MTP_T_SEND_MS   50u
#define MTP_T_INIT_MS   500u
#define MTP_T_CMD_MS    1000u
#define MTP_T_READY_MS  1000u
#define MTP_T_AFE_MS    50u   /* after the AFE reset is asserted, and after it is released */
#define MTP_T_GEOM_MS   200u

/* STATUS index 23 (hv_input.h): the furthest step, | 0x80 when it failed there. */
enum mtp_state {
    MTP_ST_OFF = 0,
    MTP_ST_IDENTITY = 1,
    MTP_ST_POWER = 2, /* the domains on and RUN read clear */
    MTP_ST_DART = 3,  /* the stream-4 entries are in place */
    MTP_ST_RTKIT = 4,
    MTP_ST_SCM = 5,
    MTP_ST_INIT = 6,
    MTP_ST_KBD_READY = 7,
    MTP_ST_TP_READY = 8, /* I2 */
    MTP_ST_FAILED = 0x80,
};

/* STATUS index 24. */
enum mtp_reason {
    MTP_E_NONE = 0,
    MTP_E_IDENTITY = 1,    /* ADT/chip/board (the glue) */
    MTP_E_AREA = 2,        /* the reserved area (the glue) */
    MTP_E_RUNNING = 3,     /* RUN already set */
    MTP_E_TTBR = 4,
    MTP_E_TCR = 5,
    MTP_E_ROOT = 6,        /* the glue refused the root's address */
    MTP_E_MAP = 7,         /* the stream-4 tables (foreign entry, valid leaf, walk mismatch) */
    MTP_E_POWER = 8,       /* a PMGR state did not reach on */
    MTP_E_RTKIT = 9,       /* RTKit boot or endpoint 0x20 */
    MTP_E_SCM_SEND = 10,   /* the mailbox stayed full */
    MTP_E_SCM_TIMEOUT = 11,/* no 0xfe */
    MTP_E_INIT_TIMEOUT = 12,
    MTP_E_LAYOUT = 13,     /* the keyboard descriptor did not map */
    MTP_E_TX = 14,         /* the TX ring refused a command */
    MTP_E_KBD_ACK = 15,    /* no acknowledgement for the enable */
    MTP_E_KBD_ENABLE = 16, /* acknowledged with a non-zero return code */
    MTP_E_KBD_READY = 17,
    MTP_E_RTKIT_FAILED = 18, /* run time: RTKit service latched */
    MTP_E_RING = 19,         /* run time or INIT: ring indices or framing */
    MTP_E_CSUM = 20,         /* run time: 3 checksum errors in a row */
    /* I2, the trackpad (STATUS 24 bits 15:8; the keyboard stays up): */
    MTP_E_TP_PARAMS = 21,   /* the parameter page or the firmware's CRC */
    MTP_E_TP_INIT = 22,     /* no complete "multi-touch" INIT */
    MTP_E_TP_ENABLE = 23,   /* the enable was not acknowledged with 0 */
    MTP_E_TP_SMC = 24,      /* the AFE reset write failed (no further SMC call) */
    MTP_E_TP_FW = 25,       /* the firmware was not acknowledged with 0 */
    MTP_E_TP_POWER = 26,    /* a power method 2 step was not acknowledged with 0 */
    MTP_E_TP_READY = 27,
    MTP_E_TP_IDENTITY = 28, /* the glue: no multi-touch nub or not the expected AFE function */
};

struct mtp_ops {
    void *ctx;
    u64 (*now_us)(void *ctx);
    /* MMIO the glue allows: ASC_CPU_CONTROL, DART TCR[4]/TTBR[4] (read); PMGR PS (read/write). */
    u32 (*read32)(void *ctx, u64 addr);
    void (*write32)(void *ctx, u64 addr, u32 value);
    /* The DART root and this side's table pages (cache maintenance is the glue's). */
    struct mtp_dart_mem dart_mem;
    void (*dart_sync)(void *ctx);
    bool (*root_ok)(void *ctx, u64 root);
    /* Boot RTKit with the premapped pool at pool_dva and start endpoint 0x20; bounded. */
    bool (*rtkit_start)(void *ctx, u64 pool_dva, u32 timeout_ms);
    /* Quiet service of at most `max` messages; endpoint 0x20 ones reach mtp_bringup_app_msg().
     * < 0 once it latched a failure. */
    int (*rtkit_service)(void *ctx, int max);
    /* One endpoint 0x20 message, never waiting; false when the mailbox is full. */
    bool (*scm_send)(void *ctx, u64 msg);
    /* The internal keyboard's new state. */
    void (*keyboard)(void *ctx, u8 mods, const u8 keys[KBD_BITMAP_BYTES], u16 consumer);
    /* I2: one relative pointer report from the trackpad. */
    void (*pointer)(void *ctx, const struct tp_report *r);
    /* I2: the trackpad AFE reset line (true: asserted); false when the write failed. */
    bool (*afe_reset)(void *ctx, bool assert);
    /* I2: write the CPU's view of [pa, pa + size) back for the MTP (the firmware buffer). */
    void (*dma_sync)(void *ctx, u64 pa, u64 size);
    /* Optional: every well-formed frame (PROBE logging). */
    void (*frame)(void *ctx, const struct mtp_msg *m);
};

struct mtp_bringup {
    const struct mtp_ops *ops;
    u64 area_pa;
    u8 *area; /* the area as this side reads and writes it (rings) */
    enum kbd_fn_mode fn_mode;

    u8 state;
    u8 reason;
    u64 t_start;
    u32 step_ms[MTP_ST_TP_READY + 1]; /* time each step finished, from the start */

    u32 asc_ctrl, ttbr, tcr;
    u64 root;
    struct mtp_dart dart;
    u64 tx_dva, rx_dva, wake_dva, pool_dva, fw_dva;
    u32 ps_before[MTP_T8152_PMGR_COUNT];
    u32 pmgr_writes;
    bool run_read_powered; /* every domain was already on when RUN was read (nothing written) */

    struct mtp_ring tx, rx, wake;
    struct mtp_comm comm;
    int kbd_iface;
    struct kbd_layout kbd;
    struct kbd_state kst;
    bool kbd_live;

    /* I2 */
    bool want_tp, tp_live, tp_geometry_device;
    int tp_iface;
    u8 tp_reason;
    u8 tp_step; /* how far the trackpad got (1 enable .. 7 READY, 8 geometry) */
    struct mtp_params params;
    struct tp_state tp;
    u32 afe_writes, afe_failed;
    u32 tp_frames, tp_bad, tp_reports, tp_other;

    struct mtp_cmd cmd;
    u64 cmd_sent_us;
    u8 seq[MTP_MAX_IFACES]; /* each interface's command sequence */
    u32 cmd_timeouts;
    bool cmd_waiting;   /* command() is reading its acknowledgement: the slot is held */
    bool doorbell_owed; /* run time: a command is on the ring, its doorbell not yet rung */
    u64 doorbell_since;
    u32 doorbells_owed;

    bool iop_ready;
    u32 doorbells, app_other, scm_sends, scm_busy;

    bool gpio_pending;
    u8 gpio_req[16];
    u16 gpio_len;
    u32 gpio_refused, gpio_dropped;

    u32 frames, kbd_reports, kbd_bad, other_frames, notifications;
    u32 csum_errors, parse_errors, csum_run, ring_errors, oversize;
    bool csum_tripped; /* 3 checksum errors in a row were seen */
    u32 polls;

    u8 frame_buf[MTP_FRAME_MAX];
    u8 tx_buf[64];
};

/*
 * Bring-up, before guest entry.  The area's rings and table pages must already be zero; with
 * `want_tp` the launcher's parameter page and firmware payload are in the area.  Returns
 * MTP_E_NONE when the keyboard is up (the trackpad's result is tp_live / tp_reason), or the
 * reason it stopped (b->state then has MTP_ST_FAILED set).
 */
int mtp_bringup_attach(struct mtp_bringup *b, const struct mtp_ops *ops, u64 area_pa,
                       void *area, enum kbd_fn_mode fn_mode, bool want_tp);

/* The RTKit app handler's message (from inside ops->rtkit_service). */
void mtp_bringup_app_msg(struct mtp_bringup *b, u8 ep, u64 msg);

/*
 * Run time: service RTKit and both rings once.  The number of frames handled, or -1 when the
 * keyboard is not live (never brought up, or latched now or earlier).
 */
int mtp_bringup_poll(struct mtp_bringup *b, u32 max_frames);

#endif
