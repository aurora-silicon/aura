/* SPDX-License-Identifier: MIT */
/*
 * The launcher's trackpad parameter page (proxyclient/aura/windows.py input_params()) read by
 * the carrier's own parser (src/hv/windows/mtp_bringup.c mtp_params_parse()).  Arguments: the
 * page file (64 bytes) and the synthetic payload file.  Prints one JSON line with what the
 * carrier would use, or the refusal.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mtp_bringup.h"

static long slurp(const char *path, u8 *buf, long max)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    long n = (long)fread(buf, 1, (size_t)max, f);
    fclose(f);
    return n;
}

int main(int argc, char **argv)
{
    static u8 page[MTP_PARAMS_LEN], payload[MTP_AREA_FW_SIZE];
    if (argc != 3 || slurp(argv[1], page, sizeof(page)) != MTP_PARAMS_LEN)
        return 2;
    long n = slurp(argv[2], payload, sizeof(payload));
    if (n < 0)
        return 2;
    struct mtp_params p;
    int rc = mtp_params_parse(page, &p);
    if (rc) {
        printf("{\"result\":\"refused\",\"reason\":%d}\n", rc);
        return 0;
    }
    printf("{\"result\":\"accepted\",\"fw_size\":%u,\"fw_crc_ok\":%s,\"iface_off\":%u,"
           "\"gain_q8\":%u,\"accel_q8\":%u,\"accel_lo\":%u,\"accel_hi\":%u,\"scroll_um\":%u,"
           "\"tap_ms\":%u,\"tap_travel_um\":%u,\"tap_key_quiet_ms\":%u,\"click_settle_ms\":%u,"
           "\"palm_major\":%u,\"tap\":%s,\"natural\":%s}\n",
           p.fw_size,
           (long)p.fw_size == n && mtp_crc32(0, payload, p.fw_size) == p.fw_crc ? "true" : "false",
           p.fw_iface_off, p.tp.gain_q8, p.tp.accel_q8, p.tp.accel_lo, p.tp.accel_hi,
           p.tp.scroll_um, p.tp.tap_ms, p.tp.tap_travel_um, p.tp.tap_key_quiet_ms,
           p.tp.click_settle_ms, p.tp.palm_major, p.tp.tap ? "true" : "false",
           p.tp.natural ? "true" : "false");
    return 0;
}
