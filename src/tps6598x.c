/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#include "tps6598x.h"
#include "adt.h"
#include "i2c.h"
#include "iodev.h"
#include "malloc.h"
#include "spmi.h"
#include "string.h"
#include "tps6598x_command_core.h"
#include "types.h"
#include "utils.h"

#define TPS_REG_MODE        0x03
#define TPS_REG_CMD1        0x08
#define TPS_CMD_INVALID     0x444d4321 // !CMD as LE u32
#define TPS_REG_INT_EVENT1  0x14
#define TPS_REG_INT_MASK1   0x16
#define TPS_REG_INT_CLEAR1  0x18
#define TPS_REG_POWER_STATE 0x20
#define TPS_MODE_DBMA       ((u32)'D' | ((u32)'B' << 8) | ((u32)'M' << 16) | ((u32)'a' << 24))

#define TPS_SPMI_REG_SELECT 0x00
#define TPS_SPMI_REG_DATA   0x20

// Write to TPS_SPMI_REG_SELECT with MSB=1 will
// trigger selection of register with the 7-bit address
#define TPS_SPMI_REG_SELECT_TRIG BIT(7)

struct tps6598x_dev {
    i2c_dev_t *i2c;
    spmi_dev_t *spmi;
    u8 addr;
    bool resetting;
    bool command_active;
    u64 command_start;
};

static bool tps6598x_command_expired(tps6598x_dev_t *dev)
{
    return dev->command_active &&
           ticks_to_msecs(get_ticks() - dev->command_start) >= TPS6598X_COMMAND_DEADLINE_MS;
}

static tps6598x_dev_t *tps6598x_init(const char *adt_node, const char *addr_prop)
{
    int adt_offset;
    adt_offset = adt_path_offset(adt, adt_node);
    if (adt_offset < 0) {
        printf("tps6598x: Error getting %s node\n", adt_node);
        return NULL;
    }

    const u8 *addr = adt_getprop(adt, adt_offset, addr_prop, NULL);
    if (addr == NULL) {
        printf("tps6598x: Error getting %s %s\n.", adt_node, addr_prop);
        return NULL;
    }

    tps6598x_dev_t *dev = calloc(1, sizeof(*dev));
    if (!dev)
        return NULL;

    dev->addr = *addr;
    return dev;
}

tps6598x_dev_t *tps6598x_init_i2c(const char *adt_node, i2c_dev_t *i2c)
{
    tps6598x_dev_t *dev = tps6598x_init(adt_node, "hpm-iic-addr");
    if (!dev)
        return NULL;
    dev->i2c = i2c;
    return dev;
}

tps6598x_dev_t *tps6598x_init_spmi(const char *adt_node, spmi_dev_t *spmi)
{
    tps6598x_dev_t *dev = tps6598x_init(adt_node, "reg");
    if (!dev || !spmi) {
        free(dev);
        return NULL;
    }
    dev->spmi = spmi;

    if (spmi_send_wakeup(dev->spmi, dev->addr) < 0)
        goto err_free;
    mdelay(10);

    return dev;

err_free:
    free(dev);
    return NULL;
}

void tps6598x_shutdown(tps6598x_dev_t *dev)
{
    if (dev->spmi && !dev->resetting)
        spmi_send_shutdown(dev->spmi, dev->addr);
    free(dev);
}

static int tps6598x_spmi_select_reg(tps6598x_dev_t *dev, const u8 reg)
{
    u8 val = ~reg;
    if (spmi_reg0_write(dev->spmi, dev->addr, reg) < 0)
        return -1;

    for (unsigned attempt = 0; attempt < 1000; attempt++) {
        if (tps6598x_command_expired(dev))
            return TPS6598X_COMMAND_TIMEOUT;
        if (spmi_ext_read(dev->spmi, dev->addr, TPS_SPMI_REG_SELECT, &val, 1) < 0)
            return -1;
        if (val == reg)
            return 0;
        if (val != (reg | TPS_SPMI_REG_SELECT_TRIG)) // Selection in progress
            return -1;
        mdelay(1);
    }
    printf("tps6598x: timed out selecting SPMI register 0x%x\n", reg);
    return -1;
}

