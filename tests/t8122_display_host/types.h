/* SPDX-License-Identifier: MIT */
/* Host stand-in for src/types.h: the types kboot_display_handoff.c uses. */
#ifndef TYPES_H
#define TYPES_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
#define PACKED    __attribute__((packed))
#define UNUSED(x) (void)(x)
#define AURA_XTEXT
#endif
