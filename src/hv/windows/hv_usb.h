/* SPDX-License-Identifier: MIT */

#ifndef HV_USB_H
#define HV_USB_H

#include "types.h"

/*
 * D4: USB keyboard and mouse for the J804 Windows guest.  The carrier passes the usb-drd1
 * (left-rear USB-C) dwc3 xHCI registers through to the guest and owns everything around them:
 *
 *   MMIO   [xhci, xhci + 0x8000) only, identity, Device-nGnRnE (hv_t8152_usb_allow); the dwc3
 *          globals, PIPE, PHY, evt, hpm, SPMI, pmgr, the DARTs and the TLBI aperture stay
 *          denied to stage 2;
 *   IRQ    AIC 1692 published as SPI 995 (level), owned only while attached; drd3 1834 -> 997;
 *   DART   both dart-usb1 instances, streams (0,0), (0,1), (1,1), TCR 0x9 (four-level, ias 42),
 *          one shared identity table set mapping exactly guest RAM [ram_start, ram_end)
 *          (hv_usb_dart.c), built here, invisible to the guest; DART faults are polled;
 *   state  host mode, PHY and VBUS as aura's j804_usb_prep() left them, verified, never
 *          re-initialised.  The only write outside the DARTs and the AIC masks is clearing the
 *          two SUSPHY bits if they are set.
 *
 * usb-drd0 (the proxy's CDC link) is never touched: ATTACH refuses it by mask bit and by
 * address.  Contract: windows-m6/D4-USB-DESIGN.md; Mu J804Windows.h and its gated SSDT
 * J804XHC1 (\_SB.XHC1) must agree: FD marker "J804USB1" at 0x120, and the launcher's 40-byte
 * /chosen/aura,windows-usb-v1 handshake (written only after ATTACH succeeded, DMA window from
 * HV_USB_S_DMA_BASE/TOP) without which Mu publishes no XHC1.
 *
 * ATTACH runs once per carrier session, before hv_start; DETACH is refused while a started
 * guest session holds the controller (hv_start's own exit tears it down).
 */

/* P_HV_WINDOWS_CAPS2 bit 1: USB passthrough v1 (this file, P_HV_WINDOWS_USB, BIT(9)). */
#define HV_WINDOWS_CAPS2_USB BIT(1)

#define HV_USB_PORT_DRD1 BIT(1)
#define HV_USB_PORT_DRD3 BIT(3)
#define HV_USB_PORTS     (HV_USB_PORT_DRD1 | HV_USB_PORT_DRD3)
#define HV_USB_XHCI_SIZE 0x8000UL
#define HV_USB_GSIV_DRD1 995u
#define HV_USB_GSIV_DRD3 997u
#define HV_USB_ERROR     ((u64) - 1)
/* F10: more level interrupts than this per second means the line never drops. */
#define HV_USB_STORM_PER_SEC 50000u

/* P_HV_WINDOWS_USB args[0]. */
enum hv_usb_op {
    HV_USB_OP_ATTACH = 1, /* args[1] = port mask (bit 1 drd1, bit 3 drd3) */
    HV_USB_OP_STATUS = 2, /* args[1] = port (1 or 3), args[2] = hv_usb_status index */
    HV_USB_OP_DETACH = 3,
};

/* STATUS indices: live while attached, else the values recorded at teardown. */
enum hv_usb_status {
    HV_USB_S_ATTACHED = 0,   /* attached port mask (any port) */
    HV_USB_S_USBSTS = 1,
    HV_USB_S_PORTSC1 = 2,
    HV_USB_S_PORTSC2 = 3,
    HV_USB_S_IRQS = 4,       /* AIC deliveries of the port's line to the guest */
    HV_USB_S_DART0_EXC = 5,  /* exception bitmaps seen (OR of every poll) */
    HV_USB_S_DART1_EXC = 6,
    HV_USB_S_GCTL = 7,
    HV_USB_S_UNOWNED = 8,    /* hardware lines masked for want of an owned alias (any port) */
    HV_USB_S_STORM = 9,      /* 1 if the storm guard took the line away */
    /*
     * The DART identity window [base, top) of the current attach (any port), for the
     * launcher's /chosen/aura,windows-usb-v1 handshake; HV_USB_ERROR when nothing is attached.
     */
    HV_USB_S_DMA_BASE = 10,
    HV_USB_S_DMA_TOP = 11,
};

/*
 * ATTACH results (negative; 0 is success).  Every refusal before the commit writes nothing;
 * a failure during the commit tears down what it did.
 */
enum hv_usb_err {
    HV_USB_E_STATE = -1,    /* not configured, not J804, no guest RAM, guest started, or an
                               ATTACH already reached the commit this session */
    HV_USB_E_PORTS = -2,    /* empty mask, drd0 or an unknown port */
    HV_USB_E_PORT_UP = -3,  /* F1: the port is not in host mode as j804_usb_prep left it */
    HV_USB_E_ADT = -4,      /* nodes, regs or interrupts not what this carrier expects */
    HV_USB_E_DART = -5,     /* F2/F3: DART protected, live, or not the Gen3 USB DART */
    HV_USB_E_XHCI = -6,     /* F4: running, CNR, DbC, or registers outside 0x8000 */
    HV_USB_E_TABLES = -7,   /* F5/F6: no room, placement or self check */
    HV_USB_E_COMMIT = -8,   /* F7: readback, TLBI or stage 2 failed (torn down again) */
};

/* hv_windows_configure(HV_WINDOWS_T8152_USB). */
void hv_usb_enable(void);
bool hv_usb_enabled(void);

/* P_HV_WINDOWS_USB. */
u64 hv_usb_proxy(u64 op, u64 a1, u64 a2);

/*
 * Stop the controller, take the DART streams down, unmap and deny the xHCI window again and
 * free the tables.  Idempotent; every exit of hv_start() calls it.
 */
void hv_usb_teardown(const char *why);

/*
 * The last-resort form for the reset path (flush_and_reboot): no locks, no stage-2 or heap
 * work; masks the line, halts the controller (bounded) and disables the streams.
 */
void hv_usb_emergency_stop(void);

/* The T8152 IRQ path delivered an owned line to the guest. */
void hv_usb_note_irq(u32 physical);

/* 1 Hz, from the host tick's report: USBSTS/PORTSC, DART exceptions, dwc3 drift, storms. */
void hv_usb_report(void);

/* The EL2 fatal report's USB part: recorded values only, no MMIO. */
void hv_usb_fatal_report(void);

/* hv_exc.c: hardware lines masked by the T8152 IRQ path for want of an owned alias. */
u64 hv_t8152_unowned_irqs(void);

#endif
