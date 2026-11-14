// SPDX-License-Identifier: MIT
/*
 * auractl: control aura's MMIO trace from inside the SPTM-virt guest (docs/auractl.md).
 *
 *   auractl status | list | console
 *   auractl arm <name | 0xbase[+0xsize]>     hook a catalogue range or a physical range
 *   auractl disarm [<name | 0xbase[+0xsize]>] release one, or everything auractl armed
 *   auractl start | stop | clear
 *   auractl read <file> [--from <seq>] [--max <records>]
 *   auractl mark <text> [value]
 *   auractl print <file>                     decode a file from read (no aura needed)
 *   auractl peek <0xpa> [words]              read plain PMGR registers
 *
 * It needs an AURACTL build of aura and a guest-side entry that carries the request block to aura
 * (entry.h): ENTRY=iokit through a kext (root, SIP off), ENTRY=dcc through an EL0 debug register
 * (no kext, no root), or ENTRY=none, which can only decode files.
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../../src/auractl_abi.h"
#include "entry.h"

#define TRC_MAGIC "AURATRC1"
#define CHUNK     (AURACTL_MAX_BUF / sizeof(struct auractl_rec))

struct trc_header {
    char magic[8];
    uint32_t version, rec_size;
    uint64_t cntfrq, first, count, head, lost;
    char build[48];
};

/* Wired, written memory: aura checks each page with the caller's own EL0 translation, and a page
 * that is not resident and writable right now is EFAULT rather than a fault it could take. */
static struct auractl_req *req;
static uint8_t *iobuf;

static void setup(void)
{
    size_t len = 2 * AURACTL_MAX_BUF + 0x10000;
    uint8_t *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    memset(p, 0, len);
    if (mlock(p, len))
        perror("auractl: warning: mlock");
    req = (struct auractl_req *)p;
    iobuf = p + 0x10000;
}

static int call(uint16_t cmd, uint64_t a0, uint64_t a1, uint64_t a2, const void *in, size_t in_len,
                size_t out_len)
{
    memset(req, 0, sizeof(*req));
    req->magic = AURACTL_REQ_MAGIC;
    req->version = AURACTL_VERSION;
    req->cmd = cmd;
    req->arg0 = a0;
    req->arg1 = a1;
    req->arg2 = a2;
    if (in) {
        memcpy(iobuf, in, in_len);
        iobuf[in_len] = 0;
        req->buf = (uint64_t)iobuf;
        req->buf_len = in_len + 1;
    } else if (out_len) {
        memset(iobuf, 0, out_len); /* resident and writable, whatever happened since setup */
        req->buf = (uint64_t)iobuf;
        req->buf_len = out_len;
    }
    return auractl_enter(req);
}

static const char *why(int e)
{
    switch (e) {
        case AURACTL_ENTRY_NONE:
            return "this auractl was built with no guest entry (ENTRY=none, docs/auractl.md)";
        case AURACTL_EPERM:
            return "not permitted (the guest's entry refused it before aura)";
        case AURACTL_ENOENT:
            return "no such catalogue name on this machine";
        case AURACTL_E2BIG:
            return "too large for one call";
        case AURACTL_EACCES:
            return "aura refuses that page: not hookable MMIO, its block was not split at setup, "
                   "another aura hook owns it, or PEEK is not allowed there";
        case AURACTL_EFAULT:
            return "a buffer is not mapped for this process";
        case AURACTL_EBUSY:
            return "busy: the trace is stopped, or the trace setup owns that page";
        case AURACTL_ENODEV:
            return "this boot has no trace ring (trace flags), or the guest is not up";
        case AURACTL_EINVAL:
            return "aura rejected the request (magic, version or arguments)";
        case AURACTL_ENOSPC:
            return "aura's hook table is full";
        case AURACTL_ENOSYS:
            return "this aura does not know that command";
        default:
            return strerror(e);
    }
}

static int fail(const char *what, int e)
{
    fprintf(stderr, "auractl: %s: %s\n", what, why(e));
    return 1;
}

static int get_info(struct auractl_info *in)
{
    int e = call(AURACTL_INFO, 0, 0, 0, NULL, 0, sizeof(*in));
    if (!e)
        memcpy(in, iobuf, sizeof(*in));
    return e;
}

