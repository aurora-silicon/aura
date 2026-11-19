/* SPDX-License-Identifier: MIT */

#include "kboot_t6031.h"
#include "adt.h"
#include "soc.h"
#include "string.h"
#include "utils.h"

#include "libfdt/libfdt.h"

#define FW_UUIDS_PROP "aurora,t6031-fw-uuids"
#define UUID_LEN      36

bool t6031_board(const void *dt)
{
    return chip_id == T6031 && !fdt_node_check_compatible(dt, 0, "apple,t6031") &&
           (!fdt_node_check_compatible(dt, 0, "apple,j514c") ||
            !fdt_node_check_compatible(dt, 0, "apple,j516c"));
}

/*
 * T6032 (M3 Ultra, Mac Studio J575D): only the firmware image UUID list runs here. The 27.0.1
 * (26A434) ADT has the same iop nubs as T6031's for every owner the list keeps, and the owners
 * whose processor the ADT does not describe (dcp, pmp) come out "absent", as designed.
 */
bool t6032_board(const void *dt)
{
    return chip_id == T6032 && !fdt_node_check_compatible(dt, 0, "apple,t6032") &&
           !fdt_node_check_compatible(dt, 0, "apple,j575d");
}

/* The ADT's uuid string: 8-4-4-4-12 hexadecimal digits and a NUL, exactly */
static bool fw_uuid_ok(const char *s, u32 len)
{
    if (!s || len != UUID_LEN + 1 || s[UUID_LEN])
        return false;
    for (int i = 0; i < UUID_LEN; i++) {
        char c = s[i];
        bool hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
        if ((i == 8 || i == 13 || i == 18 || i == 23) ? c != '-' : !hex)
            return false;
    }
    return true;
}

/* Owner name and iop nub paths, the first the ADT has being used (T6031 calls the GPU's nub0) */
static const struct {
    const char *owner;
    const char *nub[2];
} fw_nubs[] = {
    {"gfx", {"/arm-io/gfx-asc/iop-gfx-nub0", "/arm-io/gfx-asc/iop-gfx-nub"}},
    {"dcp", {"/arm-io/dcp/iop-dcp-nub", NULL}},
    {"dcpext0", {"/arm-io/dcpext0/iop-dcpext0-nub", NULL}},
    {"dcpext1", {"/arm-io/dcpext1/iop-dcpext1-nub", NULL}},
    {"dcpext2", {"/arm-io/dcpext2/iop-dcpext2-nub", NULL}},
    {"dcpext3", {"/arm-io/dcpext3/iop-dcpext3-nub", NULL}},
    {"pmp", {"/arm-io/pmp/iop-pmp-nub", NULL}},
};

void dt_t6031_fw_uuids(void *dt)
{
    char list[ARRAY_SIZE(fw_nubs) * 48];
    int len = 0, found = 0;
    bool t6031 = t6031_board(dt);

    if (!t6031 && !t6032_board(dt))
        return;
    const char *tag = t6031 ? "T6031" : "T6032";
    int chosen = fdt_path_offset(dt, "/chosen");
    if (chosen < 0 || fdt_getprop(dt, chosen, FW_UUIDS_PROP, NULL))
        return;

    for (size_t i = 0; i < ARRAY_SIZE(fw_nubs); i++) {
        const char *uuid = NULL, *value;
        u32 uuid_len = 0;
        for (int k = 0; k < 2 && fw_nubs[i].nub[k]; k++) {
            int nub = adt_path_offset(adt, fw_nubs[i].nub[k]);
            if (nub >= 0) {
                uuid = adt_getprop(adt, nub, "uuid", &uuid_len);
                break;
            }
        }
        if (fw_uuid_ok(uuid, uuid_len)) {
            value = uuid;
            found++;
        } else {
            value = uuid ? "invalid" : "absent";
        }
        /* Each entry and its NUL; the buffer has room for every one at its longest */
        len += snprintf(list + len, sizeof(list) - len, "%s=%s", fw_nubs[i].owner, value) + 1;
        printf("FDT: %s: firmware image %s=%s\n", tag, fw_nubs[i].owner, value);
    }

    if (fdt_setprop(dt, chosen, FW_UUIDS_PROP, list, len))
        printf("FDT: %s: could not add /chosen/" FW_UUIDS_PROP "\n", tag);
    else
        printf("FDT: %s: %d of %d firmware image UUIDs in /chosen/" FW_UUIDS_PROP "\n", tag, found,
               (int)ARRAY_SIZE(fw_nubs));
}

