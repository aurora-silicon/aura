/* SPDX-License-Identifier: GPL-2.0-only */
#include "boot_options.h"

static bool debug_enabled;
static bool console_requested;
static bool proxy_requested;
static bool trial_running;
static bool wdt_keep;
static bool is_stage1;

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

/*
 * An m1n1 stage before this one reads and writes the boot_args command line 4 bytes past where
 * iBoot puts it: m1n1 keeps the revisions in a naturally aligned union, at 0x70 instead of 0x6c
 * (xnuboot.h). The J716s' A/B stage 1 is such a build, and appends " m1n1.wdt=keep" to the line as
 * it sees it. When iBoot's own line is shorter than those 4 bytes (empty on a J716s), what stage 1
 * appended is a separate string after its terminator, which a scan from 0x6c never reaches. Read
 * the token there too; a longer iBoot line has the appended text inside it, and is left as is.
 * Only that token: no other option is written by an m1n1 stage.
 */
#define M1N1_CMDLINE_SKEW 4

static bool m1n1_line_has_wdt_keep(const char *line, size_t capacity)
{
    if (!line || capacity <= M1N1_CMDLINE_SKEW)
        return false;
    for (size_t i = 0; i < M1N1_CMDLINE_SKEW; i++)
        if (!line[i])
            return boot_option_present(line + M1N1_CMDLINE_SKEW, capacity - M1N1_CMDLINE_SKEW,
                                       "m1n1.wdt=keep");
    return false;
}

void boot_options_init(const char *line, size_t capacity)
{
    debug_enabled = boot_option_present(line, capacity, "aura-debug");
    console_requested = boot_option_present(line, capacity, "aura-console");
    proxy_requested = boot_option_present(line, capacity, "aura-proxy");
    trial_running = boot_option_present(line, capacity, "aura-trial");
    wdt_keep = boot_option_present(line, capacity, "m1n1.wdt=keep") ||
               m1n1_line_has_wdt_keep(line, capacity);
    is_stage1 = true;
}

bool aura_debug_enabled(void)
{
    return debug_enabled;
}

/* Keep the debug UART as the OS console (stdout-path) without the rest of aura-debug */
bool aura_console_enabled(void)
{
    return console_requested;
}

void boot_options_set_console(bool console)
{
    console_requested = console;
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
 * m1n1.wdt=keep: a stage that armed the watchdog for this one (the J716s' A/B stage 1 arms 150 s)
 * appends the token to this stage's boot arguments, at m1n1's offset (m1n1_line_has_wdt_keep()).
 * The token keeps its m1n1 spelling because the stage that appends it is an m1n1 build. It is
 * honoured in every stage, plain or ESP.
 */
bool boot_wdt_keep_enabled(void)
{
    return wdt_keep;
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
