# SPDX-License-Identifier: MIT
"""The description of a DCP handed on running (kboot.c dt_publish_live_rtkit()).

dt_publish_live_rtkit() and the reserved-memory helpers it calls are copied out of src/kboot.c
unchanged and run against in-tree libfdt on a small device tree. The RTKit session is a fixed
stand-in with the shape the M2 Mac mini's dcpext hands on: a firmware-placed crashlog and two
loader-owned buffers (syslog, ioreport).

Checked: the description lands on the DCP's node even though the buffer reservations move the
tree first, with the RTKit handoff properties other handed-over co-processors use
(apple,rtkit-handoff "dcp-live-v1", apple,rtkit-buffers in <ep dva_hi dva_lo size is_mapped>
tuples) plus apple,rtkit-version and apple,rtkit-epmap; every loader-owned buffer gets a
no-map reserved-memory region whose iommu-addresses name the DCP and the buffer's IOVA, referenced from
the DCP's memory-region; a firmware-placed buffer gets none and is_mapped 1; and a loader-owned
buffer outside the reserved range, or not in whole 16K pages, fails the boot (the DCP is already
running and DMAs into it) without describing anything.

The session reaches dt_publish_live_rtkit() through display.c's display_live_rtkit_state(), also
copied unchanged with the record layout of display.h and rtkit.h. In an installed Stage 1 ->
Stage 2 chain the stage that handed the DCP on is not the one that boots the kernel: Stage 1's
display_live_rtkit_record() makes the record chainload.c passes on in /chosen
(test_chainload_run_base.py checks where it lands) and Stage 2 publishes from it. Checked: the
tree Stage 2 produces from the record is the one the handing stage itself produces; the same
buffer checks stop its boot; a record that is missing, of another size or version, or malformed
stops the boot without describing anything; and a session the handing stage could not describe
leaves the tree alone in both stages.
"""
from pathlib import Path
import os
import subprocess

import pytest

SOURCE = Path(os.environ.get("AURA_SOURCE", Path(__file__).resolve().parents[2]))

FUNCTIONS = (
    "static int dt_device_set_reserved_mem(int node, const char *name, uint32_t phandle, u64 iova,",
    "static int dt_get_or_add_reserved_mem(const char *node_name, const char *compat, bool nomap,",
    "static int dt_device_add_mem_region(const char *alias, uint32_t phandle, const char *name)",
    "static int dt_reserve_live_rtkit_buffer(const char *path, const char *alias,",
    "static int dt_publish_live_rtkit(u64 lo, u64 hi)",
)

DISPLAY_FUNCTIONS = (
    "void display_live_rtkit_record(struct display_live_rtkit_record *rec)",
    "static int display_parse_live_rtkit_record(const void *prop, u32 len,",
    "int display_live_rtkit_state(const struct rtkit_live_state **st, const char **alias)",
)