#define FACTS_NODE     "aurora,t6031-facts"
#define FACTS_CLOCKS   4   /* disp0 clock ids kept */
#define FACTS_MCC_REGS 16  /* /arm-io/mcc reg entries kept */
#define CLOCK_ID_BASE  256 /* clock ids from 256 index /arm-io clock-frequencies */

/*
 * No T6032 (J575D) board check here: its 27.0.1 ADT has 20 /arm-io/mcc reg entries (3 header
 * windows, then 8 MCC instances per die), so the 16-entry mcc-reg cap would silently drop the
 * last four die-1 windows, and /arm-io/disp0 does not exist on the Studio (no clock facts).
 * The T6031 ADT's 11 entries all fit; this export stays T6031-boards-only until a consumer
 * needs the T6032 shape.
 */

/* disp0's clock ids and the frequency of each in /arm-io clock-frequencies (0: not there) */
static int facts_clocks(u32 *ids, u32 *freqs)
{
    u32 len = 0, flen = 0;
    int disp0 = adt_path_offset(adt, "/arm-io/disp0"), arm_io = adt_path_offset(adt, "/arm-io");
    const u32 *v = disp0 < 0 ? NULL : adt_getprop(adt, disp0, "clock-ids", &len);
    const u32 *f = arm_io < 0 ? NULL : adt_getprop(adt, arm_io, "clock-frequencies", &flen);
    int n = 0;

    for (u32 i = 0; v && i < len / 4 && n < FACTS_CLOCKS; i++, n++) {
        u32 id, freq = 0;
        memcpy(&id, &v[i], sizeof(id));
        if (f && id >= CLOCK_ID_BASE && id - CLOCK_ID_BASE < flen / 4)
            memcpy(&freq, &f[id - CLOCK_ID_BASE], sizeof(freq));
        ids[n] = id;
        freqs[n] = freq;
    }
    return n;
}

void dt_t6031_facts(void *dt)
{
    fdt32_t ids[FACTS_CLOCKS], freqs[FACTS_CLOCKS];
    fdt64_t mcc[2 * FACTS_MCC_REGS];
    u32 id[FACTS_CLOCKS], freq[FACTS_CLOCKS];
    int path[8], nmcc = 0;

    if (!t6031_board(dt))
        return;
    int nclk = facts_clocks(id, freq);
    for (int i = 0; i < nclk; i++) {
        ids[i] = cpu_to_fdt32(id[i]);
        freqs[i] = cpu_to_fdt32(freq[i]);
    }
    if (adt_path_offset_trace(adt, "/arm-io/mcc", path) >= 0) {
        for (; nmcc < FACTS_MCC_REGS; nmcc++) {
            u64 base, size;
            if (adt_get_reg(adt, path, "reg", nmcc, &base, &size) < 0)
                break;
            mcc[2 * nmcc] = cpu_to_fdt64(base);
            mcc[2 * nmcc + 1] = cpu_to_fdt64(size);
        }
    }

    int chosen = fdt_path_offset(dt, "/chosen");
    if (chosen < 0 || fdt_subnode_offset(dt, chosen, FACTS_NODE) >= 0)
        return;
    int node = fdt_add_subnode(dt, chosen, FACTS_NODE);
    int err = node < 0 || fdt_setprop_u32(dt, node, "schema-version", 1);
    if (!err && nclk)
        err = fdt_setprop(dt, node, "disp0-clock-ids", ids, nclk * sizeof(*ids)) ||
              fdt_setprop(dt, node, "disp0-clock-frequencies", freqs, nclk * sizeof(*freqs));
    if (!err && nmcc)
        err = fdt_setprop(dt, node, "mcc-reg", mcc, 2 * nmcc * sizeof(*mcc));
    if (err) {
        printf("FDT: T6031: could not add /chosen/" FACTS_NODE "\n");
        chosen = fdt_path_offset(dt, "/chosen");
        node = chosen < 0 ? -1 : fdt_subnode_offset(dt, chosen, FACTS_NODE);
        if (node >= 0)
            fdt_del_node(dt, node);
        return;
    }
    printf("FDT: T6031: /chosen/" FACTS_NODE ": %d disp0 clock(s)", nclk);
    for (int i = 0; i < nclk; i++)
        printf(" 0x%x=%u Hz", id[i], freq[i]);
    printf(", %d mcc reg window(s)\n", nmcc);
}
