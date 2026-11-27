# SPDX-License-Identifier: MIT
"""Check that RELR-packed relocations relocate an image exactly as the RELA table did.

The Makefile links with -z pack-relative-relocs when the linker can (LD_RELR). The relative
relocations then go to .relr.dyn, which start.S hands to apply_relr() (startup.c), and only what
the linker could not pack stays in .rela.dyn for apply_rela(). A RELR word already holds its
link-time value and is relocated by adding the base; a RELA word is set to base + addend. Here a
small image with runs of pointers, gaps longer than one 63-word bitmap, a pointer in .text and one
in .rodata is linked with the real linker scripts twice, with the Makefile's LDFLAGS and with
-z pack-relative-relocs added, using each available AArch64 linker. Both links are relocated at a
non-zero base and must give the same image. apply_relr() itself is compiled for the host and run
on the packed table.
"""
import re
import shutil
import struct
import subprocess

import pytest

from test_rela_layout import LINKERS, NM, OBJCOPY, READELF, R_AARCH64_RELATIVE, assemble, ldflags
from test_smp_timeouts import REPO

BASE = 0x8_0004_0000
RELR = ["-z", "pack-relative-relocs"]

IMAGE = r'''
    .section .init, "ax"
    .globl _start
_start:
    adrp x0, table
    ldr x0, [x0, :lo12:table]
    adrp x1, text_pointer
    adrp x2, rodata_pointer
    b 1f
    .quad _image_footprint
1:
    ret
    .section .text, "ax"
    ret
    .balign 8
text_pointer:
    .quad table + 16
    .section .rodata, "a"
    .balign 8
rodata_pointer:
    .quad _start + 4
    .section .data, "aw"
    .balign 8
table:
    .rept 70
    .quad _start
    .endr
    .quad 0x1234
    .rept 5
    .quad table + 8
    .quad 0
    .endr
    .skip 8 * 100
    .quad table
    .skip 8 * 62
    .quad .
'''


def relr_locations(table):
    """The words a RELR table names, decoded as apply_relr() walks it."""
    locations, where = [], None
    for (entry,) in struct.iter_unpack("<Q", table):
        if not entry & 1:
            locations.append(entry)
            where = entry + 8
        else:
            locations += [where + 8 * bit for bit in range(63) if entry >> (bit + 1) & 1]
            where += 63 * 8
    return locations


