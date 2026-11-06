/*
 * store_libgit against the shared store contract, in a throwaway repository
 * prepared by run-contract.sh (cwd). argv[1] = oid of an ordinary (non-tix)
 * commit, argv[2] = oid of a blob.
 */
#include "tix/store_contract.h"
#include "tix/store_libgit.h"
#include "tix/testkit.h"

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
