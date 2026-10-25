/* SPDX-License-Identifier: MIT */

#ifndef DISPLAY_H
#define DISPLAY_H

#include "dcp.h"
#include "types.h"
#include "xnuboot.h"

typedef enum _dcp_shutdown_mode {
    DCP_QUIESCED = 0,
    DCP_SLEEP_IF_EXTERNAL = 1,
    DCP_SLEEP = 2,
} dcp_shutdown_mode;

extern bool display_is_external;

int display_init(void);
int display_start_dcp(void);
int display_configure(const char *config);
int display_configure_second(const char *config);
int display_shutdown(dcp_shutdown_mode mode);
const display_config_t *display_get_config(void);

/* The second display's framebuffer (J873g HDMI next to USB-C), base 0 if none. */
extern struct boot_video display_second_video;

#endif
