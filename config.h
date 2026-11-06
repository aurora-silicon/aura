/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#ifndef CONFIG_H
#define CONFIG_H

#ifdef AURA_WINDOWS
#define ENABLE_VGIC_MODULE
#define ENABLE_NATIVE_AIC_PASSTHROUGH
#endif

// Enable framebuffer console
#define USE_FB
// Disable framebuffer console unless verbose boot is enabled
//#define FB_SILENT_MODE
// Initialize USB early and break into proxy if device is opened within this time (sec)
//#define EARLY_PROXY_TIMEOUT 5

// Minimal build for bring-up
//#define BRINGUP
// Disable display configuration / bringup on desktop devices
//#define NO_DISPLAY

// Print RTKit logs to the console
//#define RTKIT_SYSLOG

// Target for device-specific debug builds
//#define TARGET T8103
// Some devices like Apple TV HD use other uarts for debug console
//#define TARGET_BOARD 0x34

// Switch the DFU USB-C port to debugusb
// #define USE_DEBUG_USB

// Change the 'channel' used by the dockchannel-uart iodev (if unset: 1)
// #define DOCKCHANNEL_UART_CHANNEL 0

#ifdef RELEASE
# define FB_SILENT_MODE
#endif

#endif
