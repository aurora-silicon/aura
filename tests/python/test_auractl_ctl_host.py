# SPDX-License-Identifier: MIT
"""src/sptm_virt_ctl.c against a fake guest (tests/ctl_host/ctl_host.c), and the mutations that
prove the harness notices: every safety check below, removed, must fail it.

Runs under pytest, or standalone. Needs a host with 16 KiB pages (an Apple Silicon Mac, or Linux
on one): aura's one-page window is a real mapping there, so a copy past it is a real fault."""

import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from host_compiler import host_cc  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / "src/sptm_virt_ctl.c"
HARNESS = REPO / "tests/ctl_host/ctl_host.c"
CFLAGS = ["-O1", "-g", "-w", "-DCTL_HOST", "-DAURACTL", f"-I{REPO}/src", f"-I{REPO}/tests/ctl_host"]

# name: [(text in src/sptm_virt_ctl.c, its replacement), ...], applied together
MUTATIONS = {
    "copy ignores the page bound": [("u64 n = min(len, 0x1000 - (uva & 0xfff));", "u64 n = len;")],
    "no guest-RAM check": [(
        "if (p < ctl.ram_base || p >= ctl.ram_end || (p >= (u64)_base && p < (u64)_end))\n"
        "        return AURACTL_EFAULT;", "")],
    "device memory allowed": [(
        "if (!((par >> 60) & 0xf)) /* PAR.ATTR[7:4] = 0: Device memory */\n"
        "        return AURACTL_EFAULT;", "")],
    "PAR not restored": [("msr(PAR_EL1, save);", "")],
    "alias left mapped": [("    ctl_unmap();\n    spin_unlock(&ctl_lock);",
                           "    spin_unlock(&ctl_lock);")],
    "window not invalidated after unmap": [('    sysop("tlbi alle2is");\n', "")],
    "read not bounded by the buffer": [(
        "min(r->buf_len, (u64)AURACTL_MAX_BUF) / sizeof(struct auractl_rec));", "(u64)~0UL);")],
    "RAM block split": [("    if (base + size > ctl_dram_base())\n        return; /* RAM is never", "    if (0)\n        return; /* RAM is never")],
    "RAM hookable": [
        ("    if (base + size > ctl_dram_base())\n        return; /* RAM is never", "    if (0)\n        return; /* RAM is never"),
        ("    if (page >= ctl_dram_base())\n        return false; /* MMIO only */", ""),
    ],
    "never-read list ignored": [("if (page == never[i])\n            return false;", ";")],
    "never-read list on every chip": [("if (chip_id == T8140) {", "if (1) {")],
    "PEEK without a never-read list": [("    if (!ctl_never(&nnever))\n        return AURACTL_EACCES;\n", "")],
    "PEEK of any catalogue range": [("allowed |= !strncmp(nm, ok[k], strlen(ok[k]));", "allowed = true;")],
    "wrong immediate accepted": [("(esr & 0xffff) != AURACTL_HVC_IMM || ", "")],
    "HVC magic not checked": [(" || (u32)regs[0] != AURACTL_HVC_MAGIC", "")],
    "request magic not checked": [("r.magic != AURACTL_REQ_MAGIC || ", "")],
    "request version not checked": [(" || r.version != AURACTL_VERSION", "")],
    "write permission not checked": [("int err = ctl_at(uva, to_user, &pa);",
                                      "int err = ctl_at(uva, false, &pa);")],
    "lock not taken": [("spin_lock(&ctl_lock);", "")],
    "stage-2 change outside the entry lock": [("    sptm_virt_s2_ctl_begin();\n", "")],
    "stage-2 reference not refreshed": [("sptm_virt_s2_ctl_end(total != 0);",
                                         "sptm_virt_s2_ctl_end(false);")],
    "partial arm skips the TLB work": [("    if (total)\n        ctl_s2_invalidate();",
                                        "    if (total && !err)\n        ctl_s2_invalidate();")],
    "hook budget not enforced": [("        if (!*budget)\n            return AURACTL_E2BIG;\n", "")],
}


class Skip(Exception):
    pass


def _skip(why):
    try:
        import pytest
    except ImportError:
        raise Skip(why) from None
    pytest.skip(why)


def _require_host():
    if os.sysconf("SC_PAGE_SIZE") != 0x4000:
        _skip("needs 16 KiB host pages")


def _build_run(source, out):
    subprocess.check_call(host_cc() + CFLAGS + ["-o", str(out), str(HARNESS), str(source)])
    return subprocess.run([str(out)], capture_output=True, text=True, timeout=120)


def test_handler_against_a_fake_guest():
    _require_host()
    r = _build_run(SRC, Path(tempfile.mkdtemp()) / "h")
    assert r.returncode == 0, r.stdout + r.stderr
    print(r.stdout.strip())


def test_every_check_is_noticed_when_removed():
    _require_host()
    src = SRC.read_text()
    d = Path(tempfile.mkdtemp())
    missed = []
    for name, edits in MUTATIONS.items():
        m = src
        for a, b in edits:
            assert m.count(a) == 1, f"{name}: the checked code changed"
            m = m.replace(a, b, 1)
        (d / "m.c").write_text(m)
        if _build_run(d / "m.c", d / "m").returncode == 0:
            missed.append(name)
    assert not missed, f"not noticed: {missed}"


if __name__ == "__main__":
    failed = 0
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
            except Skip as e:
                print(f"skip {name}: {e}")
            except Exception as e:  # noqa: BLE001
                failed += 1
                print(f"FAIL {name}: {type(e).__name__}: {str(e)[:600]}")
            else:
                print(f"ok   {name}")
    sys.exit(1 if failed else 0)
