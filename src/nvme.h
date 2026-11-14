/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#ifndef NVME_H
#define NVME_H

#include "types.h"

struct sart_entry;

bool nvme_init(void);
extern bool nvme_keep_running_for_linux;
bool nvme_shutdown(void);
/*
 * Post-M4: whether an earlier stage powered ANS off and passed its SART entries on (nvme.sart=)
 * for this stage to start it again, or passed none and this stage has not used NVMe, so ANS is
 * as the earlier stage left it (running, from a T8132 Stage 1 before the powered-off handoff).
 */
bool nvme_post_m4(void);
bool nvme_post_m4_passed_off(void);
bool nvme_post_m4_untouched(void);
int nvme_sart_entries(struct sart_entry *entries, int max, bool *off);
char *nvme_sart_var(void);
bool nvme_set_sart_var(const char *val);

bool nvme_flush(u32 nsid);
/* A command of this session failed or timed out: later ones fail until a restart. */
bool nvme_io_failed(void);
/* True once an I/O queue command was lost: callers keep their read buffers. */
bool nvme_dma_uncertain(void);
bool nvme_read(u32 nsid, u64 lba, void *buffer);
#define NVME_MAX_READ_BLOCKS 256
bool nvme_read_blocks(u32 nsid, u64 lba, void *buffer, u32 count);
/* One 4 KiB block from a 4 KiB aligned buffer: only the aura-try marker is written (trial.rs) */
bool nvme_write(u32 nsid, u64 lba, const void *buffer);

#endif
