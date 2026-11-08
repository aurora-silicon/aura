/* SPDX-License-Identifier: MIT */
/*
 * Host run of the sleep path's SoC discovery (src/sptm_virt_s2r_soc.c) and the debug link's
 * power domains (pmgr.c) on a real ADT, with aura's ADT reader built for the host.
 *
 *   sptm-virt-s2r-soc-host <aura.adt> [<debug-link PHY phandle>...]
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adt.h"
#include "pmgr.h"
#include "sptm_virt_s2r_soc.h"

void *adt;
u32 board_id, chip_id;
static u32 adt_size;

/* As at runtime: the bound of every ADT read is aura's own ADT's size. */
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

/* No bus: PS words read as on, and nothing is written. */
u32 host_read32(u64 addr)
{
    (void)addr;
    return 0xff;
}
u64 host_read64(u64 addr)
{
    (void)addr;
    return 0;
}
u8 host_read8(u64 addr)
{
    (void)addr;
    return 0;
}
void host_write32(u64 addr, u32 val)
{
    (void)addr;
    (void)val;
}
void udelay(u32 d)
{
    (void)d;
}

static void *load(const char *path, u32 *size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        exit(2);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc(n);
    if (fread(buf, 1, n, f) != (size_t)n)
        exit(2);
    fclose(f);
    if (size)
        *size = n;
    return buf;
}

#define BLK(name) printf(#name " 0x%llx 0x%llx\n", (unsigned long long)s.name.base, \
                         (unsigned long long)s.name.size)

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    adt = load(argv[1], &adt_size);
    u32 phys[2] = {0, 0};
    for (int i = 2; i < argc && i < 4; i++)
        phys[i - 2] = strtoul(argv[i], NULL, 0);
    struct s2r_soc s;
    const char *missing;

    int rc = s2r_soc_from_adt(adt, phys, 2, &s, &missing);
    printf("rc %d missing %s\n", rc, missing ? missing : "-");
    BLK(soc_ps);
    BLK(cluster_ctl);
    BLK(pwrgate);
    BLK(cllt);
    BLK(nub_ap);
    BLK(aon);
    BLK(clkgen);
    BLK(soc_misc);
    BLK(aon_pst);
    printf("pms_pmc 0x%llx 0x%llx\n", (unsigned long long)s.pms_pmc[0],
           (unsigned long long)s.pms_pmc[1]);
    printf("nub_wake_status 0x%llx\n", (unsigned long long)s.nub_wake_status);
    printf("amcc 0x%llx %u\n", (unsigned long long)s.amcc, s.amcc_planes);
    for (int i = 0; i < s.n_link; i++)
        printf("link %s\n", s.link_paths[i]);

    if (rc == 0 && pmgr_init() == 0) {
        for (int i = 0; i < s.n_link; i++) {
            u64 regs[32];
            u8 flags[32];
            int n = pmgr_adt_collect_psregs(s.link_paths[i], regs, flags, 32);
            for (int j = 0; j < n; j++)
                printf("domain %s 0x%llx flags 0x%x\n", s.link_paths[i],
                       (unsigned long long)regs[j], flags[j]);
        }
    }
    return 0;
}