static int tps6598x_write_reg(tps6598x_dev_t *dev, const u8 reg, const u8 *data, size_t len)
{
    if (dev->i2c)
        return i2c_smbus_write(dev->i2c, dev->addr, reg, data, len);

    if (dev->spmi) {
        if (tps6598x_spmi_select_reg(dev, reg) < 0)
            return -1;
        if (spmi_ext_write(dev->spmi, dev->addr, TPS_SPMI_REG_DATA, data, len) < 0)
            return -1;
        return len;
    }

    return -1;
}

static int tps6598x_read_reg(tps6598x_dev_t *dev, const u8 reg, u8 *data, size_t len)
{
    if (dev->i2c)
        return i2c_smbus_read(dev->i2c, dev->addr, reg, data, len);

    if (dev->spmi) {
        if (tps6598x_spmi_select_reg(dev, reg) < 0)
            return -1;
        if (spmi_ext_read(dev->spmi, dev->addr, TPS_SPMI_REG_DATA, data, len) < 0)
            return -1;
        return len;
    }

    return -1;
}

static int tps6598x_command_write(void *ctx, u8 reg, const u8 *data, size_t len)
{
    return tps6598x_write_reg(ctx, reg, data, len);
}

static int tps6598x_command_read(void *ctx, u8 reg, u8 *data, size_t len)
{
    return tps6598x_read_reg(ctx, reg, data, len);
}

static u64 tps6598x_command_now_ms(void *ctx)
{
    (void)ctx;
    return ticks_to_msecs(get_ticks());
}

static void tps6598x_command_delay_us(void *ctx, unsigned usec)
{
    (void)ctx;
    udelay(usec);
}

int tps6598x_command(tps6598x_dev_t *dev, const char *cmd, const u8 *data_in, size_t len_in,
                     u8 *data_out, size_t len_out)
{
    static const struct tps6598x_command_ops ops = {
        .write = tps6598x_command_write,
        .read = tps6598x_command_read,
        .now_ms = tps6598x_command_now_ms,
        .delay_us = tps6598x_command_delay_us,
    };

    dev->command_start = get_ticks();
    dev->command_active = true;
    int ret = tps6598x_command_execute(&ops, dev, cmd, data_in, len_in, data_out, len_out);
    if (tps6598x_command_expired(dev))
        ret = TPS6598X_COMMAND_TIMEOUT;
    dev->command_active = false;
    return ret;
}

int tps6598x_cold_reset(tps6598x_dev_t *dev)
{
    /* Gaid resets the HPM itself, so completion cannot be polled on this session. */
    if (tps6598x_write_reg(dev, TPS_REG_CMD1, (const u8 *)"Gaid", 4) != 4)
        return -1;
    dev->resetting = true;
    return 0;
}

int tps6598x_cmd_status(tps6598x_dev_t *dev, const char *cmd)
{
    u32 cmd_status;

    if (tps6598x_read_reg(dev, TPS_REG_CMD1, (u8 *)&cmd_status, 4) != 4) {
        printf("tps6598x: read status for cmd: %s failed\n", cmd);
        return -1;
    }
    if (cmd_status == TPS_CMD_INVALID) {
        printf("tps6598x: cmd %s status invalid\n", cmd);
        return -1;
    }
    if (cmd_status) {
        printf("tps6598x: cmd %s status 0x%x\n", cmd, cmd_status);
        return -1;
    }

    return 0;
}

