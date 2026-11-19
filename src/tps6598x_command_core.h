/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef TPS6598X_COMMAND_CORE_H
#define TPS6598X_COMMAND_CORE_H

#ifdef TPS6598X_COMMAND_HOST_TEST
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#else
#include "types.h"
#endif

#define TPS6598X_COMMAND_DEADLINE_MS 1500
#define TPS6598X_COMMAND_TIMEOUT     -2

/*
 * Reset (Gaid) to application firmware running again. TI gives up to 1000 ms
 * from reset de-assertion to ready for operation (Linux tipd TPS_SETUP_MS);
 * Linux allows its reset command 2000 ms. Poll MODE within the latter.
 */
#define TPS6598X_RESET_READY_MS 2000
#define TPS6598X_RESET_POLL_US  10000

/* MODE register values, four ASCII characters read as a little-endian word. */
#define TPS6598X_TAG(a, b, c, d) ((u32)(a) | ((u32)(b) << 8) | ((u32)(c) << 16) | ((u32)(d) << 24))
#define TPS6598X_MODE_APP        TPS6598X_TAG('A', 'P', 'P', ' ')
#define TPS6598X_MODE_APP1       TPS6598X_TAG('A', 'P', 'P', '1')
#define TPS6598X_MODE_DBMA       TPS6598X_TAG('D', 'B', 'M', 'a')

struct tps6598x_command_ops {
    int (*write)(void *ctx, u8 reg, const u8 *data, size_t len);
    int (*read)(void *ctx, u8 reg, u8 *data, size_t len);
    u64 (*now_ms)(void *ctx);
    void (*delay_us)(void *ctx, unsigned usec);
    /* Optional: bring the transport back after the controller reset (SPMI wakeup). */
    int (*wake)(void *ctx);
};

int tps6598x_command_execute(const struct tps6598x_command_ops *ops, void *ctx, const char *cmd,
                             const u8 *data_in, size_t len_in, u8 *data_out, size_t len_out);
int tps6598x_reset_wait_execute(const struct tps6598x_command_ops *ops, void *ctx, u32 *mode);

#endif
