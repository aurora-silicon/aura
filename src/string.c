/* SPDX-License-Identifier: MIT */

#include <stdbool.h>
#include <stdint.h>

#include "string.h"

// Routines based on The Public Domain C Library, with strict-alignment word loops.

void *memcpy(void *s1, const void *s2, size_t n)
{
    unsigned char *dest = (unsigned char *)s1;
    const unsigned char *src = (const unsigned char *)s2;

    if (n >= 16 && (((uintptr_t)dest ^ (uintptr_t)src) & 7) == 0) {
        while ((uintptr_t)dest & 7) {
            *dest++ = *src++;
            n--;
        }
        uint64_t *d64 = (uint64_t *)dest;
        const uint64_t *s64 = (const uint64_t *)src;
        while (n >= 32) {
            uint64_t v0 = s64[0], v1 = s64[1], v2 = s64[2], v3 = s64[3];
            d64[0] = v0;
            d64[1] = v1;
            d64[2] = v2;
            d64[3] = v3;
            d64 += 4;
            s64 += 4;
            n -= 32;
        }
        while (n >= 8) {
            *d64++ = *s64++;
            n -= 8;
        }
        dest = (unsigned char *)d64;
        src = (const unsigned char *)s64;
    } else if (n >= 8 && (((uintptr_t)dest ^ (uintptr_t)src) & 3) == 0) {
        while ((uintptr_t)dest & 3) {
            *dest++ = *src++;
            n--;
        }
        uint32_t *d32 = (uint32_t *)dest;
        const uint32_t *s32 = (const uint32_t *)src;
        while (n >= 16) {
            uint32_t v0 = s32[0], v1 = s32[1], v2 = s32[2], v3 = s32[3];
            d32[0] = v0;
            d32[1] = v1;
            d32[2] = v2;
            d32[3] = v3;
            d32 += 4;
            s32 += 4;
            n -= 16;
        }
        while (n >= 4) {
            *d32++ = *s32++;
            n -= 4;
        }
        dest = (unsigned char *)d32;
        src = (const unsigned char *)s32;
    }

    while (n--) {
        *dest++ = *src++;
    }

    return s1;
}

void *memmove(void *s1, const void *s2, size_t n)
{
    unsigned char *dest = (unsigned char *)s1;
    const unsigned char *src = (const unsigned char *)s2;

    if (dest == src || n == 0)
        return s1;

    if (dest < src) {
        if (n >= 16 && (((uintptr_t)dest ^ (uintptr_t)src) & 7) == 0) {
            while ((uintptr_t)dest & 7) {
                *dest++ = *src++;
                n--;
            }
            uint64_t *d64 = (uint64_t *)dest;
            const uint64_t *s64 = (const uint64_t *)src;
            while (n >= 32) {
                uint64_t v0 = s64[0], v1 = s64[1], v2 = s64[2], v3 = s64[3];
                d64[0] = v0;
                d64[1] = v1;
                d64[2] = v2;
                d64[3] = v3;
                d64 += 4;
                s64 += 4;
                n -= 32;
            }
            while (n >= 8) {
                *d64++ = *s64++;
                n -= 8;
            }
            dest = (unsigned char *)d64;
            src = (const unsigned char *)s64;
        } else if (n >= 8 && (((uintptr_t)dest ^ (uintptr_t)src) & 3) == 0) {
            while ((uintptr_t)dest & 3) {
                *dest++ = *src++;
                n--;
            }
            uint32_t *d32 = (uint32_t *)dest;
            const uint32_t *s32 = (const uint32_t *)src;
            while (n >= 16) {
                uint32_t v0 = s32[0], v1 = s32[1], v2 = s32[2], v3 = s32[3];
                d32[0] = v0;
                d32[1] = v1;
                d32[2] = v2;
                d32[3] = v3;
                d32 += 4;
                s32 += 4;
                n -= 16;
            }
            while (n >= 4) {
                *d32++ = *s32++;
                n -= 4;
            }
            dest = (unsigned char *)d32;
            src = (const unsigned char *)s32;
        }
        while (n--) {
            *dest++ = *src++;
        }
    } else {
        src += n;
        dest += n;

        if (n >= 16 && (((uintptr_t)dest ^ (uintptr_t)src) & 7) == 0) {
            while ((uintptr_t)dest & 7) {
                *--dest = *--src;
                n--;
            }
            uint64_t *d64 = (uint64_t *)dest;
            const uint64_t *s64 = (const uint64_t *)src;
            while (n >= 32) {
                s64 -= 4;
                d64 -= 4;
                uint64_t v3 = s64[3], v2 = s64[2], v1 = s64[1], v0 = s64[0];
                d64[3] = v3;
                d64[2] = v2;
                d64[1] = v1;
                d64[0] = v0;
                n -= 32;
            }
            while (n >= 8) {
                *--d64 = *--s64;
                n -= 8;
            }
            dest = (unsigned char *)d64;
            src = (const unsigned char *)s64;
        } else if (n >= 8 && (((uintptr_t)dest ^ (uintptr_t)src) & 3) == 0) {
            while ((uintptr_t)dest & 3) {
                *--dest = *--src;
                n--;
            }
            uint32_t *d32 = (uint32_t *)dest;
            const uint32_t *s32 = (const uint32_t *)src;
            while (n >= 16) {
                s32 -= 4;
                d32 -= 4;
                uint32_t v3 = s32[3], v2 = s32[2], v1 = s32[1], v0 = s32[0];
                d32[3] = v3;
                d32[2] = v2;
                d32[1] = v1;
                d32[0] = v0;
                n -= 16;
            }
            while (n >= 4) {
                *--d32 = *--s32;
                n -= 4;
            }
            dest = (unsigned char *)d32;
            src = (const unsigned char *)s32;
        }
        while (n--) {
            *--dest = *--src;
        }
    }

    return s1;
}

