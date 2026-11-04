# SPDX-License-Identifier: MIT
"""T6041 (M4 Pro J616s) display handoff without a /chosen opt-in.

Builds tests/t6041_display_host.c with the tree's src/kboot_display_handoff.c, the host stand-in
headers of tests/t8122_display_host/ and aura's libfdt (ASan, UBSan), and runs every scenario:
a supported boot with no chosen property publishes the existing contract; an unsupported chip,
board, firmware or device tree gets nothing at all; every missing ADT node and failed validation publishes
nothing. The ADT, the MMIO and the device tree are synthetic; no hardware is exercised.
"""
from pathlib import Path
import re
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]
LIBFDT = ["fdt.c", "fdt_ro.c", "fdt_rw.c", "fdt_wip.c", "fdt_sw.c", "fdt_empty_tree.c",
          "fdt_strerror.c", "fdt_addresses.c"]


@pytest.fixture(scope="module")
def host(tmp_path_factory):
    return build_host(tmp_path_factory.mktemp("t6041"))


def build_host(tmp, unknown_profile=False):
    cc = shutil.which("cc")
    if not cc:
        pytest.skip("no host C compiler")
    src = tmp / "src"
    src.mkdir()
    # The code under test next to the stand-ins, so that its #include "utils.h" finds them
    code = (ROOT / "src/kboot_display_handoff.c").read_text()
    if unknown_profile:
        # An unqualified profile must never match even when the loaded firmware is unknown too.
        start = code.index("static const struct dcp_soc dcp_soc_t6041 = {")
        end = code.index("\n};", start)
        record = code[start:end]
        assert ".os_fw = V26_6_2," in record
        code = code[:start] + record.replace(".os_fw = V26_6_2,", ".os_fw = V_UNKNOWN,") + code[end:]
    (src / "kboot_display_handoff.c").write_text(code)
    shutil.copy(ROOT / "tests/t6041_display_host.c", src)
    for header in (ROOT / "tests/t8122_display_host").glob("*.h"):
        shutil.copy(header, src)
    binary = tmp / "t6041-display-host"
    subprocess.run(
        [cc, "-std=gnu11", "-D_GNU_SOURCE", "-O1", "-g", "-Wall", "-Wno-unused-function",
         "-Wno-format", "-Wno-format-truncation", "-fsanitize=address,undefined",
         "-fno-omit-frame-pointer", "-fno-sanitize-recover=undefined",
         "-I", str(src), "-I", str(ROOT / "src"),
         str(src / "t6041_display_host.c"), str(src / "kboot_display_handoff.c"),
         *(str(ROOT / "src/libfdt" / f) for f in LIBFDT), "-o", str(binary)],
        check=True,
    )
    return binary


def scenarios(binary):
    out = subprocess.run([str(binary), "list"], check=True, capture_output=True, text=True)
    return out.stdout.split()


def run(binary, scenario):
    env = {"ASAN_OPTIONS": "detect_leaks=0", "PATH": "/usr/bin:/bin"}
    return subprocess.run([str(binary), scenario], capture_output=True, text=True, env=env)


SCENARIOS = re.findall(r'^    \{"([a-z0-9-]+)", (?:PUBLISH|NOTHING|REFUSE|REFUSE_NO_OSLOG),',
                       (ROOT / "tests/t6041_display_host.c").read_text(), re.M)


def test_scenario_list_matches(host):
    assert scenarios(host) == SCENARIOS
    assert sorted(REASONS) == sorted(SCENARIOS)
    for required in ("publish", "wrong-chip", "wrong-board", "no-display-dt", "no-adt-nub",
                     "no-adt-dart-dcp", "wrong-os", "not-running", "outside-segment"):
        assert required in SCENARIOS


