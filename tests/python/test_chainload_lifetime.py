# SPDX-License-Identifier: GPL-2.0-only
"""Check ownership of FAT buffers after copying into the chainload image."""

from pathlib import Path
import subprocess


def test_chainload_releases_loaded_image(tmp_path):
    source = (Path(__file__).resolve().parents[2] / "src/chainload.c").read_text()
    start = source.index("int chainload_load(")
    source = source[start:source.index("#else", start)]
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
static struct { void *entry; } next_stage;
static char image[16];
static int load_error, copy_error, freed, copied, loads, windows, trial_copy_error;
static bool shutdown_ok, request, host, stage1_chained, marker, in_trial, trial_room = true;
static int trials_started, trial_loads;
#define CMDLINE_LENGTH_RV3 1024
static struct { struct { char cmdline[CMDLINE_LENGTH_RV3]; } rv3; } cur_boot_args;
static char trial_path[128];
static bool aura_trial_enabled(void) { return in_trial; }
static void boot_options_set_trial(bool trial) { in_trial = trial; }
/* The real one marks the boot arguments and adds a variable; here it only counts. */
static bool chainload_start_trial(char **vars, size_t *var_cnt, size_t var_capacity)
{
    if (!trial_room)
        return false;
    trials_started++;
    in_trial = true;
    return true;
}
static bool nvme_init(void) { return true; }
static bool nvme_has_live_post_m4_session(void) { return true; }
static bool nvme_shutdown(void) { return shutdown_ok; }
static int rust_load_image(const char *spec, void **data, size_t *size, bool *proxy_request,
                           char *trial, size_t trial_capacity)
{
    loads++;
    /* The ESP is read for the request only on the first load. */
    assert(loads == 1 || !proxy_request);
    if (proxy_request)
        *proxy_request = request;
    /* A spent marker never comes back: only the first load may take it. */
    if (trial && !trial[0] && marker) {
        assert(trial_capacity > 4);
        strcpy(trial, "try");
        marker = false;
    }
    if (trial && trial[0])
        trial_loads++;
    *data = image;
    *size = sizeof(image);
    return load_error;
}
static int chainload_image(void *data, size_t size, char **vars, size_t count)
{
    assert(data == image && size == sizeof(image) && freed == loads - 1);
    assert(count == 1 && !strcmp(vars[0], "nvme.adopt=live-rtkit-v1"));
    copied++;
    int error = copy_error;
    if (in_trial && trial_copy_error)
        error = trial_copy_error;
    next_stage.entry = error ? NULL : (void *)1;
    return error;
}
static void rust_free_image(void *data, size_t size)
{
    assert(data == image && size == sizeof(image) && freed < loads);
    freed++;
}
/* The Stage 1 log handoff is out of scope here: it only adds a variable. */
static void stage1_log_handoff(char **vars, size_t *var_cnt, size_t var_capacity) {}
static bool stage1_proxy_request_window(void)
{
    windows++;
    if (host)
        next_stage.entry = (void *)2;
    return host;
}
''' + source + r'''
int main(void)
{
    for (int failure = 0; failure < 4; failure++) {
        char *vars[1];
        size_t count = 0;
        freed = copied = loads = windows = 0;
        load_error = failure == 1 ? -1 : 0;
        copy_error = failure == 2 ? -1 : 0;
        shutdown_ok = failure != 3;
        assert(chainload_load("test", vars, &count, 1) == (failure ? -1 : 0));
        assert(freed == (failure != 1));
        assert(copied == (failure != 1));
        assert(!!next_stage.entry == !failure);
    }
    /* aura-proxy on the ESP, no host: one window, the Stage 2 is read again and booted. */
    request = true;
    host = false;
    shutdown_ok = true;
    load_error = copy_error = 0;
    freed = copied = loads = windows = 0;
    next_stage.entry = NULL;
    char *vars[1];
    size_t count = 0;
    assert(chainload_load("test", vars, &count, 1) == 0);
    assert(windows == 1 && loads == 2 && freed == 2 && copied == 1 && next_stage.entry == (void *)1);
    /* A host takes the window: nothing is chainloaded and its next stage stands. */
    host = true;
    freed = copied = loads = windows = 0;
    next_stage.entry = NULL;
    count = 0;
    assert(chainload_load("test", vars, &count, 1) == 0);
    assert(windows == 1 && loads == 1 && freed == 1 && copied == 0 && next_stage.entry == (void *)2);

    /* aura-try: the trial image is booted once, marked as a trial */
    request = host = false;
    marker = true;
    freed = copied = loads = windows = trials_started = trial_loads = 0;
    next_stage.entry = NULL;
    count = 0;
    assert(chainload_load("test", vars, &count, 1) == 0);
    assert(trials_started == 1 && in_trial && trial_loads == 1 && loads == 1 && copied == 1);
    /* A stage that already runs in a trial never takes another */
    marker = true;
    freed = copied = loads = trials_started = trial_loads = 0;
    count = 0;
    assert(chainload_load("test", vars, &count, 1) == 0);
    assert(!trials_started && !trial_loads && marker);
    in_trial = false;
    /* With aura-proxy and no host the trial image is read again after the window */
    request = true;
    marker = true;
    freed = copied = loads = windows = trials_started = trial_loads = 0;
    count = 0;
    assert(chainload_load("test", vars, &count, 1) == 0);
    assert(windows == 1 && loads == 2 && trial_loads == 2 && trials_started == 1 && in_trial);
    in_trial = false;
    request = false;
    /* A trial image that cannot be placed falls back to the configured one in the same boot */
    marker = true;
    trial_copy_error = -1;
    freed = copied = loads = trials_started = trial_loads = 0;
    count = 0;
    assert(chainload_load("test", vars, &count, 1) == 0);
    assert(trials_started == 1 && !in_trial && loads == 2 && copied == 2 && freed == 2);
    assert(next_stage.entry == (void *)1);
    trial_copy_error = 0;
    /* No room to mark the trial: the configured image boots instead */
    marker = true;
    trial_room = false;
    freed = copied = loads = trials_started = trial_loads = 0;
    count = 0;
    assert(chainload_load("test", vars, &count, 1) == 0);
    assert(!trials_started && !in_trial && loads == 2 && copied == 1 && freed == 2);
    return 0;
}
'''
    binary = tmp_path / "chainload-lifetime"
    subprocess.run(
        ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
         "-x", "c", "-", "-o", str(binary)],
        input=harness, text=True, check=True,
    )
    subprocess.run([str(binary)], check=True)
