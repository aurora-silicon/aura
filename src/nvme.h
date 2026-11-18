/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#ifndef NVME_H
#define NVME_H

#include "types.h"

struct sart_entry;

bool nvme_init(void);
extern bool nvme_keep_running_for_linux;
/* nvme_shutdown(): quiesce RTKit for an OS that wakes ANS as iBoot hands it over */
extern bool nvme_quiesce_for_next;
bool nvme_shutdown(void);
int nvme_cold_handoff(bool idle, bool no_probe);
/* Post-M4: how a stage that powered ANS off tells the next one to start it again. */
bool nvme_post_m4(void);
bool nvme_post_m4_passed_off(void);
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

/*
 * D5 (windows-m6/D5-NVME-DESIGN.md section 6): what the Windows carrier's AHCI-on-NVMe backend
 * (src/hv/windows/hv_ahci_nvme.c) needs.  Nothing here changes the behaviour of an aura stage
 * that does not call it.
 */
#define NVME_MAX_WRITE_BLOCKS 64
/*
 * `count` (1..64) 4 KiB blocks from a 4 KiB aligned buffer: one command (PRP list) once this
 * session's multi-block reads were verified, else one command per block.
 */
bool nvme_write_blocks(u32 nsid, u64 lba, const void *buffer, u32 count);
/* Identify Namespace (CNS 0) into a 4 KiB aligned 4 KiB buffer. */
bool nvme_identify_ns(u32 nsid, void *buffer);
/*
 * The I/O fence: once set, every read and write outside [first, last] of `nsid` is refused
 * before it reaches the controller, until nvme_clear_fence() or a successful nvme_shutdown().
 */
void nvme_set_fence(u32 nsid, u64 first, u64 last);
void nvme_clear_fence(void);
bool nvme_fence_active(void);
u64 nvme_fence_refusals(void);
/* Allocate the PRP list and the read self-check bounce now, not during a guest session. */
bool nvme_prepare_io(void);
/* The heap ranges the controller may DMA to (queues, TCBs, PRP list, bounce): [start, end). */
unsigned int nvme_dma_ranges(u64 (*ranges)[2], unsigned int max);
/* The next nvme_shutdown() does the normal CC.SHN shutdown even if this stage wrote nothing. */
void nvme_request_normal_shutdown(void);
/* Called from every poll loop (the carrier pets its watchdog); NULL by default. */
extern void (*nvme_poll_hook)(void);
/*
 * A reset is about to happen and nvme_shutdown() cannot run (another CPU owns the controller,
 * or a queue failed): request a normal shutdown with CC.SHN directly and wait for SHST done.
 * Returns true if the controller confirmed it (or holds no session).  Never touches the queues.
 * `mailbox` false when another CPU may still be inside a command: then only CSTS is polled and
 * the RTKit mailbox is left alone (one CPU at a time drives it).
 */
bool nvme_shutdown_for_reset(bool mailbox);
/* True while nvme_init() succeeded and no shutdown ended the session. */
bool nvme_session_live(void);
/* True after a setup or shutdown failure: every call refuses until a SoC reset. */
bool nvme_needs_reboot(void);
/* True once this image issued an NVMe write or flush (since it started, not per session). */
bool nvme_has_written(void);
/* This session's multi-block (PRP list) reads were verified and are in use. */
bool nvme_multi_io_verified(void);
/*
 * D5: while set, nvme_exec_command refuses new I/O commands (admin commands still run), so a
 * lock owner releases the controller within one command.  Cleared by nvme_init().
 */
void nvme_abort_io(bool on);
/*
 * The controller lock of D5: an owner-tagged CPU lock.  The carrier backend holds it around
 * every command it issues and the reset path takes it with a bounded wait.  `usec` 0 waits
 * forever.  Re-entry by the owner nests.
 */
bool nvme_lock_acquire(u32 usec);
void nvme_lock_release(void);
int nvme_lock_owner(void);

#endif
