/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Host replay of the T8152 guest SError attribution, src/hv/windows/hv_serror.c, compiled
 * unchanged (it is #included below, with HV_SERROR_HOST_TEST) against stub headers that
 * tests/python/test_windows_t8152_serror_attr.py writes: types.h, soc.h, utils.h.  Guest RAM,
 * stage 2 and the T8152 policy lists are small tables here; every address is synthetic.
 *
 * Failure modes this covers (i0 session 2):
 *  A1 the software stage-1 walk picks the wrong half, level, index or output bits (4 KiB and
 *     16 KiB granules, blocks, faults, EPD, non-canonical VAs, reserved granules, SCTLR.M=0);
 *  A2 the walk reads a descriptor outside guest RAM instead of stopping;
 *  A3 a load/store's address is decoded wrongly (unsigned/unscaled/pre/post/register offset,
 *     pairs, literal, exclusives, atomics, SIMD), or a non-memory instruction is decoded;
 *  A4 an IPA is classified wrongly against stage 2 and the lists (a hook or an unmapped IPA
 *     called a candidate, a passthrough not called one);
 *  A5 the report misses the candidate (the xHCI load just before the PC, a register into the
 *     AIC) or invents one, or prints off T8152.
 * The report text is printed between "report-begin"/"report-end" markers; every case ends with
 * one JSON line.
 * Usage: windows_serror_host
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HV_SERROR_HOST_TEST
#include "hv_serror.c"

/* ---- stubs ------------------------------------------------------------------------------ */

u32 chip_id = T8152;
struct hv_serror_s1 stub_s1;

#define RAM_LO 0x80000000UL
#define RAM_HI 0xc0000000UL

#define XHCI      0x40a430000UL
#define AIC       0x301000000UL
#define FB_SHADOW 0x0d0000000UL /* a mapped carrier exception (outside RAM here) */
#define GPU_FW    0x10a0000000UL
#define QSPI      0x331118000UL
#define AHCI      0xf40000000UL

static struct {
    u64 addr, val;
} mem[512];
static int nmem;
static int bad_reads; /* A2: a read outside guest RAM */

static void poke(u64 addr, u64 val)
{
    for (int i = 0; i < nmem; i++)
        if (mem[i].addr == addr) {
            mem[i].val = val;
            return;
        }
    if (nmem < 512)
        mem[nmem++] = (typeof(mem[0])){addr, val};
}

static u64 peek(u64 addr)
{
    for (int i = 0; i < nmem; i++)
        if (mem[i].addr == addr)
            return mem[i].val;
    return 0;
}

bool stub_rd_guest_ram(void *ctx, u64 ipa, u64 *out)
{
    (void)ctx;
    if ((ipa & 7) || ipa < RAM_LO || ipa >= RAM_HI)
        return false;
    *out = peek(ipa);
    return true;
}

static bool any_read(void *ctx, u64 ipa, u64 *out)
{
    (void)ctx;
    if (ipa < RAM_LO || ipa >= RAM_HI)
        bad_reads++;
    *out = peek(ipa);
    return true;
}

bool stub_read_insn(u64 ipa, u32 *out)
{
    if ((ipa & 3) || ipa < RAM_LO || ipa >= RAM_HI)
        return false;
    u64 w = peek(ipa & ~7UL);
    *out = (ipa & 4) ? (u32)(w >> 32) : (u32)w;
    return true;
}

static void poke32(u64 addr, u32 v)
{
    u64 w = peek(addr & ~7UL);
    if (addr & 4)
        w = (w & 0xffffffffUL) | ((u64)v << 32);
    else
        w = (w & ~0xffffffffUL) | v;
    poke(addr & ~7UL, w);
}

u64 stub_s2_lookup(u64 ipa)
{
    if ((ipa >= RAM_LO && ipa < RAM_HI) || (ipa >= XHCI && ipa < XHCI + 0x8000) ||
        (ipa >= AIC && ipa < AIC + 0x1cc000) || (ipa >= FB_SHADOW && ipa < FB_SHADOW + 0x400000))
        return (ipa & ~0x3fffUL) | 1;
    if (ipa >= AHCI && ipa < AHCI + 0x4000)
        return 1UL << 50; /* a C hook */
    return 0;             /* GPU firmware (registered only), QSPI (denied), anything else */
}