HARNESS = r'''
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libfdt.h"
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define BIT(x) (1UL << (x))
#define SZ_16K 0x4000
@LAYOUT@
#define bail(...) do { printf(__VA_ARGS__); return -1; } while (0)
static void *dt;
static struct rtkit_live_state session = {
    12, {0x11f, 0x821fff}, 3,
    {{1, 0, 0x802424000, 0x9e5010000, 0x10000},
     {2, 1, 0x810004000, 0x80696c000, 0x4000},
     {4, 1, 0x810000000, 0x80695c000, 0x4000}}};

/* What display.c sees: this stage's own hand-on, or the private ADT an earlier stage left. */
typedef struct { const char dcp_alias[8]; } display_config_t;
static const display_config_t config = {"dcpext"};
static bool handed_on, described = true, inherited;
static u8 chosen_prop[2 * sizeof(struct display_live_rtkit_record)];
static u32 chosen_len;
static bool have_prop;
static void *adt = chosen_prop;
static const display_config_t *display_get_config(void) { return &config; }
static bool dcp_handed_on_live(void) { return handed_on; }
static const struct rtkit_live_state *dcp_live_rtkit_state(void)
{
    return handed_on && described ? &session : NULL;
}
static bool display_inherited_live_heap(u64 *start) { (void)start; return inherited; }
static int adt_path_offset(const void *tree, const char *path)
{
    return tree == adt && !strcmp(path, "/chosen") ? 8 : -1;
}
static const void *adt_getprop(const void *tree, int node, const char *name, u32 *len)
{
    if (tree != adt || node != 8 || strcmp(name, DISPLAY_LIVE_RTKIT_ADT_PROP) || !have_prop)
        return NULL;
    *len = chosen_len;
    return chosen_prop;
}
@DISPLAY_FUNCTIONS@
static int dt_reserve_live_rtkit_buffer(const char *path, const char *alias,
                                        const struct rtkit_live_buffer *b);
@FUNCTIONS@

/* Stage 1 hands the DCP on and records it; the stage under test only inherits the record. */
static void chain(const char *how)
{
    struct display_live_rtkit_record rec;
    handed_on = true;
    described = strcmp(how, "chain-undescribed");
    display_live_rtkit_record(&rec);
    handed_on = false;
    inherited = true;
    /* Nothing of Stage 1 but the record survives the chainload */
    memset(&session, 0xee, sizeof(session));

    have_prop = strcmp(how, "chain-missing");
    chosen_len = sizeof(rec);
    if (!strcmp(how, "chain-short"))
        chosen_len -= sizeof(struct rtkit_live_buffer);
    else if (!strcmp(how, "chain-long"))
        chosen_len += 4;
    else if (!strcmp(how, "chain-version"))
        rec.version++;
    else if (!strcmp(how, "chain-size"))
        rec.size -= 4;
    else if (!strcmp(how, "chain-flags"))
        rec.flags |= 2;
    else if (!strcmp(how, "chain-reserved"))
        rec.reserved = 1;
    else if (!strcmp(how, "chain-alias"))
        memset(rec.alias, 'd', sizeof(rec.alias));
    else if (!strcmp(how, "chain-noalias"))
        rec.alias[0] = 0;
    else if (!strcmp(how, "chain-buffers"))
        rec.rtkit.n_buffers = RTKIT_LIVE_MAX_BUFFERS + 1;
    memcpy(chosen_prop, &rec, sizeof(rec));
}

int main(int argc, char **argv)
{
    static char in[1 << 16];
    FILE *f = fopen(argv[1], "rb");
    size_t n = fread(in, 1, sizeof(in), f);
    fclose(f);
    (void)n;
    dt = malloc(1 << 20);
    if (fdt_open_into(in, dt, 1 << 20))
        return 2;
    if (strcmp(argv[5], "-")) /* move the syslog buffer */
        session.buffers[1].phys = strtoull(argv[5], NULL, 0);
    if (!strcmp(argv[6], "local"))
        handed_on = true;
    else if (!strcmp(argv[6], "local-undescribed"))
        handed_on = true, described = false;
    else if (strcmp(argv[6], "none"))
        chain(argv[6]);
    int ret = dt_publish_live_rtkit(strtoull(argv[3], NULL, 0), strtoull(argv[4], NULL, 0));
    if (fdt_check_full(dt, fdt_totalsize(dt)))
        return 3;
    f = fopen(argv[2], "wb");
    fwrite(dt, 1, fdt_totalsize(dt), f);
    fclose(f);
    return ret ? 1 : 0;
}
'''

# The DCP node sits after /reserved-memory, so adding regions there moves it.
DTS = """/dts-v1/;
/ {
	#address-cells = <2>;
	#size-cells = <2>;
	aliases { dcpext = "/soc/dcp@271c00000"; };
	reserved-memory {
		#address-cells = <2>;
		#size-cells = <2>;
		ranges;
	};
	soc {
		#address-cells = <2>;
		#size-cells = <2>;
		dart: iommu@27130c000 { reg = <0x2 0x7130c000 0x0 0x4000>; #iommu-cells = <1>; };
		dcp@271c00000 {
			reg = <0x2 0x71c00000 0x0 0x4000>;
			iommus = <&dart 5>;
		};
	};
};
"""


def _fdt_cells(dtb, path, prop):
    out = subprocess.run(["fdtget", "-t", "x", str(dtb), path, prop], capture_output=True,
                         text=True)
    return [int(x, 16) for x in out.stdout.split()] if out.returncode == 0 else None


def _fdt_strings(dtb, path, prop):
    out = subprocess.run(["fdtget", "-t", "s", str(dtb), path, prop], capture_output=True,
                         text=True)
    return out.stdout.split() if out.returncode == 0 else None


def _between(text, start, end):
    start = text.index(start)
    return text[start:text.index(end, start)]


def _functions(text, heads):
    bodies = []
    for head in heads:
        # the definition, not a forward declaration: the first of '{' and ';' after it is '{'
        start = text.index(head)
        while text.index(";", start) < text.index("{", start):
            start = text.index(head, start + 1)
        end = text.index("\n}\n", start) + 3
        bodies.append(text[start:end])
    return "\n".join(bodies)


def _publish(harness, name, mode, lo="0x806000000", hi="0x807000000", syslog="-", dtb=None):
    exe, in_dtb, tmp = harness
    out = tmp / (name + ".dtb")
    run = subprocess.run([str(exe), str(dtb or in_dtb), str(out), lo, hi, syslog, mode],
                         capture_output=True, text=True)
    return run, out


