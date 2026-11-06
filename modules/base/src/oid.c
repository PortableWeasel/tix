#include "tix/base_oid.h"

#include <string.h>

size_t tix_oid_rawlen(tix_hash_algo algo)
{
	switch (algo) {
	case TIX_HASH_SHA1: return 20;
	case TIX_HASH_SHA256: return 32;
	case TIX_HASH_NONE: break;
	}
	return 0;
}

bool tix_oid_is_null(const tix_oid *o)
{
	return o->algo == TIX_HASH_NONE;
}

int tix_oid_cmp(const tix_oid *a, const tix_oid *b)
{
	if (a->algo != b->algo)
		return a->algo < b->algo ? -1 : 1;
	return memcmp(a->bytes, b->bytes, tix_oid_rawlen(a->algo));
}

bool tix_oid_eq(const tix_oid *a, const tix_oid *b)
{
	return tix_oid_cmp(a, b) == 0;
}

char *tix_oid_hex(const tix_oid *o, char *out)
{
	static const char digits[] = "0123456789abcdef";
	size_t n = tix_oid_rawlen(o->algo);
	for (size_t i = 0; i < n; i++) {
		out[2 * i] = digits[o->bytes[i] >> 4];
		out[2 * i + 1] = digits[o->bytes[i] & 15];
	}
	out[2 * n] = '\0';
	return out;
}

static int nibble(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

bool tix_oid_from_hex(tix_oid *out, const char *hex, size_t len)
{
	tix_oid o;
	memset(&o, 0, sizeof(o));
	if (len == 40)
		o.algo = TIX_HASH_SHA1;
	else if (len == 64)
		o.algo = TIX_HASH_SHA256;
	else
		return false;
	for (size_t i = 0; i < len / 2; i++) {
		int hi = nibble(hex[2 * i]), lo = nibble(hex[2 * i + 1]);
		if (hi < 0 || lo < 0)
			return false;
		o.bytes[i] = (uint8_t)(hi << 4 | lo);
	}
	*out = o;
	return true;
}
