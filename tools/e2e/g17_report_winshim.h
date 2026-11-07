/* SPDX-License-Identifier: MIT */
#ifndef G17_REPORT_WINSHIM_H
#define G17_REPORT_WINSHIM_H
#include <stdint.h>
typedef uint32_t DWORD;
typedef uint32_t UINT;
typedef int BOOL;
typedef void *HANDLE;
#define STD_OUTPUT_HANDLE ((DWORD)-11)
UINT EnumSystemFirmwareTables(DWORD, void *, DWORD);
UINT GetSystemFirmwareTable(DWORD, DWORD, void *, DWORD);
HANDLE GetStdHandle(DWORD);
BOOL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
int g17_report_main(void);
#endif
