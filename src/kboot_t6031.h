/* SPDX-License-Identifier: MIT */

#ifndef KBOOT_T6031_H
#define KBOOT_T6031_H

#include "types.h"

/*
 * The T6031 handoff (M3 Max with the 16-core CPU and 40-core GPU) runs only on a J514C or J516C:
 * chip T6031 and a device tree whose root is apple,t6031 and apple,j514c or apple,j516c. A T6034
 * (J514M/J516M, apple,t6034 device trees) is not one; J514M gets a separate T6034 profile and chosen namespace.
 *
 * The T6032 (M3 Ultra, J575D) reuses only the firmware image UUID list: its 27.0.1 ADT has the
 * same iop nubs (see dt_t6031_fw_uuids), but no /arm-io/pmp, no dart-pmp and no disp0, so the PMP
 * values and the facts handoffs stay T6031-boards-only.
 */
bool t6031_board(const void *dt);
bool t6032_board(const void *dt);
bool t6034_board(const void *dt);
bool j516m_board(const void *dt);

/*
 * /chosen/asahi,t6031-fw-uuids: one "<owner>=<uuid>" string per firmware image iBoot loaded, for
 * gfx, dcp, dcpext0..3 and pmp, read from the ADT's iop nubs (<uuid> is "absent" or "invalid"
 * when the ADT has no well-formed one). ADT memory only.
 */
void dt_t6031_fw_uuids(void *dt);

/*
 * /chosen/asahi,t6031-facts: ADT values the kernel needs at boot and no device tree carries yet:
 * disp0's clock ids and their /arm-io clock-frequencies entries (the display clock), and the
 * /arm-io/mcc register windows (the GPU's memory-cache IO mapping). ADT memory only; what the ADT
 * lacks is left out.
 */
void dt_t6031_facts(void *dt);

#endif