PUBLISHED = "handoff published (firmware "
NOT_DESCRIBED = "the device tree does not describe the display block"
# The log line of the check each scenario is meant to reach, so that none passes for another reason
REASONS = {
    "publish": PUBLISHED,
    "external": PUBLISHED,
    "external-refuse": PUBLISHED,
    "stale-optin": PUBLISHED,
    "stale-optin-zero": PUBLISHED,
    "repeat": "handoff already published",
    "oslog-in-memreserve": "lies inside /memreserve/",
    "wrong-chip": "not a J616s/J614s (chip 0x6041): nothing done",
    "wrong-board": "not a J616s/J614s (chip 0x6040): nothing done",
    "j614-no-optin": PUBLISHED,
    "j614-optin": PUBLISHED,
    "no-display-dt": NOT_DESCRIBED,
    "no-dcp-alias": NOT_DESCRIBED,
    "wrong-dcp-compat": NOT_DESCRIBED,
    "wrong-disp-compat": NOT_DESCRIBED,
    "no-adt-nub": "no /arm-io/dcp/iop-dcp-nub",
    "no-adt-dart-dcp": "ADT dcp/dart-dcp/dart-disp0 not as expected",
    "no-adt-dart-disp0": "ADT dcp/dart-dcp/dart-disp0 not as expected",
    "no-adt-dcp-reg": "ADT dcp/dart-dcp/dart-disp0 not as expected",
    "no-adt-vram": "no /vram",
    "no-adt-carveouts": "no /chosen/carveout-memory-map",
    "no-adt-pt-region": "no /arm-io/dart-dcp/pt-region-23",
    "no-adt-clock-gates": "no ADT /arm-io/dcp clock-gate has a PMGR power state",
    "wrong-os": "no qualified OS firmware for apple,t6041-dcp: nothing done",
    "unknown-os": "no qualified OS firmware for apple,t6041-dcp: nothing done",
    "stale-optin-wrong-os": "no qualified OS firmware for apple,t6041-dcp: nothing done",
    "no-disp-alias": NOT_DESCRIBED,
    "stale-optin-no-dcp": NOT_DESCRIBED,
    "stale-optin-wrong-disp": NOT_DESCRIBED,
    "bad-uuid": "no well-formed ADT uuid",
    "not-preloaded": "iBoot did not leave the dcp firmware loaded and running",
    "not-running": "DCP not running, leaving dart-dcp unlocked",
    "power-off": "display power domain not active",
    "disp-unlocked": "DCP not running or its DARTs not locked",
    "outside-segment": "outside the firmware segments",
    "wrong-dcp-reg": "ADT dcp/dart-dcp/dart-disp0 not as expected",
    "wrong-sid": "dcp iommus is not <&dart 23>",
    "dcp-enabled": 'dcp is not status "disabled"',
    "dart-enabled": 'the DART of the display nodes is not status "disabled"',
    "mbox-enabled": "dcp mailbox missing or enabled",
    "no-piodma": "device tree has no dcp/disp0/disp0-piodma aliases",
    "segment-reserved": "overlaps /reserved-memory/other@0",
    "oslog-straddles": "straddles /memreserve/",
    "dt-full": "DT write failed (full?); handoff NOT published, nothing changed",
}


@pytest.mark.parametrize("scenario", SCENARIOS)
def test_t6041_scenario(host, scenario):
    result = run(host, scenario)
    assert result.returncode == 0, result.stdout + result.stderr
    assert REASONS[scenario] in result.stdout, result.stdout
    if REASONS[scenario] != PUBLISHED and scenario != "repeat" and scenario != "oslog-in-memreserve":
        assert PUBLISHED not in result.stdout, result.stdout
    assert result.stdout.rstrip().endswith(f"{scenario}: pass"), result.stdout
    assert "FAIL" not in result.stdout, result.stdout


def test_publish_without_chosen_property(host):
    out = run(host, "publish").stdout
    assert "ok   no /chosen/asahi,t6041-dcp in the device tree handed to aura" in out
    assert "handoff published" in out
    assert "not armed" not in out


def test_unsupported_dt_does_nothing(host):
    out = run(host, "no-display-dt").stdout
    assert "the device tree does not describe the display block" in out
    assert "nothing done: no MMIO access at all" in out


def test_t6041_record_has_no_optin():
    source = (ROOT / "src/kboot_display_handoff.c").read_text()
    start = source.index("static const struct dcp_soc dcp_soc_t6041 = {")
    record = source[start:source.index("\n};", start)]
    assert ".optin" not in record
    assert 'optin_name = "asahi,t6041-dcp"' not in source
    assert "asahi,t6041-dcp" not in (ROOT / "src/kboot.c").read_text()


def test_other_socs_keep_their_optin():
    source = (ROOT / "src/kboot_display_handoff.c").read_text()
    for soc, optin in (("t6030", "asahi,t6030-dcp"), ("t8122", "asahi,t8122-dcp"),
                       ("j613_25g83", "asahi,t8122-dcp"), ("t6031", "asahi,t6031-dcp")):
        start = source.index(f"static const struct dcp_soc dcp_soc_{soc} = {{")
        record = source[start:source.index("\n};", start)]
        assert f'.optin = "{optin}",' in record, soc


def test_t8122_handoff_unchanged():
    """The T8122 host suite, including its unarmed (no opt-in) scenario."""
    if not shutil.which("cc") or not shutil.which("dtc"):
        pytest.skip("needs a host C compiler and dtc")
    result = subprocess.run(["sh", str(ROOT / "tests/run-t8122-display-handoff.sh")],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stdout[-4000:] + result.stderr[-4000:]
    assert "T8122 display handoff: every scenario passes" in result.stdout


@pytest.mark.parametrize("scenario", ["wrong-os", "unknown-os", "stale-optin-wrong-os",
                                      "stale-optin-no-dcp", "stale-optin-wrong-disp"])
def test_unqualified_profile_does_nothing(host, scenario):
    result = run(host, scenario)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "nothing done: the device tree is byte for byte unchanged" in result.stdout
    assert "nothing done: no MMIO access at all" in result.stdout


def test_unknown_profile_does_not_match_unknown_firmware(tmp_path):
    binary = build_host(tmp_path, unknown_profile=True)
    result = run(binary, "unknown-os")
    assert result.returncode == 0, result.stdout + result.stderr
    assert "no qualified OS firmware for apple,t6041-dcp: nothing done" in result.stdout
    assert "nothing done: the device tree is byte for byte unchanged" in result.stdout
    assert "nothing done: no MMIO access at all" in result.stdout
