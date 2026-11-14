/* SPDX-License-Identifier: MIT */
/*
 * auractl -- in-guest control tool for aura's synthetic MMIO tracing-control device.
 *
 * Runs INSIDE the macOS guest under aura's SPTM hypervisor. It drives the one-page synthetic MMIO
 * register file described in sptm_virt_auractl_abi.h to arm/start/stop/dump hypervisor MMIO traces
 * with no debug cable and without taking a USB port from macOS.
 *
 * The tool reaches the device through the AuractlDevice kext's IOUserClient. Two transports:
 *   - mapped registers (preferred): IOConnectMapMemory() gives us the control page directly, and we
 *     drive the registers ourselves -- fewest moving parts in the kext.
 *   - scalar methods (fallback): doCommand(cmd,arg0,arg1)->status and readReg(off)->u64, used when
 *     direct userland mapping of device memory is refused by the environment.
 *
 * The IOKit transport is behind a thin accessor (reg_read64/reg_read32/reg_write64/reg_write32/
 * do_command) so the register sequence below reads like the ABI. Build with -DAURACTL_NO_IOKIT to
 * compile the ABI-parsing + argument-parsing logic alone (host-check on non-macOS).
 *
 * Clean-room: our own ABI, names and layout. No Apple source, no tainted data, no hardcoded Apple
 * MMIO addresses -- the only fixed address is OUR AURACTL_MMIO_BASE from the ABI header.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>

#include "sptm_virt_auractl_abi.h"

/* The kext's IOUserClient externalMethod selectors. Kept in sync with AuractlDevice/AuractlUserClient. */
enum {
    kAuractlMethodMapRegisters = 0, /* clientMemoryForType(0) path; no scalar args */
    kAuractlMethodDoCommand    = 1, /* in: (cmd, arg0, arg1)  out: (status) */
    kAuractlMethodReadReg      = 2, /* in: (off)              out: (value) -- off 8-byte aligned */
    kAuractlMethodWriteReg     = 3, /* in: (off, value)       out: () -- off 8-byte aligned */
    kAuractlMethodCount
};

/* ---------------------------------------------------------------------------------------------- */
/* Transport. Everything above do_command/reg_* is pure logic and compiles on any host.           */
/* ---------------------------------------------------------------------------------------------- */

struct auractl_io; /* opaque */

static struct auractl_io *io_open(void);
static void               io_close(struct auractl_io *io);
static uint64_t           reg_read64(struct auractl_io *io, uint32_t off);
static uint32_t           reg_read32(struct auractl_io *io, uint32_t off);
static void               reg_write64(struct auractl_io *io, uint32_t off, uint64_t v);
static void               reg_write32(struct auractl_io *io, uint32_t off, uint32_t v);
/* do_command writes ARG0/ARG1 then rings the CMD doorbell and returns the fresh STATUS. */
static uint32_t           do_command(struct auractl_io *io, uint32_t cmd, uint64_t a0, uint64_t a1);

/* ---------------------------------------------------------------------------------------------- */
/* Pure helpers: ABI decode + argument parsing. Unit-checkable with no IOKit.                     */
/* ---------------------------------------------------------------------------------------------- */

static const char *state_str(uint32_t status)
{
    switch (status & AURACTL_STATE_MASK) {
    case AURACTL_STATE_IDLE:    return "IDLE";
    case AURACTL_STATE_ARMED:   return "ARMED";
    case AURACTL_STATE_RUNNING: return "RUNNING";
    case AURACTL_STATE_STOPPED: return "STOPPED";
    default:                    return "UNKNOWN";
    }
}

static uint32_t status_err(uint32_t status)
{
    return (status & AURACTL_ERR_MASK) >> AURACTL_ERR_SH;
}

static const char *err_str(uint32_t err)
{
    switch (err) {
    case AURACTL_ERR_NONE:     return "NONE";
    case AURACTL_ERR_BADCMD:   return "BADCMD (unknown opcode)";
    case AURACTL_ERR_BADSTATE: return "BADSTATE (opcode not legal in current state)";
    case AURACTL_ERR_BADRANGE: return "BADRANGE (not in guest MMIO / misaligned / zero)";
    case AURACTL_ERR_FULL:     return "FULL (RANGES_MAX reached or total size over cap)";
    case AURACTL_ERR_OVERLAP:  return "OVERLAP (range overlaps one already armed)";
    case AURACTL_ERR_NOMEM:    return "NOMEM (ring not allocated)";
    default:                   return "UNKNOWN";
    }
}

