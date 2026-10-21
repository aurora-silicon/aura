/* SPDX-License-Identifier: MIT */
#ifndef __GFX_FW_IMAGE_INFO_H__
#define __GFX_FW_IMAGE_INFO_H__

#include "firmware.h"
#include "string.h"

#define GFX_FW_HDR_OFFSET 0x4200
#define GFX_FW_HDR_BRANCH 0x14000011
#define GFX_FW_HDR_MAGIC  0x64697575
#define GFX_FW_HDR_VER    5
#define GFX_FW_HDR_SIZE   0x38
#define GFX_FW_VERSION_MAX 64

static inline int gfx_fw_hex_digit(char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

static inline bool gfx_fw_parse_uuid_hex(const char *text, int len, u8 uuid[16])
{
    if (!text || !uuid || len != 33 || text[32] != '\0')
        return false;
    for (int i = 0; i < 16; i++) {
        int hi = gfx_fw_hex_digit(text[2 * i]);
        int lo = gfx_fw_hex_digit(text[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        uuid[i] = (hi << 4) | lo;
    }
    return true;
}

/* Parse an already-read image-info header; it is an identity, not ABI approval. */
static inline bool gfx_fw_parse_image_info(const u32 hdr[GFX_FW_HDR_SIZE / 4], u32 segment_size,
                                           struct gfx_fw_image_info *out)
{
    if (!hdr || !out || hdr[0] != GFX_FW_HDR_BRANCH || hdr[1] != GFX_FW_HDR_MAGIC ||
        hdr[2] != GFX_FW_HDR_VER || !hdr[13] || hdr[13] > segment_size ||
        hdr[11] > hdr[13] || hdr[12] > hdr[13] - hdr[11])
        return false;

    struct gfx_fw_image_info found = {
        .rtkit_version_offset = hdr[3],
        .patchbay_offset = hdr[11],
        .patchbay_size = hdr[12],
        .text_size = hdr[13],
    };
    memcpy(found.uuid, &hdr[5], sizeof(found.uuid));
    *out = found;
    return true;
}

/* Accept only one bounded, printable RTKit version string. Never infer ABI. */
static inline bool gfx_fw_parse_rtkit_version(const u8 bytes[GFX_FW_VERSION_MAX],
                                              char out[GFX_FW_VERSION_MAX])
{
    if (!bytes || !out || memcmp(bytes, "RTKit-", 6))
        return false;
    for (size_t i = 0; i < GFX_FW_VERSION_MAX; i++) {
        if (bytes[i] == 0) {
            if (i <= 6)
                return false;
            memcpy(out, bytes, i + 1);
            return true;
        }
        if (bytes[i] < 0x20 || bytes[i] > 0x7e)
            return false;
    }
    return false;
}

#endif
