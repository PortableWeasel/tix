#include "tix/index_contract.h"
#include "tix/testkit.h"

#include <stdlib.h>

static tix_oid oid(int n, tix_hash_algo algo)
{
	tix_oid o;
	memset(&o, 0, sizeof(o));
	o.algo = (uint8_t)algo;
	o.bytes[0] = (uint8_t)n;
	o.bytes[tix_oid_rawlen(algo) - 1] = 0xa5;
	return o;
}

void tix_index_contract_run(tix_index *x)
{
	tix_error e = TIX_ERROR_INIT;
	tix_buf b = TIX_BUF_INIT;
	size_t n;
	tix_oid a = oid(1, TIX_HASH_SHA1), h1 = oid(2, TIX_HASH_SHA1), h2 = oid(3, TIX_HASH_SHA1);
	tix_oid a256 = oid(1, TIX_HASH_SHA256), h256 = oid(2, TIX_HASH_SHA256);

	TK_OK(tix_index_count(x, &n, &e), &e);
	TK_CHECK(n == 0);
	TK_STATUS(tix_index_get(x, TIX_ENTITY_TICKET, &a, &h1, &b, &e), TIX_ERR_NOT_FOUND, &e);
	TK_OK(tix_index_put(x, TIX_ENTITY_TICKET, &a, &h1, "one", 3, &e), &e);
	TK_OK(tix_index_get(x, TIX_ENTITY_TICKET, &a, &h1, &b, &e), &e);
	TK_CHECK(b.len == 3 && !memcmp(b.data, "one", 3));
	/* the head is part of the key: a moved ref misses */
	TK_STATUS(tix_index_get(x, TIX_ENTITY_TICKET, &a, &h2, &b, &e), TIX_ERR_NOT_FOUND, &e);
	/* types are separate namespaces; algorithms don't collide */
	TK_STATUS(tix_index_get(x, TIX_ENTITY_IDENTITY, &a, &h1, &b, &e), TIX_ERR_NOT_FOUND, &e);
	TK_STATUS(tix_index_get(x, TIX_ENTITY_TICKET, &a256, &h256, &b, &e), TIX_ERR_NOT_FOUND, &e);
	/* put replaces */
	TK_OK(tix_index_put(x, TIX_ENTITY_TICKET, &a, &h2, "two\0bytes", 9, &e), &e);
	TK_STATUS(tix_index_get(x, TIX_ENTITY_TICKET, &a, &h1, &b, &e), TIX_ERR_NOT_FOUND, &e);
	TK_OK(tix_index_get(x, TIX_ENTITY_TICKET, &a, &h2, &b, &e), &e);
	TK_CHECK(b.len == 9 && !memcmp(b.data, "two\0bytes", 9)); /* binary-safe */
	TK_OK(tix_index_put(x, TIX_ENTITY_IDENTITY, &a, &h1, "", 0, &e), &e);
	TK_OK(tix_index_get(x, TIX_ENTITY_IDENTITY, &a, &h1, &b, &e), &e);
	TK_CHECK(b.len == 0);
	TK_OK(tix_index_put(x, TIX_ENTITY_TICKET, &a256, &h256, "sha256", 6, &e), &e);
	TK_OK(tix_index_count(x, &n, &e), &e);
	TK_CHECK(n == 3);
	/* a big entry round trips */
	size_t big = 3u << 20;
	char *blob = tix_xmalloc(big);
	for (size_t i = 0; i < big; i++)
		blob[i] = (char)(i * 7);
	TK_OK(tix_index_put(x, TIX_ENTITY_CALENDAR, &a, &h1, blob, big, &e), &e);
	TK_OK(tix_index_get(x, TIX_ENTITY_CALENDAR, &a, &h1, &b, &e), &e);
	TK_CHECK(b.len == big && !memcmp(b.data, blob, big));
	free(blob);
	TK_OK(tix_index_clear(x, &e), &e);
	TK_OK(tix_index_count(x, &n, &e), &e);
	TK_CHECK(n == 0);
	TK_STATUS(tix_index_get(x, TIX_ENTITY_TICKET, &a, &h2, &b, &e), TIX_ERR_NOT_FOUND, &e);
	tix_buf_free(&b);
}
