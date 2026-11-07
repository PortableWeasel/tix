/*
 * Commit signatures (architecture §14.3): which identity a commit speaks for, and whether
 * it is signed by one of that identity's keys. Used on pull (new commits only, per
 * tix_repo_config.verify) and by tix_entity_verify (whole histories).
 */
#include "core_internal.h"

#include <stdlib.h>
#include <string.h>

void core_keys_free(core_keys *k)
{
	for (size_t i = 0; i < k->n; i++)
		tix_entity_free(k->v[i].identity);
	free(k->v);
	memset(k, 0, sizeof(*k));
}

/* The identity entity for id, loaded once per run; NULL if it doesn't load. */
static const tix_entity *identity_of(tix_repo *r, core_keys *cache, const tix_oid *id)
{
	for (size_t i = 0; i < cache->n; i++)
		if (tix_oid_eq(&cache->v[i].id, id))
			return cache->v[i].identity;
	core_key_entry k = {*id, NULL};
	if (tix_entity_load(r, TIX_ENTITY_IDENTITY, id, &k.identity, NULL))
		k.identity = NULL;
	TIX_VEC_PUSH(cache, k);
	return k.identity;
}

tix_status core_verify_commit(tix_repo *r, tix_entity_type t, const tix_oid *entity, const tix_commit *c,
			      const tix_entity *self, core_keys *cache, tix_sig_status *out, tix_error *err)
{
	tix_pack p;
	*out = TIX_SIG_BAD;
	if (tix_pack_decode(c->pack.data ? c->pack.data : "", c->pack.len, r->store->algo, &p, err))
		return TIX_OK; /* not ours to judge here: replay rejects it anyway */
	tix_oid author = p.author;
	tix_pack_free(&p);
	/* identity packs speak for the identity itself (its root has no author field) */
	if (t == TIX_ENTITY_IDENTITY && (tix_oid_is_null(&author) || tix_oid_eq(&author, entity)))
		author = *entity;
	const tix_entity *who = t == TIX_ENTITY_IDENTITY && tix_oid_eq(&author, entity) && self
					? self
					: identity_of(r, cache, &author);
	size_t n = who ? who->u.identity.nkeys : 0;
	tix_sig_key *keys = tix_xcalloc(n ? n : 1, sizeof(*keys));
	for (size_t i = 0; i < n; i++) {
		const tix_key *k = &who->u.identity.keys[i];
		keys[i] = (tix_sig_key){k->kind, k->fingerprint, k->pubkey};
	}
	size_t which;
	tix_status st = tix_store_commit_verify(r->store, &c->oid, keys, n, out, &which, err);
	free(keys);
	return st;
}

tix_status tix_entity_verify(tix_repo *r, tix_entity_type t, const tix_oid *id, tix_verify_report *rep,
			     tix_error *err)
{
	TIX_REQUIRE(r && id && rep);
	char ref[128];
	tix_oid head;
	core_ref_of(t, id, ref, sizeof(ref));
	tix_status st = tix_store_ref_read(r->store, ref, &head, err);
	if (st)
		return st;
	tix_entity *self = NULL;
	if (t == TIX_ENTITY_IDENTITY && (st = tix_entity_load(r, t, id, &self, err)))
		return st;
	core_dag d;
	if ((st = core_dag_load(r, &head, &d, err))) {
		tix_entity_free(self);
		return st;
	}
	core_keys cache = {0};
	for (size_t i = 0; i < d.commits.n && !st; i++) {
		tix_sig_status s;
		if ((st = core_verify_commit(r, t, id, &d.commits.v[i], self, &cache, &s, err)))
			break;
		if (s == TIX_SIG_GOOD)
			rep->good++;
		else if (s == TIX_SIG_NONE)
			rep->unsigned_commits++;
		else
			rep->bad++;
	}
	core_keys_free(&cache);
	core_dag_free(&d);
	tix_entity_free(self);
	return st;
}

tix_verify_policy tix_verify_policy_parse(const char *s, bool *ok)
{
	*ok = true;
	if (!s || !strcmp(s, "off") || !strcmp(s, "false") || !strcmp(s, "no"))
		return TIX_VERIFY_OFF;
	if (!strcmp(s, "warn"))
		return TIX_VERIFY_WARN;
	if (!strcmp(s, "require") || !strcmp(s, "true") || !strcmp(s, "yes"))
		return TIX_VERIFY_REQUIRE;
	*ok = false;
	return TIX_VERIFY_OFF;
}

void tix_repo_set_verify(tix_repo *r, tix_verify_policy v)
{
	TIX_REQUIRE(r && v >= TIX_VERIFY_OFF && v <= TIX_VERIFY_REQUIRE);
	r->verify = v;
}

tix_verify_policy tix_repo_verify(const tix_repo *r)
{
	return r->verify;
}

