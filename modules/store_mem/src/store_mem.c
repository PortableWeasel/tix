#include "tix/store_mem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct mem_commit {
	tix_oid oid;
	tix_oid *parents;
	size_t nparents;
	char *pack;
	size_t pack_len;
	char *message;
	char *sig; /* simulated signature: the signer's fingerprint, "" for a forged one; NULL unsigned */
} mem_commit;

typedef struct mem_store {
	tix_store base; /* must be first */
	TIX_VEC(mem_commit) commits; /* sorted by oid */
	TIX_VEC(tix_ref) refs;       /* sorted by name */
	TIX_VEC(tix_ref) local;      /* name -> value (oid unused; value in a parallel vec) */
	TIX_VEC(tix_buf) local_val;
	TIX_VEC(tix_ref) remotes;    /* name -> peer (oid unused; peer in a parallel vec) */
	TIX_VEC(tix_store *) peers;  /* BORROWED */
	char *sign_as;               /* tix_store_mem_sign_as */
} mem_store;

static const tix_store_vt mem_vt;

static mem_store *as_mem(tix_store *s)
{
	TIX_REQUIRE(s && s->vt == &mem_vt);
	return (mem_store *)s;
}

/* 4 independent FNV-1a lanes; deterministic, not cryptographic. */
static void hash_bytes(uint64_t lane[4], const void *data, size_t n)
{
	const unsigned char *p = data;
	for (size_t i = 0; i < n; i++)
		for (int l = 0; l < 4; l++)
			lane[l] = (lane[l] ^ (uint64_t)(p[i] + l)) * 0x100000001b3ull;
}

static void compute_oid(tix_hash_algo algo, const tix_oid *parents, size_t np, const char *pack,
			size_t len, const char *msg, const char *sig, tix_oid *out)
{
	uint64_t lane[4] = {0xcbf29ce484222325ull, 0x84222325cbf29ce4ull, 0x9ce484222325cbf2ull,
			    0x2325cbf29ce48422ull};
	size_t raw = tix_oid_rawlen(algo);
	hash_bytes(lane, &np, sizeof(np));
	for (size_t i = 0; i < np; i++)
		hash_bytes(lane, parents[i].bytes, raw);
	hash_bytes(lane, &len, sizeof(len));
	hash_bytes(lane, pack, len);
	hash_bytes(lane, msg, strlen(msg));
	if (sig) /* part of the commit, as a git signature is */
		hash_bytes(lane, sig, strlen(sig) + 1);
	memset(out, 0, sizeof(*out));
	out->algo = (uint8_t)algo;
	for (size_t i = 0; i < raw; i++)
		out->bytes[i] = (uint8_t)(lane[i % 4] >> (8 * (i / 4)));
}

static mem_commit *find_commit(mem_store *m, const tix_oid *oid, size_t *at)
{
	size_t lo = 0, hi = m->commits.n;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		int c = tix_oid_cmp(&m->commits.v[mid].oid, oid);
		if (!c)
			return &m->commits.v[mid];
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (at)
		*at = lo;
	return NULL;
}

static void insert_commit(mem_store *m, const tix_oid *oid, const tix_oid *parents, size_t np,
			  const char *pack, size_t len, const char *msg, const char *sig)
{
	size_t at = 0;
	if (find_commit(m, oid, &at))
		return;
	TIX_VEC_GROW(&m->commits);
	memmove(m->commits.v + at + 1, m->commits.v + at, (m->commits.n - at) * sizeof(mem_commit));
	m->commits.n++;
	mem_commit *c = &m->commits.v[at];
	c->oid = *oid;
	c->nparents = np;
	c->parents = np ? tix_xcalloc(np, sizeof(tix_oid)) : NULL;
	if (np)
		memcpy(c->parents, parents, np * sizeof(tix_oid));
	c->pack = tix_xstrndup(pack ? pack : "", len);
	c->pack_len = len;
	c->message = tix_xstrdup(msg);
	c->sig = sig ? tix_xstrdup(sig) : NULL;
}

static tix_status mem_commit_write(tix_store *s, const tix_oid *parents, size_t np, const char *pack,
				   size_t len, const char *msg, tix_oid *out, tix_error *err)
{
	mem_store *m = as_mem(s);
	for (size_t i = 0; i < np; i++) {
		TIX_REQUIRE(parents[i].algo == s->algo);
		if (!find_commit(m, &parents[i], NULL))
			return tix_error_set(err, TIX_ERR_NOT_FOUND, "parent commit not in store");
	}
	compute_oid(s->algo, parents, np, pack ? pack : "", len, msg, m->sign_as, out);
	insert_commit(m, out, parents, np, pack, len, msg, m->sign_as);
	return TIX_OK;
}

