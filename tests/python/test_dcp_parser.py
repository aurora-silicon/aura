# SPDX-License-Identifier: MIT
"""Run the production DCP property parser on host-built announce blobs.

src/dcp/parser.c reads the EPICName/EPICProviderClass/EPICUnit properties an
AFK service announces (src/afk.c). The blobs are built here from the wire
format the parser documents; the parser itself is compiled unchanged, with
only its includes replaced, under ASan/UBSan where the host compiler has them.
"""
from pathlib import Path
import struct
import subprocess

import pytest

from host_compiler import host_cc

ROOT = Path(__file__).resolve().parents[2]

DICT, ARRAY, INT64, STRING, BLOB, BOOL = 1, 2, 4, 9, 10, 11
HEADER = 0xD3

PREFIX = r'''
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t s64;
#define __packed __attribute__((packed))
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
struct dcp_parse_ctx {
    void *blob;
    u32 pos, len;
};
'''

HARNESS = r'''
static u8 *load(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    u8 *buf;
    long n;

    if (!f || fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET))
        exit(2);
    /* Exactly sized, so any read past the blob is an ASan report. */
    buf = malloc(n ? (size_t)n : 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n)
        exit(2);
    fclose(f);
    *size = (size_t)n;
    return buf;
}

/* One announce: parse() and parse_epic_service_init(), as src/afk.c calls them. */
static int run(u8 *blob, size_t size, bool print)
{
    struct dcp_parse_ctx ctx;
    char *name = (char *)1, *class = (char *)1;
    s64 unit = 0x5a5a;
    int ret = parse(blob, size, &ctx);

    if (ret) {
        if (print)
            printf("parse=%d\n", ret);
        return ret;
    }
    ret = parse_epic_service_init(&ctx, &name, &class, &unit);
    if (ret) {
        /* On failure both outputs are cleared and nothing is left allocated. */
        if (name || class)
            abort();
    } else if (!name || !class) {
        abort();
    }
    if (print) {
        printf("parse=0 init=%d", ret);
        if (!ret)
            printf(" name=%s class=%s unit=%lld", name, class, (long long)unit);
        printf("\n");
    }
    free(name);
    free(class);
    return ret;
}

int main(int argc, char **argv)
{
    size_t size;
    u8 *blob;

    if (argc == 3 && !strcmp(argv[1], "one")) {
        blob = load(argv[2], &size);
        run(blob, size, true);
        free(blob);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "truncate")) {
        /* Every strict prefix of a complete announce is refused. */
        u8 *full = load(argv[2], &size);
        size_t n;

        for (n = 0; n < size; n++) {
            blob = malloc(n ? n : 1);
            memcpy(blob, full, n);
            if (!run(blob, n, false)) {
                printf("prefix %zu accepted\n", n);
                return 1;
            }
            free(blob);
        }
        free(full);
        printf("ok %zu\n", size);
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "mutate")) {
        /* Deterministic byte corruption: never a crash, a leak or an overread. */
        u8 *full = load(argv[2], &size);
        unsigned long iters = strtoul(argv[3], NULL, 0), i, accepted = 0;
        u32 state = 0x2545f491;

        for (i = 0; i < iters; i++) {
            unsigned int flips, j;

            blob = malloc(size);
            memcpy(blob, full, size);
            state = state * 1664525u + 1013904223u;
            flips = 1 + (state >> 28) % 3;
            for (j = 0; j < flips; j++) {
                state = state * 1664525u + 1013904223u;
                /* Keep the header, corrupt the properties. */
                blob[4 + (state >> 8) % (size - 4)] ^= (u8)(1u << ((state >> 3) & 7));
            }
            if (!run(blob, size, false))
                accepted++;
            free(blob);
        }
        free(full);
        printf("ok %lu %lu\n", iters, accepted);
        return 0;
    }
    return 2;
}
'''


def tag(kind, size, padding=0, last=False):
    return struct.pack("<I", size | kind << 24 | padding << 29 | (last << 31))


class Blob:
    """Little-endian property stream; every tag starts 32-bit aligned."""

    def __init__(self):
        self.data = bytearray(struct.pack("<I", HEADER))

    def _tag(self, kind, size, **kw):
        self.data += b"\0" * (-len(self.data) % 4)
        self.data += tag(kind, size, **kw)
        return self

    def dict(self, n, **kw):
        return self._tag(DICT, n, **kw)

    def array(self, n):
        return self._tag(ARRAY, n)

    def string(self, s, **kw):
        raw = s.encode()
        self._tag(STRING, len(raw), **kw)
        self.data += raw
        return self

    def blob(self, raw):
        self._tag(BLOB, len(raw))
        self.data += raw
        return self

    def int64(self, v):
        self._tag(INT64, 0)
        self.data += struct.pack("<q", v)
        return self

    def boolean(self, v):
        return self._tag(BOOL, 1 if v else 0)

    def bytes(self):
        return bytes(self.data)


