/* SPDX-License-Identifier: MIT */

#ifndef HV_INPUT_H
#define HV_INPUT_H

#include "types.h"

/*
 * I0: keyboard and pointer for the J804 Windows guest, fed from this Mac.  The device model is
 * input_model.c; this is its carrier glue: the trapped register page, the virtual SPI, the
 * host feed on the RX direction of the vUART CDC pipe (IODEV_USB_VUART), and the proxy op the
 * launcher drives (proxyclient/aura/windows.py, tools/run_guest.py --windows-input).
 *
 * Contract (windows-m6/INPUT-DESIGN.md; Mu J804Windows.h must agree, docs/windows-t8152.md
 * "I0" lists the Mu side):
 *   page   IPA 0xF50000000, 16 KiB, hooked, never mapped;
 *   SPI    GSIV 991, level, active-high, no AIC line;
 *   feed   from ATTACH on, the vUART's RX bytes belong to this file: the guest's URXH reads
 *          empty and UTRSTAT/UFSTAT never show RX data.  The boot CPU's 5 kHz tick drains at
 *          most HV_INPUT_RX_BUDGET bytes per tick, right after hv_vuart_poll();
 *   Mu     /chosen/aura,windows-input-v1 (24 bytes, written by the launcher only after ATTACH
 *          returned 0) and the FD marker "J804INP1" at 0x140.
 *
 * ATTACH runs once per carrier session, before hv_start.  Sources: the host feed (bit 0,
 * always required), I1 the M6's internal keyboard (bit 1) and I2 its trackpad (bit 2, needs
 * the keyboard): with them, a2/a3 name the 8 MiB reserved MTP area (with the trackpad, the
 * launcher's parameter page and firmware payload are already in it) and ATTACH brings the MTP
 * up before returning (hv_mtp_t8152.c).  An MTP failure leaves the host source attached and
 * ATTACH still returns 0; FEATURES bits 17/18 (and STATUS 23/24) say what is live.  The
 * trackpad reports as the relative pointer (report ID 3).  a1 bit 8 selects the Fn default
 * "fkeys" (F1-F12 without Fn) instead of "media".
 */

#define HV_INPUT_BASE      0xf50000000UL
#define HV_INPUT_SIZE      0x4000UL
#define HV_INPUT_INTID     991u
#define HV_INPUT_VERSION   1
#define HV_INPUT_RX_BUDGET 256u
/* hv_input_guest_entered(): at most this many pre-entry bytes are discarded (the CDC ring). */
#define HV_INPUT_ENTRY_DRAIN_MAX 0x100000u
#define HV_INPUT_WATCHDOG_MS 2000u
#define HV_INPUT_ERROR     ((u64) - 1)

/* P_HV_WINDOWS_CAPS2 bit 3: input v1 (this file, P_HV_WINDOWS_INPUT, configure BIT(10)). */
#define HV_WINDOWS_CAPS2_INPUT BIT(3)

/* ATTACH a1: the sources. */
#define HV_INPUT_SRC_HOST     BIT(0)
#define HV_INPUT_SRC_KEYBOARD BIT(1) /* I1: the internal keyboard, needs the host source */
#define HV_INPUT_SRC_TRACKPAD BIT(2) /* I2: the internal trackpad, needs the keyboard */
#define HV_INPUT_SRC_SUPPORTED (HV_INPUT_SRC_HOST | HV_INPUT_SRC_KEYBOARD | HV_INPUT_SRC_TRACKPAD)
#define HV_INPUT_SRC_MASK      0xffUL
/* ATTACH a1 options above the sources. */
#define HV_INPUT_OPT_FN_FKEYS BIT(8) /* keyboard: F1-F12 without Fn, media keys with Fn */
#define HV_INPUT_OPT_MASK     HV_INPUT_OPT_FN_FKEYS

/* P_HV_WINDOWS_INPUT args[0]. */
enum hv_input_op {
    HV_INPUT_OP_QUERY = 0,  /* a1 = hv_input_query index */
    HV_INPUT_OP_ATTACH = 1, /* a1 = sources, a2/a3 = reserved area base/size (0 for host) */
    HV_INPUT_OP_STATUS = 2, /* a1 = hv_input_status index */
    HV_INPUT_OP_DETACH = 3, /* before guest entry only; hv_start then refuses */
    HV_INPUT_OP_PROBE = 4,  /* before entry, keyboard live: service and log for a1 ms (<= 10 s);
                               returns the frames handled */
};

enum hv_input_query {
    HV_INPUT_Q_VERSION = 0,
    HV_INPUT_Q_BASE = 1,
    HV_INPUT_Q_SIZE = 2,
    HV_INPUT_Q_INTID = 3,
    HV_INPUT_Q_FEATURES = 4, /* report bits | supported source bits << 16 */
    HV_INPUT_Q_SOURCES = 5,  /* ATTACH source bits this carrier accepts */
};

