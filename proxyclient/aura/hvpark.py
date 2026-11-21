# SPDX-License-Identifier: MIT
"""The virtualized SPTM's parks from the host (src/sptm_virt_park.c).

- The request: PARK_MAGIC written on the proxy pipe while the guest runs.
- START: what a parked HV sends before its proxy loop. It arrives in the middle of the console
  text on the same pipe; ConsoleTail tells it from the text, so a park isn't taken for the end.
- The park's state in aura (sptm_virt_park_info), and the late NVRAM write enable: XNU's original
  words put back at the nvram_ro sites while every guest core is stopped.
"""

import json
import pathlib
import re
import struct
import subprocess
import time

PARK_MAGIC = b"\x1bAURAPRK"
REQ_BOOT = 0x04AA55FF
START_SYNC = struct.pack("<I", REQ_BOOT)
START_LEN = 36
START_HV = 3
START_RESUMES = 1  # START's reserved word for a park that P_EXIT resumes
CODES = {0x47505521: "GPU!", 0x50524b21: "PRK!", 0x52535421: "RST!", 0x534d5021: "SMP!",
         0x53325221: "S2R!"}

# struct sptm_virt_park_info
PARK_INFO = struct.Struct("<IiII7QII")
PARK_INFO_FIELDS = ("state", "cpu", "code", "relinks", "count", "waiting", "stragglers",
                    "wait_ticks", "t_start", "t_end", "requests", "poke_back", "pad")
PARK_STATES = {0: "none", 1: "held, resumes on P_EXIT", 2: "held, one-way"}

POKE_ERRORS = {-1: "this isn't the owner's proxy in a park that resumes",
               -2: "a guest core isn't stopped", -3: "the word there isn't the expected one",
               -4: "the write didn't take (read back from DRAM)",
               -5: "not 4-byte aligned guest RAM"}


def checksum(data):
    s = 0xDEADBEEF
    for c in data:
        s = (s * 31337 + (c ^ 0x5A)) & 0xFFFFFFFF
    return (s ^ 0xADDEDBAD) & 0xFFFFFFFF


class Start:
    def __init__(self, raw):
        _, status, self.reason, self.code, self.info, self.reserved, csum = struct.unpack(
            "<IiIIQQI", raw)
        self.ok = status == 0 and csum == checksum(raw[:-4])

    @property
    def resumes(self):
        return self.reason == START_HV and self.reserved == START_RESUMES

    @property
    def name(self):
        return CODES.get(self.code, f"0x{self.code:08x}")

    def __str__(self):
        how = "resumes on P_EXIT" if self.resumes else "one-way"
        return f"START {self.name}, {how}, context at 0x{self.info:x}"


def start_packet(reason, code, info=0, reserved=0):
    """A START as the HV sends it (for tests)."""
    raw = struct.pack("<IiIIQQ", REQ_BOOT, 0, reason, code, info, reserved)
    return raw + struct.pack("<I", checksum(raw))