def announce(name="dcpav-service-epic", cls="DCPDP13Service", unit=0, order=("n", "c", "u")):
    b = Blob().dict(len(order))
    for k in order:
        if k == "n":
            b.string("EPICName").string(name)
        elif k == "c":
            b.string("EPICProviderClass").string(cls)
        elif k == "u":
            b.string("EPICUnit").int64(unit)
    return b.bytes()


@pytest.fixture(scope="module")
def parser(tmp_path_factory):
    tmp = tmp_path_factory.mktemp("dcp-parser")
    source = (ROOT / "src/dcp/parser.c").read_text()
    body = "\n".join(l for l in source.splitlines() if not l.startswith("#include"))
    c = tmp / "parser_host.c"
    c.write_text(PREFIX + body + HARNESS)
    exe = tmp / "parser_host"
    base = host_cc() + ["-std=gnu11", "-O1", "-g", "-Wall", "-Werror",
                        "-Wno-unused-function", str(c), "-o", str(exe)]
    # The wire only 4-byte aligns an INT64 payload and parse_int() copies it
    # through an s64 pointer, which clang's alignment check reports. aura
    # runs with the MMU on, where AArch64 permits the unaligned access.
    sanitized = base + ["-fsanitize=address,undefined", "-fno-sanitize=alignment",
                        "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
    if subprocess.run(sanitized, capture_output=True).returncode:
        subprocess.run(base, check=True)
    return tmp, exe


def run(parser, mode, data, *args):
    tmp, exe = parser
    path = tmp / "blob.bin"
    path.write_bytes(data)
    res = subprocess.run([str(exe), mode, str(path), *map(str, args)],
                         capture_output=True, text=True, timeout=120)
    assert res.returncode == 0, res.stdout + res.stderr
    return res.stdout.strip()


def test_announce_properties(parser):
    assert run(parser, "one", announce()) == \
        "parse=0 init=0 name=dcpav-service-epic class=DCPDP13Service unit=0"
    assert run(parser, "one", announce(name="dcpdptx-port-epic", cls="AppleDCPDPTXRemotePort",
                                       unit=1)) == \
        "parse=0 init=0 name=dcpdptx-port-epic class=AppleDCPDPTXRemotePort unit=1"


@pytest.mark.parametrize("order", [("u", "c", "n"), ("c", "n", "u"), ("u", "n", "c")])
def test_key_order_is_free(parser, order):
    assert run(parser, "one", announce(order=order, unit=3)) == \
        "parse=0 init=0 name=dcpav-service-epic class=DCPDP13Service unit=3"


@pytest.mark.parametrize("unit", [-1, 0x7fffffffffffffff, -0x8000000000000000, 0x10000])
def test_unit_is_64_bit_signed(parser, unit):
    assert run(parser, "one", announce(unit=unit)).endswith("unit=%d" % unit)


@pytest.mark.parametrize("name", ["a", "ab", "abc", "abcd", "abcde"])
def test_unaligned_strings_realign(parser, name):
    """String payloads of every length mod 4; the next tag is re-aligned."""
    assert run(parser, "one", announce(name=name, cls=name * 2)) == \
        "parse=0 init=0 name=%s class=%s unit=0" % (name, name * 2)


def test_other_properties_are_skipped(parser):
    b = (Blob().dict(8)
         .string("EPICRole").string("x")
         .string("Nested").dict(2).string("a").int64(1).string("b").array(2).boolean(True).blob(b"\x01\x02\x03")
         .string("EPICName").string("dcpav-service-epic")
         .string("Flag").boolean(False)
         .string("Raw").blob(b"\xff" * 7)
         .string("EPICProviderClass").string("AppleDCPDP2HDMI")
         .string("List").array(3).int64(5).string("yz").boolean(True)
         .string("EPICUnit").int64(2))
    assert run(parser, "one", b.bytes()) == \
        "parse=0 init=0 name=dcpav-service-epic class=AppleDCPDP2HDMI unit=2"


@pytest.mark.parametrize("drop", ["n", "c", "u"])
def test_every_property_is_required(parser, drop):
    order = tuple(k for k in ("n", "c", "u") if k != drop)
    assert run(parser, "one", announce(order=order)) == "parse=0 init=-1"


def test_empty_dictionary(parser):
    assert run(parser, "one", Blob().dict(0).bytes()) == "parse=0 init=-1"


@pytest.mark.parametrize("blob", [
    # EPICUnit as a string
    Blob().dict(3).string("EPICName").string("n").string("EPICProviderClass").string("c")
    .string("EPICUnit").string("0").bytes(),
    # EPICName as an integer
    Blob().dict(3).string("EPICName").int64(0).string("EPICProviderClass").string("c")
    .string("EPICUnit").int64(0).bytes(),
    # EPICProviderClass as a blob
    Blob().dict(3).string("EPICName").string("n").string("EPICProviderClass").blob(b"c")
    .string("EPICUnit").int64(0).bytes(),
    # A key that is not a string
    Blob().dict(3).int64(7).string("n").string("EPICProviderClass").string("c")
    .string("EPICUnit").int64(0).bytes(),
], ids=["unit-string", "name-int", "class-blob", "key-int"])
def test_wrong_types_are_refused(parser, blob):
    """The outputs are cleared again, even after one property was parsed."""
    assert run(parser, "one", blob) == "parse=0 init=-1"


@pytest.mark.parametrize("blob", [
    Blob().array(1).dict(0).bytes(),
    Blob().string("EPICName").bytes(),
    Blob().int64(0).bytes(),
], ids=["array", "string", "int"])
def test_top_level_must_be_dictionary(parser, blob):
    assert run(parser, "one", blob) == "parse=0 init=-1"


def test_padding_bits_are_refused(parser):
    good = announce()
    bad = bytearray(good)
    # The dictionary tag follows the 4-byte header.
    bad[4:8] = tag(DICT, 3, padding=1)
    assert run(parser, "one", bytes(bad)) == "parse=0 init=-1"
    # A key string with padding bits.
    b = Blob().dict(3).string("EPICName", padding=2).string("n").bytes()
    assert run(parser, "one", b) == "parse=0 init=-1"


def test_last_bit_is_ignored(parser):
    b = (Blob().dict(3, last=True).string("EPICName").string("n")
         .string("EPICProviderClass").string("c").string("EPICUnit", last=True).int64(4))
    assert run(parser, "one", b.bytes()) == "parse=0 init=0 name=n class=c unit=4"


@pytest.mark.parametrize("data", [b"", b"\xd3", b"\xd3\0\0", struct.pack("<I", 0xd4),
                                  struct.pack("<I", 0xd3000000)],
                         ids=["empty", "1", "3", "d4", "be"])
def test_header(parser, data):
    assert run(parser, "one", data) == "parse=-1"


def test_header_only(parser):
    assert run(parser, "one", struct.pack("<I", HEADER)) == "parse=0 init=-1"


def test_oversized_string_is_bounded(parser):
    """A length running past the blob is refused without reading past it."""
    b = Blob().dict(3).string("EPICName")
    b.data += tag(STRING, 0xFFFFFF) + b"abc"
    assert run(parser, "one", b.bytes()) == "parse=0 init=-1"


def test_oversized_skip_is_bounded(parser):
    """A skipped blob longer than the stream leaves no property to parse."""
    b = Blob().dict(4).string("Raw")
    b.data += tag(BLOB, 0xFFFFFF)
    b.string("EPICName").string("n")
    assert run(parser, "one", b.bytes()) == "parse=0 init=-1"


def test_every_truncation_is_refused(parser):
    full = Blob().dict(5).string("Nested").dict(1).string("k").array(2).int64(1).boolean(True)
    full.string("EPICName").string("dcpav-service-epic")
    full.string("EPICProviderClass").string("DCPDP13Service")
    full.string("Raw").blob(b"\x00" * 5)
    full.string("EPICUnit").int64(9)
    data = full.bytes()
    assert run(parser, "one", data).endswith("unit=9")
    assert run(parser, "truncate", data) == "ok %d" % len(data)


def test_corrupted_announces(parser):
    data = (Blob().dict(4).string("Nested").dict(1).string("k").array(1).blob(b"\x01\x02")
            .string("EPICName").string("dcpav-service-epic")
            .string("EPICProviderClass").string("DCPDP13Service")
            .string("EPICUnit").int64(0)).bytes()
    out = run(parser, "mutate", data, 20000)
    iters, accepted = map(int, out.split()[1:])
    assert iters == 20000
    # Some flips land in string payloads and are still valid announces.
    assert 0 < accepted < iters
