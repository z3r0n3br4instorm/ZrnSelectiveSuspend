// SPDX-License-Identifier: GPL-2.0
/*
 * Measures what retention costs an application at upload time.
 * Usage: bench_retain [megabytes] [blob-megabytes]   (uses $XDG_CACHE_HOME)
 * Run it twice against the same cache to see a first and a second launch.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "zss_hash.h"
#include "zss_retain.h"

static double now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    size_t total_mb = argc > 1 ? (size_t)atoi(argv[1]) : 256, blob_mb = argc > 2 ? (size_t)atoi(argv[2]) : 4;
    size_t blob = blob_mb << 20, count = total_mb / blob_mb, kept = 0;
    uint8_t *data = malloc(blob), hash[16];
    struct zss_ret ret;
    unsigned seed = 1;
    double t0, t_hash, t_put, t_flush;

    for (size_t i = 0; i < blob; i++)
        data[i] = (uint8_t)((seed = seed * 1103515245u + 12345u) >> 16);

    t0 = now();
    for (size_t i = 0; i < count; i++) {
        data[0] = (uint8_t)i;
        zss_hash128(data, blob, hash);
    }
    t_hash = now() - t0;

    /* What the submitting thread pays: hash, look up, and copy if the blob is new. */
    t0 = now();
    for (size_t i = 0; i < count; i++) {
        data[0] = (uint8_t)i;
        data[1] = (uint8_t)(i >> 8);
        kept += zss_retain_put(data, blob, &ret);
    }
    t_put = now() - t0;
    t0 = now();
    zss_retain_flush(120000);
    t_flush = now() - t0;

    printf("hash:   %zu MB in %.3f s  (%.0f MB/s)\n", count * blob_mb, t_hash, (double)(count * blob_mb) / t_hash);
    printf("upload: %zu MB in %.3f s on the submitting thread (%.0f MB/s), %zu of %zu blobs retained\n",
           count * blob_mb, t_put, (double)(count * blob_mb) / t_put, kept, count);
    printf("writer: %.3f s more until everything was on disk\n", t_flush);
    free(data);
    return 0;
}
