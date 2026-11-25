# SPDX-License-Identifier: MIT
"""Link a small image with the real linker scripts and check where relocation sections land.

start.S's load header stores _image_footprint with .quad. GNU ld keeps the dynamic relocation for
that .init word in a .rela.init piece (an R_AARCH64_NONE once the absolute symbol is resolved).
Unlisted, that piece was placed as an orphan after the closing ALIGN(0x10000) of .rela.dyn, which
moved .data, .bss and the stacks up by 64 KiB. The image here carries the same kind of entry plus
one relative relocation and is linked with the Makefile's LDFLAGS, using each available AArch64
linker. The checks: no orphan warning, only R_AARCH64_RELATIVE entries in [_rela_start, _rela_end)
(apply_rela() accepts nothing else), and nothing past the padding, so _rodata_end is still the
64 KiB boundary after _rela_end. With GNU ld, a control link without the script's .rela.init line
must show the old layout.
"""
import platform
import re
import shutil
import struct
import subprocess

import pytest

from test_smp_timeouts import REPO

R_AARCH64_RELATIVE = 1027
FIX_LINE = "        *(.rela.init)\n"

IMAGE = r'''
    .section .init, "ax"
    .globl _start
_start:
    adrp x0, ptr
    ldr x0, [x0, :lo12:ptr]
    b 1f
    .quad _image_footprint
1:
    ret
    .section .text, "ax"
    ret
    .section .rodata, "a"
    .ascii "rodata"
    .section .data, "aw"
    .balign 8
ptr:
    .quad _start
'''


def tool(*names):
    return next((path for path in map(shutil.which, names) if path), None)


def gnu_ld():
    native = ["ld.bfd", "ld"] if platform.machine() in ("aarch64", "arm64") else []
    for path in filter(None, map(shutil.which, ["aarch64-linux-gnu-ld", *native])):
        if "GNU ld" in subprocess.run([path, "--version"], capture_output=True, text=True).stdout:
            return path
    return None


LINKERS = {"gnu": gnu_ld(), "lld": tool("ld.lld")}
NM = tool("llvm-nm", "aarch64-linux-gnu-nm", "nm")
READELF = tool("llvm-readelf", "aarch64-linux-gnu-readelf", "readelf")
OBJCOPY = tool("llvm-objcopy", "aarch64-linux-gnu-objcopy", "objcopy")


def assemble(tmp_path):
    obj = tmp_path / "image.o"
    clang = shutil.which("clang")
    if clang:
        cmd = [clang, "--target=aarch64-linux-gnu", "-c", "-x", "assembler", "-", "-o", str(obj)]
    else:
        gcc = tool("aarch64-linux-gnu-gcc") or (shutil.which("gcc") if platform.machine() == "aarch64" else None)
        if not gcc:
            pytest.skip("needs an AArch64 assembler")
        cmd = [gcc, "-c", "-x", "assembler", "-", "-o", str(obj)]
    subprocess.run(cmd, input=IMAGE, text=True, check=True)
    return obj


def ldflags():
    make = (REPO / "Makefile").read_text().replace("\\\n", " ")
    flags = re.search(r"^LDFLAGS := (.*)$", make, re.M).group(1).split()
    assert "-pie" in flags
    return flags


def layout(tmp_path, linker, script, name):
    obj = assemble(tmp_path)
    elf = tmp_path / f"{name}.elf"
    result = subprocess.run([linker, "-T", str(script), *ldflags(), "-o", str(elf), str(obj)],
                            cwd=REPO, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    sym = {}
    for line in subprocess.run([NM, str(elf)], check=True, capture_output=True, text=True).stdout.splitlines():
        fields = line.split()
        if len(fields) == 3:
            sym[fields[2]] = int(fields[0], 16)
    sections = subprocess.run([READELF, "-S", "-W", str(elf)], check=True, capture_output=True, text=True).stdout
    match = re.search(r"\] \.rela\.dyn\s+\S+\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)", sections)
    vma, size = int(match.group(1), 16), int(match.group(2), 16)
    raw = tmp_path / f"{name}.rela"
    subprocess.run([OBJCOPY, "-O", "binary", "--only-section=.rela.dyn", str(elf), str(raw)], check=True)
    data = raw.read_bytes()[sym["_rela_start"] - vma:sym["_rela_end"] - vma]
    types = [struct.unpack_from("<QQq", data, off)[1] & 0xFFFFFFFF for off in range(0, len(data), 24)]
    return sym, vma + size, types, result.stderr


@pytest.mark.skipif(not (NM and READELF and OBJCOPY), reason="needs nm, readelf and objcopy")
@pytest.mark.parametrize("script", ["aura-raw.ld", "aura.ld"])
@pytest.mark.parametrize("kind", ["gnu", "lld"])
def test_rela_init_stays_inside_rela_dyn_padding(tmp_path, kind, script):
    linker = LINKERS[kind]
    if not linker:
        pytest.skip(f"no {kind} AArch64 linker")
    sym, end, types, warnings = layout(tmp_path, linker, REPO / script, "fixed")
    assert "orphan" not in warnings, warnings
    assert types and all(t == R_AARCH64_RELATIVE for t in types), types
    boundary = (sym["_rela_end"] + 0xFFFF) & ~0xFFFF
    assert end == boundary == sym["_rodata_end"], (hex(sym["_rela_end"]), hex(end), hex(sym["_rodata_end"]))

    if kind != "gnu":
        return
    # Control: without the .rela.init line, GNU ld places the piece after the padding, as it did.
    text = (REPO / script).read_text()
    assert text.count(FIX_LINE) == 1
    unlisted = tmp_path / script
    unlisted.write_text(text.replace(FIX_LINE, ""))
    sym, end, types, warnings = layout(tmp_path, linker, unlisted, "control")
    assert "orphan section `.rela.init'" in warnings, warnings
    assert sym["_rodata_end"] == end > ((sym["_rela_end"] + 0xFFFF) & ~0xFFFF)
