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
bool display_needs_live_receiver(void);
int display_prepare_live_handoff(void);
bool display_handed_on_live(void);

#define DISPLAY_LIVE_ADT_PROP "aurora,dcp-retained-heap"
bool display_inherited_live_heap(u64 *start);

/*
 * The stage that hands a DCP on running passes what it knows of the DCP's RTKit session to the
 * next aura stage in /chosen of the private ADT copy, next to DISPLAY_LIVE_ADT_PROP: only that
 * stage negotiated the session, and the last stage has to describe it to the kernel
 * (kboot.c dt_publish_live_rtkit()). The property holds exactly one record. A receiver accepts
 * only the version and size it was built for; a layout change needs a new version.
 */
#define DISPLAY_LIVE_RTKIT_ADT_PROP  "aurora,dcp-live-rtkit"
#define DISPLAY_LIVE_RTKIT_VERSION   1
#define DISPLAY_LIVE_RTKIT_F_SESSION BIT(0) /* rtkit describes the session */

struct display_live_rtkit_record {
    u32 version; /* DISPLAY_LIVE_RTKIT_VERSION */
    u32 size;    /* of the whole record */
    u32 flags;
    u32 reserved;  /* zero */
    char alias[8]; /* DT alias of the DCP, NUL terminated */
    /* Buffer addresses: the DART input address (no asc-dram-mask) and the physical address */
    struct rtkit_live_state rtkit;
};

#define DISPLAY_LIVE_RTKIT_RECORD_SIZE 192

void display_live_rtkit_record(struct display_live_rtkit_record *rec);
int display_live_rtkit_state(const struct rtkit_live_state **st, const char **alias);

/* The second display's framebuffer (J873g HDMI next to USB-C), base 0 if none. */
extern struct boot_video display_second_video;

#endif
