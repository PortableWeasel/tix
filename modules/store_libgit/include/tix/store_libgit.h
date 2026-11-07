/*
 * tix/store_libgit.h -- tix_store on a real git repository, via git.git's libgit.a.
 *
 * Layout written (architecture §3.2): each op-pack commit's tree holds exactly
 * one regular file, `ops.ndjson`. Commits read back with any other tree shape
 * are TIX_ERR_INVALID. Author/committer come from git config (informational;
 * a missing user.name falls back to git's non-strict defaults instead of dying).
 *
 * Process-wide: libgit keeps global state (the_repository), so open at most one
 * store per process. Not thread-safe. libgit may die() (exit) on corrupt object
 * databases or I/O failures. That is accepted in v1 (CLI frontend only).
 */
#ifndef TIX_STORE_LIBGIT_H
#define TIX_STORE_LIBGIT_H

#include "tix/store.h"

/*
 * Discovers the repository from the current directory (git's rules; GIT_DIR is
 * honoured) and loads its config.
 * RETURNS TIX_ERR_NOT_FOUND outside a repository.
 * REQUIRES: not called before in this process.
 * ENSURES on TIX_OK: *out owned by caller (tix_store_destroy).
 */
tix_status tix_store_libgit_open(tix_store **out, tix_error *err);

/* git config lookup (all scopes). RETURNS TIX_ERR_NOT_FOUND if unset. */
tix_status tix_store_libgit_config_get(tix_store *s, const char *key, tix_buf *out, tix_error *err);
/* Sets key in the repository-local config file ($GIT_DIR/config). */
tix_status tix_store_libgit_config_set(tix_store *s, const char *key, const char *value, tix_error *err);

/* Absolute path of $GIT_DIR/tix/<name> (creating $GIT_DIR/tix), for per-clone files
 * such as the index cache. REQUIRES name is a local name or that plus ".db". */
tix_status tix_store_libgit_local_path(tix_store *s, const char *name, tix_buf *out, tix_error *err);

/*
 * The configured git signing key (user.signingkey under gpg.format ssh or openpgp) as an
 * identity key: kind "ssh"/"gpg", its fingerprint, and the public key (ssh: "type base64";
 * gpg: armored export). For `tix id edit --add-signing-key` (architecture §14.3).
 * RETURNS TIX_ERR_NOT_FOUND without a usable key.
 */
tix_status tix_store_libgit_signing_key(tix_store *s, tix_buf *kind, tix_buf *fingerprint, tix_buf *pubkey,
				       tix_error *err);

/*
 * A password or token from git's credential helpers (`git credential fill` as a child
 * process, never prompting): for issue-tracker bridges (architecture §14.4).
 * RETURNS TIX_ERR_NOT_FOUND when no helper has one. The caller wipes *password after use.
 */
tix_status tix_store_libgit_credential(tix_store *s, const char *protocol, const char *host, const char *path,
				      tix_buf *password, tix_error *err);

/* The version of the linked git.git library, e.g. "2.47.3". Static storage. */
const char *tix_store_libgit_git_version(void);

#endif
