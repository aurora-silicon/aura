/* SPDX-License-Identifier: MIT */
/* Host stand-in for src/types.h, for tests/kboot_audio_host.c: the host libc's own types */
#ifndef TYPES_H
#define TYPES_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

#define PACKED __attribute__((packed))

#define HAVE_PTRDIFF_T 1
#define HAVE_UINTPTR_T 1
#define UPTRDIFF_T     uintptr_t

#endif