def link(tmp_path, linker, script, name, extra):
    obj = assemble(tmp_path, IMAGE)
    elf = tmp_path / f"{name}.elf"
    result = subprocess.run([linker, "-T", str(script), *ldflags(), *extra, "-o", str(elf), str(obj)],
                            cwd=REPO, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    assert "orphan" not in result.stderr, result.stderr
    sym = {}
    for line in subprocess.run([NM, str(elf)], check=True, capture_output=True, text=True).stdout.splitlines():
        fields = line.split()
        if len(fields) == 3:
            sym[fields[2]] = int(fields[0], 16)
    raw = tmp_path / f"{name}.bin"
    subprocess.run([OBJCOPY, "-O", "binary", str(elf), str(raw)], check=True)
    sections = subprocess.run([READELF, "-S", "-W", str(elf)], check=True, capture_output=True, text=True).stdout
    return sym, bytearray(raw.read_bytes()), sections


def apply_rela(sym, image, base):
    relocated = set()
    for off in range(sym["_rela_start"], sym["_rela_end"], 24):
        where, info, addend = struct.unpack_from("<QQq", image, off)
        assert info & 0xFFFFFFFF == R_AARCH64_RELATIVE
        struct.pack_into("<Q", image, where, base + addend)
        relocated.add(where)
    return relocated


@pytest.mark.skipif(not (NM and READELF and OBJCOPY), reason="needs nm, readelf and objcopy")
@pytest.mark.parametrize("script", ["aura-raw.ld", "aura.ld"])
@pytest.mark.parametrize("kind", ["gnu", "lld"])
def test_relr_relocates_like_rela(tmp_path, kind, script):
    linker = LINKERS[kind]
    if not linker:
        pytest.skip(f"no {kind} AArch64 linker")
    probe = subprocess.run([linker, "-EL", "-maarch64elf", "-pie", *RELR, "--fatal-warnings", "-o",
                            str(tmp_path / "probe"), str(assemble(tmp_path, ".globl _start\n_start:\n", "probe"))],
                           capture_output=True, text=True)
    if probe.returncode:
        pytest.skip(f"{linker} cannot pack relocations: {probe.stderr.strip()}")

    rela_sym, rela_image, _ = link(tmp_path, linker, REPO / script, "rela", [])
    relr_sym, relr_image, sections = link(tmp_path, linker, REPO / script, "relr", RELR)

    # Every relative relocation is packed, into the section the script names, with no new step.
    assert re.search(r"\] \.relr\.dyn\s", sections), sections
    assert relr_sym["_rela_start"] == relr_sym["_rela_end"]
    assert relr_sym["_relr_end"] > relr_sym["_relr_start"] == rela_sym["_relr_start"]
    for name in ("_rodata_end", "_data_start", "_end"):
        assert relr_sym[name] == rela_sym[name], name

    relocated = apply_rela(rela_sym, rela_image, BASE)
    assert len(relocated) == 70 + 5 + 4
    table = bytes(relr_image[relr_sym["_relr_start"]:relr_sym["_relr_end"]])
    locations = relr_locations(table)
    assert sorted(locations) == sorted(relocated)
    for where in locations:
        (value,) = struct.unpack_from("<Q", relr_image, where)
        struct.pack_into("<Q", relr_image, where, value + BASE)

    # The relocated images are the same, apart from the relocation tables themselves.
    tables = slice(rela_sym["_relr_start"], rela_sym["_rodata_end"])
    rela_image[tables] = relr_image[tables] = b""
    assert relr_image == rela_image

    # startup.c's apply_relr(), built for the host, relocates the packed table the same way.
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not cc:
        return
    source = (REPO / "src" / "startup.c").read_text()
    function = re.search(r"^void apply_relr\(.*?^\}\n", source, re.S | re.M).group(0)
    harness = tmp_path / "relr.c"
    harness.write_text("""#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef uint64_t u64;
""" + function + """
/* relr IMAGE TABLE OUT: relocate IMAGE at the address it was read to, print that address */
int main(int argc, char **argv)
{
    static u64 image[1 << 16], table[1 << 10];
    FILE *f = fopen(argv[1], "rb");
    size_t n = fread(image, 1, sizeof(image), f);
    fclose(f);
    f = fopen(argv[2], "rb");
    size_t t = fread(table, 1, sizeof(table), f) / 8;
    fclose(f);
    apply_relr((uint64_t)image, table, table + t);
    f = fopen(argv[3], "wb");
    fwrite(image, 1, n, f);
    fclose(f);
    printf("%llx\\n", (unsigned long long)(uint64_t)image);
    return 0;
}
""")
    exe = tmp_path / "relr"
    subprocess.run([cc, "-O2", "-Wall", "-Werror", "-o", str(exe), str(harness)], check=True)
    _, packed, _ = link(tmp_path, linker, REPO / script, "packed", RELR)
    assert len(packed) <= 8 << 16 and len(table) <= 8 << 10
    (tmp_path / "packed.bin").write_bytes(packed)
    (tmp_path / "table.bin").write_bytes(table)
    host = int(subprocess.run([str(exe), str(tmp_path / "packed.bin"), str(tmp_path / "table.bin"),
                               str(tmp_path / "out.bin")], check=True, capture_output=True,
                              text=True).stdout, 16)
    out = (tmp_path / "out.bin").read_bytes()
    expected = bytearray(packed)
    for where in locations:
        (value,) = struct.unpack_from("<Q", expected, where)
        struct.pack_into("<Q", expected, where, value + host)
    assert out == expected
