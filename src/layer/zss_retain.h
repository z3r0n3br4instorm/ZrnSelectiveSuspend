/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Retention store: keeps a copy of data uploaded to the GPU so it can be
 * put back after the device is lost. Content-addressed, on disk by default,
 * shared between runs. Deliberately independent of the rest of the layer.
 */
#ifndef ZSS_RETAIN_H
#define ZSS_RETAIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A reference to retained data. Zero-initialised means "nothing retained". */
struct zss_ret {
    uint8_t hash[16];
    uint64_t size;
    bool valid;
};

/*
 * Retains `size` bytes. Returns false, leaving *out invalid, when retention
 * is off or too much is already waiting to be written; the caller then
 * treats the data as unrecoverable. Never blocks on the disk.
 */
bool zss_retain_put(const void *data, size_t size, struct zss_ret *out);

/* Copies retained data into dst (out->size bytes). False if it is not available. */
bool zss_retain_get(const struct zss_ret *ret, void *dst);

/* Drops one reference and invalidates *ret. */
void zss_retain_release(struct zss_ret *ret);

/* Waits until everything queued has reached the disk, up to timeout_ms. */
void zss_retain_flush(int timeout_ms);

/* Enforces the size limit now. Normally run by the writer as the store grows. */
void zss_retain_trim(void);

#endif
