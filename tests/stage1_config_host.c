/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include "stage1_config.c"
static void reset(void) { config_valid = -1; }
static void checksum(void) { config.crc32 = tinf_crc32(&config.version, sizeof(config) - 20); reset(); }
int main(int argc, char **argv)
{
    assert(argc == 2);
    assert(!stage1_config_target()); /* factory image is proxy-only */
    FILE *file = fopen(argv[1], "rb"); assert(file);
    assert(fread(&config, 1, sizeof(config), file) == sizeof(config)); fclose(file); reset();
    assert(stage1_config_target());
    assert(!strcmp(stage1_config_target(), "01234567-89ab-cdef-0123-456789abcdef;aurora/boot.bin"));
    assert(stage1_config_window_ms() == 15000);
    config.crc32 ^= 1; reset(); assert(!stage1_config_target());
    checksum(); config.window_ms = 100000; checksum(); assert(!stage1_config_target());
    config.window_ms = 15000; strcpy(config.stage2_path, ";../boot.bin"); checksum(); assert(!stage1_config_target());
    strcpy(config.stage2_path, ";aurora/boot.bin"); config.stage2_path[191] = 'x'; checksum(); assert(!stage1_config_target());
    config.stage2_path[191] = 0; config.esp_uuid[0] = 'X'; checksum(); assert(!stage1_config_target());
    return 0;
}
