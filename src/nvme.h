/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#ifndef NVME_H
#define NVME_H

#include "types.h"

bool nvme_init(void);
extern bool nvme_adopt_live_session;
extern bool nvme_keep_running_for_linux;
bool nvme_shutdown(void);
bool nvme_has_live_post_m4_session(void);

bool nvme_flush(u32 nsid);
bool nvme_read(u32 nsid, u64 lba, void *buffer);
#define NVME_MAX_READ_BLOCKS 256
bool nvme_read_blocks(u32 nsid, u64 lba, void *buffer, u32 count);
/* One 4 KiB block from a 4 KiB aligned buffer: only the aura-try marker is written (trial.rs) */
bool nvme_write(u32 nsid, u64 lba, const void *buffer);

#endif
