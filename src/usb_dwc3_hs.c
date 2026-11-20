/* SPDX-License-Identifier: MIT */
#define USB_DWC3_SUPERSPEED        0
#define dwc3_dev                   dwc3_hs_dev
#define dwc3_dev_t                 dwc3_hs_dev_t
#define usb_dwc3_init              usb_dwc3_hs_init
#define usb_dwc3_shutdown          usb_dwc3_hs_shutdown
#define usb_dwc3_handle_events     usb_dwc3_hs_handle_events
#define usb_dwc3_can_read          usb_dwc3_hs_can_read
#define usb_dwc3_can_write         usb_dwc3_hs_can_write
#define usb_dwc3_getbyte           usb_dwc3_hs_getbyte
#define usb_dwc3_putbyte           usb_dwc3_hs_putbyte
#define usb_dwc3_read              usb_dwc3_hs_read
#define usb_dwc3_write             usb_dwc3_hs_write
#define usb_dwc3_queue             usb_dwc3_hs_queue
#define usb_dwc3_queue_nonblocking usb_dwc3_hs_queue_nonblocking
#define usb_dwc3_flush             usb_dwc3_hs_flush
#include "usb_dwc3_impl.inc"
