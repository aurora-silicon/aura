/* SPDX-License-Identifier: MIT */
#ifndef HV_WINDOWS_SERIAL_CAPTURE_H
#define HV_WINDOWS_SERIAL_CAPTURE_H
#include "../../types.h"

bool hv_windows_serial_available(void);
void hv_windows_serial_putc(u8 byte);
void hv_windows_serial_flush(void);
void hv_windows_serial_finish(void);
#endif
