/* SPDX-License-Identifier: MIT */
/*
 * T8132 (M4) board rules. They live here, not in kboot.c: kboot.c's code is linked into Stage 1's
 * .init slot (aura-raw.ld), which has almost no room left.
 */

#include "kboot.h"
#include "adt.h"
#include "firmware.h"
#include "soc.h"
#include "utils.h"

#include "libfdt/libfdt.h"

/*
 * J773g (Mac mini, M4): the J773g admissions in this tree were checked against one ADT, that
 * of 26.6.2 (target J773gAP, board-id 0x2a, iBoot mBoot-18000.161.10). Admit them only when
 * the live machine, its firmware and the OS tree all say J773g.
 */
bool kboot_is_j773g(const void *fdt)
{
    return fdt && chip_id == T8132 && board_id == 0x2a && adt_is_compatible(adt, 0, "J773gAP") &&
           os_firmware.version == V26_6_2 && !fdt_node_check_compatible(fdt, 0, "apple,j773g") &&
           !fdt_node_check_compatible(fdt, 0, "apple,t8132");
}
