/*
 * tix/store.h -- the storage port: op-pack commits and refs.
 *
 * Adapters: store_libgit (a real git repository via libgit.a), store_mem (tests).
 *
 * The port speaks in op-pack commits, not git trees: an adapter stores a commit
 * whose tree holds exactly one blob, `ops.ndjson` (architecture §3.2). That keeps
 * the on-disk layout inside the adapter and the interface small.
 *
 * Interface convention: implementations embed `tix_store` as their first member.
 * Call through the tix_store_* wrappers, which check preconditions.
 * Threading: none. A store is used from one thread.
 */
#ifndef TIX_STORE_H
#define TIX_STORE_H

#include "tix/base.h"
#include "tix/base_buf.h"
#include "tix/base_oid.h"

/* A stored op-pack commit, as read back. OWNS all members; free with tix_commit_free. */
typedef struct tix_commit {
	tix_oid oid;
	tix_oid *parents;
	size_t nparents;
	tix_buf pack;  /* ops.ndjson bytes */
	char *message; /* commit message (informational) */
} tix_commit;

void tix_commit_free(tix_commit *c);

typedef struct tix_ref {
	char *name;
	tix_oid oid;
} tix_ref;

/* Sorted by name (strcmp). OWNS; free with tix_refs_free. */
typedef struct tix_refs {
	tix_ref *v;
	size_t n;
} tix_refs;

void tix_refs_free(tix_refs *r);

typedef struct tix_store tix_store;

typedef struct tix_store_vt {
	tix_status (*commit_write)(tix_store *s, const tix_oid *parents, size_t nparents,
				   const char *pack, size_t pack_len, const char *message,
				   tix_oid *out, tix_error *err);
	tix_status (*commit_read)(tix_store *s, const tix_oid *oid, tix_commit *out, tix_error *err);
	tix_status (*ref_read)(tix_store *s, const char *name, tix_oid *out, tix_error *err);
	tix_status (*ref_list)(tix_store *s, const char *prefix, tix_refs *out, tix_error *err);
	tix_status (*ref_cas)(tix_store *s, const char *name, const tix_oid *old_oid,
			      const tix_oid *new_oid, tix_error *err);
	tix_status (*local_get)(tix_store *s, const char *name, tix_buf *out, tix_error *err);
	tix_status (*local_set)(tix_store *s, const char *name, const char *data, size_t len,
				tix_error *err);
	tix_status (*remote_fetch)(tix_store *s, const char *remote, tix_error *err);
	tix_status (*remote_push)(tix_store *s, const char *remote, const char *const *refs, size_t n,
				  tix_error *err);
	void (*destroy)(tix_store *s);
} tix_store_vt;

struct tix_store {
	const tix_store_vt *vt;
	tix_hash_algo algo; /* every oid this store returns uses this algorithm */
};

/* Largest ops.ndjson blob an adapter will read (bigger ones are TIX_ERR_INVALID).
 * Generous on purpose: model applies the real limit. */
#define TIX_STORE_MAX_BLOB (4u << 20)

/* RETURNS true for local-state names: 1-32 chars of [a-z0-9-]. */
bool tix_local_name_valid(const char *name);

/* RETURNS true for remote names tix accepts: 1-64 chars of [A-Za-z0-9._-], not
 * starting with '-' or '.', no "..", not ending in ".lock". Never a URL or path. */
bool tix_remote_name_valid(const char *name);

/* RETURNS true for "refs/..." names made of [A-Za-z0-9._-] components, no "..",
 * no component starting with '.', none ending in ".lock", total length <= 255. */
bool tix_refname_valid(const char *name);

/*
 * Writes a commit with the given parents whose tree holds `pack` as ops.ndjson.
 * REQUIRES: every parent was returned by this store; message is a NUL-terminated string.
 * ENSURES on TIX_OK: *out is the new commit's oid (content-addressed: identical
 * input gives an identical oid).
 */
static inline tix_status tix_store_commit_write(tix_store *s, const tix_oid *parents, size_t nparents,
						const char *pack, size_t pack_len, const char *message,
						tix_oid *out, tix_error *err)
{
	TIX_REQUIRE(s && (parents || !nparents) && (pack || !pack_len) && message && out);
	return s->vt->commit_write(s, parents, nparents, pack, pack_len, message, out, err);
}

/* RETURNS TIX_ERR_NOT_FOUND if absent; TIX_ERR_INVALID if it is not an op-pack commit.
 * ENSURES on TIX_OK: *out is filled and owned by the caller. */
static inline tix_status tix_store_commit_read(tix_store *s, const tix_oid *oid, tix_commit *out,
					       tix_error *err)
{
	TIX_REQUIRE(s && oid && out);
	return s->vt->commit_read(s, oid, out, err);
}