class ConsoleTail:
    """aura's console as it arrives on the proxy device, line by line, until a START."""

    def __init__(self, dev, emit=None):
        self.dev = dev
        self.buf = b""  # bytes not yet known to be text (a START may begin in them)
        self.text = b""  # text after the last complete line
        self.emit = emit or (lambda line: print("TTY> " + line, flush=True))

    def _take(self, text, final=False):
        *lines, self.text = (self.text + text).split(b"\n")
        for line in lines:
            self.emit(line.decode("latin1", "replace").rstrip("\r"))
        if final and self.text:
            self.emit(self.text.decode("latin1", "replace").rstrip("\r"))
            self.text = b""

    def feed(self, chunk):
        """Take some bytes; a START if one is complete in them (the text before it is emitted,
        whatever follows it is kept in buf for the proxy's reader), else None."""
        self.buf += chunk
        while True:
            k = self.buf.find(START_SYNC)
            if k < 0:
                # Keep a tail that may be the start of the sync word.
                hold = next((n for n in (3, 2, 1)
                             if n <= len(self.buf) and START_SYNC.startswith(self.buf[-n:])), 0)
                self._take(self.buf[:len(self.buf) - hold])
                self.buf = self.buf[len(self.buf) - hold:]
                return None
            if len(self.buf) - k < START_LEN:
                self._take(self.buf[:k])
                self.buf = self.buf[k:]
                return None
            st = Start(self.buf[k:k + START_LEN])
            if st.ok:
                self._take(self.buf[:k], final=True)
                self.buf = self.buf[k + START_LEN:]
                return st
            # Text that only looks like a sync word: one byte of it is text, scan on.
            self._take(self.buf[:k + 1])
            self.buf = self.buf[k + 1:]

    def run(self, secs, idle=0, poll=None):
        """Read until a START (("park", start)), idle seconds of silence (("idle",)), secs in all
        (("time",)) or the device going away (("gone", error)). poll() runs between reads."""
        end = time.time() + secs
        last = time.time()
        while time.time() < end:
            if poll:
                poll()
            if idle and time.time() - last > idle:
                return ("idle",)
            try:
                chunk = self.dev.read(4096)
            except OSError as e:
                return ("gone", e)
            if not chunk:
                continue
            last = time.time()
            st = self.feed(chunk)
            if st:
                return ("park", st)
        return ("time",)

    def flush(self):
        self._take(self.buf, final=True)
        self.buf = b""


def request(dev):
    dev.write(PARK_MAGIC)
    dev.flush()


# ---- aura's side, by symbol ------------------------------------------------------------------

class Symbols:
    """aura symbols at their run addresses: run base + (nm(sym) - nm(_base))."""

    def __init__(self, elf, run_base, nm="llvm-nm"):
        self.elf = pathlib.Path(elf)
        self.syms = {}
        for line in subprocess.check_output([nm, str(elf)], text=True).splitlines():
            f = line.split()
            if len(f) == 3:
                self.syms[f[2]] = int(f[0], 16)
        self.run_base = run_base

    def __getitem__(self, name):
        return self.run_base + self.syms[name] - self.syms["_base"]

    def __contains__(self, name):
        return name in self.syms


def elf_load_bytes(elf, vaddr, size):
    """size bytes at vaddr in the ELF's PT_LOAD segments (as linked, before relocation)."""
    data = pathlib.Path(elf).read_bytes()
    phoff, = struct.unpack_from("<Q", data, 0x20)
    phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
    for i in range(phnum):
        p_type, _f, off, va, _pa, filesz, _m, _a = struct.unpack_from("<IIQQQQQQ", data,
                                                                      phoff + i * phentsize)
        if p_type == 1 and va <= vaddr and vaddr + size <= va + filesz:
            return data[off + vaddr - va:off + vaddr - va + size]
    return None


def check_image(readmem, sym, funcs=("sptm_virt_park_proxy", "sptm_virt_park_poke32",
                                     "sptm_virt_park_unstopped")):
    """Refuse a proxy that isn't running sym.elf: its code at funcs must be the ELF's."""
    for name in funcs:
        if name not in sym:
            raise SystemExit(f"{sym.elf} has no {name}: not an image with resumable parks")
        want = elf_load_bytes(sym.elf, sym.syms[name], 64)
        if want is None or readmem(sym[name], 64) != want:
            raise SystemExit(f"the proxy at base 0x{sym.run_base:x} isn't running {sym.elf} (its "
                             f"code at {name} differs): nothing called or written")


def park_info(readmem, sym):
    return dict(zip(PARK_INFO_FIELDS, PARK_INFO.unpack(
        readmem(sym["sptm_virt_park_info"], PARK_INFO.size))))


def s32(v):
    v &= 0xffffffff
    return v - (1 << 32) if v & 0x80000000 else v


# ---- the late NVRAM write enable ------------------------------------------------------------

