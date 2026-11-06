#include "tix/store_contract.h"
#include "tix/testkit.h"

#include <stdio.h>

static tix_error e = TIX_ERROR_INIT;

static void commits(tix_store *s)
{
	tix_oid a, b, a2, bogus;
	tix_commit c;
	const char *pack = "{\"x\":1}\n";

	TK_OK(tix_store_commit_write(s, NULL, 0, pack, strlen(pack), "contract root", &a, &e), &e);
	TK_CHECK(a.algo == s->algo);
	TK_OK(tix_store_commit_write(s, &a, 1, "p2\n", 3, "contract child", &b, &e), &e);
	TK_CHECK(!tix_oid_eq(&a, &b));

	TK_OK(tix_store_commit_read(s, &a, &c, &e), &e);
	TK_CHECK(c.nparents == 0 && tix_oid_eq(&c.oid, &a));
	TK_STR_EQ(tix_buf_cstr(&c.pack), pack);
	TK_CHECK(c.message && !strncmp(c.message, "contract root", 13));
	tix_commit_free(&c);

	TK_OK(tix_store_commit_read(s, &b, &c, &e), &e);
	TK_CHECK(c.nparents == 1 && tix_oid_eq(&c.parents[0], &a));
	TK_STR_EQ(tix_buf_cstr(&c.pack), "p2\n");
	tix_commit_free(&c);

	/* merge commit keeps parent order */
	tix_oid m, parents[2];
	TK_OK(tix_store_commit_write(s, &a, 1, "p3\n", 3, "contract side", &a2, &e), &e);
	parents[0] = b;
	parents[1] = a2;
	TK_OK(tix_store_commit_write(s, parents, 2, "", 0, "contract merge", &m, &e), &e);
	TK_OK(tix_store_commit_read(s, &m, &c, &e), &e);
	TK_CHECK(c.nparents == 2 && tix_oid_eq(&c.parents[0], &b) && tix_oid_eq(&c.parents[1], &a2));
	TK_CHECK(c.pack.len == 0);
	tix_commit_free(&c);

	bogus = a;
	bogus.bytes[0] ^= 0xff;
	bogus.bytes[1] ^= 0xff;
	TK_STATUS(tix_store_commit_read(s, &bogus, &c, &e), TIX_ERR_NOT_FOUND, &e);
	TK_STATUS(tix_store_commit_write(s, &bogus, 1, "x", 1, "m", &a2, &e), TIX_ERR_NOT_FOUND, &e);
}

static void refs(tix_store *s)
{
	tix_oid a, b, got;
	tix_refs r;
	const char *x = "refs/tix/contract/x";

	TK_OK(tix_store_commit_write(s, NULL, 0, "a\n", 2, "contract a", &a, &e), &e);
	TK_OK(tix_store_commit_write(s, &a, 1, "b\n", 2, "contract b", &b, &e), &e);

	TK_STATUS(tix_store_ref_read(s, x, &got, &e), TIX_ERR_NOT_FOUND, &e);
	TK_OK(tix_store_ref_cas(s, x, NULL, &a, &e), &e);
	TK_STATUS(tix_store_ref_cas(s, x, NULL, &b, &e), TIX_ERR_EXISTS, &e);
	TK_STATUS(tix_store_ref_cas(s, x, &b, &b, &e), TIX_ERR_CONFLICT, &e);
	TK_OK(tix_store_ref_read(s, x, &got, &e), &e);
	TK_CHECK(tix_oid_eq(&got, &a)); /* refused CAS left it alone */
	TK_OK(tix_store_ref_cas(s, x, &a, &b, &e), &e);
	TK_OK(tix_store_ref_read(s, x, &got, &e), &e);
	TK_CHECK(tix_oid_eq(&got, &b));

	tix_oid bogus = a;
	bogus.bytes[0] ^= 0xff;
	bogus.bytes[1] ^= 0xff;
	TK_STATUS(tix_store_ref_cas(s, "refs/tix/contract/y", NULL, &bogus, &e), TIX_ERR_NOT_FOUND, &e);
	TK_STATUS(tix_store_ref_read(s, "refs/tix/contract/y", &got, &e), TIX_ERR_NOT_FOUND, &e);

	TK_OK(tix_store_ref_cas(s, "refs/tix/contract/a", NULL, &a, &e), &e);
	TK_OK(tix_store_ref_cas(s, "refs/tix/contractx", NULL, &a, &e), &e); /* byte prefix */
	TK_OK(tix_store_ref_list(s, "refs/tix/contract/", &r, &e), &e);
	TK_CHECK(r.n == 2);
	if (r.n == 2) {
		TK_STR_EQ(r.v[0].name, "refs/tix/contract/a");
		TK_STR_EQ(r.v[1].name, x);
		TK_CHECK(tix_oid_eq(&r.v[1].oid, &b));
	}
	tix_refs_free(&r);
	TK_OK(tix_store_ref_list(s, "refs/tix/nothing-here/", &r, &e), &e);
	TK_CHECK(r.n == 0);
	tix_refs_free(&r);

	TK_STATUS(tix_store_ref_cas(s, "refs/tix/../x", NULL, &a, &e), TIX_ERR_INVALID, &e);
	TK_STATUS(tix_store_ref_read(s, "tix/x", &got, &e), TIX_ERR_INVALID, &e);
}

