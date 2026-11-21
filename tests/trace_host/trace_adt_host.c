/* SPDX-License-Identifier: MIT */
/*
 * Host run of src/sptm_virt_trace_adt.c (with sptm_virt_s2r_soc.c's link lookup and
 * sptm_virt_gfxrec_adt.c's cover) on a real ADT, with aura's ADT reader (rust/src/adt.rs) built
 * for the host (tests/python/test_sptm_virt_trace_host.py).
 *
 *   trace-adt-host <aura.adt> <phandle,...|-> <pa>:<len> ...
 *
 * prints "link 1" (or 0: no port found from those PHYs), then per range "ok <pa> - <cover>" or
 * "refused <pa> <device> <cover>", as aura's block trace decides before it hooks anything. Any
 * register access exits 3: the decision takes the ADT alone.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adt.h"
#include "sptm_virt_gfxrec.h"
#include "sptm_virt_trace.h"

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

u32 host_read32(u64 addr)
{
    fprintf(stderr, "a register access at 0x%llx\n", (unsigned long long)addr);
    exit(3);
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
    host_read32(addr);
}
void udelay(u32 d)
{
    (void)d;
}

int main(int argc, char **argv)
{
    if (argc < 3)
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
    u32 phys[2] = {0, 0};
    int n = 0;
    for (char *p = argv[2]; *p && *p != '-' && n < 2; n++) {
        phys[n] = strtoul(p, &p, 0);
        if (*p == ',')
            p++;
    }
    printf("link %d\n", sptm_virt_trace_adt_link(phys, 2));
    for (int i = 3; i < argc; i++) {
        char *p;
        u64 pa = strtoull(argv[i], &p, 0), len = strtoull(p + 1, NULL, 0);
        const char *dev = sptm_virt_trace_adt_refused(pa, len);
        char node[48];
        sptm_virt_gfxrec_adt_cover(pa, len, node, sizeof(node));
        printf("%s 0x%llx %s %s\n", dev ? "refused" : "ok", (unsigned long long)pa, dev ? dev : "-",
               node[0] ? node : "(no cover)");
    }
    return 0;
}