int tps6598x_disable_irqs(tps6598x_dev_t *dev, tps6598x_irq_state_t *state)
{
    size_t read;
    int written;
    static const u8 zeros[CD3218B12_IRQ_WIDTH] = {0x00};
    static const u8 ones[CD3218B12_IRQ_WIDTH] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                 0xFF, 0xFF, 0xFF, 0xFF};

    // store IntEvent 1 to restore it later
    read = tps6598x_read_reg(dev, TPS_REG_INT_MASK1, state->int_mask1, sizeof(state->int_mask1));
    if (read != CD3218B12_IRQ_WIDTH) {
        printf("tps6598x: reading TPS_REG_INT_MASK1 failed\n");
        return -1;
    }
    state->valid = 1;

    // mask interrupts and ack all interrupt flags
    written = tps6598x_write_reg(dev, TPS_REG_INT_CLEAR1, ones, sizeof(ones));
    if (written != sizeof(zeros)) {
        printf("tps6598x: writing TPS_REG_INT_CLEAR1 failed, written: %d\n", written);
        return -1;
    }
    written = tps6598x_write_reg(dev, TPS_REG_INT_MASK1, zeros, sizeof(zeros));
    if (written != sizeof(ones)) {
        printf("tps6598x: writing TPS_REG_INT_MASK1 failed, written: %d\n", written);
        return -1;
    }

#ifdef DEBUG
    u8 tmp[CD3218B12_IRQ_WIDTH] = {0x00};
    read = tps6598x_read_reg(dev, TPS_REG_INT_MASK1, tmp, CD3218B12_IRQ_WIDTH);
    if (read != CD3218B12_IRQ_WIDTH)
        printf("tps6598x: failed verification, can't read TPS_REG_INT_MASK1\n");
    else {
        printf("tps6598x: verify: TPS_REG_INT_MASK1 vs. saved IntMask1\n");
        hexdump(tmp, sizeof(tmp));
        hexdump(state->int_mask1, sizeof(state->int_mask1));
    }
#endif
    return 0;
}

int tps6598x_restore_irqs(tps6598x_dev_t *dev, tps6598x_irq_state_t *state)
{
    int written;

    written =
        tps6598x_write_reg(dev, TPS_REG_INT_MASK1, state->int_mask1, sizeof(state->int_mask1));
    if (written != sizeof(state->int_mask1)) {
        printf("tps6598x: restoring TPS_REG_INT_MASK1 failed\n");
        return -1;
    }

#ifdef DEBUG
    int read;
    u8 tmp[CD3218B12_IRQ_WIDTH];
    read = tps6598x_read_reg(dev, TPS_REG_INT_MASK1, tmp, sizeof(tmp));
    if (read != sizeof(tmp))
        printf("tps6598x: failed verification, can't read TPS_REG_INT_MASK1\n");
    else {
        printf("tps6598x: verify saved IntMask1 vs. TPS_REG_INT_MASK1:\n");
        hexdump(state->int_mask1, sizeof(state->int_mask1));
        hexdump(tmp, sizeof(tmp));
    }
#endif

    return 0;
}

int tps6598x_powerup(tps6598x_dev_t *dev)
{
    u8 power_state;

    if (tps6598x_read_reg(dev, TPS_REG_POWER_STATE, &power_state, 1) != 1)
        return -1;

    if (power_state == 0)
        return 0;

    const u8 data = 0;
    if (tps6598x_command(dev, "SSPS", &data, 1, NULL, 0))
        return -1;

    if (tps6598x_read_reg(dev, TPS_REG_POWER_STATE, &power_state, 1) != 1)
        return -1;

    if (power_state != 0)
        return -1;

    return 0;
}

/*
 * Unlock the port controller with the board key, enter DBMa, run one DVEn
 * route action and leave DBMa again. The route action selects what the
 * USB2 pair of this port carries; see tps6598x_enter_kis() and
 * tps6598x_route_ap_usb2().
 */
