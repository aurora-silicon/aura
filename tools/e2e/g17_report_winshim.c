/* SPDX-License-Identifier: MIT
 * Fixture system-firmware backend for the unchanged production reader flow.
 * No hardware or Windows execution is claimed by this process.
 */
#include "g17_report_winshim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char fixture[4096];
static size_t fixture_size;
static const char *scenario;
static unsigned calls;
static int is(const char *name) { return strcmp(scenario, name) == 0; }

UINT EnumSystemFirmwareTables(DWORD provider, void *buffer, DWORD size)
{
    uint32_t ids[4] = {0x50434146, 0x37314741, 0x54525343, 0x37314741};
    UINT length = is("duplicate") ? 16 : 12;
    ++calls;
    fprintf(stderr, "enum:%u:%u\n", calls, size);
    if (provider != 0x41435049) abort();
    if (is("missing")) ids[1] = 0x54445344;
    if (is("enum-error")) return 0;
    if (is("enum-oversized")) return 4100;
    if (is("enum-unaligned")) return 11;
    if (buffer != NULL) {
        if (size < length) abort();
        memcpy(buffer, ids, length);
        if (is("enum-race")) return length + 4;
    }
    return length;
}

UINT GetSystemFirmwareTable(DWORD provider, DWORD id, void *buffer, DWORD size)
{
    ++calls;
    fprintf(stderr, "get:%u:%u\n", calls, size);
    if (provider != 0x41435049 || id != 0x37314741) abort();
    if (is("get-error")) return 0;
    if (buffer == NULL) return (UINT)fixture_size;
    if (fixture_size > size) abort();
    memcpy(buffer, fixture, fixture_size);
    if (is("get-short")) return (UINT)fixture_size - 1;
    if (is("get-race")) return (UINT)fixture_size + 1;
    return (UINT)fixture_size;
}

HANDLE GetStdHandle(DWORD which)
{
    if (which != STD_OUTPUT_HANDLE) abort();
    return stdout;
}

BOOL WriteFile(HANDLE handle, const void *data, DWORD size, DWORD *written, void *overlapped)
{
    if (handle != stdout || overlapped != NULL) abort();
    *written = (DWORD)fwrite(data, 1, size, stdout);
    return *written == size;
}

int main(int argc, char **argv)
{
    FILE *input;
    if (argc != 3) return 90;
    scenario = argv[1];
    input = fopen(argv[2], "rb");
    if (input == NULL) return 91;
    fixture_size = fread(fixture, 1, sizeof(fixture), input);
    if (ferror(input) || !feof(input)) return 92;
    fclose(input);
    return g17_report_main();
}