def _undescribed(out):
    dcp = "/soc/dcp@271c00000"
    return all(_fdt_cells(out, dcp, prop) is None
               for prop in ("apple,rtkit-handoff", "apple,rtkit-buffers", "apple,rtkit-version",
                            "apple,rtkit-epmap", "memory-region"))


@pytest.fixture(scope="module")
def harness(tmp_path_factory):
    for tool in ("cc", "dtc", "fdtget"):
        if subprocess.run(["sh", "-c", "command -v " + tool], capture_output=True).returncode:
            pytest.skip(tool + " not available")
    tmp = tmp_path_factory.mktemp("live_rtkit")
    code = tmp / "live.c"
    rtkit_h = (SOURCE / "src/rtkit.h").read_text()
    display_h = (SOURCE / "src/display.h").read_text()
    display_c = (SOURCE / "src/display.c").read_text()
    layout = (_between(rtkit_h, "#define RTKIT_LIVE_MAX_BUFFERS", "bool rtkit_get_live_state(") +
              _between(display_h, "#define DISPLAY_LIVE_RTKIT_ADT_PROP",
                       "void display_live_rtkit_record(") +
              _between(display_c, "_Static_assert(sizeof(struct display_live_rtkit_record)",
                       "/*\n * The record of the DCP this stage handed on running"))
    code.write_text(HARNESS.replace("@LAYOUT@", layout)
                    .replace("@DISPLAY_FUNCTIONS@", _functions(display_c, DISPLAY_FUNCTIONS))
                    .replace("@FUNCTIONS@", _functions((SOURCE / "src/kboot.c").read_text(),
                                                       FUNCTIONS)))
    libfdt = SOURCE / "src/libfdt"
    exe = tmp / "live"
    subprocess.run(["cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wno-unused-function",
                    "-I", str(libfdt), str(code)] +
                   [str(libfdt / f) for f in ("fdt.c", "fdt_ro.c", "fdt_rw.c", "fdt_wip.c",
                                              "fdt_sw.c", "fdt_strerror.c", "fdt_check.c")] +
                   ["-o", str(exe)], check=True)
    dts = tmp / "in.dts"
    dts.write_text(DTS)
    dtb = tmp / "in.dtb"
    subprocess.run(["dtc", "-q", "-I", "dts", "-O", "dtb", "-o", str(dtb), str(dts)], check=True)
    return exe, dtb, tmp


def test_description_and_buffer_regions(harness):
    run, out = _publish(harness, "ok", "local")
    assert run.returncode == 0, run.stdout
    dcp = "/soc/dcp@271c00000"
    assert _fdt_strings(out, dcp, "apple,rtkit-handoff") == ["dcp-live-v1"]
    assert _fdt_cells(out, dcp, "apple,rtkit-version") == [12]
    assert _fdt_cells(out, dcp, "apple,rtkit-epmap") == [0x11f, 0x821fff, 0, 0, 0, 0, 0, 0]
    assert _fdt_cells(out, dcp, "apple,rtkit-buffers") == [
        1, 0x8, 0x02424000, 0x10000, 1,
        2, 0x8, 0x10004000, 0x4000, 0,
        4, 0x8, 0x10000000, 0x4000, 0]
    assert _fdt_cells(out, dcp, "apple,rtkit-live") is None
    assert _fdt_cells(out, dcp, "apple,rtkit-live-buffers") is None
    phandle = _fdt_cells(out, dcp, "phandle")
    assert phandle and phandle[0]
    assert _fdt_strings(out, dcp, "memory-region-names") == ["rtkit-ep2", "rtkit-ep4"]
    regions = _fdt_cells(out, dcp, "memory-region")
    assert regions and len(regions) == 2
    for ep, iova, phys in ((2, 0x810004000, 0x80696c000), (4, 0x810000000, 0x80695c000)):
        node = "/reserved-memory/dcpext-rtkit-ep%d@%x" % (ep, phys)
        assert _fdt_cells(out, node, "reg") == [phys >> 32, phys & 0xffffffff, 0, 0x4000]
        assert _fdt_cells(out, node, "iommu-addresses") == [phandle[0], iova >> 32,
                                                             iova & 0xffffffff, 0, 0x4000]
        assert _fdt_cells(out, node, "phandle")[0] in regions
        # no-map: the kernel maps the pages write-combined, never also in its linear map
        assert _fdt_cells(out, node, "no-map") == []
    # The firmware-placed crashlog lives in a carveout the DT already describes
    assert _fdt_cells(out, "/reserved-memory/dcpext-rtkit-ep1@9e5010000", "reg") is None


