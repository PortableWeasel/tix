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

static const tix_index_row *row_for(const tix_index_rows *rows, const tix_oid *id)
{
	for (size_t i = 0; i < rows->n; i++)
		if (tix_oid_eq(&rows->v[i].id, id))
			return &rows->v[i];
	return NULL;
}

/* scan: per type, every entry once, with its current head and exact summary bytes. */
static void scan(tix_index *x, tix_error *e)
{
	tix_oid t1 = oid(10, TIX_HASH_SHA1), t2 = oid(11, TIX_HASH_SHA1), t3 = oid(12, TIX_HASH_SHA256);
	tix_oid h1 = oid(20, TIX_HASH_SHA1), h2 = oid(21, TIX_HASH_SHA1), h3 = oid(22, TIX_HASH_SHA256);
	tix_index_rows rows;
	TK_OK(tix_index_scan(x, TIX_ENTITY_RESOURCE, &rows, e), e);
	TK_CHECK(rows.n == 0);
	tix_index_rows_free(&rows);
	TK_OK(tix_index_put(x, TIX_ENTITY_RESOURCE, &t1, &h1, "d1", 2, "s\0one", 5, e), e);
	TK_OK(tix_index_put(x, TIX_ENTITY_RESOURCE, &t2, &h2, "d2", 2, NULL, 0, e), e);
	TK_OK(tix_index_put(x, TIX_ENTITY_RESOURCE, &t3, &h3, "d3", 2, "sha256", 6, e), e);
	TK_OK(tix_index_put(x, TIX_ENTITY_RESOURCE, &t1, &h2, "d1b", 3, "moved", 5, e), e); /* replaces */
	TK_OK(tix_index_scan(x, TIX_ENTITY_RESOURCE, &rows, e), e);
	TK_CHECK(rows.n == 3);
	const tix_index_row *r1 = row_for(&rows, &t1), *r2 = row_for(&rows, &t2), *r3 = row_for(&rows, &t3);
	TK_CHECK(r1 && tix_oid_eq(&r1->head, &h2) && r1->summary.len == 5 && !memcmp(r1->summary.data, "moved", 5));
	TK_CHECK(r2 && tix_oid_eq(&r2->head, &h2) && r2->summary.len == 0);
	TK_CHECK(r3 && tix_oid_eq(&r3->head, &h3) && r3->summary.len == 6 && r3->id.algo == TIX_HASH_SHA256);
	tix_index_rows_free(&rows);
	/* other types are not in it */
	TK_OK(tix_index_scan(x, TIX_ENTITY_CALENDAR, &rows, e), e);
	for (size_t i = 0; i < rows.n; i++)
		TK_CHECK(!tix_oid_eq(&rows.v[i].id, &t3));
	tix_index_rows_free(&rows);
	/* binary summary round trip */
	TK_OK(tix_index_put(x, TIX_ENTITY_RESOURCE, &t2, &h2, "d2", 2, "s\0one", 5, e), e);
	TK_OK(tix_index_scan(x, TIX_ENTITY_RESOURCE, &rows, e), e);
	r2 = row_for(&rows, &t2);
	TK_CHECK(r2 && r2->summary.len == 5 && !memcmp(r2->summary.data, "s\0one", 5));
	tix_index_rows_free(&rows);
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
	TK_OK(tix_index_put(x, TIX_ENTITY_TICKET, &a, &h1, "one", 3, NULL, 0, &e), &e);
	TK_OK(tix_index_get(x, TIX_ENTITY_TICKET, &a, &h1, &b, &e), &e);
	TK_CHECK(b.len == 3 && !memcmp(b.data, "one", 3));
	/* the head is part of the key: a moved ref misses */
	TK_STATUS(tix_index_get(x, TIX_ENTITY_TICKET, &a, &h2, &b, &e), TIX_ERR_NOT_FOUND, &e);
	/* types are separate namespaces; algorithms don't collide */
	TK_STATUS(tix_index_get(x, TIX_ENTITY_IDENTITY, &a, &h1, &b, &e), TIX_ERR_NOT_FOUND, &e);
	TK_STATUS(tix_index_get(x, TIX_ENTITY_TICKET, &a256, &h256, &b, &e), TIX_ERR_NOT_FOUND, &e);
	/* put replaces */
	TK_OK(tix_index_put(x, TIX_ENTITY_TICKET, &a, &h2, "two\0bytes", 9, NULL, 0, &e), &e);
	TK_STATUS(tix_index_get(x, TIX_ENTITY_TICKET, &a, &h1, &b, &e), TIX_ERR_NOT_FOUND, &e);
	TK_OK(tix_index_get(x, TIX_ENTITY_TICKET, &a, &h2, &b, &e), &e);
	TK_CHECK(b.len == 9 && !memcmp(b.data, "two\0bytes", 9)); /* binary-safe */
	TK_OK(tix_index_put(x, TIX_ENTITY_IDENTITY, &a, &h1, "", 0, NULL, 0, &e), &e);
	TK_OK(tix_index_get(x, TIX_ENTITY_IDENTITY, &a, &h1, &b, &e), &e);
	TK_CHECK(b.len == 0);
	TK_OK(tix_index_put(x, TIX_ENTITY_TICKET, &a256, &h256, "sha256", 6, NULL, 0, &e), &e);
	TK_OK(tix_index_count(x, &n, &e), &e);
	TK_CHECK(n == 3);
	/* a big entry round trips */
	size_t big = 3u << 20;
	char *blob = tix_xmalloc(big);
	for (size_t i = 0; i < big; i++)
		blob[i] = (char)(i * 7);
	TK_OK(tix_index_put(x, TIX_ENTITY_CALENDAR, &a, &h1, blob, big, NULL, 0, &e), &e);
	TK_OK(tix_index_get(x, TIX_ENTITY_CALENDAR, &a, &h1, &b, &e), &e);
	TK_CHECK(b.len == big && !memcmp(b.data, blob, big));
	free(blob);
	scan(x, &e);
	TK_OK(tix_index_clear(x, &e), &e);
	TK_OK(tix_index_count(x, &n, &e), &e);
	TK_CHECK(n == 0);
	tix_index_rows rows;
	TK_OK(tix_index_scan(x, TIX_ENTITY_TICKET, &rows, &e), &e);
	TK_CHECK(rows.n == 0);
	tix_index_rows_free(&rows);
	TK_STATUS(tix_index_get(x, TIX_ENTITY_TICKET, &a, &h2, &b, &e), TIX_ERR_NOT_FOUND, &e);
	tix_buf_free(&b);
}
