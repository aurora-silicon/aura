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
    /* T8152: display off, AFK endpoints stopped, RTKit left running for the next owner. */
    DCP_HANDOFF = 3,
} dcp_shutdown_mode;

extern bool display_is_external;

int display_init(void);
int display_start_dcp(void);
int display_configure(const char *config);
int display_configure_second(const char *config);
bool display_second_attempted(void);
int display_dcp_handoff_state(void);
u32 display_live_endpoints(const char *adt_path);
bool display_set_afk_live(const char *val);
char *display_afk_var(void);
int display_shutdown(dcp_shutdown_mode mode);
const display_config_t *display_get_config(void);
bool display_dart_tables_inherited(void);
bool display_handed_on_live(void);

/* The second display's framebuffer (J873g HDMI next to USB-C), base 0 if none. */
extern struct boot_video display_second_video;

#endif
