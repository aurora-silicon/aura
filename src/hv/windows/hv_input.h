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
 * ATTACH runs once per carrier session, before hv_start.  Only the host source exists in this
 * carrier: the internal keyboard (bit 1) and trackpad (bit 2) are refused.
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
#define HV_INPUT_SRC_KEYBOARD BIT(1) /* I1: refused by this carrier */
#define HV_INPUT_SRC_TRACKPAD BIT(2) /* I2: refused by this carrier */
#define HV_INPUT_SRC_SUPPORTED HV_INPUT_SRC_HOST

/* P_HV_WINDOWS_INPUT args[0]. */
enum hv_input_op {
    HV_INPUT_OP_QUERY = 0,  /* a1 = hv_input_query index */
    HV_INPUT_OP_ATTACH = 1, /* a1 = sources, a2/a3 = reserved area base/size (0 for host) */
    HV_INPUT_OP_STATUS = 2, /* a1 = hv_input_status index */
    HV_INPUT_OP_DETACH = 3, /* before guest entry only; hv_start then refuses */
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

/* The boot CPU's host tick: drain the host feed (bounded), run the watchdog. */
void hv_input_poll(void);

/* hv_vuart.c: does the host feed own the vUART's RX direction now? */
bool hv_input_owns_vuart_rx(void);

/* The 1 Hz HV report: one line when the counters moved. */
void hv_input_report(void);

/*
 * The EL2 fatal report's input part (guest SError attribution): attach state, the counters
 * (read without the lock: another CPU may hold it), the guest's page accesses with the last
 * one, and the last host-feed poll (the boot CPU's read of the proxy's USB CDC link).
 */
void hv_input_fatal_report(void);

#endif
