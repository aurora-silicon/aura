/* SPDX-License-Identifier: GPL-2.0-only */
#include "boot_options.h"

static bool debug_enabled;
static bool proxy_requested;
static bool trial_running;
static bool is_stage1;
static bool nv2_option;
static bool pv_option;
static int nv2_override = -1;

static bool boot_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool boot_option_present(const char *line, size_t capacity, const char *option)
{
    if (!line || !option || !option[0])
        return false;
    size_t length = 0;
    while (length < capacity && line[length])
        length++;
    if (length == capacity)
        return false;
    for (size_t pos = 0; pos < length;) {
        if (boot_space(line[pos])) {
            pos++;
            continue;
        }
        size_t end = pos;
        while (end < length && !boot_space(line[end]))
            end++;
        size_t i = 0;
        while (pos + i < end && option[i] && line[pos + i] == option[i])
            i++;
        if (pos + i == end && !option[i])
            return true;
        pos = end;
    }
    return false;
}

void boot_options_init(const char *line, size_t capacity)
{
    debug_enabled = boot_option_present(line, capacity, "aura-debug");
    proxy_requested = boot_option_present(line, capacity, "aura-proxy");
    trial_running = boot_option_present(line, capacity, "aura-trial");
    nv2_option =
#ifdef NV2_BOOT
        true;
#else
        false;
#endif
    nv2_option = nv2_option || boot_option_present(line, capacity, "aura-nv2");
    pv_option = boot_option_present(line, capacity, "aura-pv");
    /* A value-bearing token is useful when the OS supplies the boot command
     * line. Keep the historical bare token as the opt-in spelling. */
    if (boot_option_present(line, capacity, "aura-nv2=1") ||
        boot_option_present(line, capacity, "aura-nv2=on"))
        nv2_option = true;
    else if (boot_option_present(line, capacity, "aura-nv2=0") ||
             boot_option_present(line, capacity, "aura-nv2=off"))
        nv2_option = false;
    nv2_override = -1;
    is_stage1 = true;
}

bool aura_debug_enabled(void)
{
    return debug_enabled;
}

/* Run the OS at virtual EL2 under FEAT_NV2 so it can host VMs (opt-in). */
bool aura_nv2_enabled(void)
{
    return nv2_override >= 0 ? nv2_override : nv2_option;
}

void boot_options_set_nv2_override(int enabled)
{
    nv2_override = enabled < 0 ? -1 : !!enabled;
}

/* Keep PV discovery/RUN separate from the NV2 host mode. */
bool aura_pv_enabled(void)
{
    return pv_option;
}

void boot_options_set_stage1(bool stage1)
{
    is_stage1 = stage1;
}

bool aura_proxy_enabled(void)
{
    return is_stage1 && proxy_requested;
}

/*
 * Set by the stage that took an aura-try marker (chainload.c) in the boot arguments of the next
 * stage, which every later aura stage copies forward: the boot runs under the trial watchdog.
 */
bool aura_trial_enabled(void)
{
    return trial_running;
}

void boot_options_set_trial(bool trial)
{
    trial_running = trial;
}

bool boot_options_append(char *line, size_t capacity, const char *option)
{
    if (!line || !option || !option[0] || boot_option_present(line, capacity, option))
        return false;
    size_t length = 0, option_length = 0;
    while (length < capacity && line[length])
        length++;
    while (option[option_length])
        option_length++;
    size_t space = length ? 1 : 0;
    if (length == capacity || option_length + space >= capacity - length)
        return false;
    if (space)
        line[length++] = ' ';
    for (size_t i = 0; i <= option_length; i++)
        line[length + i] = option[i];
    return true;
}
