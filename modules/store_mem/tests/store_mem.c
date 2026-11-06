/* store_mem: the shared store contract, plus its simulation helpers. */
#include "tix/store_contract.h"
#include "tix/store_mem.h"
#include "tix/testkit.h"

static void test_content_addressed(void)
{
	tix_error e = TIX_ERROR_INIT;
	tix_store *s = tix_store_mem_new(TIX_HASH_SHA1);
	tix_oid a, a2;
	TK_OK(tix_store_commit_write(s, NULL, 0, "p1\n", 3, "m1", &a, &e), &e);
	TK_OK(tix_store_commit_write(s, NULL, 0, "p1\n", 3, "m1", &a2, &e), &e);
	TK_CHECK(tix_oid_eq(&a, &a2));
	TK_CHECK(tix_store_mem_count(s) == 1);
	tix_store_destroy(s);
}

static void test_fetch(void)
{
	tix_error e = TIX_ERROR_INIT;
	tix_store *a = tix_store_mem_new(TIX_HASH_SHA1), *b = tix_store_mem_new(TIX_HASH_SHA1);
	tix_oid c1, got;
	tix_commit c;

	TK_OK(tix_store_commit_write(a, NULL, 0, "x", 1, "x", &c1, &e), &e);
	TK_OK(tix_store_ref_cas(a, "refs/tix/tickets/t", NULL, &c1, &e), &e);
	tix_store_mem_fetch(b, a, "origin");
	TK_OK(tix_store_ref_read(b, "refs/remotes/origin/tix/tickets/t", &got, &e), &e);
	TK_CHECK(tix_oid_eq(&got, &c1));
	TK_OK(tix_store_commit_read(b, &c1, &c, &e), &e);
	tix_commit_free(&c);
	TK_STATUS(tix_store_ref_read(b, "refs/tix/tickets/t", &got, &e), TIX_ERR_NOT_FOUND, &e);
	tix_store_destroy(a);
	tix_store_destroy(b);
}

int main(void)
{
	tix_store *s = tix_store_mem_new(TIX_HASH_SHA1);
	tix_store_contract_run(s);
	tix_store_destroy(s);
	s = tix_store_mem_new(TIX_HASH_SHA256);
	tix_store_contract_run(s);
	tix_store_destroy(s);
	test_content_addressed();
	test_fetch();
	return tk_finish();
}