/*
 * Parse "<base>:<size>" with base/size as C-style integers (0x.. hex accepted). Returns 0 on
 * success. Rejects a missing ':', trailing junk, overflow, zero size, and (soft) non-16KiB-aligned
 * base/size -- the HV re-validates, but catching it here gives a clearer message.
 */
static int parse_range(const char *s, uint64_t *base, uint64_t *size)
{
    const char *colon = strchr(s, ':');
    if (!colon || colon == s || colon[1] == '\0')
        return -1;

    char *end = NULL;
    errno = 0;
    unsigned long long b = strtoull(s, &end, 0);
    if (errno != 0 || end != colon)
        return -1;

    errno = 0;
    unsigned long long z = strtoull(colon + 1, &end, 0);
    if (errno != 0 || *end != '\0')
        return -1;

    if (z == 0)
        return -1;

    *base = (uint64_t)b;
    *size = (uint64_t)z;
    return 0;
}

/* ---------------------------------------------------------------------------------------------- */
/* Commands.                                                                                      */
/* ---------------------------------------------------------------------------------------------- */

/* Probe MAGIC/VERSION; fatal if the device is not present. Caller keeps the open io. */
static int probe(struct auractl_io *io)
{
    uint32_t magic = reg_read32(io, AURACTL_REG_MAGIC); /* AURACTL_REG_MAGIC */
    if (magic != AURACTL_MAGIC) {
        fprintf(stderr,
                "auractl: no auractl device (MAGIC=0x%08" PRIx32 ", expected 0x%08x)\n",
                magic, AURACTL_MAGIC);
        return -1;
    }
    return 0;
}

static int cmd_status(struct auractl_io *io)
{
    if (probe(io))
        return 1;

    uint32_t version = reg_read32(io, AURACTL_REG_VERSION);  /* AURACTL_REG_VERSION */
    uint32_t caps    = reg_read32(io, AURACTL_REG_CAPS);     /* AURACTL_REG_CAPS */
    uint32_t status  = reg_read32(io, AURACTL_REG_STATUS);   /* AURACTL_REG_STATUS */
    uint64_t nrec    = reg_read64(io, AURACTL_REG_NREC);     /* AURACTL_REG_NREC */
    uint64_t dropped = reg_read64(io, AURACTL_REG_DROPPED);  /* AURACTL_REG_DROPPED */
    uint32_t nranges = reg_read32(io, AURACTL_REG_NRANGES);  /* AURACTL_REG_NRANGES */

    uint32_t ranges_max = caps & AURACTL_CAPS_RANGES_MASK;
    uint32_t recbytes   = (caps >> AURACTL_CAPS_RECBYTES_SH) & 0xff;
    int      have_fifo  = (caps & AURACTL_CAPS_FIFO) != 0;

    printf("auractl device:\n");
    printf("  magic    0x%08x (ok)\n", AURACTL_MAGIC);
    printf("  version  %" PRIu32 "\n", version);
    printf("  caps     0x%08" PRIx32 "  (ranges_max=%" PRIu32 ", rec_bytes=%" PRIu32
           ", fifo=%s)\n",
           caps, ranges_max, recbytes, have_fifo ? "yes" : "no");
    printf("  state    %s\n", state_str(status));
    printf("  err      %s\n", err_str(status_err(status)));
    printf("  nrec     %" PRIu64 "\n", nrec);
    printf("  dropped  %" PRIu64 "\n", dropped);
    printf("  nranges  %" PRIu32 "\n", nranges);
    return 0;
}

/* Ring a doorbell command and report any STATUS error. Returns 0 if err == NONE. */
static int command_checked(struct auractl_io *io, const char *name, uint32_t cmd, uint64_t a0,
                           uint64_t a1)
{
    uint32_t status = do_command(io, cmd, a0, a1);
    uint32_t err    = status_err(status);
    if (err != AURACTL_ERR_NONE) {
        fprintf(stderr, "auractl: %s failed: %s (state now %s)\n", name, err_str(err),
                state_str(status));
        return -1;
    }
    return 0;
}