int hv_t8152_describe_ipa(u64 ipa, const char **name)
{
    *name = NULL;
    if (ipa >= RAM_LO && ipa < RAM_HI)
        return *name = "guest RAM", HV_T8152_IPA_RAM;
    if (ipa >= XHCI && ipa < XHCI + 0x8000)
        return *name = "usb-drd1 xHCI (D4)", HV_T8152_IPA_MMIO;
    if (ipa >= AIC && ipa < AIC + 0x1cc000)
        return *name = "aic (native AIC passthrough)", HV_T8152_IPA_MMIO;
    if (ipa >= FB_SHADOW && ipa < FB_SHADOW + 0x400000)
        return *name = "fb shadow", HV_T8152_IPA_EXCEPTION;
    if (ipa >= GPU_FW && ipa < GPU_FW + 0x100000)
        return *name = "gpu fw data", HV_T8152_IPA_EXCEPTION;
    if (ipa >= QSPI && ipa < QSPI + 0x8000)
        return *name = "qspi", HV_T8152_IPA_DENIED;
    return HV_T8152_IPA_UNLISTED;
}

/* ---- a guest stage 1 built in "RAM" ----------------------------------------------------- */

static u64 next_table;

static u64 alloc_table(unsigned int g)
{
    u64 t = next_table;
    next_table += 1UL << g;
    return t;
}

/* Map one granule (level 3) or, with `block_level`, a block at that level. */
static void map(u64 ttbr, unsigned int in, unsigned int g, u64 va, u64 pa, int block_level)
{
    unsigned int stride = g - 3;
    int levels = (int)((in - g + stride - 1) / stride);
    u64 table = ttbr;
    for (int level = 4 - levels; level <= 3; level++) {
        unsigned int shift = g + stride * (unsigned int)(3 - level);
        unsigned int bits = level == 4 - levels ? in - shift : stride;
        u64 slot = table + ((va >> shift) & ((1UL << bits) - 1)) * 8;
        if (level == block_level) {
            poke(slot, (pa & ~((1UL << shift) - 1)) | 1);
            return;
        }
        if (level == 3) {
            poke(slot, (pa & ~((1UL << g) - 1)) | 3);
            return;
        }
        u64 d = peek(slot);
        if (!(d & 1)) {
            d = alloc_table(g) | 3;
            poke(slot, d);
        }
        table = d & GENMASK(47, g);
    }
}

/* 4 KiB granule, 48-bit halves: T0SZ = T1SZ = 16, TG0 = 0b00, TG1 = 0b10. */
#define TCR_4K ((16UL) | (16UL << 16) | (0UL << 14) | (2UL << 30))
/* 16 KiB granule, 47-bit: T0SZ = T1SZ = 17, TG0 = 0b10, TG1 = 0b01. */
#define TCR_16K ((17UL) | (17UL << 16) | (2UL << 14) | (1UL << 30))

/* ---- harness ----------------------------------------------------------------------------- */

static int failures;
static const char *current;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d [%s] CHECK(%s)\n", __FILE__, __LINE__, current, #cond);         \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

