# SPDX-License-Identifier: MIT
"""The host's side of the virtualized SPTM's parks (proxyclient/aura/hvpark.py, tools/sptm_park.py,
run_guest_sptm.py's tail and patch log) against src/sptm_virt_park.c.

What must not happen, each checked below:
  - a START split across reads, at any byte, or holding newline and sync bytes in its fields, is
    missed, or leaks into the console text, or the text before it is lost;
  - console text that only looks like a sync word is taken for a park;
  - the host and the HV disagree on the request bytes, START's resume flag or code, or the layout
    of sptm_virt_park_info (a park would never come, or --continue would end a one-way park);
  - the late NVRAM write enable writes anything while a guest core isn't stopped or while any site
    holds something other than its patch word, writes the wrong word or site, or leaves the guest
    with some sites restored and some not;
  - the sites it restores aren't the ones run_guest_sptm patched at boot.
"""

import importlib.util
import re
import struct
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from host_compiler import host_cc  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "proxyclient"))
from aura import hvpark  # noqa: E402

SRC = (REPO / "src/sptm_virt_park.c").read_text()

TEXT = b"SPTM-virt[gpu]: the write the host asked for: parking cpu9 in the proxy at XNU pc 0x1\n"
# A context address whose bytes hold a newline and the first bytes of a sync word.
START = hvpark.start_packet(hvpark.START_HV, 0x47505521, 0x10000a55ff0a, hvpark.START_RESUMES)


def tail_lines(chunks):
    lines = []
    t = hvpark.ConsoleTail(None, lines.append)
    for i, c in enumerate(chunks):
        st = t.feed(c)
        if st:
            return lines, st, t.buf + b"".join(chunks[i + 1:])
    t.flush()
    return lines, None, t.buf


def test_start_found_at_every_split():
    stream = TEXT + b"partial line, no newline" + START + b"\xff\x55\xaa\x01after"
    for cut in range(1, len(stream)):
        for cut2 in (cut, min(len(stream), cut + 7)):
            lines, st, rest = tail_lines([stream[:cut], stream[cut:cut2], stream[cut2:]])
            assert st, cut
            assert lines == [TEXT[:-1].decode(), "partial line, no newline"], cut
            assert st.resumes and st.name == "GPU!" and st.info == 0x10000a55ff0a
            assert rest == b"\xff\x55\xaa\x01after"


def test_text_that_looks_like_a_sync_word_is_text():
    junk = b"x\xff\x55\xaa\x04" + b"\0" * 40 + b"\n"
    lines, st, _ = tail_lines([TEXT, junk, b"end\n"])
    assert st is None
    assert lines[0] == TEXT[:-1].decode() and lines[-1] == "end" and len(lines) == 3


def test_one_way_start():
    raw = hvpark.start_packet(hvpark.START_HV, 0x52535421, 0x1234, 0)
    _, st, _ = tail_lines([raw])
    assert st and not st.resumes and st.name == "RST!"


def test_host_and_hv_agree():
    magic = int(re.search(r"#define PARK_MAGIC\s+(0x[0-9a-f]+)UL", SRC)[1], 16)
    assert struct.pack("<Q", magic) == hvpark.PARK_MAGIC
    code = int(re.search(r"#define PARK_CODE_HOST (0x[0-9a-f]+)", SRC)[1], 16)
    assert hvpark.CODES[code] == "PRK!"
    assert re.search(r"#define PARK_START_RESUMES \(\(void \*\)1\)", SRC)
    assert hvpark.START_RESUMES == 1


def test_park_info_layout(tmp_path):
    struct_src = re.search(r"struct sptm_virt_park_info \{.*?\} sptm_virt_park_info;", SRC, re.S)[0]
    fields = re.findall(r"^\s+(?:u32|s32|u64) ([a-z_, ]+);", struct_src, re.M)
    names = [n.strip() for f in fields for n in f.split(",")]
    assert tuple(names) == hvpark.PARK_INFO_FIELDS
    prog = ("#include <stdio.h>\n#include <stddef.h>\n#include <stdint.h>\n"
            "typedef uint32_t u32; typedef int32_t s32; typedef uint64_t u64;\n"
            + struct_src + "\nint main(void) {\n"
            + "".join(f'    printf("%zu ", offsetof(struct sptm_virt_park_info, {n}));\n'
                      for n in names)
            + '    printf("%zu\\n", sizeof(struct sptm_virt_park_info));\n    return 0;\n}\n')
    (tmp_path / "l.c").write_text(prog)
    subprocess.check_call(host_cc() + ["-o", str(tmp_path / "l"), str(tmp_path / "l.c")])
    got = [int(x) for x in subprocess.check_output([str(tmp_path / "l")], text=True).split()]
    fmt = hvpark.PARK_INFO.format.lstrip("<")
    want, off = [], 0
    for ch in re.findall(r"\d*[IiQ]", fmt):
        n = int(ch[:-1] or 1)
        for _ in range(n):
            want.append(off)
            off += 8 if ch[-1] == "Q" else 4
    assert got == want + [hvpark.PARK_INFO.size]