static int cmd_status(void)
{
    struct auractl_info in;
    int e = get_info(&in);
    if (e)
        return fail("status", e);
    printf("aura %s, auractl ABI %u\n", in.build, in.version);
    printf("trace: %s, %s\n", in.flags & AURACTL_INFO_TRACE ? "ring present" : "NO RING",
           in.flags & AURACTL_INFO_RECORDING ? "recording" : "not recording");
    printf("ring: %" PRIu64 " records capacity, %" PRIu64 " appended, %" PRIu64 " Hz timestamps\n",
           in.nrec, in.head, in.cntfrq);
    printf("hooks: %u pages hooked, %u armed through auractl; %u catalogue ranges\n", in.pages,
           in.ctl_pages, in.ncatalog);
    printf("boot: %s; entry: %s\n", in.flags & AURACTL_INFO_ESP ? "ESP" : "host", auractl_entry_name());
    return 0;
}

static int cmd_list(void)
{
    int e = call(AURACTL_LIST, 0, 0, 0, NULL, 0, AURACTL_MAX_BUF);
    if (e)
        return fail("list", e);
    const struct auractl_range *r = (const void *)iobuf;
    size_t n = req->buf_used / sizeof(*r);
    printf("%-20s %-14s %-10s %-7s %s\n", "NAME", "BASE", "SIZE", "HOOKED", "STATE");
    for (size_t i = 0; i < n; i++)
        printf("%-20.24s 0x%-12" PRIx64 " 0x%-8" PRIx64 " %-7u %s\n", r[i].name, r[i].base,
               r[i].size, r[i].pages_hooked,
               !(r[i].flags & AURACTL_RANGE_FOUND)  ? "absent on this machine"
               : r[i].flags & AURACTL_RANGE_ARMED   ? "armed"
               : r[i].flags & AURACTL_RANGE_PARTIAL ? "partly armed"
                                                    : "-");
    if (req->ret0 > n)
        printf("(%" PRIu64 " ranges in all)\n", req->ret0);
    return 0;
}

static bool parse_range(const char *s, uint64_t *base, uint64_t *size)
{
    char *end;
    if (strncmp(s, "0x", 2))
        return false;
    *base = strtoull(s, &end, 16);
    *size = 0x4000;
    if (*end == '+')
        *size = strtoull(end + 1, &end, 0);
    return !*end && *size;
}

static int cmd_hook(int argc, char **argv, bool arm)
{
    const char *verb = arm ? "arm" : "disarm";
    uint16_t cmd = arm ? AURACTL_ARM : AURACTL_DISARM;
    int e;
    uint64_t base, size;
    if (!argc && arm) {
        fprintf(stderr, "usage: auractl arm <name | 0xbase[+0xsize]>\n");
        return 2;
    }
    if (!argc)
        e = call(cmd, AURACTL_SEL_ALL, 0, 0, NULL, 0, 0);
    else if (parse_range(argv[0], &base, &size))
        e = call(cmd, AURACTL_SEL_RANGE, base, size, NULL, 0, 0);
    else
        e = call(cmd, AURACTL_SEL_NAME, 0, 0, argv[0], strlen(argv[0]), 0);
    if (e) {
        if (req->ret0)
            fprintf(stderr, "auractl: %" PRIu64 " pages changed before the refusal\n", req->ret0);
        return fail(verb, e);
    }
    printf("%s: %" PRIu64 " pages changed\n", verb, req->ret0);
    return 0;
}

