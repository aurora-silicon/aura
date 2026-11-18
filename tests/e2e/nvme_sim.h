/* SPDX-License-Identifier: MIT */
/*
 * D5 host tests: a simulated T8152 ANS NVMe controller (windows-m6/D5-NVME-DESIGN.md 14.1).
 *
 * Written for these tests from the NVMe base specification (CC, CSTS, AQA/ASQ/ACQ, admin and
 * I/O commands, PRP lists, completion queue phase) and the T8152 register layout aura's own
 * driver uses (secure queue registers at +0x4000, linear submission doorbells, NVMMU TCBs).
 * Nothing here comes from Apple code.
 *
 * Media has two layers, as the M6 behaves (memory: m6-hard-reset-loses-writes): `volatile`
 * holds every completed write; `durable` changes only when a normal shutdown (CC.SHN = 1)
 * reaches SHST = done.  sim_power_cut() drops `volatile`, as a SoC reset or power loss does.
 */
#ifndef NVME_SIM_H
#define NVME_SIM_H

#include <stdbool.h>

#include "types.h"

#define SIM_BASE  0x5f00000000ULL /* synthetic MMIO base: NVMe and NVMMU share it (T8152) */
#define SIM_BLOCK 4096u

struct sim_faults {
    u64 drop_completion_at; /* I/O command number (1-based) whose CQE never comes */
    u64 error_at;           /* I/O command number completed with an error status */
    u64 delay_at;           /* I/O command number completed late ... */
    u64 delay_us;           /* ... by this much simulated time */
    bool shst_stuck;        /* a normal shutdown never reaches SHST done */
    u32 shst_polls;         /* polls (CSTS reads) before SHST done; default 3 */
    bool flush_durable;     /* model a controller whose Flush is durable (not the M6) */
    u32 lbads;              /* Identify LBA data size shift; default 12 */
    bool identify_fails;
    bool quiesce_fails;   /* RTKit quiesce is not acknowledged */
    bool boot_fails;      /* RTKit boot fails with the ASC left running (a failed second wake) */
    bool prp_list_broken; /* PRP-list DMA lands every page after the first on list entry 0 */
};

struct sim_stats {
    u64 io_commands, admin_commands;
    u64 reads, writes, flushes, identifies;
    u64 blocks_written;
    u64 prp_in_guest;               /* invariant 1 violations: a PRP page inside guest RAM */
    u64 tcb_mismatch;               /* TCB and SQE disagree */
    u64 out_of_range;               /* I/O beyond the namespace */
    u64 after_reset;                /* commands issued after sim_reset_happened() */
    u64 io_min_block, io_max_block; /* I/O range touched since sim_track_io(true) */
    u64 boots, quiesces, shutdowns_done;
    u64 multi_writes; /* write commands of more than one block */
};

void sim_init(u64 nsze, const char *image);
void sim_set_guest(u64 lo, u64 hi);
void sim_track_io(bool on);
struct sim_faults *sim_faults(void);
const struct sim_stats *sim_stats(void);
/* Event log, one letter per event: E enable, D disable, S shutdown requested, C committed
 * (SHST done), W write, F flush, R read, I identify, X power cut / reset, Q quiesce, B boot. */
const char *sim_events(void);
void sim_event(char c);
/* Called from inside a doorbell, before the I/O command numbered `n` executes. */
void sim_set_hook(u64 n, void (*hook)(void));
/* The reset happened: snapshot `durable` for the checks, then drop `volatile`. */
void sim_reset_happened(void);
void sim_power_cut(void);
/* Set blocks [block, block + count) to `v` in both layers (an "empty" partition). */
void sim_fill(u64 block, u64 count, u8 v);
bool sim_enabled(void);
/* What a reader sees now (volatile over durable), and what survives a power cut. */
const u8 *sim_view(u64 block);
const u8 *sim_durable(u64 block);
const u8 *sim_snapshot(u64 block); /* durable at sim_reset_happened() */
u64 sim_nsze(void);
int sim_dump_durable(const char *path);

#endif