static tix_status mem_commit_verify(tix_store *s, const tix_oid *oid, const tix_sig_key *keys, size_t nkeys,
				    tix_sig_status *out, size_t *which, tix_error *err)
{
	mem_commit *c = find_commit(as_mem(s), oid, NULL);
	if (!c)
		return tix_error_set(err, TIX_ERR_NOT_FOUND, "no such commit");
	if (!c->sig)
		return TIX_OK; /* NONE */
	*out = TIX_SIG_BAD;
	for (size_t i = 0; i < nkeys && *c->sig; i++)
		if (!strcmp(keys[i].fingerprint, c->sig)) {
			*out = TIX_SIG_GOOD;
			*which = i;
			break;
		}
	return TIX_OK;
}

void tix_store_mem_sign_as(tix_store *s, const char *fingerprint)
{
	mem_store *m = as_mem(s);
	free(m->sign_as);
	m->sign_as = fingerprint ? tix_xstrdup(fingerprint) : NULL;
}

static tix_status mem_commit_read(tix_store *s, const tix_oid *oid, tix_commit *out, tix_error *err)
{
	mem_commit *c = find_commit(as_mem(s), oid, NULL);
	if (!c)
		return tix_error_set(err, TIX_ERR_NOT_FOUND, "no such commit");
	memset(out, 0, sizeof(*out));
	out->oid = c->oid;
	out->nparents = c->nparents;
	if (c->nparents) {
		out->parents = tix_xcalloc(c->nparents, sizeof(tix_oid));
		memcpy(out->parents, c->parents, c->nparents * sizeof(tix_oid));
	}
	tix_buf_add(&out->pack, c->pack, c->pack_len);
	out->message = tix_xstrdup(c->message);
	return TIX_OK;
}

static tix_ref *find_ref(mem_store *m, const char *name, size_t *at)
{
	size_t lo = 0, hi = m->refs.n;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		int c = strcmp(m->refs.v[mid].name, name);
		if (!c)
			return &m->refs.v[mid];
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (at)
		*at = lo;
	return NULL;
}

static void set_ref(mem_store *m, const char *name, const tix_oid *oid)
{
	size_t at = 0;
	tix_ref *r = find_ref(m, name, &at);
	if (r) {
		r->oid = *oid;
		return;
	}
	TIX_VEC_GROW(&m->refs);
	memmove(m->refs.v + at + 1, m->refs.v + at, (m->refs.n - at) * sizeof(tix_ref));
	m->refs.n++;
	m->refs.v[at].name = tix_xstrdup(name);
	m->refs.v[at].oid = *oid;
}

static tix_status mem_ref_read(tix_store *s, const char *name, tix_oid *out, tix_error *err)
{
	tix_ref *r = find_ref(as_mem(s), name, NULL);
	if (!r)
		return tix_error_set(err, TIX_ERR_NOT_FOUND, "no ref %s", name);
	*out = r->oid;
	return TIX_OK;
}

static tix_status mem_ref_list(tix_store *s, const char *prefix, tix_refs *out, tix_error *err)
{
	mem_store *m = as_mem(s);
	size_t plen = strlen(prefix);
	out->v = NULL;
	out->n = 0;
	for (size_t i = 0; i < m->refs.n; i++) {
		if (strncmp(m->refs.v[i].name, prefix, plen))
			continue;
		out->v = tix_xreallocarray(out->v, out->n + 1, sizeof(tix_ref));
		out->v[out->n].name = tix_xstrdup(m->refs.v[i].name);
		out->v[out->n].oid = m->refs.v[i].oid;
		out->n++;
	}
	return TIX_OK;
}

static tix_status mem_ref_cas(tix_store *s, const char *name, const tix_oid *old_oid,
			      const tix_oid *new_oid, tix_error *err)
{
	mem_store *m = as_mem(s);
	tix_ref *r = find_ref(m, name, NULL);
	if (!find_commit(m, new_oid, NULL))
		return tix_error_set(err, TIX_ERR_NOT_FOUND, "%s: new value is not a stored commit", name);
	if (!old_oid && r)
		return tix_error_set(err, TIX_ERR_EXISTS, "%s already exists", name);
	if (old_oid && (!r || !tix_oid_eq(&r->oid, old_oid)))
		return tix_error_set(err, TIX_ERR_CONFLICT, "%s changed underneath", name);
	set_ref(m, name, new_oid);
	return TIX_OK;
}