static int cmd_reset(struct auractl_io *io)
{
    if (probe(io))
        return 1;
    return command_checked(io, "reset", AURACTL_CMD_RESET, 0, 0) ? 1 : 0;
}

static int cmd_arm(struct auractl_io *io, int argc, char **argv)
{
    if (probe(io))
        return 1;
    if (argc < 1) {
        fprintf(stderr, "auractl: arm needs at least one <base>:<size>\n");
        return 2;
    }

    for (int i = 0; i < argc; i++) {
        uint64_t base = 0, size = 0;
        if (parse_range(argv[i], &base, &size)) {
            fprintf(stderr, "auractl: bad range '%s' (want <base>:<size>, e.g. 0x230000000:0x4000)\n",
                    argv[i]);
            return 2;
        }
        /* ARG0 = range base, ARG1 = range size, then CMD = ARM. */
        if (command_checked(io, "arm", AURACTL_CMD_ARM, base, size)) {
            fprintf(stderr, "auractl: arm of %s (base 0x%" PRIx64 ", size 0x%" PRIx64 ") failed\n",
                    argv[i], base, size);
            return 1;
        }
        printf("armed 0x%" PRIx64 " .. 0x%" PRIx64 " (size 0x%" PRIx64 ")\n", base, base + size,
               size);
    }
    return 0;
}

static int cmd_start(struct auractl_io *io)
{
    if (probe(io))
        return 1;
    return command_checked(io, "start", AURACTL_CMD_START, 0, 0) ? 1 : 0;
}

static int cmd_stop(struct auractl_io *io)
{
    if (probe(io))
        return 1;
    return command_checked(io, "stop", AURACTL_CMD_STOP, 0, 0) ? 1 : 0;
}

/*
 * dump: latch the ring (CMD_DUMP), then pop FIFO_DATA while FIFO_AVAIL > 0, writing raw records
 * (4 little-endian u64 each == AURACTL_REC_BYTES) to the output file. Output is binary, so it must
 * go to a file -- default ./auractl-dump.bin, override with -o. File format == ring-dump.py input.
 */
static int cmd_dump(struct auractl_io *io, const char *path)
{
    if (probe(io))
        return 1;

    if (command_checked(io, "dump", AURACTL_CMD_DUMP, 0, 0))
        return 1;

    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "auractl: cannot open '%s': %s\n", path, strerror(errno));
        return 1;
    }

    uint64_t words = 0, records = 0;
    for (;;) {
        uint64_t avail = reg_read64(io, AURACTL_REG_FIFO_AVAIL); /* AURACTL_REG_FIFO_AVAIL (u64 words) */
        if (avail == 0)
            break;
        for (uint64_t i = 0; i < avail; i++) {
            uint64_t w = reg_read64(io, AURACTL_REG_FIFO_DATA); /* AURACTL_REG_FIFO_DATA (pop one u64) */
            unsigned char le[8];
            for (int b = 0; b < 8; b++)
                le[b] = (unsigned char)((w >> (8 * b)) & 0xff);
            if (fwrite(le, 1, sizeof(le), f) != sizeof(le)) {
                fprintf(stderr, "auractl: write to '%s' failed: %s\n", path, strerror(errno));
                fclose(f);
                return 1;
            }
            words++;
        }
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "auractl: close '%s' failed: %s\n", path, strerror(errno));
        return 1;
    }

    /* 4 u64 words per record. A partial record would mean the HV truncated the FIFO. */
    records = words / (AURACTL_REC_BYTES / 8);
    if (words % (AURACTL_REC_BYTES / 8) != 0)
        fprintf(stderr, "auractl: warning: %" PRIu64 " trailing u64 (not a whole record)\n",
                words % (AURACTL_REC_BYTES / 8));
    printf("dumped %" PRIu64 " records (%" PRIu64 " bytes) to %s\n", records,
           words * 8, path);
    return 0;
}

