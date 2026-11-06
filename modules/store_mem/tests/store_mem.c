/* store_mem against the tix_store contract (these cases also apply to store_libgit). */
#include "tix/store_mem.h"
#include "tix/testkit.h"

static void test_commits(tix_hash_algo algo)
{
	tix_error e = TIX_ERROR_INIT;
	tix_store *s = tix_store_mem_new(algo);
	tix_oid a, b, a2, bogus;
	tix_commit c;

	TK_OK(tix_store_commit_write(s, NULL, 0, "p1\n", 3, "m1", &a, &e), &e);
	TK_CHECK(a.algo == algo);
	TK_OK(tix_store_commit_write(s, NULL, 0, "p1\n", 3, "m1", &a2, &e), &e);
	TK_CHECK(tix_oid_eq(&a, &a2)); /* content-addressed */
	TK_CHECK(tix_store_mem_count(s) == 1);
	TK_OK(tix_store_commit_write(s, &a, 1, "p2\n", 3, "m2", &b, &e), &e);
	TK_CHECK(!tix_oid_eq(&a, &b));

	TK_OK(tix_store_commit_read(s, &b, &c, &e), &e);
	TK_CHECK(c.nparents == 1 && tix_oid_eq(&c.parents[0], &a));
	TK_STR_EQ(tix_buf_cstr(&c.pack), "p2\n");
	TK_STR_EQ(c.message, "m2");
	tix_commit_free(&c);

	bogus = a;
	bogus.bytes[0] ^= 0xff;
	TK_STATUS(tix_store_commit_read(s, &bogus, &c, &e), TIX_ERR_NOT_FOUND, &e);
	TK_STATUS(tix_store_commit_write(s, &bogus, 1, "x", 1, "m", &a2, &e), TIX_ERR_NOT_FOUND, &e);
	tix_store_destroy(s);
}

static void test_refs(void)
{
	tix_error e = TIX_ERROR_INIT;
	tix_store *s = tix_store_mem_new(TIX_HASH_SHA1);
	tix_oid a, b, got;
	tix_refs refs;

	TK_OK(tix_store_commit_write(s, NULL, 0, "a", 1, "a", &a, &e), &e);
	TK_OK(tix_store_commit_write(s, &a, 1, "b", 1, "b", &b, &e), &e);

	TK_STATUS(tix_store_ref_read(s, "refs/tix/tickets/x", &got, &e), TIX_ERR_NOT_FOUND, &e);
	TK_OK(tix_store_ref_cas(s, "refs/tix/tickets/x", NULL, &a, &e), &e);
	TK_STATUS(tix_store_ref_cas(s, "refs/tix/tickets/x", NULL, &b, &e), TIX_ERR_EXISTS, &e);
	TK_STATUS(tix_store_ref_cas(s, "refs/tix/tickets/x", &b, &b, &e), TIX_ERR_CONFLICT, &e);
	TK_OK(tix_store_ref_cas(s, "refs/tix/tickets/x", &a, &b, &e), &e);
	TK_OK(tix_store_ref_read(s, "refs/tix/tickets/x", &got, &e), &e);
	TK_CHECK(tix_oid_eq(&got, &b));

	TK_OK(tix_store_ref_cas(s, "refs/tix/tickets/a", NULL, &a, &e), &e);
	TK_OK(tix_store_ref_cas(s, "refs/tix/identities/i", NULL, &a, &e), &e);
	TK_OK(tix_store_ref_list(s, "refs/tix/tickets/", &refs, &e), &e);
	TK_CHECK(refs.n == 2);
	if (refs.n == 2) {
		TK_STR_EQ(refs.v[0].name, "refs/tix/tickets/a");
		TK_STR_EQ(refs.v[1].name, "refs/tix/tickets/x");
	}
	tix_refs_free(&refs);

	TK_STATUS(tix_store_ref_cas(s, "refs/tix/../x", NULL, &a, &e), TIX_ERR_INVALID, &e);
	TK_STATUS(tix_store_ref_read(s, "tix/x", &got, &e), TIX_ERR_INVALID, &e);
	tix_store_destroy(s);
}

static void test_refname(void)
{
	TK_CHECK(tix_refname_valid("refs/tix/tickets/0123abc"));
	TK_CHECK(tix_refname_valid("refs/remotes/origin/tix/tickets/x"));
	TK_CHECK(!tix_refname_valid("refs/"));
	TK_CHECK(!tix_refname_valid("refs/tix//x"));
	TK_CHECK(!tix_refname_valid("refs/tix/.hidden"));
	TK_CHECK(!tix_refname_valid("refs/tix/x.lock"));
	TK_CHECK(!tix_refname_valid("refs/tix/a..b"));
	TK_CHECK(!tix_refname_valid("refs/tix/sp ace"));
	TK_CHECK(!tix_refname_valid("refs/tix/x/"));
	TK_CHECK(!tix_refname_valid("heads/main"));
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
	test_commits(TIX_HASH_SHA1);
	test_commits(TIX_HASH_SHA256);
	test_refs();
	test_refname();
	test_fetch();
	return tk_finish();
}
