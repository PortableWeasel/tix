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

#endif