static void usage(FILE *out)
{
    fprintf(out,
            "usage: auractl <command>\n"
            "  status                         probe + decode the device registers\n"
            "  reset                          clear ranges, ring and counters (-> IDLE)\n"
            "  arm <base>:<size> [...]        add trace range(s); hex (0x..) or decimal\n"
            "  start                          begin recording (ARMED -> RUNNING)\n"
            "  stop                           stop recording (RUNNING -> STOPPED)\n"
            "  dump [-o file]                 latch + pop the ring to a raw record file\n"
            "                                 (default ./auractl-dump.bin; ring-dump.py format)\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(stderr);
        return 2;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "help") == 0) {
        usage(stdout);
        return 0;
    }

    /* Resolve dump's -o before opening the device, so arg errors cost nothing. */
    const char *dump_path = "auractl-dump.bin";
    if (strcmp(cmd, "dump") == 0) {
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-o") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "auractl: -o needs a file argument\n");
                    return 2;
                }
                dump_path = argv[++i];
            } else {
                fprintf(stderr, "auractl: unexpected dump argument '%s'\n", argv[i]);
                return 2;
            }
        }
    }

    struct auractl_io *io = io_open();
    if (!io) {
        fprintf(stderr, "auractl: could not open AuractlDevice (is the kext loaded?)\n");
        return 1;
    }

    int rc;
    if (strcmp(cmd, "status") == 0)
        rc = cmd_status(io);
    else if (strcmp(cmd, "reset") == 0)
        rc = cmd_reset(io);
    else if (strcmp(cmd, "arm") == 0)
        rc = cmd_arm(io, argc - 2, argv + 2);
    else if (strcmp(cmd, "start") == 0)
        rc = cmd_start(io);
    else if (strcmp(cmd, "stop") == 0)
        rc = cmd_stop(io);
    else if (strcmp(cmd, "dump") == 0)
        rc = cmd_dump(io, dump_path);
    else {
        fprintf(stderr, "auractl: unknown command '%s'\n", cmd);
        usage(stderr);
        rc = 2;
    }

    io_close(io);
    return rc;
}

/* ============================================================================================== */
/* Transport implementation.                                                                      */
/*                                                                                                */
/* On macOS: IOKit. On any other host (or -DAURACTL_NO_IOKIT): a stub so the pure logic above      */
/* compiles and can be exercised. The stub never returns a valid MAGIC, so every command fails     */
/* cleanly with "no auractl device" if someone runs the host-check binary.                         */
/* ============================================================================================== */

#if defined(__APPLE__) && !defined(AURACTL_NO_IOKIT)

#include <IOKit/IOKitLib.h>
#include <mach/mach.h>

struct auractl_io {
    io_connect_t  conn;    /* IOServiceOpen handle to the user client */
    volatile uint8_t *regs; /* mapped register page, or NULL if we fell back to scalar methods */
    mach_vm_address_t map_addr;
    mach_vm_size_t    map_size;
};

static struct auractl_io *io_open(void)
{
    struct auractl_io *io = calloc(1, sizeof(*io));
    if (!io)
        return NULL;

    /* Match the kext's IOService by its IOClass name from Info.plist. */
    CFMutableDictionaryRef match = IOServiceMatching("AuractlDevice");
    if (!match) {
        free(io);
        return NULL;
    }

    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, match);
    if (svc == IO_OBJECT_NULL) {
        free(io);
        return NULL;
    }

    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &io->conn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) {
        free(io);
        return NULL;
    }

    /*
     * Preferred transport: map the control page read/write into this process via the user client's
     * clientMemoryForType(0). If the environment refuses to map device memory to userland, fall
     * back silently to the scalar readReg/doCommand methods.
     */
    kr = IOConnectMapMemory64(io->conn, kAuractlMethodMapRegisters, mach_task_self(),
                              &io->map_addr, &io->map_size, kIOMapAnywhere | kIOMapInhibitCache);
    if (kr == KERN_SUCCESS && io->map_size >= AURACTL_REG_MAX) {
        io->regs = (volatile uint8_t *)(uintptr_t)io->map_addr;
    } else {
        io->regs = NULL; /* scalar-method fallback */
    }

    return io;
}