NVRAM_QUIRK = "nvram_ro"
_PATCH_LINE = re.compile(r"^XNU (?P<what>.+?): \[0x(?P<pa>[0-9a-f]+)\] 0x(?P<before>[0-9a-f]{8}) "
                         r"-> 0x(?P<after>[0-9a-f]{8})\s*$")


def nvram_sites(patch_log=None, hv_log=None, names=None):
    """The nvram_ro sites patch_xnu wrote at boot, in order: [(what, pa, original, patched)].
    From run_guest_sptm's AURA_XNU_PATCH_LOG file, or from its printed lines in hv.log (names:
    the nvram_ro patch names to pick there)."""
    if patch_log:
        recs = json.loads(pathlib.Path(patch_log).read_text())
        sites = [(r["what"], r["pa"], r["original"], r["patched"]) for r in recs
                 if r["quirk"] == NVRAM_QUIRK]
    else:
        sites = []
        for line in pathlib.Path(hv_log).read_text("latin1").splitlines():
            m = _PATCH_LINE.match(line)
            if m and m["what"] in names:
                sites.append((m["what"], int(m["pa"], 16), int(m["before"], 16),
                              int(m["after"], 16)))
    pas = [s[1] for s in sites]
    if not sites:
        raise SystemExit("no nvram_ro sites recorded: was the guest booted with the nvram_ro "
                         "patch (not AURA_SHARE_NVRAM)?")
    if len(set(pas)) != len(pas):
        raise SystemExit("the record names an nvram_ro site twice: more than one boot in it?")
    return sites


def restore_nvram_write(call, readmem, sym, sites, confirm_secs=10.0, out=print):
    """Put XNU's original words back at the nvram_ro sites, from the owner's proxy in a park that
    resumes. Refuses, writing nothing, unless every guest core is stopped and every site holds
    exactly its patch word. Each word goes through sptm_virt_park_poke32 (which checks both again,
    writes, cleans and invalidates to the point of coherency, invalidates every I-cache and reads
    the word back from DRAM). A failure after the first site puts the patch words back, so the
    guest never runs with half of them. call(addr, *args) and readmem(addr, size) are the
    proxy's."""
    def read32(a):
        return struct.unpack("<I", readmem(a, 4))[0]

    end = time.time() + confirm_secs
    while True:
        missing = call(sym["sptm_virt_park_unstopped"])
        if not missing:
            break
        if time.time() > end:
            raise SystemExit(f"refused: guest cores 0x{missing:x} aren't stopped after "
                             f"{confirm_secs:g} s (running guest code, idle or inside aura): "
                             f"nothing written")
        time.sleep(0.1)
    out("every guest core is stopped")
    for what, pa, original, patched in sites:
        have = read32(pa)
        if have != patched:
            raise SystemExit(f"refused: {what} [0x{pa:x}] = 0x{have:08x}, not the patch word "
                             f"0x{patched:08x}: nothing written")
    done = []
    for what, pa, original, patched in sites:
        rc = s32(call(sym["sptm_virt_park_poke32"], pa, patched, original))
        if rc == 0:
            done.append((what, pa, original, patched))
            out(f"{what}: [0x{pa:x}] 0x{patched:08x} -> 0x{read32(pa):08x} (XNU's own)")
            continue
        msg = POKE_ERRORS.get(rc, f"error {rc}")
        if rc == -4:
            msg += f": reads 0x{park_info(readmem, sym)['poke_back']:08x}"
        for w2, pa2, orig2, patch2 in reversed(done):
            rc2 = s32(call(sym["sptm_virt_park_poke32"], pa2, orig2, patch2))
            out(f"{w2}: [0x{pa2:x}] back to the patch word 0x{patch2:08x}: "
                + ("ok" if rc2 == 0 else POKE_ERRORS.get(rc2, f"error {rc2}")))
        raise SystemExit(f"{what} [0x{pa:x}]: {msg}; the NVRAM write stays disabled")
    return done
