/* SPDX-License-Identifier: MIT */
/*
 * Host run of src/sptm_virt_gfxrec_adt.c on a real ADT, with pmgr.c and aura's ADT reader
 * (rust/src/adt.rs) built for the host (tests/python/test_sptm_virt_gfxrec_host.py).
 *
 *   gfxrec-adt-host <aura.adt> <pa> <len>
 *
 * prints "cover <path> reg[i] +<offset>" and one "ps <name> <pa>" per GFX/AGX/SGX PS word.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adt.h"
#include "pmgr.h"
#include "sptm_virt_gfxrec.h"

void *adt;
u32 board_id, chip_id;
static u32 adt_size;

u32 adt_get_size(void)
{
    return adt_size;
}

int debug_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(stderr, fmt, ap);
    va_end(ap);
    return n;
}

/* No bus: pmgr_init's reads see every PS word on; nothing after it may touch a register. */
static int no_bus;
u32 host_read32(u64 addr)
{
    if (no_bus) {
        fprintf(stderr, "a register read at 0x%llx\n", (unsigned long long)addr);
        exit(3);
    }
    return 0xff;
}
u64 host_read64(u64 addr)
{
    return host_read32(addr);
}
u8 host_read8(u64 addr)
{
    return host_read32(addr);
}
void host_write32(u64 addr, u32 val)
{
    (void)val;
    no_bus = 1;
    host_read32(addr);
}
void udelay(u32 d)
{
    (void)d;
}

int main(int argc, char **argv)
{
    if (argc < 4)
        return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f)
        return 2;
    fseek(f, 0, SEEK_END);
    adt_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    adt = malloc(adt_size);
    if (fread(adt, 1, adt_size, f) != adt_size)
        return 2;
    fclose(f);
    char out[48];
    sptm_virt_gfxrec_adt_cover(strtoull(argv[2], NULL, 0), strtoull(argv[3], NULL, 0), out,
                               sizeof(out));
    printf("cover %s\n", out);
    if (pmgr_init())
        return 4;
    no_bus = 1;
    u64 ps[GFXREC_PS_MAX];
    char names[GFXREC_PS_MAX][17];
    int n = sptm_virt_gfxrec_adt_ps(ps, names, GFXREC_PS_MAX);
    for (int i = 0; i < n; i++)
        printf("ps %s 0x%llx\n", names[i], (unsigned long long)ps[i]);
    return 0;
}
