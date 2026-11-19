# SPDX-License-Identifier: GPL-2.0-only
import os
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def test_early_debug_option(tmp_path):
    test = tmp_path / 'test.c'
    test.write_text(r'''
#include "boot_options.h"
#include <assert.h>
#include <string.h>
int main(void) {
    const char *yes[] = {"aura-debug", "-v aura-debug keepsyms=1", "\taura-debug\r\n", "aura-debug aura-debug"};
    const char *no[] = {"", "no-aura-debug", "aura-debugger", "aura-debug=0", "other=aura-debug", "-v"};
    for (unsigned i=0; i<sizeof(yes)/sizeof(*yes); i++) {
        boot_options_init(yes[i], strlen(yes[i])+1);
        assert(aura_debug_enabled());
    }
    for (unsigned i=0; i<sizeof(no)/sizeof(*no); i++) {
        boot_options_init(no[i], strlen(no[i])+1);
        assert(!aura_debug_enabled());
    }
    const char proxy[] = "aura-proxy aura-debug";
    boot_options_init(proxy, sizeof(proxy));
    assert(aura_proxy_enabled() && aura_debug_enabled());
    boot_options_set_stage1(false);
    assert(!aura_proxy_enabled() && aura_debug_enabled());
    boot_options_set_stage1(true);
    assert(aura_proxy_enabled());
    boot_options_init("aura-proxy=0", 13);
    assert(!aura_proxy_enabled());

    boot_options_init("-v aura-console", 16);
    assert(aura_console_enabled() && !aura_debug_enabled());
    boot_options_init("aura-consoles aura-debug", 25);
    assert(!aura_console_enabled() && aura_debug_enabled());

    char truncated[] = {'a','u','r','a','-','d','e','b','u','g'};
    boot_options_init(truncated,sizeof(truncated));
    assert(!aura_debug_enabled());
    char native[1024] = {0};
    memset(native,' ',900);
    memcpy(native+900,"aura-debug",11);
    boot_options_init(native,sizeof(native));
    assert(aura_debug_enabled());
    boot_options_init(native,608);
    assert(!aura_debug_enabled());
    boot_options_init(0,0);
    assert(!aura_debug_enabled());
    assert(!boot_option_present("aura-debug",11,""));

    /* aura-trial: set by the stage that starts a trial, copied forward by every later one */
    boot_options_init("-v aura-trial", 14);
    assert(aura_trial_enabled() && !aura_debug_enabled());
    boot_options_init("aura-trials", 12);
    assert(!aura_trial_enabled());
    boot_options_set_trial(true);
    assert(aura_trial_enabled());
    char line[24] = "-v";
    assert(boot_options_append(line, sizeof(line), "aura-trial") && !strcmp(line, "-v aura-trial"));
    assert(!boot_options_append(line, sizeof(line), "aura-trial") && !strcmp(line, "-v aura-trial"));
    char empty[11] = "";
    assert(boot_options_append(empty, sizeof(empty), "aura-trial") && !strcmp(empty, "aura-trial"));
    char tight[13] = "-v";
    assert(!boot_options_append(tight, sizeof(tight), "aura-trial") && !strcmp(tight, "-v"));
    char fits[14] = "-v";
    assert(boot_options_append(fits, sizeof(fits), "aura-trial") && !strcmp(fits, "-v aura-trial"));
    char full[4] = {'a','b','c','d'};
    assert(!boot_options_append(full, sizeof(full), "x"));
    assert(!boot_options_append(0, 8, "x") && !boot_options_append(line, sizeof(line), ""));
    return 0;
}
''')
    binary = tmp_path/'test'
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + ['-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'src'),str(test),str(ROOT/'src/boot_options.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)


def test_wdt_keep_option(tmp_path):
    test = tmp_path / 'test.c'
    test.write_text(r'''
#include "boot_options.h"
#include <assert.h>
#include <string.h>
int main(void) {
    const char *yes[] = {"m1n1.wdt=keep", "-v m1n1.wdt=keep", "chainload.x=1\tm1n1.wdt=keep\n"};
    const char *no[] = {"", "m1n1.wdt", "m1n1.wdt=", "m1n1.wdt=keeps", "m1n1.wdt=off",
                        "xm1n1.wdt=keep", "aura-debug aura-proxy"};
    for (unsigned i=0; i<sizeof(yes)/sizeof(*yes); i++) {
        boot_options_init(yes[i], strlen(yes[i])+1);
        assert(boot_wdt_keep_enabled());
    }
    for (unsigned i=0; i<sizeof(no)/sizeof(*no); i++) {
        boot_options_init(no[i], strlen(no[i])+1);
        assert(!boot_wdt_keep_enabled());
    }
    /* Independent of the stage: a chainloaded stage 2 is where it matters */
    boot_options_init("m1n1.wdt=keep aura-proxy", 25);
    boot_options_set_stage1(false);
    assert(boot_wdt_keep_enabled() && !aura_proxy_enabled());
    char truncated[] = {'m','1','n','1','.','w','d','t','=','k','e','e','p'};
    boot_options_init(truncated, sizeof(truncated));
    assert(!boot_wdt_keep_enabled());
    boot_options_init(0, 0);
    assert(!boot_wdt_keep_enabled());
    return 0;
}
''')
    binary = tmp_path/'test'
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + ['-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'src'),str(test),str(ROOT/'src/boot_options.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)


