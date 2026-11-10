/* SPDX-License-Identifier: MIT */
/* Host stand-in for src/firmware.h: only the versions the display handoff compares. */
#ifndef FIRMWARE_H
#define FIRMWARE_H
#include "types.h"
enum fw_version {
    V_UNKNOWN = 0,
    V13_5 = 50,
    V14_6 = 99,
    V14_7 = 100,
    V14_8_3 = 101,
    V26_6_2 = 102,
    V27_0 = 103,
};
struct fw_version_info {
    enum fw_version version;
    const char *string;
    u32 num[5];
    size_t num_length;
    const char *iboot;
};
extern struct fw_version_info os_firmware;
int firmware_set_fdt(void *fdt, int node, const char *prop, const struct fw_version_info *ver);
#endif
