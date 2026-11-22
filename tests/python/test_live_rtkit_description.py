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

HARNESS = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libfdt.h"
typedef uint32_t u32;
typedef uint64_t u64;
#define RTKIT_LIVE_MAX_BUFFERS 4
#define RTKIT_LIVE_BUFFER_OWNED 1
#define SZ_16K 0x4000
struct rtkit_live_buffer { u32 ep; u32 flags; u64 iova; u64 phys; u64 size; };
struct rtkit_live_state {
    u32 version;
    u32 epmap[8];
    u32 n_buffers;
    struct rtkit_live_buffer buffers[RTKIT_LIVE_MAX_BUFFERS];
};
#define bail(...) do { printf(__VA_ARGS__); return -1; } while (0)
static void *dt;
static struct rtkit_live_state session = {
    12, {0x11f, 0x821fff}, 3,
    {{1, 0, 0x802424000, 0x9e5010000, 0x10000},
     {2, 1, 0x810004000, 0x80696c000, 0x4000},
     {4, 1, 0x810000000, 0x80695c000, 0x4000}}};
static const struct rtkit_live_state *display_live_rtkit_state(const char **alias)
{
    *alias = "dcpext";
    return &session;
}
static int dt_reserve_live_rtkit_buffer(const char *path, const char *alias,
                                        const struct rtkit_live_buffer *b);
@FUNCTIONS@
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
    if (argc > 5) /* move the syslog buffer */
        session.buffers[1].phys = strtoull(argv[5], NULL, 0);
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


@pytest.fixture(scope="module")
def harness(tmp_path_factory):
    for tool in ("cc", "dtc", "fdtget"):
        if subprocess.run(["sh", "-c", "command -v " + tool], capture_output=True).returncode:
            pytest.skip(tool + " not available")
    text = (SOURCE / "src/kboot.c").read_text()
    bodies = []
    for head in FUNCTIONS:
        # the definition, not a forward declaration: the first of '{' and ';' after it is '{'
        start = text.index(head)
        while text.index(";", start) < text.index("{", start):
            start = text.index(head, start + 1)
        end = text.index("\n}\n", start) + 3
        bodies.append(text[start:end])
    tmp = tmp_path_factory.mktemp("live_rtkit")
    code = tmp / "live.c"
    code.write_text(HARNESS.replace("@FUNCTIONS@", "\n".join(bodies)))
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
    exe, dtb, tmp = harness
    out = tmp / "ok.dtb"
    run = subprocess.run([str(exe), str(dtb), str(out), "0x806000000", "0x807000000"],
                         capture_output=True, text=True)
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
    exe, dtb, tmp = harness
    out = tmp / "unreserved.dtb"
    # The reserved range misses the syslog buffer
    run = subprocess.run([str(exe), str(dtb), str(out), "0x80695c000", "0x806960000"],
                         capture_output=True, text=True)
    assert run.returncode == 1, run.stdout
    assert "buffer ep 2 [0x80696c000+0x4000) outside [0x80695c000, 0x806960000)" in run.stdout
    assert _fdt_cells(out, "/soc/dcp@271c00000", "apple,rtkit-handoff") is None
    assert _fdt_cells(out, "/soc/dcp@271c00000", "memory-region") is None


def test_missing_node_fails_the_boot(harness):
    exe, dtb, tmp = harness
    out = tmp / "nonode.dtb"
    nodts = tmp / "nonode.dts"
    nodts.write_text(DTS.replace('dcpext = "/soc/dcp@271c00000";', 'dcpext = "/soc/dcp@0";'))
    nodtb = tmp / "nonode.dtb.in"
    subprocess.run(["dtc", "-q", "-I", "dts", "-O", "dtb", "-o", str(nodtb), str(nodts)],
                   check=True)
    run = subprocess.run([str(exe), str(nodtb), str(out), "0x806000000", "0x807000000"],
                         capture_output=True, text=True)
    assert run.returncode == 1, run.stdout
    assert "no node for alias dcpext" in run.stdout


def test_unaligned_owned_buffer_fails_the_boot(harness):
    exe, dtb, tmp = harness
    out = tmp / "unaligned.dtb"
    # A no-map region that is not whole pages would unmap the kernel's neighbouring pages too
    run = subprocess.run([str(exe), str(dtb), str(out), "0x806000000", "0x807000000",
                          "0x80696e000"], capture_output=True, text=True)
    assert run.returncode == 1, run.stdout
    assert "buffer ep 2 [0x80696e000+0x4000) not 16K aligned" in run.stdout
    assert _fdt_cells(out, "/soc/dcp@271c00000", "apple,rtkit-handoff") is None
    assert _fdt_cells(out, "/soc/dcp@271c00000", "memory-region") is None
