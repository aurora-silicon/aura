# SPDX-License-Identifier: GPL-2.0-only
# Assisted-by: LLM
"""/chosen stdout-path on a debug-only UART stays only on debug boots."""

from pathlib import Path
import subprocess

import pytest

REPO = Path(__file__).resolve().parents[2]

# The Apple DTS shapes: M1/M2 laptops (t8103/t8112/t600x dtsi) point serial0 at the Samsung
# UART, the MacBook Neo (t8140-j700.dts) at the DockChannel UART. The J616s DTS sets none.
BOARDS = {
    "m2-s5l": ("apple,s5l-uart", "serial0"),
    "m1pro-s5l": ("apple,s5l-uart", "serial0:115200n8"),
    "neo-dockchannel": ("apple,dockchannel-uart", "serial0"),
    "neo-full-path": ("apple,dockchannel-uart", "/soc/serial@30812c000"),
}

# Each tree is built with libfdt (no dtc on the CI runners):
#   / { aliases { serial0 = "/soc/serial@30812c000"; serial1 = "/soc/serial@1000"; };
#       chosen { [stdout-path] [linux,stdout-path] };
#       soc { serial@30812c000 { compatible = <UART>; }; serial@1000 { "vendor,real-uart" }; }; }


def definition(source, start_text, terminator="\n}\n"):
    start = source.index(start_text)
    return source[start:source.index(terminator, start) + len(terminator)]


HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libfdt.h"
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define bail(...) do { printf(__VA_ARGS__); return -1; } while (0)
#define MAX_CHOSEN_PARAMS 16
static char *chosen_params[MAX_CHOSEN_PARAMS][2];
static void *dt;
static bool debug_on, console_on, host_on;
static bool aura_debug_enabled(void) { return debug_on; }
static bool aura_console_enabled(void) { return console_on; }
static bool dockchannel_uart_host_seen(void) { return host_on; }
''' + "%(code)s" + r'''
static unsigned char blob[65536];
/* path "-" adds no stdout-path; legacy also adds linux,stdout-path */
static void build(const char *compatible, const char *path, bool legacy)
{
    dt = blob;
    assert(fdt_create_empty_tree(dt, sizeof(blob)) == 0);
    /* Adding a node moves the offsets after it: look each one up again before use */
    assert(fdt_add_subnode(dt, 0, "aliases") >= 0);
    assert(fdt_add_subnode(dt, 0, "chosen") >= 0);
    assert(fdt_add_subnode(dt, 0, "soc") >= 0);
    int aliases = fdt_path_offset(dt, "/aliases");
    assert(!fdt_setprop_string(dt, aliases, "serial0", "/soc/serial@30812c000"));
    assert(!fdt_setprop_string(dt, aliases, "serial1", "/soc/serial@1000"));
    assert(fdt_add_subnode(dt, fdt_path_offset(dt, "/soc"), "serial@30812c000") >= 0);
    assert(fdt_add_subnode(dt, fdt_path_offset(dt, "/soc"), "serial@1000") >= 0);
    assert(!fdt_setprop_string(dt, fdt_path_offset(dt, "/soc/serial@30812c000"), "compatible",
                               compatible));
    assert(!fdt_setprop_string(dt, fdt_path_offset(dt, "/soc/serial@1000"), "compatible",
                               "vendor,real-uart"));
    int chosen = fdt_path_offset(dt, "/chosen");
    if (strcmp(path, "-")) {
        assert(!fdt_setprop_string(dt, chosen, "stdout-path", path));
        if (legacy)
            assert(!fdt_setprop_string(dt, chosen, "linux,stdout-path", path));
    }
}
static bool has(const char *prop)
{
    return fdt_getprop(dt, fdt_path_offset(dt, "/chosen"), prop, NULL) != NULL;
}
static void set_config(const char *name, const char *value)
{
    memset(chosen_params, 0, sizeof(chosen_params));
    if (name) {
        chosen_params[0][0] = (char *)name;
        chosen_params[0][1] = (char *)value;
    }
}
/* argv: compatible, stdout path or "-", legacy, debug, console, host, config name or "-",
 * config value, expect kept */
