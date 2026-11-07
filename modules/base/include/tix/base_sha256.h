/*
 * tix/base_sha256.h -- SHA-256 and HMAC-SHA256 (FIPS 180-4, RFC 2104), pure.
 * Used to check webhook signatures (architecture §15.6); not for storage, where object
 * ids come from git.
 */
#ifndef TIX_BASE_SHA256_H
#define TIX_BASE_SHA256_H

#include "tix/base.h"

#define TIX_SHA256_LEN 32

void tix_sha256(const void *data, size_t len, uint8_t out[TIX_SHA256_LEN]);
void tix_hmac_sha256(const void *key, size_t key_len, const void *data, size_t len, uint8_t out[TIX_SHA256_LEN]);
/* Compares without an early exit (for secrets). */
bool tix_bytes_eq_ct(const uint8_t *a, const uint8_t *b, size_t n);

#endif
