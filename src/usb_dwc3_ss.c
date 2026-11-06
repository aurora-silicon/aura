/* SPDX-License-Identifier: MIT */
#define USB_DWC3_SUPERSPEED     1
#define dwc3_dev                dwc3_ss_dev
#define dwc3_dev_t              dwc3_ss_dev_t
#define usb_dwc3_init           usb_dwc3_ss_init
#define usb_dwc3_shutdown       usb_dwc3_ss_shutdown
#define usb_dwc3_handle_events  usb_dwc3_ss_handle_events
#define usb_dwc3_can_read       usb_dwc3_ss_can_read
#define usb_dwc3_can_write      usb_dwc3_ss_can_write
#define usb_dwc3_getbyte        usb_dwc3_ss_getbyte
#define usb_dwc3_putbyte        usb_dwc3_ss_putbyte
#define usb_dwc3_read           usb_dwc3_ss_read
#define usb_dwc3_write          usb_dwc3_ss_write
#define usb_dwc3_queue          usb_dwc3_ss_queue
#define usb_dwc3_flush          usb_dwc3_ss_flush
#define usb_dwc3_vuart_capacity usb_dwc3_ss_vuart_capacity
#define usb_dwc3_vuart_write    usb_dwc3_ss_vuart_write
#define usb_dwc3_vuart_poll     usb_dwc3_ss_vuart_poll
#include "usb_dwc3_impl.inc"
