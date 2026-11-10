/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#ifndef TPS6598X_H
#define TPS6598X_H

#include "i2c.h"
#include "spmi.h"
#include "types.h"

typedef struct tps6598x_dev tps6598x_dev_t;

tps6598x_dev_t *tps6598x_init_i2c(const char *adt_path, i2c_dev_t *i2c);
tps6598x_dev_t *tps6598x_init_spmi(const char *adt_path, spmi_dev_t *spmi);
void tps6598x_shutdown(tps6598x_dev_t *dev);

int tps6598x_command(tps6598x_dev_t *dev, const char *cmd, const u8 *data_in, size_t len_in,
                     u8 *data_out, size_t len_out);
int tps6598x_cold_reset(tps6598x_dev_t *dev);
int tps6598x_powerup(tps6598x_dev_t *dev);

typedef bool(hpm_match_t)(char *hpm_path, void *data);
// Return codes for hpm_action_t
#define HPM_ACTION_ERROR    -1
#define HPM_ACTION_CONTINUE 0
#define HPM_ACTION_STOP     1
typedef int(hpm_action_t)(char *hpm_path, tps6598x_dev_t *tps, void *data);
// Return codes for tps6598x_foreach_hpm
#define HPM_FOREACH_MATCH    0
#define HPM_FOREACH_NO_MATCH -1
int tps6598x_foreach_hpm(hpm_match_t *match, hpm_action_t *action, void *data);

int tps6598x_enter_kis(tps6598x_dev_t *dev);
int tps6598x_route_ap_usb2(tps6598x_dev_t *dev);
int tps6598x_enable_debugusb(void);

#define CD3218B12_IRQ_WIDTH 9

typedef struct tps6598x_irq_state {
    u8 int_mask1[CD3218B12_IRQ_WIDTH];
    bool valid;
} tps6598x_irq_state_t;

int tps6598x_disable_irqs(tps6598x_dev_t *dev, tps6598x_irq_state_t *state);
int tps6598x_restore_irqs(tps6598x_dev_t *dev, tps6598x_irq_state_t *state);

/* Shared TI logical-register fields used by the ACE3 SPMI transport. */
#define TPS6598X_REG_DATA_STATUS         0x5f
#define TPS6598X_STATUS_PLUG_PRESENT     BIT(0)
#define TPS6598X_STATUS_PLUG_UPSIDE_DOWN BIT(4)
#define TPS6598X_DATA_USB3_CONNECTION    BIT(5)
#define TPS6598X_DATA_DP_CONNECTION      BIT(8)
#define TPS6598X_DATA_TBT_CONNECTION     BIT(16)
#define TPS6598X_DATA_USB4_CONNECTION    BIT(23)

#endif