/* STATUS: 0..15 are the model's INPUT_ST_* counters; the rest below. */
enum hv_input_status {
    HV_INPUT_S_ATTACHED = 16, /* attached source mask, 0 when not attached */
    HV_INPUT_S_CTRL = 17,     /* the driver's CTRL */
    HV_INPUT_S_DEPTH = 18,    /* queued reports */
    HV_INPUT_S_DROPPED = 19,  /* DROPPED (since ENABLE) */
    HV_INPUT_S_LEDS = 20,     /* the last keyboard LED output report */
    HV_INPUT_S_LINE = 21,     /* the SPI level now */
    HV_INPUT_S_OWNS_RX = 22,  /* 1 while the vUART RX belongs to the host feed */
    /* I1, the MTP (hv_mtp_t8152.c; 0 without the internal keyboard): */
    HV_INPUT_S_MTP_STATE = 23,  /* enum mtp_state: furthest step, | 0x80 when it stopped there */
    HV_INPUT_S_MTP_REASON = 24, /* enum mtp_reason | trackpad reason << 8 | trackpad step << 16 */
    HV_INPUT_S_MTP_KBD = 25,    /* keyboard reports | bad reports << 32 */
    HV_INPUT_S_MTP_TP = 26,     /* trackpad frames | bad frames << 32 (I2) */
    HV_INPUT_S_MTP_RTKIT = 27,  /* RTKit app messages | doorbells << 32 */
    HV_INPUT_S_MTP_POLL = 28,   /* longest service call, us */
    HV_INPUT_S_MTP_ERRORS = 29, /* checksum | ring << 16 | oversize << 32 | parse << 48 */
    HV_INPUT_S_MTP_SMC = 30,    /* SMC pcIO writes made | failed << 16 (0 or 2; 1 = left in reset) */
    HV_INPUT_S_MTP_GPIO = 31,   /* GPIO requests refused */
};

/* ATTACH results (negative; 0 is success).  A refusal changes nothing. */
enum hv_input_err {
    HV_INPUT_E_STATE = -1,   /* not configured, not T8152/J804, guest started, or an earlier
                                ATTACH this session */
    HV_INPUT_E_SOURCES = -2, /* empty mask, unknown bits, or a source this carrier lacks */
    HV_INPUT_E_AREA = -3,    /* a reserved area given for the host source alone */
    HV_INPUT_E_HOOK = -4,    /* the page could not be hooked */
};

/* hv_windows_configure(HV_WINDOWS_T8152_INPUT). */
void hv_input_enable(void);
bool hv_input_enabled(void);

/* P_HV_WINDOWS_INPUT.  Returns the op's value, or HV_INPUT_ERROR / a negative hv_input_err. */
u64 hv_input_proxy(u64 op, u64 a1, u64 a2, u64 a3);

/*
 * hv_start(), after hv_t8152_verify(): with the configure flag set, refuse unless ATTACH
 * succeeded and the page is wholly this file's hook.  0, or -1 with the reason.
 */
int hv_input_verify(void);
/* Just before entry: discard host bytes buffered since ATTACH, start the parser clean. */
void hv_input_guest_entered(void);
/* hv_start() returned: every host key and button up (state only, the vSPI is not driven). */
void hv_input_guest_exited(int status);

/* The boot CPU's host tick: service the MTP (1 kHz floor), drain the host feed (bounded), run
 * the watchdog, then drive the SPI. */
void hv_input_poll(void);

/* The unlocked WFI path: service the MTP (rate-limited, one servicer); never drives the SPI. */
void hv_input_idle_poll(void);

/* hv_mtp_t8152.c: the internal keyboard's new state (modifiers, bitmap, consumer usage). */
void hv_input_internal_keyboard(u8 mods, const u8 keys[20], u16 consumer);
/* hv_mtp_t8152.c: one trackpad pointer report (button state, deltas). */
void hv_input_internal_pointer(u8 buttons, s16 dx, s16 dy, s8 wheel, s8 pan);

/* hv_vuart.c: does the host feed own the vUART's RX direction now? */
bool hv_input_owns_vuart_rx(void);

/* The 1 Hz HV report: one line when the counters moved. */
void hv_input_report(void);
/* G5b status block: host frames ok and bad so far (relaxed reads, no lock). */
void hv_input_counts(u32 *ok, u32 *bad);

/*
 * The EL2 fatal report's input part (guest SError attribution): attach state, the counters
 * (read without the lock: another CPU may hold it), the guest's page accesses with the last
 * one, and the last host-feed poll (the boot CPU's read of the proxy's USB CDC link).
 */
void hv_input_fatal_report(void);

#endif
