# SPDX-License-Identifier: MIT
"""The auractl XNU-entry finder (proxyclient/aura/auractl_xnu.py).

These checks carry no Apple bytes. They pin the ABI the patch emits to the shared header, decode the
patch words independently of the encoder that built them, and prove the finder fails closed on input
that is not the kernelcache it knows. The end-to-end proof -- the finder's full plan against the real
26A428 kernelcache -- runs only when AURACTL_XNU_KC points at that (private) file, so nothing tainted
lives in the tree.
"""

import os
import re
import struct
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]

import sys

sys.path.insert(0, str(REPO / "proxyclient"))

from aura import auractl_xnu as x  # noqa: E402


def _abi_define(name):
    """The integer value of a #define in src/auractl_abi.h, the shared source of truth."""
    text = (REPO / "src" / "auractl_abi.h").read_text()
    m = re.search(rf"^#define\s+{name}\s+(0x[0-9A-Fa-f]+|\d+)", text, re.M)
    assert m, f"{name} not found in auractl_abi.h"
    return int(m.group(1), 0)


def test_abi_constants_match_header():
    """The finder mirrors AURACTL_HVC_IMM/MAGIC from the C header; drift would emit a wrong hvc."""
    assert x.AURACTL_HVC_IMM == _abi_define("AURACTL_HVC_IMM")
    assert x.AURACTL_HVC_MAGIC == _abi_define("AURACTL_HVC_MAGIC")


def test_patch_words_decode_to_the_documented_sequence():
    """Build the five words the finder writes and decode each one back, independently of the
    encoder, to the instruction the module's docstring promises:
        ldr x1,[uap] ; movz w0,#lo ; movk w0,#hi,lsl#16 ; hvc #IMM ; b <epilogue>."""
    uap, site, epilogue = 19, 0x10000, 0x10000 - 0x44  # a backward branch, as on the real KC
    ldr = x._ldr_x_imm0(1, uap)
    movz = x._movz_w(0, x.AURACTL_HVC_MAGIC & 0xFFFF)
    movk = x._movk_w_hw1(0, (x.AURACTL_HVC_MAGIC >> 16) & 0xFFFF)
    hvc = x._hvc(x.AURACTL_HVC_IMM)
    br = x._b(site + 16, epilogue)

    # ldr Xt,[Xn,#0]: LDR (unsigned offset, 64-bit) opcode, imm12 == 0, Rn == uap, Rt == 1.
    assert ldr & 0xFFC00000 == 0xF9400000 and (ldr >> 10) & 0xFFF == 0
    assert (ldr >> 5) & 0x1F == uap and ldr & 0x1F == 1
    # movz/movk into w0, hw fields 0 and 1, the two halves of the magic.
    assert movz >> 23 == (0x52800000 >> 23) and (movz >> 21) & 3 == 0  # hw == 0
    assert (movz >> 5) & 0xFFFF == x.AURACTL_HVC_MAGIC & 0xFFFF and movz & 0x1F == 0
    assert movk >> 23 == (0x72800000 >> 23) and (movk >> 21) & 3 == 1  # hw == 1 (lsl #16)
    assert (movk >> 5) & 0xFFFF == (x.AURACTL_HVC_MAGIC >> 16) & 0xFFFF and movk & 0x1F == 0
    # hvc #imm: 0xD4 .. ..010, imm16 in bits 20:5.
    assert hvc & 0xFFE0001F == 0xD4000002 and (hvc >> 5) & 0xFFFF == x.AURACTL_HVC_IMM
    # b <epilogue>: signed imm26 * 4 from the branch's own address (site + 16).
    assert br >> 26 == 0x5  # 0b000101
    imm = br & 0x3FFFFFF
    if imm & 0x2000000:
        imm -= 0x4000000
    assert (site + 16) + imm * 4 == epilogue


def test_fail_closed_on_non_macho():
    with pytest.raises(x.XnuEntryError):
        x.find_acct_entry(b"not a mach-o at all" * 64)


def test_fail_closed_on_truncated_macho():
    # A 64-bit Mach-O magic and nothing else: no segments, no fixups -- must raise, never guess.
    with pytest.raises(x.XnuEntryError):
        x.find_acct_entry(struct.pack("<I", 0xFEEDFACF) + bytes(60))


@pytest.mark.skipif(not os.environ.get("AURACTL_XNU_KC"),
                    reason="set AURACTL_XNU_KC to a decompressed 26A428 kernelcache (private)")
def test_plan_against_real_kernelcache():
    """The deliverable: the finder's full plan on the real kernelcache is internally consistent --
    the words it expects are the words in the file at the site, the replacement decodes to the
    documented sequence, and the branch lands on acct's epilogue. Gated on a private file."""
    data = Path(os.environ["AURACTL_XNU_KC"]).read_bytes()
    plan = x.find_acct_entry(data)

    assert plan["replacement"][0] == x._ldr_x_imm0(1, plan["uap_reg"])
    assert plan["replacement"][3] == x._hvc(x.AURACTL_HVC_IMM)
    # The expected words must be exactly what sits at the site in the file (the runner verifies the
    # same words against the loaded BootKC before it writes -- fail closed on any mismatch).
    tx_fo = x._segments(data)[0]["__TEXT_EXEC"][2]
    for i, w in enumerate(plan["expected"]):
        assert struct.unpack_from("<I", data, tx_fo + plan["text_off"] + 4 * i)[0] == w
    # The final branch returns into acct's errno epilogue.
    br = plan["replacement"][4]
    imm = br & 0x3FFFFFF
    if imm & 0x2000000:
        imm -= 0x4000000
    assert (plan["site_va"] + 16) + imm * 4 == plan["epilogue_va"]
