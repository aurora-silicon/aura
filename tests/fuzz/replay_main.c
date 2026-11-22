/* SPDX-License-Identifier: MIT */
/*
 * Run a fuzz target over files without libFuzzer, for compilers that have none (gcc) and for
 * the regression inputs in tests/fuzz/regress/ (tests/python/test_fuzz_regress.py).
 *
 *   <target> <file>...
 */
#include <stdio.h>
#include <stdlib.h>

#include "types.h"

int LLVMFuzzerTestOneInput(const u8 *data, size_t size);

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        long n;
        u8 *buf;

        if (!f || fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) {
            perror(argv[i]);
            return 2;
        }
        buf = malloc(n ? n : 1);
        if (!buf || fread(buf, 1, n, f) != (size_t)n) {
            perror(argv[i]);
            return 2;
        }
        fclose(f);
        LLVMFuzzerTestOneInput(buf, n);
        free(buf);
        printf("ok %s\n", argv[i]);
    }
    return 0;
}