static int cmd_read(int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "usage: auractl read <file> [--from <seq>] [--max <records>]\n");
        return 2;
    }
    uint64_t next = 0, max = UINT64_MAX;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--from"))
            next = strtoull(argv[i + 1], NULL, 0);
        else if (!strcmp(argv[i], "--max"))
            max = strtoull(argv[i + 1], NULL, 0);
        else {
            fprintf(stderr, "auractl: unknown option %s\n", argv[i]);
            return 2;
        }
    }
    struct auractl_info in;
    int e = get_info(&in);
    if (e)
        return fail("read", e);
    if (!(in.flags & AURACTL_INFO_TRACE))
        return fail("read", AURACTL_ENODEV);
    if (in.rec_size != sizeof(struct auractl_rec)) {
        fprintf(stderr, "auractl: aura's records are %u bytes, this tool's %zu\n", in.rec_size,
                sizeof(struct auractl_rec));
        return 1;
    }
    int fd = open(argv[0], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror(argv[0]);
        return 1;
    }
    struct trc_header h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, TRC_MAGIC, 8);
    h.version = 1;
    h.rec_size = in.rec_size;
    h.cntfrq = in.cntfrq;
    h.head = in.head;
    memcpy(h.build, in.build, sizeof(h.build));
    if (write(fd, &h, sizeof(h)) != sizeof(h)) {
        perror("write");
        return 1;
    }
    /* Up to the head at the start: a running trace would otherwise never end. */
    uint64_t stop = in.head, total = 0;
    bool first = true;
    while (next < stop && total < max) {
        uint64_t want = stop - next;
        if (want > CHUNK)
            want = CHUNK;
        if (want > max - total)
            want = max - total;
        e = call(AURACTL_READ, next, want, 0, NULL, 0, want * sizeof(struct auractl_rec));
        if (e) {
            close(fd);
            return fail("read", e);
        }
        uint64_t got = req->ret1, at = req->ret0;
        if (at > next) {
            h.lost += at - next;
            if (!first)
                fprintf(stderr, "auractl: the ring overwrote %" PRIu64 " records during the read\n",
                        at - next);
        }
        if (first) {
            h.first = at;
            first = false;
        }
        if (!got)
            break;
        size_t bytes = got * sizeof(struct auractl_rec);
        if (write(fd, iobuf, bytes) != (ssize_t)bytes) {
            perror("write");
            close(fd);
            return 1;
        }
        total += got;
        next = at + got;
    }
    h.count = total;
    if (pwrite(fd, &h, sizeof(h), 0) != sizeof(h)) {
        perror("pwrite");
        close(fd);
        return 1;
    }
    close(fd);
    printf("%" PRIu64 " records, sequence %" PRIu64 " on, to %s (%" PRIu64 " older ones gone)\n",
           total, h.first, argv[0], h.lost);
    return 0;
}

static int cmd_mark(int argc, char **argv)
{
    if (argc < 1 || strlen(argv[0]) > AURACTL_MAX_MARK) {
        fprintf(stderr, "usage: auractl mark <text, up to %d chars> [value]\n", AURACTL_MAX_MARK);
        return 2;
    }
    uint64_t v = argc > 1 ? strtoull(argv[1], NULL, 0) : 0;
    int e = call(AURACTL_MARK, v, 0, 0, argv[0], strlen(argv[0]), 0);
    if (e)
        return fail("mark", e);
    printf("marked \"%.8s\" at about record %" PRIu64 "\n", argv[0], req->ret0);
    return 0;
}

static int cmd_console(void)
{
    int e = call(AURACTL_CONSOLE, 0, 0, 0, NULL, 0, AURACTL_MAX_BUF);
    if (e)
        return fail("console", e);
    fwrite(iobuf, 1, req->buf_used, stdout);
    return 0;
}

static int cmd_peek(int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "usage: auractl peek <0xpa> [words, up to %d]\n", AURACTL_MAX_PEEK);
        return 2;
    }
    uint64_t pa = strtoull(argv[0], NULL, 0), n = argc > 1 ? strtoull(argv[1], NULL, 0) : 1;
    int e = call(AURACTL_PEEK, pa, n, 0, NULL, 0, n * 4);
    if (e)
        return fail("peek", e);
    const uint32_t *v = (const uint32_t *)iobuf;
    for (uint64_t i = 0; i < req->buf_used / 4; i++)
        printf("%s0x%09" PRIx64 ": 0x%08x", i % 4 ? "  " : (i ? "\n" : ""), pa + 4 * i, v[i]);
    printf("\n");
    return 0;
}

