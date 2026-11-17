/* SPDX-License-Identifier: MIT */

#ifndef HV_AHCI_NVME_H
#define HV_AHCI_NVME_H

#include "types.h"

/*
 * D5: the carrier's AHCI disk on a dedicated GPT partition of the internal NVMe
 * (windows-m6/D5-NVME-DESIGN.md).  Windows still sees hv_ahci.c's emulated AHCI disk; this is
 * the backend glue between the pure backend (ahci_nvme.c), the pure GPT checks (gpt_check.c)
 * and aura's NVMe driver (nvme.c): attach, the controller lock, the exit and reset paths, the
 * status and the W0 gate test.
 *
 * Data safety (section 9): on this machine only a controller shutdown (CC.SHN normal, SHST
 * done) is trusted to keep written data across a reset.  Every exit the carrier controls closes
 * the backend with that shutdown before anything can reset the SoC:
 *   - hv_start() returns (PSCI SYSTEM_OFF/RESET, ^C + exit, refusals): hv_ahci_nvme_close();
 *   - flush_and_reboot() (panic, assert, EL2 exceptions, SError, watchdog, P_REBOOT):
 *     hv_ahci_nvme_reset_close(), which waits a bounded time for the controller lock and
 *     otherwise requests the shutdown directly; the caller resets only if it returns true.
 * A hard reset (macvdmtool, power-button hold, power loss) is outside the carrier: after a
 * session that wrote, it risks the Windows partition.  The launcher prints CLEAN or FAILED.
 */

#define HV_AHCI_NVME_REQ_MAGIC   "AWNVME01"
#define HV_AHCI_NVME_REQ_VERSION 1
#define HV_AHCI_NVME_NAME_UNITS  36

/* ATTACH_NVME flags. */
#define HV_AHCI_NVME_RW          BIT(0) /* writes reach the partition */
#define HV_AHCI_NVME_FLUSH_EVERY BIT(1) /* an NVMe flush after every write command */
#define HV_AHCI_NVME_GATE        BIT(2) /* no guest: STATUS, TEST and DETACH only (R0/W0) */
#define HV_AHCI_NVME_SCRATCH     BIT(3) /* GATE only: no disk image required (an empty one) */
#define HV_AHCI_NVME_FLAGS       0xfUL

/* The ATTACH_NVME request (P_HV_WINDOWS_AHCI op 5, a1 = its address), little-endian. */
struct hv_ahci_nvme_req {
    char magic[8];
    u32 version;
    u32 flags;
    u8 disk_guid[16]; /* on-disk GUID byte order */
    u8 part_guid[16];
    u8 type_guid[16];
    u64 first_lba, last_lba; /* NVMe (4 KiB) LBAs, inclusive */
    u64 min_bytes;           /* the inner disk must be at least this big (the image) */
    u64 reserved;
    u16 name[HV_AHCI_NVME_NAME_UNITS];
    u8 pad[0x18];
};
_Static_assert(sizeof(struct hv_ahci_nvme_req) == 0xc0, "request layout is launcher ABI");
_Static_assert(__builtin_offsetof(struct hv_ahci_nvme_req, name) == 0x60, "launcher ABI");

enum hv_ahci_nvme_state {
    HV_AHCI_NVME_DETACHED = 0,
    HV_AHCI_NVME_ATTACHED = 1,
    HV_AHCI_NVME_RUNNING = 2,
    HV_AHCI_NVME_CLEAN = 3,  /* closed: the controller confirmed the shutdown, or nothing written */
    HV_AHCI_NVME_FAILED = 4, /* written, and the shutdown was not confirmed: a reset may lose it */
};

/* NVME_STATUS indices (op 7). */
enum hv_ahci_nvme_status_index {
    HV_AHCI_NVME_S_STATE = 0,
    HV_AHCI_NVME_S_FLAGS = 1,
    HV_AHCI_NVME_S_COMMANDS = 2,
    HV_AHCI_NVME_S_DEV_COMMANDS = 3,
    HV_AHCI_NVME_S_LONGEST_US = 4,
    HV_AHCI_NVME_S_FLUSHES = 5,
    HV_AHCI_NVME_S_RMW = 6,
    HV_AHCI_NVME_S_GUARD_HITS = 7,
    HV_AHCI_NVME_S_FIRST = 8,
    HV_AHCI_NVME_S_LAST = 9,
    HV_AHCI_NVME_S_WROTE = 10,
    HV_AHCI_NVME_S_FAILED_COMMANDS = 11,
    HV_AHCI_NVME_S_BLOCKS_READ = 12,
    HV_AHCI_NVME_S_BLOCKS_WRITTEN = 13,
    HV_AHCI_NVME_S_INNER_BYTES = 14,
    HV_AHCI_NVME_S_LONGEST_FLUSH_US = 15,
};

/* NVME_TEST ops (op 8, gate only). */
#define HV_AHCI_NVME_TEST_WRITE  1 /* SCRATCH + RW: write the seeded pattern */
#define HV_AHCI_NVME_TEST_VERIFY 2 /* read and compare with the seeded pattern: mismatches */
#define HV_AHCI_NVME_TEST_FLUSH  3
#define HV_AHCI_NVME_TEST_CRC    4 /* read: CRC-32 of the blocks (compare with Linux) */

struct ahci_nvme;

/*
 * Attach (proxy context, one CPU, before hv_start).  `have_ram`: guest RAM [ram_lo, ram_hi) is
 * set and the pool must lie outside it.  0 and *sectors (512-byte AHCI sectors), or -1 with the
 * reason printed; the controller is shut down again on every refusal.
 */
int hv_ahci_nvme_attach(u64 req_addr, bool have_ram, u64 ram_lo, u64 ram_hi, u64 *sectors);
/* The pure backend to put in struct ahci_env (opaque), or NULL when not attached. */
struct ahci_nvme *hv_ahci_nvme_backend(void);
bool hv_ahci_nvme_attached(void);
bool hv_ahci_nvme_gate(void);
bool hv_ahci_nvme_read_only(void);
enum hv_ahci_nvme_state hv_ahci_nvme_state(void);
/* hv_ahci_verify(): the backend is ready for a guest (attached, not gate, fence set). */
int hv_ahci_nvme_verify(void);
void hv_ahci_nvme_guest_entered(void);
/*
 * Close with the controller shutdown; idempotent.  Every hv_start() return, DETACH.  Returns
 * the state afterwards (CLEAN, FAILED, or DETACHED if never attached).
 */
enum hv_ahci_nvme_state hv_ahci_nvme_close(const char *why);
/*
 * flush_and_reboot(): may run on any CPU at any moment, inside the backend included.  True if
 * a reset cannot lose acknowledged data (closed CLEAN, nothing written, or no backend and
 * nvme_shutdown() succeeded); false: park.
 */
bool hv_ahci_nvme_reset_close(void);
/* proxy.c: the backend owns the controller (P_NVME_* refuse). */
bool hv_ahci_nvme_owns_controller(void);
/* proxy.c P_REBOOT: refused while a guest session runs or after FAILED (unless forced). */
bool hv_ahci_nvme_reboot_allowed(bool force);
u64 hv_ahci_nvme_status(u64 index);
u64 hv_ahci_nvme_test(u64 rel_block, u64 count, u64 seed, u64 op);
void hv_ahci_nvme_report(bool final);
void hv_ahci_nvme_fatal_report(void);

/* The W0 pattern word (the launcher's tools reproduce it to check from Linux). */
u64 hv_ahci_nvme_pattern(u64 seed, u64 rel_block, u32 word);

#endif