static tix_status mem_local_get(tix_store *s, const char *name, tix_buf *out, tix_error *err)
{
	mem_store *m = as_mem(s);
	for (size_t i = 0; i < m->local.n; i++)
		if (!strcmp(m->local.v[i].name, name)) {
			tix_buf_reset(out);
			tix_buf_add(out, m->local_val.v[i].data, m->local_val.v[i].len);
			return TIX_OK;
		}
	return tix_error_set(err, TIX_ERR_NOT_FOUND, "local state '%s' unset", name);
}

static tix_status mem_local_set(tix_store *s, const char *name, const char *data, size_t len, tix_error *err)
{
	mem_store *m = as_mem(s);
	for (size_t i = 0; i < m->local.n; i++)
		if (!strcmp(m->local.v[i].name, name)) {
			tix_buf_reset(&m->local_val.v[i]);
			tix_buf_add(&m->local_val.v[i], data, len);
			return TIX_OK;
		}
	tix_ref r = {tix_xstrdup(name), {0}};
	tix_buf b = TIX_BUF_INIT;
	tix_buf_add(&b, data, len);
	TIX_VEC_PUSH(&m->local, r);
	TIX_VEC_PUSH(&m->local_val, b);
	return TIX_OK;
}

static tix_store *find_peer(mem_store *m, const char *remote)
{
	for (size_t i = 0; i < m->remotes.n; i++)
		if (!strcmp(m->remotes.v[i].name, remote))
			return m->peers.v[i];
	return NULL;
}

static tix_status mem_remote_fetch(tix_store *s, const char *remote, tix_error *err)
{
	tix_store *peer = find_peer(as_mem(s), remote);
	if (!peer)
		return tix_error_set(err, TIX_ERR_NOT_FOUND, "no remote named '%s'", remote);
	tix_store_mem_fetch(s, peer, remote);
	return TIX_OK;
}

/* RETURNS true if `a` is `b` or one of its ancestors. */
static bool mem_is_ancestor(mem_store *m, const tix_oid *a, const tix_oid *b)
{
	TIX_VEC(tix_oid) todo = {0};
	TIX_VEC(tix_oid) seen = {0};
	bool found = false;
	TIX_VEC_PUSH(&todo, *b);
	while (todo.n && !found) {
		tix_oid o = todo.v[--todo.n];
		bool dup = false;
		for (size_t i = 0; i < seen.n && !dup; i++)
			dup = tix_oid_eq(&seen.v[i], &o);
		if (dup)
			continue;
		TIX_VEC_PUSH(&seen, o);
		if (tix_oid_eq(&o, a))
			found = true;
		mem_commit *c = find_commit(m, &o, NULL);
		for (size_t k = 0; c && k < c->nparents; k++)
			TIX_VEC_PUSH(&todo, c->parents[k]);
	}
	free(todo.v);
	free(seen.v);
	return found;
}

static void copy_commits(mem_store *dst, const mem_store *src)
{
	for (size_t i = 0; i < src->commits.n; i++) {
		const mem_commit *c = &src->commits.v[i];
		insert_commit(dst, &c->oid, c->parents, c->nparents, c->pack, c->pack_len, c->message, c->sig);
	}
}

static tix_status mem_remote_push(tix_store *s, const char *remote, const char *const *refs, size_t n,
				  tix_error *err)
{
	mem_store *m = as_mem(s);
	tix_store *peer_s = find_peer(m, remote);
	if (!peer_s)
		return tix_error_set(err, TIX_ERR_NOT_FOUND, "no remote named '%s'", remote);
	mem_store *peer = as_mem(peer_s);
	size_t rejected = 0;
	copy_commits(peer, m); /* like a pack upload: objects first, then refs */
	for (size_t i = 0; i < n; i++) {
		tix_ref *mine = find_ref(m, refs[i], NULL);
		TIX_REQUIRE(mine);
		tix_ref *theirs = find_ref(peer, refs[i], NULL);
		if (!theirs || mem_is_ancestor(peer, &theirs->oid, &mine->oid))
			set_ref(peer, refs[i], &mine->oid);
		else
			rejected++;
	}
	if (rejected)
		return tix_error_set(err, TIX_ERR_CONFLICT, "%zu ref(s) rejected (non-fast-forward)", rejected);
	return TIX_OK;
}