static int tps6598x_dbma_route(tps6598x_dev_t *dev, const u8 vdm[4], const char *who, bool reset_ok)
{
    u32 target_len = 0;
    const u8 *target = adt_getprop(adt, 0, "target-type", &target_len);
    u8 key[4] = {0};
    const u8 key_null[4] = {0, 0, 0, 0};
    u32 mode = 0;
    u8 out = 0;
    u8 en = 1;
    int ret;

    if (!target || target_len < 4)
        return -1;

    // reverse the key
    for (int i = 0; i < 4; i++)
        key[i] = target[3 - i];

    // check status and soft reset if it fails
    if (tps6598x_cmd_status(dev, "LOCK")) {
        if (!reset_ok)
            return -1;
        if (tps6598x_command(dev, "Gaid", NULL, 0, NULL, 0))
            return -1;
        mdelay(20);
    }

    ret = tps6598x_command(dev, "LOCK", key, 4, (u8 *)&out, 1);
    if (ret || (out & 0xf)) {
        printf("%s: Failed to unlock using '%.4s': ret=%d result=0x%hhx\n", who, key, ret,
               out & 0xf);
        return -1;
    }

    ret = tps6598x_command(dev, "DBMa", &en, 1, &out, 1);
    if (ret || (out & 0xf)) {
        printf("%s: DBMa cmd failed: ret=%d result=0x%hhx\n", who, ret, out & 0xf);
        return -1;
    }

    ret = tps6598x_read_reg(dev, TPS_REG_MODE, (u8 *)&mode, 4);
    if (ret != 4 || mode != TPS_MODE_DBMA) {
        printf("%s: Failed to enter DBMa mode, mode=0x%08x\n", who, mode);
        return -1;
    }

    ret = tps6598x_command(dev, "DVEn", vdm, 4, &out, 1);
    if (ret || (out & 0xf)) {
        printf("%s: DVEn cmd failed: ret=%d result=0x%hhx\n", who, ret, out & 0xf);
        return -1;
    }

    en = 0;
    if (tps6598x_command(dev, "DBMa", &en, 1, NULL, 0) ||
        tps6598x_command(dev, "LOCK", key_null, 4, NULL, 0))
        return -1;

    return ret;
}

int tps6598x_enter_kis(tps6598x_dev_t *dev)
{
    /* DVEn 0x01824606: the debug (KIS) function on this port's USB2 pair. */
    static const u8 vdm[] = {0x06, 0x46, 0x82, 0x01};
    return tps6598x_dbma_route(dev, vdm, "tps6598x_enter_kis", true);
}

int tps6598x_route_ap_usb2(tps6598x_dev_t *dev)
{
    /*
     * DVEn 0x01820606: connect this port's primary USB2 pair to the AP USB
     * controller instead of the debug function. This ends KIS on the port
     * until the host routes it back (macvdmctl debugusb). It is not the
     * separate 0x0106 action, which reboots into DFU.
     */
    static const u8 vdm[] = {0x06, 0x06, 0x82, 0x01};
    return tps6598x_dbma_route(dev, vdm, "tps6598x_route_ap_usb2", false);
}

