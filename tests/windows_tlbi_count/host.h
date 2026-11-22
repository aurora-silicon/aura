/*
 * Modeled boundary for the guest TLBI trap count replay. run.py appends the
 * production macros, the per-CPU record, the counter and the exit report
 * verbatim; only the CPU number (one per host thread, as TPIDR_EL2 is one per
 * core) and the console are modeled here.
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned long u64;
typedef long s64;
typedef uint32_t u32;

static _Thread_local int model_cpu;
static inline int smp_id(void)
{
    return model_cpu;
}

/* aura's printf formats into a 512-byte buffer; keep every call's text. */
static char console[1 << 16];
static size_t console_len;
static unsigned printf_calls;
static pthread_mutex_t console_lock = PTHREAD_MUTEX_INITIALIZER;

static int model_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int model_printf(const char *fmt, ...)
{
    char line[512];
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&console_lock);
    printf_calls++;
    if (n > 0 && (size_t)n < sizeof(line) && console_len + n < sizeof(console)) {
        memcpy(console + console_len, line, n);
        console_len += n;
        console[console_len] = 0;
    }
    pthread_mutex_unlock(&console_lock);
    return n;
}
#define printf model_printf