static void mem_destroy(tix_store *s)
{
	mem_store *m = as_mem(s);
	for (size_t i = 0; i < m->remotes.n; i++)
		free(m->remotes.v[i].name);
	free(m->remotes.v);
	free(m->peers.v);
	free(m->sign_as);
	for (size_t i = 0; i < m->local.n; i++) {
		free(m->local.v[i].name);
		tix_buf_free(&m->local_val.v[i]);
	}
	free(m->local.v);
	free(m->local_val.v);
	for (size_t i = 0; i < m->commits.n; i++) {
		free(m->commits.v[i].parents);
		free(m->commits.v[i].pack);
		free(m->commits.v[i].message);
		free(m->commits.v[i].sig);
	}
	free(m->commits.v);
	for (size_t i = 0; i < m->refs.n; i++)
		free(m->refs.v[i].name);
	free(m->refs.v);
	free(m);
}

static void mem_refresh(tix_store *s)
{
	(void)s; /* nothing cached: every call reads the live state */
}

static const tix_store_vt mem_vt = {
	.commit_write = mem_commit_write,
	.commit_read = mem_commit_read,
	.ref_read = mem_ref_read,
	.ref_list = mem_ref_list,
	.ref_cas = mem_ref_cas,
	.local_get = mem_local_get,
	.local_set = mem_local_set,
	.remote_fetch = mem_remote_fetch,
	.remote_push = mem_remote_push,
	.refresh = mem_refresh,
	.commit_verify = mem_commit_verify,
	.destroy = mem_destroy,
};

tix_store *tix_store_mem_new(tix_hash_algo algo)
{
	TIX_REQUIRE(algo == TIX_HASH_SHA1 || algo == TIX_HASH_SHA256);
	mem_store *m = tix_xcalloc(1, sizeof(*m));
	m->base.vt = &mem_vt;
	m->base.algo = algo;
	return &m->base;
}

void tix_store_mem_fetch(tix_store *dst, tix_store *src, const char *remote)
{
	mem_store *d = as_mem(dst), *s = as_mem(src);
	TIX_REQUIRE(dst->algo == src->algo && remote && !strchr(remote, '/'));
	for (size_t i = 0; i < s->commits.n; i++) {
		mem_commit *c = &s->commits.v[i];
		insert_commit(d, &c->oid, c->parents, c->nparents, c->pack, c->pack_len, c->message, c->sig);
	}
	char pfx[128];
	tix_remote_mirror_prefix(remote, pfx, sizeof(pfx));
	tix_buf prefix = TIX_BUF_INIT;
	tix_buf_adds(&prefix, pfx);
	size_t keep = 0;
	for (size_t i = 0; i < d->refs.n; i++) { /* prune: the namespace is a mirror */
		if (!strncmp(d->refs.v[i].name, prefix.data, prefix.len))
			free(d->refs.v[i].name);
		else
			d->refs.v[keep++] = d->refs.v[i];
	}
	d->refs.n = keep;
	tix_buf_free(&prefix);
	for (size_t i = 0; i < s->refs.n; i++) {
		const char *name = s->refs.v[i].name;
		if (strncmp(name, "refs/tix/", 9))
			continue;
		tix_buf rn = TIX_BUF_INIT;
		tix_buf_addf(&rn, "%s%s", pfx, name + 9);
		TIX_REQUIRE(tix_refname_valid(rn.data));
		set_ref(d, rn.data, &s->refs.v[i].oid);
		tix_buf_free(&rn);
	}
}

void tix_store_mem_add_remote(tix_store *s, const char *name, tix_store *peer)
{
	mem_store *m = as_mem(s);
	TIX_REQUIRE(tix_remote_name_valid(name) && peer != s && as_mem(peer)->base.algo == s->algo &&
		    !find_peer(m, name));
	tix_ref r = {tix_xstrdup(name), {0}};
	TIX_VEC_PUSH(&m->remotes, r);
	TIX_VEC_PUSH(&m->peers, peer);
}

size_t tix_store_mem_count(tix_store *s)
{
	return as_mem(s)->commits.n;
}