class FakePark:
    """The parked HV's proxy, as far as the NVRAM restore sees it."""

    def __init__(self, words, unstopped=(0,), fail_at=None):
        self.mem = dict(words)
        self.unstopped = list(unstopped)
        self.fail_at = fail_at
        self.pokes = []
        self.sym = {"sptm_virt_park_unstopped": 1, "sptm_virt_park_poke32": 2,
                    "sptm_virt_park_info": 0x1000}

    def call(self, addr, *a):
        if addr == 1:
            return self.unstopped.pop(0) if len(self.unstopped) > 1 else self.unstopped[0]
        pa, expect, val = a
        self.pokes.append((pa, expect, val))
        if pa == self.fail_at:
            self.fail_at = None
            return (-4) & 0xffffffffffffffff
        if self.mem[pa] != expect:
            return (-3) & 0xffffffffffffffff
        self.mem[pa] = val
        return 0

    def readmem(self, addr, size):
        if addr == 0x1000:
            return bytes(hvpark.PARK_INFO.size)
        return struct.pack("<I", self.mem[addr])


SITES = [("AppleARMNORNVRAM::write", 0x10001abc73c, 0xaa0003e8, 0x52800000),
         ("AppleARMNORNVRAM::write return", 0x10001abc740, 0x52805840, 0xd65f03c0)]
PATCHED = {pa: patched for _, pa, _, patched in SITES}


def restore(fake, secs=0.5):
    return hvpark.restore_nvram_write(fake.call, fake.readmem, fake.sym, SITES, secs,
                                      out=lambda *a: None)


def test_restore_puts_the_words_back_in_order():
    f = FakePark(PATCHED, unstopped=(0x30, 0x10, 0))  # two cores arrive while it waits
    restore(f)
    assert f.pokes == [(pa, patched, orig) for _, pa, orig, patched in SITES]
    assert f.mem == {pa: orig for _, pa, orig, _ in SITES}


def test_restore_refuses_a_core_that_isnt_stopped():
    f = FakePark(PATCHED, unstopped=(0x8,))
    with pytest.raises(SystemExit, match="0x8 aren't stopped"):
        restore(f, 0.3)
    assert not f.pokes and f.mem == PATCHED


def test_restore_refuses_any_word_but_the_patch():
    for _, pa, orig, _ in SITES:
        f = FakePark({**PATCHED, pa: orig})
        with pytest.raises(SystemExit, match="not the patch word"):
            restore(f)
        assert not f.pokes


def test_restore_never_leaves_half_the_sites():
    f = FakePark(PATCHED, fail_at=SITES[1][1])
    with pytest.raises(SystemExit, match="stays disabled"):
        restore(f)
    assert f.mem == PATCHED  # the first site is back to its patch word


def test_sites_are_the_ones_patched_at_boot(tmp_path, monkeypatch, capsys):
    spec = importlib.util.spec_from_file_location("rgs", REPO / "proxyclient/tools/run_guest_sptm.py")
    rgs = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(rgs)
    quirk = next(q for q in rgs.XNU_QUIRKS if q["fingerprint"][1] == 0x3acaa78)  # T8132 26B123
    base = 0x10001a00000
    mem = {base + off: orig for _, _, off, orig, _ in quirk["nvram_ro"] + quirk["pac_keys"]}

    class P:
        def read32(self, a):
            return mem[a]

        def write32(self, a, v):
            mem[a] = v

        def call(self, *a):
            pass

    class Plan:
        regions = {"BootKC-rx": (base, 0x4000000)}

    rgs.XNU_PATCHES.clear()
    rgs.patch_xnu(P(), {"sptm_virt_flush": 0}, Plan, quirk["pac_keys"], "pac_keys")
    rgs.patch_xnu(P(), {"sptm_virt_flush": 0}, Plan, quirk["nvram_ro"], "nvram_ro")
    log = tmp_path / "xnu-patches.json"
    monkeypatch.setenv("AURA_XNU_PATCH_LOG", str(log))
    rgs.write_patch_log()
    want = [(w, base + off, orig, new) for w, _, off, orig, new in quirk["nvram_ro"]]
    assert hvpark.nvram_sites(patch_log=log) == want
    hv_log = tmp_path / "hv.log"
    hv_log.write_text(capsys.readouterr().out)
    names = {w for w, *_ in quirk["nvram_ro"]}
    assert hvpark.nvram_sites(hv_log=hv_log, names=names) == want
    with pytest.raises(SystemExit, match="twice"):
        hv_log.write_text(hv_log.read_text() * 2)
        hvpark.nvram_sites(hv_log=hv_log, names=names)