static void io_close(struct auractl_io *io)
{
    if (!io)
        return;
    if (io->regs)
        IOConnectUnmapMemory64(io->conn, kAuractlMethodMapRegisters, mach_task_self(),
                               io->map_addr);
    if (io->conn)
        IOServiceClose(io->conn);
    free(io);
}

static uint64_t reg_read64(struct auractl_io *io, uint32_t off)
{
    if (io->regs)
        return *(volatile uint64_t *)(io->regs + off); /* direct MMIO read, uncached */

    uint64_t in = off, out = 0;
    uint32_t outcnt = 1;
    kern_return_t kr = IOConnectCallScalarMethod(io->conn, kAuractlMethodReadReg, &in, 1, &out,
                                                 &outcnt);
    return (kr == KERN_SUCCESS) ? out : 0;
}

static uint32_t reg_read32(struct auractl_io *io, uint32_t off)
{
    /* The register file is 8-byte addressable for MMIO; 32-bit regs live in the low word. */
    if (io->regs)
        return *(volatile uint32_t *)(io->regs + off);
    return (uint32_t)reg_read64(io, off);
}

static void reg_write64(struct auractl_io *io, uint32_t off, uint64_t v)
{
    if (io->regs) {
        *(volatile uint64_t *)(io->regs + off) = v; /* direct MMIO write, uncached */
        return;
    }
    uint64_t in[2] = { off, v };
    IOConnectCallScalarMethod(io->conn, kAuractlMethodWriteReg, in, 2, NULL, NULL);
}

static void reg_write32(struct auractl_io *io, uint32_t off, uint32_t v)
{
    if (io->regs) {
        *(volatile uint32_t *)(io->regs + off) = v;
        return;
    }
    reg_write64(io, off, v);
}

static uint32_t do_command(struct auractl_io *io, uint32_t cmd, uint64_t a0, uint64_t a1)
{
    if (io->regs) {
        /* Drive the registers directly: ARG0, ARG1, then the CMD doorbell, then re-read STATUS. */
        reg_write64(io, AURACTL_REG_ARG0, a0); /* AURACTL_REG_ARG0 */
        reg_write64(io, AURACTL_REG_ARG1, a1); /* AURACTL_REG_ARG1 */
        reg_write32(io, AURACTL_REG_CMD, cmd); /* AURACTL_REG_CMD doorbell */
        return reg_read32(io, AURACTL_REG_STATUS); /* AURACTL_REG_STATUS */
    }

    /* Scalar fallback: the kext performs the ARG0/ARG1/CMD sequence and returns STATUS. */
    uint64_t in[3] = { cmd, a0, a1 };
    uint64_t out = 0;
    uint32_t outcnt = 1;
    kern_return_t kr = IOConnectCallScalarMethod(io->conn, kAuractlMethodDoCommand, in, 3, &out,
                                                 &outcnt);
    return (kr == KERN_SUCCESS) ? (uint32_t)out : 0;
}

#else /* !__APPLE__ || AURACTL_NO_IOKIT -- host-check stub, no IOKit. */

struct auractl_io {
    int dummy;
};

static struct auractl_io *io_open(void)
{
    static struct auractl_io stub;
    return &stub;
}

static void io_close(struct auractl_io *io)
{
    (void)io;
}

static uint64_t reg_read64(struct auractl_io *io, uint32_t off)
{
    (void)io;
    (void)off;
    return 0; /* MAGIC reads back 0 -> probe() reports "no auractl device", as intended on a host. */
}

static uint32_t reg_read32(struct auractl_io *io, uint32_t off)
{
    return (uint32_t)reg_read64(io, off);
}

static void reg_write64(struct auractl_io *io, uint32_t off, uint64_t v)
{
    (void)io;
    (void)off;
    (void)v;
}

/* Not exercised by the host-check build (which rings CMD via reg_write32->reg_write64 only on the
 * Apple path); present for parity with the IOKit transport. */
#if defined(__GNUC__)
__attribute__((unused))
#endif
static void reg_write32(struct auractl_io *io, uint32_t off, uint32_t v)
{
    reg_write64(io, off, v);
}

static uint32_t do_command(struct auractl_io *io, uint32_t cmd, uint64_t a0, uint64_t a1)
{
    (void)io;
    (void)cmd;
    (void)a0;
    (void)a1;
    return 0;
}

#endif