int tps6598x_foreach_hpm(hpm_match_t *match, hpm_action_t *action, void *data)
{
    char hpm_path[64] = {0};
    char bus_path[64] = {0};
    int node;
    int ret;
    bool stop = false; // Whether we should stop iteration after a non-zero action return value
    int matched = HPM_FOREACH_NO_MATCH; // Whether we found any matching hpms; used as return value

    node = adt_path_offset(adt, "/arm-io");
    if (node < 0)
        return HPM_FOREACH_NO_MATCH;

    ADT_FOREACH_CHILD(adt, node)
    {
        int mngr_node;

        if (adt_is_compatible(adt, node, "i2c,s5l8940x")) {
            mngr_node = adt_first_child_offset(adt, node);
            if (mngr_node < 0 || !adt_is_compatible(adt, mngr_node, "usbc,manager"))
                continue;

            ret = snprintf(bus_path, sizeof(bus_path), "/arm-io/%s", adt_get_name(adt, node));
            if (ret < 0 || (size_t)ret >= sizeof(bus_path))
                continue;

            i2c_dev_t *i2c = NULL;

            int it = mngr_node;
            ADT_FOREACH_CHILD(adt, it)
            {
                if (!adt_is_compatible(adt, it, "usbc,cd3217"))
                    continue;

                const char *name = adt_get_name(adt, it);

                ret = snprintf(hpm_path, sizeof(hpm_path), "/arm-io/%s/%s/%s",
                               adt_get_name(adt, node), adt_get_name(adt, mngr_node), name);
                if (ret < 0 || (size_t)ret >= sizeof(hpm_path))
                    continue;

                if (!match(hpm_path, data))
                    continue;
                matched = HPM_FOREACH_MATCH;

                if (!i2c) {
                    i2c = i2c_init(bus_path);
                    if (!i2c) {
                        printf("tps6598x: i2c_init failed for %s.\n", bus_path);
                        break; // skip to the next bus
                    }
                }

                tps6598x_dev_t *tps = tps6598x_init_i2c(hpm_path, i2c);
                if (!tps) {
                    printf("tps6598x: init failed for %s.\n", hpm_path);
                    continue; // try the next hpm on this bus
                }

                ret = action(hpm_path, tps, data);

                tps6598x_shutdown(tps);

                if (ret != 0) {
                    stop = true; // action indicated end of iteration: Do not iterate another bus
                    break;
                }
            }
            if (i2c)
                i2c_shutdown(i2c);
        } else if (adt_is_compatible(adt, node, "aapl,spmi") ||
                   adt_is_compatible(adt, node, "spmi,gen3")) {

            ret = snprintf(bus_path, sizeof(bus_path), "/arm-io/%s", adt_get_name(adt, node));
            if (ret < 0 || (size_t)ret >= sizeof(bus_path))
                continue;

            spmi_dev_t *spmi = NULL;

            int it = node;
            ADT_FOREACH_CHILD(adt, it)
            {
                if (!adt_is_compatible(adt, it, "usbc,sn201202x,spmi"))
                    continue;

                const char *name = adt_get_name(adt, it);

                ret = snprintf(hpm_path, sizeof(hpm_path), "/arm-io/%s/%s", adt_get_name(adt, node),
                               name);
                if (ret < 0 || (size_t)ret >= sizeof(hpm_path))
                    continue;

                if (!match(hpm_path, data))
                    continue;
                matched = HPM_FOREACH_MATCH;

                if (!spmi) {
                    spmi = spmi_init(bus_path);
                    if (!spmi) {
                        printf("tps6598x: spmi_init failed for %s.\n", bus_path);
                        break; // skip to the next bus
                    }
                }

                tps6598x_dev_t *tps = tps6598x_init_spmi(hpm_path, spmi);
                if (!tps) {
                    printf("tps6598x: init failed for %s.\n", hpm_path);
                    continue; // try the next hpm on this bus
                }

                ret = action(hpm_path, tps, data);

                tps6598x_shutdown(tps);

                if (ret != HPM_ACTION_CONTINUE) {
                    stop = true; // action indicated end of iteration: Do not iterate another bus
                    break;
                }
            }
            if (spmi)
                spmi_shutdown(spmi);
        }
        if (stop)
            return ret;
    }

    return matched;
}

static int tps6598x_enable_debugusb_one(char *hpm_path, tps6598x_dev_t *tps, void *unused)
{
    (void)unused;
    printf("tps6598x: enable debugusb for %s\n", hpm_path);

    if (tps6598x_powerup(tps) < 0) {
        printf("tps6598x_enable_debugusb: tps6598x_powerup failed for %s.\n", hpm_path);
        return HPM_ACTION_ERROR;
    }

    if (tps6598x_enter_kis(tps))
        return HPM_ACTION_ERROR;

    return HPM_ACTION_STOP; // stop iterating
}

static bool tps6598x_is_dfu(char *hpm_path, void *unused)
{
    (void)unused;
    size_t len = strlen(hpm_path);
    if (len < 4)
        return false;
    return !strcmp(hpm_path + len - 4, "hpm0");
}

int tps6598x_enable_debugusb(void)
{
    int ret = tps6598x_foreach_hpm(tps6598x_is_dfu, tps6598x_enable_debugusb_one, NULL);
    if (ret < 0) {
        printf("tps6598x_enable_debugusb failed (node not found?)\n");
        return ret;
    }
    return 0;
}
