/*
 * Signed op-pack commits in a real repository prepared by run-signing.sh (cwd), with
 * git config tix.sign=true and user.signingkey set (ssh or openpgp). argv[1] = an unsigned
 * commit, argv[2] = kind and argv[3] = fingerprint and argv[4] = public key of a key that
 * did NOT sign anything. Second mode: argv[1] = "tampered", argv[2] = a commit whose signature
 * no longer matches its content: it must verify BAD with the signing key.
 */
#include "tix/store_libgit.h"
#include "tix/testkit.h"

#include <string.h>

int main(int argc, char **argv)
{
	TIX_REQUIRE(argc == 5 || (argc == 3 && !strcmp(argv[1], "tampered")));
	tix_error e = TIX_ERROR_INIT;
	tix_store *s = NULL;
	TK_OK(tix_store_libgit_open(&s, &e), &e);
	if (!s)
		return tk_finish();
	tix_buf kind = TIX_BUF_INIT, fpr = TIX_BUF_INIT, pub = TIX_BUF_INIT;
	TK_OK(tix_store_libgit_signing_key(s, &kind, &fpr, &pub, &e), &e);
	if (argc == 3) {
		tix_oid t;
		tix_sig_key mine = {tix_buf_cstr(&kind), tix_buf_cstr(&fpr), tix_buf_cstr(&pub)};
		tix_sig_status st;
		size_t which;
		TK_CHECK(tix_oid_from_hex(&t, argv[2], strlen(argv[2])));
		TK_OK(tix_store_commit_verify(s, &t, &mine, 1, &st, &which, &e), &e);
		TK_CHECK(st == TIX_SIG_BAD);
		tix_buf_free(&kind);
		tix_buf_free(&fpr);
		tix_buf_free(&pub);
		tix_store_destroy(s);
		return tk_finish();
	}
	TK_CHECK(!strcmp(tix_buf_cstr(&kind), argv[2])); /* the configured format */
	tix_oid signed_c, unsigned_c;
	TK_OK(tix_store_commit_write(s, NULL, 0, "signed\n", 7, "a signed pack", &signed_c, &e), &e);
	TK_CHECK(tix_oid_from_hex(&unsigned_c, argv[1], strlen(argv[1])));

	tix_sig_key mine = {tix_buf_cstr(&kind), tix_buf_cstr(&fpr), tix_buf_cstr(&pub)};
	tix_sig_key other = {argv[2], argv[3], argv[4]};
	tix_sig_key both[] = {other, mine};
	tix_sig_status st;
	size_t which;
	TK_OK(tix_store_commit_verify(s, &signed_c, &mine, 1, &st, &which, &e), &e);
	TK_CHECK(st == TIX_SIG_GOOD && which == 0);
	TK_OK(tix_store_commit_verify(s, &signed_c, both, 2, &st, &which, &e), &e);
	TK_CHECK(st == TIX_SIG_GOOD && which == 1); /* the key that matches, not the first */
	TK_OK(tix_store_commit_verify(s, &signed_c, &other, 1, &st, &which, &e), &e);
	TK_CHECK(st == TIX_SIG_BAD && which == SIZE_MAX); /* someone else's key */
	TK_OK(tix_store_commit_verify(s, &signed_c, NULL, 0, &st, &which, &e), &e);
	TK_CHECK(st == TIX_SIG_BAD); /* signed, but no keys to check against */
	tix_sig_key wrong_kind = {strcmp(argv[2], "ssh") ? "ssh" : "gpg", tix_buf_cstr(&fpr), tix_buf_cstr(&pub)};
	TK_OK(tix_store_commit_verify(s, &signed_c, &wrong_kind, 1, &st, &which, &e), &e);
	TK_CHECK(st == TIX_SIG_BAD);
	TK_OK(tix_store_commit_verify(s, &unsigned_c, &mine, 1, &st, &which, &e), &e);
	TK_CHECK(st == TIX_SIG_NONE);
	/* run-signing.sh tampers with this commit and checks it in the second mode */
	char hex[TIX_OID_MAX_HEX + 1];
	printf("%s\n", tix_oid_hex(&signed_c, hex));
	tix_buf_free(&kind);
	tix_buf_free(&fpr);
	tix_buf_free(&pub);
	tix_store_destroy(s);
	return tk_finish();
}