/* RETURNS TIX_ERR_NOT_FOUND if the ref doesn't exist, TIX_ERR_INVALID for a bad name. */
static inline tix_status tix_store_ref_read(tix_store *s, const char *name, tix_oid *out,
					    tix_error *err)
{
	TIX_REQUIRE(s && name && out);
	if (!tix_refname_valid(name))
		return tix_error_set(err, TIX_ERR_INVALID, "bad ref name '%s'", name);
	return s->vt->ref_read(s, name, out, err);
}

/* Lists refs whose name starts with prefix (byte prefix). ENSURES: sorted by name. */
static inline tix_status tix_store_ref_list(tix_store *s, const char *prefix, tix_refs *out,
					    tix_error *err)
{
	TIX_REQUIRE(s && prefix && out);
	return s->vt->ref_list(s, prefix, out, err);
}

/*
 * Compare-and-swap: sets `name` to *new_oid only if it currently equals *old_oid,
 * or doesn't exist when old_oid is NULL.
 * RETURNS TIX_ERR_CONFLICT (old value mismatch), TIX_ERR_EXISTS (old_oid NULL but
 * ref exists), TIX_ERR_NOT_FOUND (new_oid is not a commit in this store).
 * Refs are never deleted through this port.
 */
static inline tix_status tix_store_ref_cas(tix_store *s, const char *name, const tix_oid *old_oid,
					   const tix_oid *new_oid, tix_error *err)
{
	TIX_REQUIRE(s && name && new_oid && !tix_oid_is_null(new_oid));
	if (!tix_refname_valid(name))
		return tix_error_set(err, TIX_ERR_INVALID, "bad ref name '%s'", name);
	return s->vt->ref_cas(s, name, old_oid, new_oid, err);
}

/*
 * Per-clone local state (never synced): small named values such as the Lamport
 * clock. store_libgit keeps them in $GIT_DIR/tix/<name>, written atomically.
 * local_get RETURNS TIX_ERR_NOT_FOUND if unset; out receives the exact bytes.
 */
static inline tix_status tix_store_local_get(tix_store *s, const char *name, tix_buf *out, tix_error *err)
{
	TIX_REQUIRE(s && name && out && tix_local_name_valid(name));
	return s->vt->local_get(s, name, out, err);
}

static inline tix_status tix_store_local_set(tix_store *s, const char *name, const char *data, size_t len,
					     tix_error *err)
{
	TIX_REQUIRE(s && name && (data || !len) && tix_local_name_valid(name));
	return s->vt->local_set(s, name, data, len, err);
}

/*
 * Network (architecture §6). Both talk to a *configured* remote by name.
 *
 * remote_fetch mirrors the remote's refs/tix/(all) into refs/remotes/<remote>/tix/(all)
 * (forced: that namespace is a mirror, never written by tix otherwise; refs gone
 * from the remote are pruned). It never touches refs/tix/(all). Fetched data is
 * untrusted: the caller validates before adopting anything.
 * RETURNS TIX_ERR_INVALID (bad name), TIX_ERR_NOT_FOUND (no such remote),
 * TIX_ERR_IO (transport failure; err carries the transport's message).
 */
static inline tix_status tix_store_remote_fetch(tix_store *s, const char *remote, tix_error *err)
{
	TIX_REQUIRE(s && remote);
	if (!tix_remote_name_valid(remote))
		return tix_error_set(err, TIX_ERR_INVALID, "bad remote name '%s'", remote);
	return s->vt->remote_fetch(s, remote, err);
}

/*
 * Pushes each local ref in refs[] to the same name on the remote, never forced.
 * Not atomic: refs that fast-forward are updated even if others are rejected.
 * REQUIRES: every refs[i] is a valid ref name that exists locally.
 * RETURNS TIX_OK when every ref was updated or already up to date;
 * TIX_ERR_CONFLICT when at least one was rejected as not a fast-forward (the
 * caller should pull and retry); TIX_ERR_INVALID / NOT_FOUND / IO as for fetch.
 * n == 0 is a no-op.
 */
static inline tix_status tix_store_remote_push(tix_store *s, const char *remote, const char *const *refs,
					       size_t n, tix_error *err)
{
	TIX_REQUIRE(s && remote && (refs || !n));
	if (!tix_remote_name_valid(remote))
		return tix_error_set(err, TIX_ERR_INVALID, "bad remote name '%s'", remote);
	for (size_t i = 0; i < n; i++)
		TIX_REQUIRE(tix_refname_valid(refs[i]));
	return n ? s->vt->remote_push(s, remote, refs, n, err) : TIX_OK;
}

static inline void tix_store_destroy(tix_store *s)
{
	if (s)
		s->vt->destroy(s);
}

#endif
