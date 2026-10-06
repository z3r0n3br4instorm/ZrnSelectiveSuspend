/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef ZSS_HASH_H
#define ZSS_HASH_H

#include <stddef.h>
#include <stdint.h>

/*
 * MurmurHash3 x64-128 (Austin Appleby, public domain), seed 0. Used as a
 * content key for the retention store, not for anything security-related.
 */
void zss_hash128(const void *data, size_t len, uint8_t out[16]);

#endif