int main(int argc, char **argv)
{
    assert(argc == 10);
    build(argv[1], argv[2], argv[3][0] == '1');
    argv += 2;
    debug_on = argv[2][0] == '1';
    console_on = argv[3][0] == '1';
    host_on = argv[4][0] == '1';
    set_config(strcmp(argv[5], "-") ? argv[5] : NULL, argv[6]);
    bool before_std = has("stdout-path"), before_linux = has("linux,stdout-path");
    assert(dt_set_stdout_path(fdt_path_offset(dt, "/chosen")) == 0);
    bool keep = argv[7][0] == '1';
    assert(has("stdout-path") == (before_std && keep));
    assert(has("linux,stdout-path") == (before_linux && keep));
    /* The UART node itself always stays, so console=ttyDC0 keeps working */
    assert(fdt_path_offset(dt, "serial0") >= 0);
    return 0;
}
'''


@pytest.fixture(scope="module")
def runner(tmp_path_factory):
    tmp = tmp_path_factory.mktemp("stdout-path")
    source = (REPO / "src/kboot.c").read_text()
    code = "\n".join((
        "#define DEBUGUSB_PROXY 1",
        source[source.index("#define SERIAL_CONSOLE_SWITCH"):
               source.index("static const char *const debug_uart_compatibles")],
        definition(source, "static const char *const debug_uart_compatibles", ";\n"),
        definition(source, "static const char *kboot_chosen_param("),
        definition(source, "static bool dt_stdout_is_debug_uart("),
        definition(source, "static const char *kboot_serial_console_reason("),
        definition(source, "static int dt_set_stdout_path("),
    ))
    libfdt = REPO / "src/libfdt"
    sources = [libfdt / name for name in (
        "fdt.c", "fdt_addresses.c", "fdt_check.c", "fdt_empty_tree.c",
        "fdt_ro.c", "fdt_rw.c", "fdt_strerror.c", "fdt_sw.c", "fdt_wip.c",
    )]
    binary = tmp / "stdout-path-policy"
    subprocess.run(
        ["cc", "-D_GNU_SOURCE", "-std=c11", "-Wall", "-Wextra", "-Werror",
         "-Wno-unused-parameter", "-Wno-sizeof-pointer-memaccess", "-I", str(libfdt),
         "-x", "c", "-", *map(str, sources), "-o", str(binary)],
        input=HARNESS % {"code": code}, text=True, check=True,
    )

    def run(compatible, path, legacy=False, debug=0, console=0, host=0, config=("-", ""),
            keep=False):
        subprocess.run([str(binary), compatible, path, "1" if legacy else "0", str(debug),
                        str(console), str(host), config[0], config[1], "1" if keep else "0"],
                       check=True)

    return run


@pytest.mark.parametrize("board", BOARDS)
@pytest.mark.parametrize("legacy", [False, True], ids=["stdout-path", "with-linux,stdout-path"])
def test_normal_boot_removes_debug_uart_console(runner, board, legacy):
    compatible, path = BOARDS[board]
    runner(compatible, path, legacy, keep=False)


@pytest.mark.parametrize("board", BOARDS)
@pytest.mark.parametrize("flags", [
    {"debug": 1}, {"console": 1}, {"host": 1},
], ids=["aura-debug", "aura-console", "debugusb-host"])
def test_debug_boot_keeps_it(runner, board, flags):
    compatible, path = BOARDS[board]
    runner(compatible, path, True, keep=True, **flags)


@pytest.mark.parametrize("board", BOARDS)
def test_config_opt_in_keeps_it(runner, board):
    compatible, path = BOARDS[board]
    runner(compatible, path, config=("aura,serial-console", "1"), keep=True)
    # An owner's own stdout-path config line is an opt-in as well
    runner(compatible, path, config=("stdout-path", "serial0"), keep=True)


@pytest.mark.parametrize("board", BOARDS)
def test_config_opt_out_wins_over_debug_boot(runner, board):
    compatible, path = BOARDS[board]
    runner(compatible, path, debug=1, console=1, host=1,
           config=("aura,serial-console", "0"), keep=False)


def test_other_consoles_are_untouched(runner):
    # A stdout-path to a UART that is not a debug-only one, or to nothing, is left alone
    runner("apple,s5l-uart", "serial1", keep=True)
    runner("apple,s5l-uart", "serial7", keep=True)
    runner("apple,dockchannel-uart", "/soc/nothing", keep=True)
    # U-Boot points it at the framebuffer on M1/M2 when it finds a keyboard
    runner("apple,s5l-uart", "/chosen/framebuffer", keep=True)


def test_no_stdout_path_is_fine(runner):
    # J616s (T6040) sets no stdout-path at all
    runner("apple,dockchannel-uart", "-", keep=False)


def test_boot_option_parsing():
    source = (REPO / "src/boot_options.c").read_text()
    assert 'boot_option_present(line, capacity, "aura-console")' in source
    kboot = (REPO / "src/kboot.c").read_text()
    chosen = definition(kboot, "static int dt_set_chosen(void)")
    # The policy runs after the config's chosen lines are applied, so it sees them
    assert chosen.index("dt_set_stdout_path(node)") > chosen.index('printf("FDT: set chosen.%s')


def test_stage1_passes_debugusb_on_to_stage2():
    # Stage 2 ships without DEBUGUSB_PROXY: Stage 1 turns a debugusb host into aura-console
    source = (REPO / "src/chainload.c").read_text()
    block = source[source.index("#if defined(DEBUGUSB_PROXY) && !defined(ESP_STAGE2)"):]
    load = source[source.index("int chainload_load("):]
    guard = load.index("#ifdef DEBUGUSB_PROXY\n    /* The next aura stage skips")
    stage2_guard = load.rindex("#ifndef ESP_STAGE2", 0, guard)
    handoff = load.index('boot_options_append(cur_boot_args.rv3.cmdline, capacity, "aura-console")')
    assert stage2_guard < guard < handoff < load.index("#endif", guard)
    assert "dockchannel_uart_host_seen()" in load[guard:handoff]
    assert "boot_options_set_console(true)" in load[handoff:load.index("#endif", guard)]
    assert block
