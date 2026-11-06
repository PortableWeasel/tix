/*
 * store_libgit against the shared store contract, in a throwaway repository
 * prepared by run-contract.sh (cwd). argv[1] = oid of an ordinary (non-tix)
 * commit, argv[2] = oid of a blob.
 */
#include "tix/store_contract.h"
#include "tix/store_libgit.h"
#include "tix/testkit.h"

#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Runs the real git (argv only), as another process changing the repo would. */
static int run_git(char *const argv[])
{
	pid_t p = fork();
	if (p == 0) {
		execvp("git", argv);
		_exit(127);
	}
	int st;
	if (p < 0 || waitpid(p, &st, 0) < 0)
		return -1;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* Refs changed by another process are seen after tix_store_refresh. */
static void external_changes(tix_store *s)
{
	tix_error e = TIX_ERROR_INIT;
	tix_oid a, b, got;
	tix_refs r;
	char ahex[TIX_OID_MAX_HEX + 1], bhex[TIX_OID_MAX_HEX + 1];
	const char *moved = "refs/tix/contract-ext/moved", *added = "refs/tix/contract-ext/added";
	TK_OK(tix_store_commit_write(s, NULL, 0, "ext a\n", 6, "ext a", &a, &e), &e);
	TK_OK(tix_store_commit_write(s, &a, 1, "ext b\n", 6, "ext b", &b, &e), &e);
	TK_OK(tix_store_ref_cas(s, moved, NULL, &a, &e), &e);
	TK_OK(tix_store_ref_list(s, "refs/tix/contract-ext/", &r, &e), &e); /* warms libgit's caches */
	TK_CHECK(r.n == 1);
	tix_refs_free(&r);
	tix_oid_hex(&a, ahex);
	tix_oid_hex(&b, bhex);
	char *up1[] = {"git", "update-ref", (char *)moved, bhex, ahex, NULL};
	char *up2[] = {"git", "update-ref", (char *)added, ahex, NULL};
	char *gc[] = {"git", "pack-refs", "--all", NULL};
	TK_CHECK(run_git(up1) == 0 && run_git(up2) == 0 && run_git(gc) == 0);
	tix_store_refresh(s);
	TK_OK(tix_store_ref_list(s, "refs/tix/contract-ext/", &r, &e), &e);
	TK_CHECK(r.n == 2);
	if (r.n == 2) {
		TK_STR_EQ(r.v[0].name, added);
		TK_CHECK(tix_oid_eq(&r.v[1].oid, &b));
	}
	tix_refs_free(&r);
	TK_OK(tix_store_ref_read(s, moved, &got, &e), &e);
	TK_CHECK(tix_oid_eq(&got, &b));
	/* and CAS builds on what the other process wrote */
	TK_STATUS(tix_store_ref_cas(s, moved, &a, &a, &e), TIX_ERR_CONFLICT, &e);
	TK_OK(tix_store_ref_cas(s, moved, &b, &a, &e), &e);
}

int main(int argc, char **argv)
{
	tix_error e = TIX_ERROR_INIT;
	tix_store *s = NULL;
	tix_commit c;
	tix_oid foreign, blob;
	tix_buf b = TIX_BUF_INIT;

	TIX_REQUIRE(argc == 3);
	TK_OK(tix_store_libgit_open(&s, &e), &e);
	if (!s)
		return tk_finish();
	tix_store_contract_run(s);
	tix_store_contract_run_remote(s, "peer");
	external_changes(s);

	/* an ordinary commit (tree with other files) is not an op-pack commit */
	TK_CHECK(tix_oid_from_hex(&foreign, argv[1], strlen(argv[1])));
	TK_STATUS(tix_store_commit_read(s, &foreign, &c, &e), TIX_ERR_INVALID, &e);
	/* a blob is not a commit, either as a read or as a ref target */
	TK_CHECK(tix_oid_from_hex(&blob, argv[2], strlen(argv[2])));
	TK_STATUS(tix_store_commit_read(s, &blob, &c, &e), TIX_ERR_INVALID, &e);
	TK_STATUS(tix_store_ref_cas(s, "refs/tix/contract/blob", NULL, &blob, &e), TIX_ERR_NOT_FOUND, &e);

	TK_STATUS(tix_store_libgit_config_get(s, "tix.unset", &b, &e), TIX_ERR_NOT_FOUND, &e);
	TK_OK(tix_store_libgit_config_set(s, "tix.contract", "yes", &e), &e);
	TK_OK(tix_store_libgit_config_get(s, "tix.contract", &b, &e), &e);
	TK_STR_EQ(tix_buf_cstr(&b), "yes");
	tix_buf_free(&b);
	tix_store_destroy(s);
	return tk_finish();
}