def test_wdt_keep_from_an_m1n1_stage1(tmp_path):
    """The J716s' A/B stage 1 is m1n1: its boot_args union is naturally aligned, so it sees the
    command line at 0x70 and appends m1n1.wdt=keep there, while aura reads it at iBoot's 0x6c
    (xnuboot.h, test_xnuboot_layout.py). With iBoot's line empty, as on the J716s, the token is a
    separate string 4 bytes in, and a scan from 0x6c alone disarms the watchdog stage 1 armed.
    The stage 1 side below is m1n1's struct and its chainload_image() append, the aura side what
    startup.c passes."""
    test = tmp_path / 'test.c'
    test.write_text(r'''
#define _DEFAULT_SOURCE
#include "boot_options.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* m1n1's xnuboot.h, as the stage 1 build has it */
struct m1n1_boot_args {
    uint16_t revision, version;
    uint64_t virt_base, phys_base, mem_size, top_of_kernel_data;
    struct { uint64_t base, display, stride, width, height, depth; } video;
    uint32_t machine_type;
    uint64_t devtree;
    uint32_t devtree_size;
    union {
        struct { char cmdline[256]; uint64_t boot_flags, mem_size_actual; } rv1;
        struct { char cmdline[608]; uint64_t boot_flags, mem_size_actual; } rv2;
        struct { char cmdline[1024]; uint64_t boot_flags, mem_size_actual; } rv3;
    };
};
_Static_assert(offsetof(struct m1n1_boot_args, rv1.cmdline) == 0x70, "m1n1 cmdline");
#define IBOOT_CMDLINE 0x6c /* aura's rv*.cmdline */

static union { struct m1n1_boot_args m; unsigned char raw[sizeof(struct m1n1_boot_args)]; } ba;

/* iBoot writes its line at 0x6c; stage 1 (chainload.wdt=150) appends the token as m1n1 sees it */
static void boot(const char *iboot, int stage1_appends)
{
    memset(&ba, 0, sizeof(ba));
    memcpy(ba.raw + IBOOT_CMDLINE, iboot, strlen(iboot) + 1);
    if (stage1_appends) {
        char *cmd = ba.m.rv1.cmdline;
        const char *keep = " m1n1.wdt=keep";
        size_t len = strnlen(cmd, 256);
        if (!strstr(cmd, "m1n1.wdt=keep") && len + strlen(keep) < 256)
            memcpy(cmd + len, keep, strlen(keep) + 1);
    }
}

int main(void) {
    static const size_t capacity[] = {256, 608, 1024}; /* startup.c: by boot_args revision */
    for (unsigned c = 0; c < 3; c++) {
        const char *line = (const char *)ba.raw + IBOOT_CMDLINE;
        /* J716s: iBoot's line is empty */
        boot("", 1);
        boot_options_init(line, capacity[c]);
        assert(boot_wdt_keep_enabled());
        /* Shorter than the skew: m1n1 sees an empty line after the terminator */
        boot("-v", 1);
        boot_options_init(line, capacity[c]);
        assert(boot_wdt_keep_enabled() && !aura_debug_enabled());
        /* Exactly the skew: the append replaces the terminator, one string */
        boot("abcd", 1);
        boot_options_init(line, capacity[c]);
        assert(boot_wdt_keep_enabled());
        /* Longer: the appended token is inside aura's view, as before */
        boot("debug=0x14e serial=3", 1);
        boot_options_init(line, capacity[c]);
        assert(boot_wdt_keep_enabled());
        /* No token from stage 1 (chainload.wdt=0), and nothing read past a long line */
        boot("", 0);
        boot_options_init(line, capacity[c]);
        assert(!boot_wdt_keep_enabled());
        boot("-v", 0);
        boot_options_init(line, capacity[c]);
        assert(!boot_wdt_keep_enabled());
        boot("abcdm1n1.wdt=keep", 0);
        boot_options_init(line, capacity[c]);
        assert(!boot_wdt_keep_enabled());
    }
    /* Too short a buffer for the skewed view */
    boot_options_init("", 1);
    assert(!boot_wdt_keep_enabled());
    return 0;
}
''')
    binary = tmp_path/'test'
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + ['-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'src'),str(test),str(ROOT/'src/boot_options.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
