# SPDX-License-Identifier: GPL-2.0-only
"""Admit J773g only when the live machine, its firmware and the OS tree all say so."""
from pathlib import Path
import subprocess

from test_j713_dapf_instance import block


def test_j773g_board_gate(tmp_path):
    repo = Path(__file__).resolve().parents[2]
    production = block((repo / "src/kboot_t8132.c").read_text(), "bool kboot_is_j773g(")
    # Each case breaks one fact of the J773g ADT (target J773gAP, board-id 42, chip-id
    # 0x8132, firmware-version mBoot-18000.161.10) or of the OS tree.
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#define T8132 0x8132
enum fw_version { V_UNKNOWN, V26_6, V26_6_2, V26_7 };
static struct { enum fw_version version; } os_firmware;
static unsigned chip_id, board_id;
static void *adt = (void *)1;
static const char *live, *board, *soc;
static bool adt_is_compatible(const void *tree, int node, const char *compatible)
{
    return tree == adt && node == 0 && !strcmp(compatible, live);
}
static int fdt_node_check_compatible(const void *fdt, int node, const char *compatible)
{
    assert(fdt && node == 0);
    return strcmp(compatible, board) && strcmp(compatible, soc);
}
static void j773g(void)
{
    chip_id = T8132; board_id = 0x2a; os_firmware.version = V26_6_2;
    live = "J773gAP"; board = "apple,j773g"; soc = "apple,t8132";
}
''' + production + r'''
int main(void)
{
    static char tree;
    j773g(); assert(kboot_is_j773g(&tree));
    assert(!kboot_is_j773g(NULL));
    j773g(); chip_id = 0x8122; assert(!kboot_is_j773g(&tree));
    j773g(); board_id = 0x2c; assert(!kboot_is_j773g(&tree));
    j773g(); live = "J713AP"; assert(!kboot_is_j773g(&tree));
    j773g(); board = "apple,j713"; assert(!kboot_is_j773g(&tree));  /* wrong OS tree */
    j773g(); soc = "apple,t8122"; assert(!kboot_is_j773g(&tree));
    j773g(); os_firmware.version = V26_6; assert(!kboot_is_j773g(&tree));
    j773g(); os_firmware.version = V26_7; assert(!kboot_is_j773g(&tree));
    j773g(); os_firmware.version = V_UNKNOWN; assert(!kboot_is_j773g(&tree));
    return 0;
}
'''
    source = tmp_path / "j773g-board.c"
    source.write_text(harness)
    binary = tmp_path / "j773g-board"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