static int cmd_print(int argc, char **argv)
{
    if (argc != 1) {
        fprintf(stderr, "usage: auractl print <file>\n");
        return 2;
    }
    FILE *f = fopen(argv[0], "rb");
    struct trc_header h;
    if (!f) {
        perror(argv[0]);
        return 1;
    }
    if (fread(&h, sizeof(h), 1, f) != 1 || memcmp(h.magic, TRC_MAGIC, 8) || h.version != 1 ||
        h.rec_size != sizeof(struct auractl_rec) || !h.cntfrq) {
        fprintf(stderr, "auractl: %s is not a trace from auractl read\n", argv[0]);
        fclose(f);
        return 1;
    }
    printf("# aura %.48s: %" PRIu64 " records from sequence %" PRIu64 ", %" PRIu64 " Hz, %" PRIu64
           " older gone\n",
           h.build, h.count, h.first, h.cntfrq, h.lost);
    struct auractl_rec r;
    uint64_t t0 = 0, n = 0;
    while (fread(&r, sizeof(r), 1, f) == 1) {
        if (!n)
            t0 = r.t;
        double t = (double)(r.t - t0) / (double)h.cntfrq;
        if (AURACTL_REC_MARKER(r.meta)) {
            char tag[9] = {0};
            memcpy(tag, &r.pc, 8);
            printf("%" PRIu64 " %.6f c%u MARK %-8s 0x%" PRIx64 "\n", h.first + n, t,
                   (unsigned)AURACTL_REC_CPU(r.meta), tag, r.val);
        } else {
            printf("%" PRIu64 " %.6f c%u %c%-2u 0x%09" PRIx64 " 0x%" PRIx64 " pc 0x%" PRIx64 "\n",
                   h.first + n, t, (unsigned)AURACTL_REC_CPU(r.meta),
                   AURACTL_REC_WRITE(r.meta) ? 'W' : 'R', 8u << AURACTL_REC_WIDTH(r.meta),
                   AURACTL_REC_PA(r.meta), r.val, r.pc);
        }
        n++;
    }
    fclose(f);
    if (n != h.count) {
        fprintf(stderr, "auractl: %s holds %" PRIu64 " records, its header says %" PRIu64 "\n",
                argv[0], n, h.count);
        return 1;
    }
    return 0;
}

static int usage(void)
{
    fprintf(stderr, "usage: auractl status | list | console | start | stop | clear\n"
                    "       auractl arm <name | 0xbase[+0xsize]>\n"
                    "       auractl disarm [<name | 0xbase[+0xsize]>]\n"
                    "       auractl read <file> [--from <seq>] [--max <records>]\n"
                    "       auractl mark <text> [value]\n"
                    "       auractl peek <0xpa> [words]\n"
                    "       auractl print <file>\n");
    return 2;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return usage();
    const char *c = argv[1];
    if (!strcmp(c, "print"))
        return cmd_print(argc - 2, argv + 2);
    bool known = false;
    static const char *const cmds[] = {"status", "list", "console", "start", "stop",
                                       "clear",  "arm",  "disarm",  "read",  "mark", "peek"};
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        known |= !strcmp(c, cmds[i]);
    if (!known)
        return usage();
    if (!strcmp(auractl_entry_name(), "none"))
        return fail(c, AURACTL_ENTRY_NONE);
    /* The DCC entry is a plain EL0 debug-register access: it needs no privilege. Other entries
     * (the kext's user client) do. */
    if (geteuid() != 0 && strcmp(auractl_entry_name(), "dcc") != 0) {
        fprintf(stderr, "auractl: needs root\n");
        return 1;
    }
    setup();
    if (!strcmp(c, "status"))
        return cmd_status();
    if (!strcmp(c, "list"))
        return cmd_list();
    if (!strcmp(c, "console"))
        return cmd_console();
    if (!strcmp(c, "arm"))
        return cmd_hook(argc - 2, argv + 2, true);
    if (!strcmp(c, "disarm"))
        return cmd_hook(argc - 2, argv + 2, false);
    if (!strcmp(c, "read"))
        return cmd_read(argc - 2, argv + 2);
    if (!strcmp(c, "mark"))
        return cmd_mark(argc - 2, argv + 2);
    if (!strcmp(c, "peek"))
        return cmd_peek(argc - 2, argv + 2);
    uint16_t op = !strcmp(c, "start")  ? AURACTL_START
                  : !strcmp(c, "stop") ? AURACTL_STOP
                                       : AURACTL_CLEAR;
    int e = call(op, 0, 0, 0, NULL, 0, 0);
    if (e)
        return fail(c, e);
    if (op == AURACTL_CLEAR)
        printf("cleared %" PRIu64 " records\n", req->ret0);
    return 0;
}
