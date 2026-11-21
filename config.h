/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#ifndef CONFIG_H
#define CONFIG_H

// Enable framebuffer console
#define USE_FB
// Disable framebuffer console unless verbose boot is enabled
//#define FB_SILENT_MODE

// Minimal build for bring-up
//#define BRINGUP

// Print RTKit logs to the console
//#define RTKIT_SYSLOG

// Target for device-specific debug builds
//#define TARGET T8103

// Change the 'channel' used by the dockchannel-uart iodev (if unset: 1)
// #define DOCKCHANNEL_UART_CHANNEL 0

#ifdef RELEASE
# define FB_SILENT_MODE
#endif

#endif