#define EQ(a, b)                                                                                   \
    do {                                                                                           \
        unsigned long long _a = (unsigned long long)(a), _b = (unsigned long long)(b);             \
        if (_a != _b) {                                                                            \
            fprintf(stderr, "%s:%d [%s] %s = 0x%llx, want 0x%llx\n", __FILE__, __LINE__, current,  \
                    #a, _a, _b);                                                                   \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static void begin(const char *name)
{
    current = name;
    nmem = 0;
    bad_reads = 0;
    next_table = RAM_LO;
    chip_id = T8152;
    fflush(stdout);
}

static void end(int before)
{
    fflush(stdout);
    printf("{\"case\": \"%s\", \"result\": \"%s\"}\n", current,
           failures == before ? "passed" : "failed");
}

/* ---- cases -------------------------------------------------------------------------------- */

/* A1, A2 */
static void case_walk(void)
{
    int before = failures;
    begin("walk");
    u64 ipa;

    u64 ttbr0 = alloc_table(12), ttbr1 = alloc_table(12);
    struct hv_serror_s1 s1 = {.sctlr = 1, .tcr = TCR_4K, .ttbr0 = ttbr0, .ttbr1 = ttbr1 | 1};
    map(ttbr1, 48, 12, 0xfffff80000101000UL, 0x80100000UL, -1);
    map(ttbr1, 48, 12, 0xffffd00000030000UL, XHCI, -1);
    map(ttbr1, 48, 12, 0xffffd00000200000UL, 0x80200000UL, 2); /* 2 MiB block */
    map(ttbr0, 48, 12, 0x00007ff600001000UL, 0x80300000UL, -1);

    EQ(hv_serror_s1_walk(&s1, 0xfffff80000101234UL, stub_rd_guest_ram, NULL, &ipa),
       HV_SERR_S1_OK);
    EQ(ipa, 0x80100234UL);
    EQ(hv_serror_s1_walk(&s1, 0xffffd00000030020UL, stub_rd_guest_ram, NULL, &ipa),
       HV_SERR_S1_OK);
    EQ(ipa, XHCI + 0x20);
    EQ(hv_serror_s1_walk(&s1, 0xffffd000002abcdeUL, stub_rd_guest_ram, NULL, &ipa),
       HV_SERR_S1_OK);
    EQ(ipa, 0x802abcdeUL);
    EQ(hv_serror_s1_walk(&s1, 0x00007ff600001008UL, stub_rd_guest_ram, NULL, &ipa),
       HV_SERR_S1_OK);
    EQ(ipa, 0x80300008UL);
    /* TBI: the top byte does not select the half or fail the range check. */
    EQ(hv_serror_s1_walk(&s1, 0x0a007ff600001008UL, stub_rd_guest_ram, NULL, &ipa),
       HV_SERR_S1_OK);
    EQ(ipa, 0x80300008UL);
    /* Unmapped, non-canonical, EPD1, reserved granule, stage 1 off. */
    EQ(hv_serror_s1_walk(&s1, 0xfffff80000102000UL, stub_rd_guest_ram, NULL, &ipa),
       HV_SERR_S1_FAULT);
    EQ(hv_serror_s1_walk(&s1, 0x0080000000000000UL, stub_rd_guest_ram, NULL, &ipa),
       HV_SERR_S1_RANGE);
    EQ(hv_serror_s1_walk(&s1, 0xfffe800000000000UL, stub_rd_guest_ram, NULL, &ipa),
       HV_SERR_S1_RANGE);
    s1.tcr |= 1UL << 23;
    EQ(hv_serror_s1_walk(&s1, 0xfffff80000101000UL, stub_rd_guest_ram, NULL, &ipa),
       HV_SERR_S1_FAULT);
    s1.tcr = TCR_4K & ~(3UL << 30);
    EQ(hv_serror_s1_walk(&s1, 0xfffff80000101000UL, stub_rd_guest_ram, NULL, &ipa),
       HV_SERR_S1_GRANULE);
    s1.tcr = TCR_4K;
    s1.sctlr = 0;
    EQ(hv_serror_s1_walk(&s1, 0x1234, stub_rd_guest_ram, NULL, &ipa), HV_SERR_S1_OFF);
    EQ(ipa, 0x1234);
    s1.sctlr = 1;

    /* A2: a table pointer outside guest RAM stops the walk; nothing outside RAM is read. */
    s1.ttbr0 = XHCI;
    EQ(hv_serror_s1_walk(&s1, 0x00007ff600001008UL, stub_rd_guest_ram, NULL, &ipa),
       HV_SERR_S1_UNREADABLE);
    s1.ttbr0 = ttbr0;
    map(ttbr0, 48, 12, 0x0000100000000000UL, 0, -1);
    /* point that L1 entry at the AIC instead of a table in RAM */
    u64 l0 = peek(ttbr0 + ((0x0000100000000000UL >> 39) & 0x1ff) * 8) & GENMASK(47, 12);
    poke(l0 + ((0x0000100000000000UL >> 30) & 0x1ff) * 8, AIC | 3);
    EQ(hv_serror_s1_walk(&s1, 0x0000100000000000UL, stub_rd_guest_ram, NULL, &ipa),
       HV_SERR_S1_UNREADABLE);
    /* The same walk with a reader that reads anything would have read the AIC. */
    hv_serror_s1_walk(&s1, 0x0000100000000000UL, any_read, NULL, &ipa);
    EQ(bad_reads, 1);

    /* 16 KiB granule, 47-bit: three levels from L1, 32 MiB blocks at L2. */
    nmem = 0;
    next_table = RAM_LO;
    u64 t16 = alloc_table(14);
    struct hv_serror_s1 s16 = {.sctlr = 1, .tcr = TCR_16K, .ttbr0 = 0, .ttbr1 = t16};
    map(t16, 47, 14, 0xffffc00012344000UL, 0x80104000UL, -1);
    map(t16, 47, 14, 0xffffc00020000000UL, 0x82000000UL, 2);
    EQ(hv_serror_s1_walk(&s16, 0xffffc00012345678UL, stub_rd_guest_ram, NULL, &ipa),
       HV_SERR_S1_OK);
    EQ(ipa, 0x80105678UL);
    EQ(hv_serror_s1_walk(&s16, 0xffffc00021234567UL, stub_rd_guest_ram, NULL, &ipa),
       HV_SERR_S1_OK);
    EQ(ipa, 0x83234567UL);
    EQ(hv_serror_s1_walk(&s16, 0xffff400000000000UL, stub_rd_guest_ram, NULL, &ipa),
       HV_SERR_S1_RANGE); /* VA[47] clear: outside the 47-bit upper half */
    end(before);
}

/* A3: encodings from the LLVM assembler (aarch64, +lse). */
static void case_decode(void)
{
    int before = failures;
    begin("decode");
    u64 regs[31] = {0};
    regs[1] = 0x1000;
    regs[2] = 0x3;
    regs[3] = 0x4000;
    u64 sp = 0x9010;
    struct hv_serror_mem m;

    CHECK(hv_serror_decode_mem(0xf9400420, 0, regs, sp, true, &m)); /* ldr x0, [x1, #8] */
    EQ(m.addr, 0x1008);
    EQ(m.dir, 0);
    EQ(m.rn, 1);
    CHECK(hv_serror_decode_mem(0xb9000062, 0, regs, sp, true, &m)); /* str w2, [x3] */
    EQ(m.addr, 0x4000);
    EQ(m.dir, 1);
    /* ldp x29, x30, [sp], #16: executed -> the old sp; at the PC -> sp now */
    CHECK(hv_serror_decode_mem(0xa8c17bfd, 0, regs, sp, true, &m));
    EQ(m.addr, 0x9000);
    EQ(m.rn, 31);
    EQ(m.dir, 0);
    CHECK(hv_serror_decode_mem(0xa8c17bfd, 0, regs, sp, false, &m));
    EQ(m.addr, 0x9010);
    /* str x0, [x1, #-8]!: executed -> x1 is the address; not yet -> x1 - 8 */
    CHECK(hv_serror_decode_mem(0xf81f8c20, 0, regs, sp, true, &m));
    EQ(m.addr, 0x1000);
    EQ(m.dir, 1);
    CHECK(hv_serror_decode_mem(0xf81f8c20, 0, regs, sp, false, &m));
    EQ(m.addr, 0xff8);
    CHECK(hv_serror_decode_mem(0xf8627820, 0, regs, sp, true, &m)); /* ldr x0,[x1,x2,lsl #3] */
    EQ(m.addr, 0x1018);
    regs[2] = 0xffffffff00000003UL;
    CHECK(hv_serror_decode_mem(0xb8625869, 0, regs, sp, true, &m)); /* ldr w9,[x3,w2,uxtw #2] */
    EQ(m.addr, 0x400c);
    regs[2] = 3;
    CHECK(hv_serror_decode_mem(0xc8dffc20, 0, regs, sp, true, &m)); /* ldar x0, [x1] */
    EQ(m.addr, 0x1000);
    EQ(m.dir, 0);
    CHECK(hv_serror_decode_mem(0x58000040, 0x2000, regs, sp, true, &m)); /* ldr x0, pc+8 */
    EQ(m.addr, 0x2008);
    CHECK(m.pc_relative);
    CHECK(!hv_serror_decode_mem(0x91000420, 0, regs, sp, true, &m)); /* add x0, x1, #1 */
    CHECK(hv_serror_decode_mem(0xb9402060, 0, regs, sp, true, &m)); /* ldr w0, [x3, #0x20] */
    EQ(m.addr, 0x4020);
    CHECK(hv_serror_decode_mem(0xb81fc064, 0, regs, sp, true, &m)); /* stur w4, [x3, #-4] */
    EQ(m.addr, 0x3ffc);
    EQ(m.dir, 1);
    regs[2] = 0x5010;
    CHECK(hv_serror_decode_mem(0xa9bf0440, 0, regs, sp, true, &m)); /* stp x0,x1,[x2,#-16]! */
    EQ(m.addr, 0x5010);
    CHECK(hv_serror_decode_mem(0xa9bf0440, 0, regs, sp, false, &m));
    EQ(m.addr, 0x5000);
    CHECK(hv_serror_decode_mem(0x3dc00820, 0, regs, sp, true, &m)); /* ldr q0, [x1, #32] */
    EQ(m.addr, 0x1020);
    EQ(m.dir, 0);
    CHECK(hv_serror_decode_mem(0xf9800020, 0, regs, sp, true, &m)); /* prfm pldl1keep, [x1] */
    EQ(m.dir, -1);
    CHECK(hv_serror_decode_mem(0xf8200041, 0, regs, sp, true, &m)); /* ldadd x0, x1, [x2] */
    EQ(m.addr, 0x5010);
    EQ(m.dir, -1);
    end(before);
}

/* A4 */
static void case_classify(void)
{
    int before = failures;
    begin("classify");
    const char *name;
    EQ(classify(0x80001000UL, &name), HV_SERR_IPA_RAM);
    EQ(classify(XHCI + 0x20, &name), HV_SERR_IPA_PASSTHROUGH);
    CHECK(name && !strcmp(name, "usb-drd1 xHCI (D4)"));
    EQ(classify(AIC + 0x2000, &name), HV_SERR_IPA_PASSTHROUGH);
    EQ(classify(FB_SHADOW, &name), HV_SERR_IPA_EXCEPTION);
    EQ(classify(GPU_FW + 0x40, &name), HV_SERR_IPA_REGISTERED);
    EQ(classify(AHCI + 0x118, &name), HV_SERR_IPA_EMULATED);
    CHECK(name && !strcmp(name, "carrier hook"));
    EQ(classify(QSPI, &name), HV_SERR_IPA_DENIED);
    EQ(classify(0x500000000UL, &name), HV_SERR_IPA_UNMAPPED);
    for (int k = 0; k < HV_SERR_IPA_KINDS; k++)
        EQ(hv_serror_kind_candidate(k), k == HV_SERR_IPA_PASSTHROUGH ||
                                            k == HV_SERR_IPA_EXCEPTION ||
                                            k == HV_SERR_IPA_OTHER_HW);
    CHECK(!strcmp(hv_serror_kind_name(99), "?"));
    end(before);
}

static void build_guest(u64 *regs, u64 *sp)
{
    u64 ttbr1 = alloc_table(12), ttbr0 = alloc_table(12);
    stub_s1 = (struct hv_serror_s1){.sctlr = 1, .tcr = TCR_4K, .ttbr0 = ttbr0, .ttbr1 = ttbr1};
    map(ttbr1, 48, 12, 0xfffff80000101000UL, 0x80100000UL, -1); /* code */
    map(ttbr1, 48, 12, 0xfffff80000100000UL, 0x80101000UL, -1); /* the page before it */
    map(ttbr1, 48, 12, 0xffffd00000030000UL, XHCI, -1);
    map(ttbr1, 48, 12, 0xffffd00000040000UL, AHCI, -1);
    map(ttbr1, 48, 12, 0xffffd00000050000UL, GPU_FW, -1);
    map(ttbr1, 48, 12, 0xfffff80000400000UL, 0x80400000UL, -1); /* stack */
    memset(regs, 0, 31 * sizeof(u64));
    regs[1] = 0xfffff80000400100UL; /* RAM: not printed */
    regs[2] = 7;                    /* small: skipped */
    sp[0] = 0;
    sp[1] = 0xfffff80000400ff0UL;
    /* The code: a load from the xHCI page just before the PC, a non-memory op at it. The
     * window starts on the previous page (ELR - 32 = ...100fe4). */
    poke32(0x80101fe8UL, 0xf9400420); /* ldr x0, [x1, #8] (stack/RAM) */
    poke32(0x80101ffcUL, 0xd503201f); /* nop */
    poke32(0x80100000UL, 0xb9402060); /* ldr w0, [x3, #0x20] */
    poke32(0x80100004UL, 0x91000420); /* add x0, x1, #1 (the PC) */
}

/* A5 */
static void case_report(void)
{
    int before = failures;
    begin("report_device");
    u64 regs[31], sp[2];
    build_guest(regs, sp);
    regs[3] = 0xffffd00000030000UL; /* xHCI */
    regs[5] = AIC;                  /* a raw AIC address */
    regs[7] = 0xffffd00000040000UL; /* AHCI hook page: printed, not a candidate */
    regs[8] = 0xffffd00000050000UL; /* GPU firmware (registered, unmapped) */
    printf("report-begin %s\n", current);
    hv_serror_attribute(regs, sp, 0xbe000000UL, 0xfffff80000101004UL, 0x60000005);
    printf("report-end %s\n", current);
    end(before);

    before = failures;
    begin("report_none");
    build_guest(regs, sp);
    regs[3] = 0xfffff80000400200UL; /* the load before the PC now reads the stack page */
    printf("report-begin %s\n", current);
    hv_serror_attribute(regs, sp, 0xbe000000UL, 0xfffff80000101004UL, 0x60000005);
    printf("report-end %s\n", current);
    /* Off T8152: nothing at all. */
    chip_id = 0x8142;
    printf("report-begin off\n");
    hv_serror_attribute(regs, sp, 0xbe000000UL, 0xfffff80000101004UL, 0x60000005);
    printf("report-end off\n");
    end(before);
}

int main(void)
{
    case_walk();
    case_decode();
    case_classify();
    case_report();
    return failures ? 1 : 0;
}