static void local(tix_store *s)
{
	tix_buf b = TIX_BUF_INIT;
	TK_STATUS(tix_store_local_get(s, "contract-unset", &b, &e), TIX_ERR_NOT_FOUND, &e);
	TK_OK(tix_store_local_set(s, "contract-v", "42\n", 3, &e), &e);
	TK_OK(tix_store_local_get(s, "contract-v", &b, &e), &e);
	TK_STR_EQ(tix_buf_cstr(&b), "42\n");
	TK_OK(tix_store_local_set(s, "contract-v", "7", 1, &e), &e);
	TK_OK(tix_store_local_get(s, "contract-v", &b, &e), &e);
	TK_STR_EQ(tix_buf_cstr(&b), "7");
	TK_CHECK(!tix_local_name_valid("../x") && !tix_local_name_valid("") && !tix_local_name_valid("Clock"));
	tix_buf_free(&b);
}

void tix_store_contract_run(tix_store *s)
{
	TK_CHECK(tix_refname_valid("refs/tix/tickets/0123abc"));
	TK_CHECK(!tix_refname_valid("refs/tix/x.lock"));
	commits(s);
	refs(s);
	local(s);
}

static bool tracking(tix_store *s, const char *remote, tix_oid *out)
{
	char name[160];
	snprintf(name, sizeof(name), "refs/remotes/%s/tix/contract-sync/x", remote);
	return tix_store_ref_read(s, name, out, NULL) == TIX_OK;
}

void tix_store_contract_run_remote(tix_store *s, const char *remote)
{
	const char *x = "refs/tix/contract-sync/x";
	const char *refs[] = {x};
	tix_oid a, b, c, got;
	char prefix[160];
	tix_refs r;

	TK_CHECK(tix_remote_name_valid("origin") && tix_remote_name_valid("my.remote_2"));
	TK_CHECK(!tix_remote_name_valid("-x") && !tix_remote_name_valid(".x") && !tix_remote_name_valid("a/b") &&
		 !tix_remote_name_valid("a..b") && !tix_remote_name_valid("") &&
		 !tix_remote_name_valid("https://h/x") && !tix_remote_name_valid("x.lock"));
	TK_STATUS(tix_store_remote_fetch(s, "--upload-pack=x", &e), TIX_ERR_INVALID, &e);
	TK_STATUS(tix_store_remote_push(s, "-f", refs, 1, &e), TIX_ERR_INVALID, &e);
	TK_STATUS(tix_store_remote_fetch(s, "contract-no-such-remote", &e), TIX_ERR_NOT_FOUND, &e);
	TK_STATUS(tix_store_remote_push(s, "contract-no-such-remote", refs, 1, &e), TIX_ERR_NOT_FOUND, &e);

	/* an empty remote fetches fine and mirrors nothing */
	TK_OK(tix_store_remote_fetch(s, remote, &e), &e);
	snprintf(prefix, sizeof(prefix), "refs/remotes/%s/tix/contract-sync/", remote);
	TK_OK(tix_store_ref_list(s, prefix, &r, &e), &e);
	TK_CHECK(r.n == 0);
	tix_refs_free(&r);
	TK_OK(tix_store_remote_push(s, remote, refs, 0, &e), &e); /* nothing to push */

	/* create, then fast-forward */
	TK_OK(tix_store_commit_write(s, NULL, 0, "sync a\n", 7, "contract sync a", &a, &e), &e);
	TK_OK(tix_store_ref_cas(s, x, NULL, &a, &e), &e);
	TK_OK(tix_store_remote_push(s, remote, refs, 1, &e), &e);
	TK_OK(tix_store_remote_push(s, remote, refs, 1, &e), &e); /* up to date is fine */
	TK_OK(tix_store_remote_fetch(s, remote, &e), &e);
	TK_CHECK(tracking(s, remote, &got) && tix_oid_eq(&got, &a));
	TK_OK(tix_store_commit_write(s, &a, 1, "sync b\n", 7, "contract sync b", &b, &e), &e);
	TK_OK(tix_store_ref_cas(s, x, &a, &b, &e), &e);
	TK_OK(tix_store_remote_push(s, remote, refs, 1, &e), &e);
	TK_OK(tix_store_remote_fetch(s, remote, &e), &e);
	TK_CHECK(tracking(s, remote, &got) && tix_oid_eq(&got, &b));
	TK_OK(tix_store_ref_read(s, x, &got, &e), &e);
	TK_CHECK(tix_oid_eq(&got, &b)); /* fetch never touches refs/tix */

	/* a non-fast-forward is rejected, never forced */
	TK_OK(tix_store_commit_write(s, NULL, 0, "sync c\n", 7, "contract sync c", &c, &e), &e);
	TK_OK(tix_store_ref_cas(s, x, &b, &c, &e), &e);
	TK_STATUS(tix_store_remote_push(s, remote, refs, 1, &e), TIX_ERR_CONFLICT, &e);
	TK_OK(tix_store_remote_fetch(s, remote, &e), &e);
	TK_CHECK(tracking(s, remote, &got) && tix_oid_eq(&got, &b));
	/* going back to an ancestor is not a fast-forward either */
	TK_OK(tix_store_ref_cas(s, x, &c, &a, &e), &e);
	TK_STATUS(tix_store_remote_push(s, remote, refs, 1, &e), TIX_ERR_CONFLICT, &e);
	TK_OK(tix_store_ref_cas(s, x, &a, &b, &e), &e);
}