def test_unreserved_owned_buffer_fails_the_boot(harness):
    # The reserved range misses the syslog buffer
    run, out = _publish(harness, "unreserved", "local", "0x80695c000", "0x806960000")
    assert run.returncode == 1, run.stdout
    assert "buffer ep 2 [0x80696c000+0x4000) outside [0x80695c000, 0x806960000)" in run.stdout
    assert _fdt_cells(out, "/soc/dcp@271c00000", "apple,rtkit-handoff") is None
    assert _fdt_cells(out, "/soc/dcp@271c00000", "memory-region") is None


def test_missing_node_fails_the_boot(harness):
    exe, dtb, tmp = harness
    nodts = tmp / "nonode.dts"
    nodts.write_text(DTS.replace('dcpext = "/soc/dcp@271c00000";', 'dcpext = "/soc/dcp@0";'))
    nodtb = tmp / "nonode.dtb.in"
    subprocess.run(["dtc", "-q", "-I", "dts", "-O", "dtb", "-o", str(nodtb), str(nodts)],
                   check=True)
    run, out = _publish(harness, "nonode", "local", dtb=nodtb)
    assert run.returncode == 1, run.stdout
    assert "no node for alias dcpext" in run.stdout


def test_unaligned_owned_buffer_fails_the_boot(harness):
    # A no-map region that is not whole pages would unmap the kernel's neighbouring pages too
    run, out = _publish(harness, "unaligned", "local", syslog="0x80696e000")
    assert run.returncode == 1, run.stdout
    assert "buffer ep 2 [0x80696e000+0x4000) not 16K aligned" in run.stdout
    assert _fdt_cells(out, "/soc/dcp@271c00000", "apple,rtkit-handoff") is None
    assert _fdt_cells(out, "/soc/dcp@271c00000", "memory-region") is None


def test_no_handed_on_dcp_leaves_the_tree_alone(harness):
    run, out = _publish(harness, "none", "none")
    assert run.returncode == 0, run.stdout
    assert _undescribed(out)


def test_chained_stage_publishes_the_record_like_the_handing_stage(harness):
    local, local_out = _publish(harness, "local", "local")
    chain, chain_out = _publish(harness, "chain", "chain")
    assert local.returncode == 0 and chain.returncode == 0, local.stdout + chain.stdout
    assert _fdt_strings(chain_out, "/soc/dcp@271c00000", "apple,rtkit-handoff") == ["dcp-live-v1"]
    assert _fdt_strings(chain_out, "/soc/dcp@271c00000",
                        "memory-region-names") == ["rtkit-ep2", "rtkit-ep4"]
    assert _fdt_cells(chain_out, "/reserved-memory/dcpext-rtkit-ep2@80696c000",
                      "iommu-addresses")[1:] == [0x8, 0x10004000, 0, 0x4000]
    assert chain_out.read_bytes() == local_out.read_bytes()


def test_chained_stage_applies_the_buffer_checks(harness):
    run, out = _publish(harness, "chain-unreserved", "chain", "0x80695c000", "0x806960000")
    assert run.returncode == 1, run.stdout
    assert "buffer ep 2 [0x80696c000+0x4000) outside [0x80695c000, 0x806960000)" in run.stdout
    assert _undescribed(out)
    run, out = _publish(harness, "chain-unaligned", "chain", syslog="0x80696e000")
    assert run.returncode == 1, run.stdout
    assert "buffer ep 2 [0x80696e000+0x4000) not 16K aligned" in run.stdout
    assert _undescribed(out)


@pytest.mark.parametrize("how", ["missing", "short", "long", "version", "size", "flags",
                                 "reserved", "alias", "noalias", "buffers"])
def test_chained_stage_without_a_usable_record_fails_the_boot(harness, how):
    run, out = _publish(harness, "chain-" + how, "chain-" + how)
    assert run.returncode == 1, run.stdout
    assert "no usable record of the DCP an earlier stage handed on" in run.stdout
    assert _undescribed(out)


@pytest.mark.parametrize("mode", ["local-undescribed", "chain-undescribed"])
def test_undescribed_session_is_passed_on_as_such(harness, mode):
    # rtkit_get_live_state() had nothing to describe (no complete endpoint map, or a crash)
    run, out = _publish(harness, mode, mode)
    assert run.returncode == 0, run.stdout
    assert _undescribed(out)


def test_both_owners_publish_after_the_display_handoff():
    # One call for the stage that handed the DCP on and for the stage that inherited it, after
    # dt_set_display(): its handoff paths refuse a DCP node that already has a memory-region.
    text = (SOURCE / "src/kboot.c").read_text()
    prepare = text[text.index("int kboot_prepare_dt("):]
    call = ("    if ((local_dcp || inherited_dcp) && "
            "dt_publish_live_rtkit(live_rtkit_lo, live_rtkit_hi))")
    assert text.count("dt_publish_live_rtkit(live_rtkit_lo") == 1
    assert prepare.index("dt_set_display()") < prepare.index(call)
