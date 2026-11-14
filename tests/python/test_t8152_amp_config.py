# SPDX-License-Identifier: MIT
"""The J873g (T8152) DT gets the speaker amp configuration and safety cleanup.

dt_prepare_t8152() and dt_set_speaker_safety() come from src/kboot.c and run on
a real libfdt tree: /chosen with a stale asahi-speaker-safety node and one
ti,sn012776 amp. The amp configuration is a stand-in that writes one property
the way src/kboot_audio.c does (tests/run-kboot-audio.sh covers the real one);
every other DT step is a recorded leaf.

Ways the T8152 path can go wrong, each checked here:
- It returns before the amp configuration: the amp gets no apple,* properties
  and the kernel's sound driver refuses the speaker.
- The configuration runs after fdt_pack(): the packed tree has no room, the
  property write fails, and as the configuration never stops the boot, the amp
  is silently left unconfigured.
- Only one of the two loader modes (with or without aurora,minimal-boot) gets it.
- A stale asahi-speaker-safety node under /chosen survives, or a failed
  cleanup goes on to boot.
- The other T8152 steps change, or change order.
"""

from pathlib import Path
import subprocess

from test_locked_dart_tables import function

REPO = Path(__file__).resolve().parents[2]

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "libfdt.h"
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
#define T8140 0x8140
#define T8152 0x8152
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define bail(...) do { printf(__VA_ARGS__); return -1; } while (0)
#define ADT_GETPROP(adt, node, name, val) adt_getprop(val)
static int chip_id = T8152;
static void *adt = (void *)0xad7;
static void *dt;
static bool dt_ready;
static char calls[256];
static int adt_path_offset(void *tree, const char *path)
{ assert(!"T8152 reads no ADT speaker profile"); return -1; }
static u32 adt_getprop(void *val) { assert(!"T8152 reads no ADT speaker profile"); return 0; }
static int leaf(const char *name) { strcat(calls, name); strcat(calls, " "); return 0; }
static int dt_set_serial_number(void) { return leaf("serial"); }
static int dt_set_display_t8152(void) { return leaf("display"); }
static int dt_set_gpu(void *tree) { assert(tree == dt); return leaf("gpu"); }
static int dt_set_cpus(void) { return leaf("cpus"); }
static int dt_set_mac_addresses(void) { return leaf("mac"); }
static int dt_set_fb2_t8152(void) { return leaf("fb2"); }
static int dt_set_memory(void) { return leaf("memory"); }
/* As src/kboot_audio.c: one property on the amp, and nothing stops the boot. */
static int amp_written;
static int kboot_audio_amp_config(void *tree, const void *from)
{
    static const u8 row[4] = {0xc0, 0xc1, 0xc2, 0xc3};
    int node = fdt_node_offset_by_compatible(tree, -1, "ti,sn012776");
    assert(tree == dt && from == adt && node >= 0);
    amp_written = !fdt_setprop(tree, node, "apple,speaker-config", row, sizeof(row));
    leaf("amp");
    return 0;
}
@BODY@
static u8 blob[65536];
static void build(bool minimal, bool chosen)
{
    static const char aic[] = "apple,t8152-aic3\0apple,t8122-aic3";
    fdt32_t reg = cpu_to_fdt32(0x38);
    dt = blob;
    assert(!fdt_create_empty_tree(dt, sizeof(blob)));
    if (minimal)
        assert(!fdt_setprop(dt, 0, "aurora,minimal-boot", NULL, 0));
    if (chosen) {
        int node = fdt_add_subnode(dt, 0, "chosen");
        assert(node >= 0 && fdt_add_subnode(dt, node, "asahi-speaker-safety") >= 0);
    }
    int soc = fdt_add_subnode(dt, 0, "soc");
    assert(soc >= 0);
    int irq = fdt_add_subnode(dt, soc, "interrupt-controller@301000000");
    assert(irq >= 0 && !fdt_setprop(dt, irq, "compatible", aic, sizeof(aic)));
    int i2c = fdt_add_subnode(dt, soc, "i2c@33101c000");
    int amp = i2c < 0 ? i2c : fdt_add_subnode(dt, i2c, "codec@38");
    assert(amp >= 0 && !fdt_setprop_string(dt, amp, "compatible", "ti,sn012776") &&
           !fdt_setprop(dt, amp, "reg", &reg, sizeof(reg)));
    calls[0] = 0;
    amp_written = 0;
    dt_ready = false;
}
int main(void)
{
    static const u8 row[4] = {0xc0, 0xc1, 0xc2, 0xc3};
    for (int minimal = 0; minimal <= 1; minimal++) {
        build(minimal, true);
        assert(dt_prepare_t8152() == 0 && dt_ready);
        assert(!fdt_check_full(dt, fdt_totalsize(dt)));
        /* packed: nothing can be added after this */
        assert(fdt_totalsize(dt) == fdt_off_dt_strings(dt) + fdt_size_dt_strings(dt));
        int len, amp = fdt_path_offset(dt, "/soc/i2c@33101c000/codec@38");
        const void *val = amp < 0 ? NULL : fdt_getprop(dt, amp, "apple,speaker-config", &len);
        if (!amp_written || !val || len != sizeof(row) || memcmp(val, row, sizeof(row))) {
            printf("T8152 (minimal-boot %d): the amp has no configuration\n", minimal);
            return 1;
        }
        assert(fdt_path_offset(dt, "/chosen") >= 0);
        assert(fdt_path_offset(dt, "/chosen/asahi-speaker-safety") == -FDT_ERR_NOTFOUND);
        /* The other steps, unchanged, and the amp before the CPUs as on every other chip */
        const char *want = minimal ? "serial display gpu cpus mac fb2 memory "
                                   : "cpus mac fb2 memory ";
        char rest[256] = "";
        for (char *word = strtok(calls, " "); word; word = strtok(NULL, " "))
            if (strcmp(word, "amp")) {
                strcat(rest, word);
                strcat(rest, " ");
            } else {
                assert(!strstr(rest, "cpus"));
            }
        if (strcmp(rest, want)) {
            printf("T8152 (minimal-boot %d): steps \"%s\", want \"%s\"\n", minimal, rest, want);
            return 1;
        }
    }
    /* Without /chosen the safety cleanup fails, and so does the preparation */
    build(false, false);
    assert(dt_prepare_t8152() == -1 && !dt_ready);
    puts("T8152 amp configuration OK");
    return 0;
}
'''


def test_t8152_dt_gets_the_amp_configuration(tmp_path):
    source = (REPO / "src/kboot.c").read_text()
    body = function(source, "dt_set_speaker_safety") + "\n" + function(source, "dt_prepare_t8152")
    libfdt = REPO / "src/libfdt"
    sources = [libfdt / name for name in (
        "fdt.c", "fdt_addresses.c", "fdt_check.c", "fdt_empty_tree.c",
        "fdt_ro.c", "fdt_rw.c", "fdt_strerror.c", "fdt_sw.c", "fdt_wip.c",
    )]
    binary = tmp_path / "t8152-amp-config"
    subprocess.run(
        ["cc", "-D_GNU_SOURCE", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
         "-Wno-unused-parameter", "-Wno-unused-function", "-I", str(libfdt), "-x", "c", "-",
         *map(str, sources), "-o", str(binary)],
        input=HARNESS.replace("@BODY@", body), text=True, check=True,
    )
    subprocess.run([str(binary)], check=True)
