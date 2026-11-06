/*
 * tix/base_oid.h -- object ids, independent of git's own types.
 *
 * An oid is the hash of a stored commit (git: SHA-1 or SHA-256). Entity ids are
 * root-commit oids. Comparison is bytewise and defines the tie-break order.
 */
#ifndef TIX_BASE_OID_H
#define TIX_BASE_OID_H

#include "tix/base.h"

typedef enum tix_hash_algo {
	TIX_HASH_NONE = 0,
	TIX_HASH_SHA1 = 1,   /* 20 bytes, 40 hex */
	TIX_HASH_SHA256 = 2, /* 32 bytes, 64 hex */
} tix_hash_algo;

#define TIX_OID_MAX_RAW 32
#define TIX_OID_MAX_HEX 64

typedef struct tix_oid {
	uint8_t algo; /* tix_hash_algo */
	uint8_t bytes[TIX_OID_MAX_RAW];
} tix_oid;

/* RETURNS raw length for algo (0 for NONE). */
size_t tix_oid_rawlen(tix_hash_algo algo);
bool tix_oid_is_null(const tix_oid *o); /* algo NONE */
/* Total order: by algo, then bytes. */
int tix_oid_cmp(const tix_oid *a, const tix_oid *b);
bool tix_oid_eq(const tix_oid *a, const tix_oid *b);

/* REQUIRES: out has room for TIX_OID_MAX_HEX + 1. Lowercase hex. RETURNS out. */
char *tix_oid_hex(const tix_oid *o, char *out);
/* Parses exactly 40 (SHA-1) or 64 (SHA-256) lowercase hex chars of `hex[0..len)`.
 * RETURNS false on any other input (uppercase is rejected: ids are canonical). */
bool tix_oid_from_hex(tix_oid *out, const char *hex, size_t len);

#endif