int memcmp(const void *s1, const void *s2, size_t n)
{
    const unsigned char *p1 = (const unsigned char *)s1;
    const unsigned char *p2 = (const unsigned char *)s2;

    if (n >= 16 && (((uintptr_t)p1 ^ (uintptr_t)p2) & 7) == 0) {
        while ((uintptr_t)p1 & 7) {
            if (*p1 != *p2)
                return *p1 - *p2;
            p1++;
            p2++;
            n--;
        }
        const uint64_t *w1 = (const uint64_t *)p1;
        const uint64_t *w2 = (const uint64_t *)p2;
        while (n >= 8) {
            if (*w1 != *w2)
                break;
            w1++;
            w2++;
            n -= 8;
        }
        p1 = (const unsigned char *)w1;
        p2 = (const unsigned char *)w2;
    }

    while (n--) {
        if (*p1 != *p2) {
            return *p1 - *p2;
        }

        ++p1;
        ++p2;
    }

    return 0;
}

void *memset(void *s, int c, size_t n)
{
    unsigned char *p = (unsigned char *)s;
    unsigned char byte = (unsigned char)c;

    if (n >= 16) {
        while ((uintptr_t)p & 7) {
            *p++ = byte;
            n--;
        }
        uint64_t w = byte;
        w |= w << 8;
        w |= w << 16;
        w |= w << 32;
        uint64_t *p64 = (uint64_t *)p;
        while (n >= 32) {
            p64[0] = w;
            p64[1] = w;
            p64[2] = w;
            p64[3] = w;
            p64 += 4;
            n -= 32;
        }
        while (n >= 8) {
            *p64++ = w;
            n -= 8;
        }
        p = (unsigned char *)p64;
    }

    while (n--) {
        *p++ = byte;
    }

    return s;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = (const unsigned char *)s;

    while (n--) {
        if (*p == (unsigned char)c) {
            return (void *)p;
        }

        ++p;
    }

    return NULL;
}

char *strcpy(char *s1, const char *s2)
{
    char *rc = s1;

    while ((*s1++ = *s2++)) {
        /* EMPTY */
    }

    return rc;
}

char *strncpy(char *s1, const char *s2, size_t n)
{
    char *rc = s1;

    while (n && (*s1++ = *s2++)) {
        /* Cannot do "n--" in the conditional as size_t is unsigned and we have
           to check it again for >0 in the next loop below, so we must not risk
           underflow.
        */
        --n;
    }

    /* Checking against 1 as we missed the last --n in the loop above. */
    while (n-- > 1) {
        *s1++ = '\0';
    }

    return rc;
}

int strcmp(const char *s1, const char *s2)
{
    while ((*s1) && (*s1 == *s2)) {
        ++s1;
        ++s2;
    }

    return (*(unsigned char *)s1 - *(unsigned char *)s2);
}

int strncmp(const char *s1, const char *s2, size_t n)
{
    while (n && *s1 && (*s1 == *s2)) {
        ++s1;
        ++s2;
        --n;
    }

    if (n == 0) {
        return 0;
    } else {
        return (*(unsigned char *)s1 - *(unsigned char *)s2);
    }
}

size_t strlen(const char *s)
{
    size_t rc = 0;

    while (s[rc]) {
        ++rc;
    }

    return rc;
}

size_t strnlen(const char *s, size_t n)
{
    size_t rc = 0;

    while (rc < n && s[rc]) {
        ++rc;
    }

    return rc;
}

char *strchr(const char *s, int c)
{
    do {
        if (*s == (char)c) {
            return (char *)s;
        }
    } while (*s++);

    return NULL;
}

char *strrchr(const char *s, int c)
{
    size_t i = 0;

    while (s[i++]) {
        /* EMPTY */
    }

    do {
        if (s[--i] == (char)c) {
            return (char *)s + i;
        }
    } while (i);

    return NULL;
}

char *strstr(const char *s1, const char *s2)
{
    const char *p1 = s1;
    const char *p2;

    while (*s1) {
        p2 = s2;

        while (*p2 && (*p1 == *p2)) {
            ++p1;
            ++p2;
        }

        if (!*p2) {
            return (char *)s1;
        }

        ++s1;
        p1 = s1;
    }

    return NULL;
}

/* Very naive, no attempt to check for errors */
long atol(const char *s)
{
    long val = 0;
    bool neg = false;

    if (*s == '-') {
        neg = true;
        s++;
    }

    while (*s >= '0' && *s <= '9')
        val = (val * 10) + (*s++ - '0');

    if (neg)
        val = -val;

    return val;
}
