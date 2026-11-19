/* SPDX-License-Identifier: GPL-2.0-only */

#include "tps6598x_command_core.h"

#define TPS_REG_MODE    0x03
#define TPS_REG_CMD1    0x08
#define TPS_REG_DATA1   0x09
#define TPS_CMD_INVALID TPS6598X_TAG('!', 'C', 'M', 'D')
#define TPS_CMD_GAID    TPS6598X_TAG('G', 'a', 'i', 'd')

static bool deadline_expired(const struct tps6598x_command_ops *ops, void *ctx, u64 start)
{
    return ops->now_ms(ctx) - start >= TPS6598X_COMMAND_DEADLINE_MS;
}

int tps6598x_command_execute(const struct tps6598x_command_ops *ops, void *ctx, const char *cmd,
                             const u8 *data_in, size_t len_in, u8 *data_out, size_t len_out)
{
    if (!ops || !ops->write || !ops->read || !ops->now_ms || !ops->delay_us || !cmd)
        return -1;

    u64 start = ops->now_ms(ctx);
    if (len_in && ops->write(ctx, TPS_REG_DATA1, data_in, len_in) != (int)len_in)
        goto transport_error;
    if (deadline_expired(ops, ctx, start))
        return TPS6598X_COMMAND_TIMEOUT;
    if (ops->write(ctx, TPS_REG_CMD1, (const u8 *)cmd, 4) != 4)
        goto transport_error;

    for (;;) {
        if (deadline_expired(ops, ctx, start))
            return TPS6598X_COMMAND_TIMEOUT;
        u8 bytes[4];
        if (ops->read(ctx, TPS_REG_CMD1, bytes, sizeof(bytes)) != sizeof(bytes))
            goto transport_error;
        u32 status = TPS6598X_TAG(bytes[0], bytes[1], bytes[2], bytes[3]);
        if (status == TPS_CMD_INVALID)
            return -1;
        if (!status)
            break;
        ops->delay_us(ctx, 100);
    }

    if (len_out && ops->read(ctx, TPS_REG_DATA1, data_out, len_out) != (int)len_out)
        goto transport_error;
    return deadline_expired(ops, ctx, start) ? TPS6598X_COMMAND_TIMEOUT : 0;

transport_error:
    return deadline_expired(ops, ctx, start) ? TPS6598X_COMMAND_TIMEOUT : -1;
}

/*
 * Wait for the controller to come back after Gaid was written to CMD1. Gaid
 * never completes like other commands: the controller resets, which clears
 * CMD1, and then boots its application firmware. So first see CMD1 lose Gaid
 * (or the transport drop while the controller is in reset), then wait for
 * MODE to read APP/APP1. A MODE read before CMD1 lost Gaid could still be
 * the old firmware and does not count. Transport errors during the reset are
 * expected: wake the transport again (SPMI) and keep polling until the
 * deadline. Returns 0, or TPS6598X_COMMAND_TIMEOUT with the last MODE in
 * *mode (0 if it never read).
 */
int tps6598x_reset_wait_execute(const struct tps6598x_command_ops *ops, void *ctx, u32 *mode)
{
    if (!ops || !ops->read || !ops->now_ms || !ops->delay_us || !mode)
        return -1;

    u64 start = ops->now_ms(ctx);
    bool reset_seen = false;
    *mode = 0;
    for (;;) {
        u8 bytes[4];
        bool transport_ok = true;

        if (!reset_seen) {
            if (ops->read(ctx, TPS_REG_CMD1, bytes, sizeof(bytes)) != sizeof(bytes))
                transport_ok = false;
            if (!transport_ok ||
                TPS6598X_TAG(bytes[0], bytes[1], bytes[2], bytes[3]) != TPS_CMD_GAID)
                reset_seen = true;
        }
        if (reset_seen && transport_ok) {
            if (ops->read(ctx, TPS_REG_MODE, bytes, sizeof(bytes)) == sizeof(bytes)) {
                *mode = TPS6598X_TAG(bytes[0], bytes[1], bytes[2], bytes[3]);
                if (*mode == TPS6598X_MODE_APP || *mode == TPS6598X_MODE_APP1)
                    return 0;
            } else {
                transport_ok = false;
            }
        }
        if (ops->now_ms(ctx) - start >= TPS6598X_RESET_READY_MS)
            return TPS6598X_COMMAND_TIMEOUT;
        if (!transport_ok && ops->wake)
            ops->wake(ctx);
        ops->delay_us(ctx, TPS6598X_RESET_POLL_US);
    }
}
