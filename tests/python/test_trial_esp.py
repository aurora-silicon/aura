# SPDX-License-Identifier: GPL-2.0-only
"""An aura-try trial only starts its next stage on the ESP its Stage 1 names.

Without /chosen/asahi,efi-system-partition U-Boot boots the first ESP with an EFI loader, which on a
disk with several installs is another system. kboot_prepare_dt() must refuse such a trial (the trial
path then reboots into the configured Stage 2) and leave every other boot as it was.
"""
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def function_body(source, signature):
    start = source.index(signature)
    depth = 0
    for i in range(source.index('{', start), len(source)):
        depth += {'{': 1, '}': -1}.get(source[i], 0)
        if depth == 0:
            return source[start:i + 1]
    raise AssertionError(signature)


def harness(body):
    function = function_body((ROOT / 'src/kboot.c').read_text(), 'static int dt_check_trial_esp(void)')
    return r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define T8152 0x8152
static unsigned chip_id = 0x8112;
static bool trial, chosen_node = true, esp_prop;
static char dt_buf[1];
static void *dt = dt_buf;
static bool aura_trial_enabled(void) { return trial; }
static int fdt_path_offset(const void *fdt, const char *path)
{ (void)fdt; return strcmp(path, "/chosen") || !chosen_node ? -1 : 8; }
static const void *fdt_getprop(const void *fdt, int node, const char *name, int *len)
{
    (void)fdt; (void)len;
    return node == 8 && esp_prop && !strcmp(name, "asahi,efi-system-partition") ? "x" : NULL;
}
''' + function + '\nint main(void)\n{\n' + body + '\n    return 0;\n}\n'


def run(tmp_path, body):
    binary = tmp_path / 'trial-esp'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-fsanitize=address,undefined',
                    '-x', 'c', '-', '-o', str(binary)], input=harness(body), text=True, check=True)
    subprocess.run([str(binary)], check=True)


def test_a_trial_without_the_stage1_esp_is_refused(tmp_path):
    run(tmp_path, r'''
    trial = true;
    assert(dt_check_trial_esp() == -1);
    chosen_node = false;
    assert(dt_check_trial_esp() == -1);
    chosen_node = true; esp_prop = true;
    assert(dt_check_trial_esp() == 0);
''')


def test_other_boots_are_unchanged(tmp_path):
    run(tmp_path, r'''
    assert(dt_check_trial_esp() == 0);
    trial = true; chip_id = T8152;
    assert(dt_check_trial_esp() == 0);
''')


def test_kboot_prepare_dt_stops_on_it_before_the_record():
    body = function_body((ROOT / 'src/kboot.c').read_text(), 'int kboot_prepare_dt(void *fdt)')
    check = body.index('if (dt_check_trial_esp())')
    assert body.index('dt_set_chosen()') < check < body.index('secboot_export_fdt(dt);')
    assert body[check:].split('\n')[1].strip() == 'return -1;'
