/* SPDX-License-Identifier: MIT
 * Read-only Windows diagnostic reader for docs/windows-g17-report.md.
 * Hardware profile facts are independently authored from public Linux
 * e50ba38b24140fc0751b811ea093c8f843bcec76, t8142-j813-gpu.dtsi and m5/live_boot.rs.
 * No driver loading, physical mapping, interrupt or device-control API is used.
 */
#include <stdint.h>
#include <stddef.h>
#if defined(G17_REPORT_REPLAY)
#include "g17_report_winshim.h"
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#error Build for Windows or the explicit firmware API replay
#endif

enum { REPORT_SIZE = 688, TABLE_SIZE = 724, ENUM_CAPACITY = 4096 };
static uint32_t table_ids[ENUM_CAPACITY / 4];
static unsigned char table[TABLE_SIZE];
static char output[8192];
static unsigned output_size;
static int output_overflow;

static uint16_t get16(const unsigned char *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t get32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t get64(const unsigned char *p)
{
    return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}
static int same(const unsigned char *a, const unsigned char *b, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; ++i) if (a[i] != b[i]) return 0;
    return 1;
}
static uint32_t crc(const unsigned char *p, unsigned size, unsigned zero_offset)
{
    uint32_t value = UINT32_MAX;
    unsigned i, bit;
    for (i = 0; i < size; ++i) {
        value ^= (i >= zero_offset && i < zero_offset + 4) ? 0 : p[i];
        for (bit = 0; bit < 8; ++bit)
            value = (value >> 1) ^ ((value & 1) ? UINT32_C(0xedb88320) : 0);
    }
    return ~value;
}
static int preservation_valid(const unsigned char *p)
{
    static const unsigned char firmware[32] = "M5-RTKit-3255.160.4.release";
    static const unsigned char model[16] = "Mac17,3";
    uint64_t bases[8], ends[8], guest_start, guest_end, dram_start, dram_end;
    unsigned i, j;
    if (!same(p, (const unsigned char *)"AG17", 4) || get16(p+4) != 1 ||
        get16(p+6) != 304 || get32(p+8) != 0x8142 || get32(p+12) != 0x24 ||
        get32(p+16) != 8 || get32(p+20) != 1 || get32(p+108) != 0 ||
        !same(p+56, firmware, 32) || !same(p+88, model, 16) ||
        get32(p+104) != crc(p, 304, 104)) return 0;
    guest_start = get64(p+24); guest_end = get64(p+32);
    dram_start = get64(p+40); dram_end = get64(p+48);
    if (dram_start == 0 || dram_start >= dram_end || dram_end > (UINT64_C(1)<<42) ||
        guest_start < dram_start || guest_end > dram_end || guest_start >= guest_end ||
        ((dram_start | dram_end | guest_start | guest_end) & 0x3fff) != 0) return 0;
    for (i = 0; i < 8; ++i) {
        const unsigned char *r = p + 112 + i*24;
        uint32_t classification = get32(r+4);
        uint64_t size = get64(r+16);
        bases[i] = get64(r+8);
        if (get32(r) != i+1 || bases[i] < dram_start || bases[i] >= dram_end ||
            size == 0 || size > dram_end-bases[i] || ((bases[i] | size) & 0x3fff)) return 0;
        ends[i] = bases[i]+size;
        if (classification == 1) {
            if (bases[i] < guest_start || ends[i] > guest_end) return 0;
        } else if (classification == 2) {
            if (ends[i] > guest_start) return 0;
        } else if (classification == 3) {
            if (bases[i] < guest_end) return 0;
        } else return 0;
        for (j = 0; j < i; ++j)
            if (bases[i] < ends[j] && bases[j] < ends[i]) return 0;
    }
    return ends[2]-bases[2] >= 0x44000 && ends[3]-bases[3] >= 0x400 &&
           ends[4]-bases[4] == 0x134000 && ends[5]-bases[5] == 0x130000 &&
           ends[6]-bases[6] == 0x4c000 && ends[7]-bases[7] == 0x48000;
}
static int table_valid(void)
{
    static const uint64_t bases[6] = {UINT64_C(0x580000000), UINT64_C(0x582600000),
        UINT64_C(0x582050000), UINT64_C(0x582e00000), UINT64_C(0x582850000), UINT64_C(0x580e80000)};
    static const uint64_t sizes[6] = {0x4000000, 0x88000, 8, 0x88000, 8, 0x30000};
    static const unsigned roles[6] = {0, 1, 1, 2, 2, 0};
    static const uint32_t irqs[4] = {1123, 1126, 1127, 1130};
    static const uint32_t power[8] = {0, 6, 0, 0x110, 0x10000, 1, 0, 0};
    const unsigned char *p = table+36;
    unsigned i;
    unsigned char sum = 0;
    for (i = 0; i < TABLE_SIZE; ++i) sum = (unsigned char)(sum + table[i]);
    if (sum != 0 || !same(table, (const unsigned char *)"AG17", 4) ||
        get32(table+4) != TABLE_SIZE || table[8] != 1 ||
        !same(table+10, (const unsigned char *)"AURORA", 6) ||
        !same(table+16, (const unsigned char *)"G17RPTV1", 8) || get32(table+24) != 1 ||
        !same(table+28, (const unsigned char *)"AURA", 4) || get32(table+32) != 1) return 0;
    if (!same(p, (const unsigned char *)"G17R", 4) || get16(p+4) != 1 ||
        get16(p+6) != 32 || get32(p+8) != REPORT_SIZE || get32(p+12) != 0 ||
        get32(p+16) != 304 || get32(p+20) != 6 || get32(p+24) != 4 ||
        get32(p+28) != crc(p, REPORT_SIZE, 28) || !preservation_valid(p+32)) return 0;
    for (i = 0; i < 6; ++i) {
        const unsigned char *r = p+336+i*32;
        if (get16(r) != i+1 || get16(r+2) != roles[i] || get16(r+4) != (i != 0) ||
            get16(r+6) != 0 || get64(r+8) != bases[i] || get64(r+16) != sizes[i] ||
            get32(r+24) != 0 || get32(r+28) != 0) return 0;
    }
    for (i = 0; i < 4; ++i) {
        const unsigned char *r = p+528+i*24;
        if (get16(r) != 1+i/2 || get16(r+2) != 1+i%2 || get32(r+4) != irqs[i] ||
            get32(r+8) != 0 || get32(r+12) != 0 || get32(r+16) != 1 || get32(r+20) != 0) return 0;
    }
    if (get16(p+624) != 42 || get16(p+626) != 42 || get16(p+628) != 14 ||
        get16(p+630) != 2 || get32(p+632) != 3 || get32(p+636) != 4 ||
        get64(p+640) != 0x40000 || get32(p+648) != 0 || get32(p+652) != 0) return 0;
    for (i = 0; i < 8; ++i) if (get32(p+656+i*4) != power[i]) return 0;
    return 1;
}
static void append_char(char c)
{
    if (output_size < sizeof(output)) output[output_size++] = c;
    else output_overflow = 1;
}
static void append(const char *s)
{
    while (*s) append_char(*s++);
}
static void number(uint32_t n)
{
    char digits[10];
    unsigned count = 0;
    do { digits[count++] = (char)('0' + n%10); n /= 10; } while (n);
    while (count) append_char(digits[--count]);
}
static void hex64(uint64_t n)
{
    static const char digits[] = "0123456789abcdef";
    unsigned i;
    append("\"0x");
    for (i = 16; i; --i) append_char(digits[(n >> ((i-1)*4)) & 15]);
    append_char('"');
}
static int emit(int status)
{
    DWORD written = 0;
    if (output_overflow || !WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), output,
                                     output_size, &written, NULL) || written != output_size) return 3;
    return status;
}
static int reject(const char *reason)
{
    append("{\"result\":\"rejected\",\"error\":\""); append(reason); append("\"}\n");
    return emit(2);
}
int g17_report_main(void)
{
    static const char digits[] = "0123456789abcdef";
    UINT bytes, returned;
    unsigned i, matches = 0;
    output_size = 0; output_overflow = 0;
    bytes = EnumSystemFirmwareTables(0x41435049, NULL, 0);
    if (bytes == 0 || bytes > ENUM_CAPACITY || bytes%4) return reject("invalid enumeration length");
    returned = EnumSystemFirmwareTables(0x41435049, table_ids, bytes);
    if (returned != bytes) return reject("enumeration changed or failed");
    for (i = 0; i < bytes/4; ++i) if (table_ids[i] == 0x37314741) ++matches;
    if (matches != 1) return reject("report absent or duplicate");
    bytes = GetSystemFirmwareTable(0x41435049, 0x37314741, NULL, 0);
    if (bytes != TABLE_SIZE) return reject("invalid report table length");
    returned = GetSystemFirmwareTable(0x41435049, 0x37314741, table, TABLE_SIZE);
    if (returned != TABLE_SIZE) return reject("report read changed or failed");
    if (!table_valid()) return reject("invalid diagnostic report");
    append("{\"result\":\"diagnostic-only\",\"activation_authority\":0,");
    append("\"published_gsivs\":[0,0,0,0],\"physical_irqs\":[1123,1126,1127,1130],");
    append("\"mmio_description_count\":6,\"uat_ias\":42,\"uat_oas\":42,\"regions\":[");
    for (i = 0; i < 8; ++i) {
        const unsigned char *r = table+68+112+i*24;
        if (i) append_char(',');
        append("{\"tag\":"); number(get32(r));
        append(",\"classification\":"); number(get32(r+4));
        append(",\"base\":"); hex64(get64(r+8));
        append(",\"size\":"); hex64(get64(r+16)); append_char('}');
    }
    append("],\"table_hex\":\"");
    for (i = 0; i < TABLE_SIZE; ++i) {
        append_char(digits[table[i]>>4]); append_char(digits[table[i]&15]);
    }
    append("\"}\n");
    return emit(0);
}
#if defined(_WIN32) && !defined(G17_REPORT_REPLAY)
void mainCRTStartup(void)
{
    ExitProcess((UINT)g17_report_main());
}
#endif
