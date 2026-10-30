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
