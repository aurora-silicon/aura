/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AURA_BOOT_OPTIONS_H
#define AURA_BOOT_OPTIONS_H
#include <stdbool.h>
#include <stddef.h>

bool boot_option_present(const char *line, size_t capacity, const char *option);
void boot_options_init(const char *line, size_t capacity);
bool aura_debug_enabled(void);
void boot_options_set_stage1(bool stage1);
bool aura_proxy_enabled(void);
bool aura_trial_enabled(void);
void boot_options_set_trial(bool trial);
/* Add a whole-word option to a NUL-terminated line of capacity bytes; false if it does not fit */
bool boot_options_append(char *line, size_t capacity, const char *option);
#endif
