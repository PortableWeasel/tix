#include "tix/sim.h"
#include "tix/store_mem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIM_EPOCH 1700000000

void tix_sim_clone_init(tix_sim_clone *c, const char *name, tix_hash_algo algo)
{
	memset(c, 0, sizeof(*c));
	TIX_REQUIRE(strlen(name) < sizeof(c->name));
	strcpy(c->name, name);
	c->store = tix_store_mem_new(algo);
}

void tix_sim_clone_free(tix_sim_clone *c)
{
	tix_store_destroy(c->store);
	c->store = NULL;
}

void tix_sim_dag_free(tix_sim_dag *d)
{
	for (size_t i = 0; i < d->n; i++)
		tix_commit_free(&d->commits[i]);
	free(d->commits);
	free(d->nodes);
	memset(d, 0, sizeof(*d));
}

static bool seen_oid(const tix_sim_dag *d, const tix_oid *o)
{
	for (size_t i = 0; i < d->n; i++)
		if (tix_oid_eq(&d->commits[i].oid, o))
			return true;
	return false;
}

tix_status tix_sim_dag_load(tix_store *s, const tix_oid *head, tix_sim_dag *out, tix_error *err)
{
	TIX_VEC(tix_oid) todo = {0};
	size_t cap = 0;
	memset(out, 0, sizeof(*out));
	TIX_VEC_PUSH(&todo, *head);
	while (todo.n) {
		tix_oid o = todo.v[--todo.n];
		if (seen_oid(out, &o))
			continue;
		if (out->n == cap) {
			cap = cap ? cap * 2 : 16;
			out->commits = tix_xreallocarray(out->commits, cap, sizeof(tix_commit));
		}
		tix_status st = tix_store_commit_read(s, &o, &out->commits[out->n], err);
		if (st) {
			free(todo.v);
			tix_sim_dag_free(out);
			return st;
		}
		const tix_commit *c = &out->commits[out->n++];
		for (size_t k = 0; k < c->nparents; k++)
			TIX_VEC_PUSH(&todo, c->parents[k]);
	}
	free(todo.v);
	out->nodes = tix_xcalloc(out->n, sizeof(tix_dag_node));
	for (size_t i = 0; i < out->n; i++) {
		const tix_commit *c = &out->commits[i];
		out->nodes[i] = (tix_dag_node){c->oid, c->parents, c->nparents, c->pack.data, c->pack.len};
	}
	return TIX_OK;
}

char *tix_sim_nonce(tix_sim_clone *c, char *out)
{
	uint64_t h = 0xcbf29ce484222325ull;
	for (const char *p = c->name; *p; p++)
		h = (h ^ (unsigned char)*p) * 0x100000001b3ull;
	snprintf(out, TIX_NONCE_HEX + 1, "%016llx%016llx", (unsigned long long)h,
		 (unsigned long long)++c->nonces);
	return out;
}

char *tix_sim_ref(tix_entity_type t, const tix_oid *id, char *out)
{
	char hex[TIX_OID_MAX_HEX + 1];
	snprintf(out, 128, "%s%s", tix_entity_ref_prefix(t), tix_oid_hex(id, hex));
	return out;
}

static tix_status load_at(tix_sim_clone *c, tix_entity_type t, const tix_oid *head, tix_entity **out,
			  tix_error *err)
{
	tix_sim_dag d;
	tix_status st = tix_sim_dag_load(c->store, head, &d, err);
	if (st)
		return st;
	st = tix_replay(t, d.nodes, d.n, out, err);
	tix_sim_dag_free(&d);
	if (!st && (*out)->max_lamport > c->clock)
		c->clock = (*out)->max_lamport;
	return st;
}

tix_status tix_sim_load(tix_sim_clone *c, tix_entity_type t, const char *ref, tix_entity **out,
			tix_error *err)
{
	tix_oid head;
	tix_status st = tix_store_ref_read(c->store, ref, &head, err);
	return st ? st : load_at(c, t, &head, out, err);
}

static tix_status write_pack(tix_sim_clone *c, tix_entity_type t, const tix_oid *author,
			     const tix_oid *parents, size_t np, const tix_op *ops, size_t n,
			     tix_oid *out, tix_error *err)
{
	tix_pack p = {t, {0}, ++c->clock, SIM_EPOCH + (int64_t)c->clock, (tix_op *)ops, n};
	if (author)
		p.author = *author;
	tix_buf b = TIX_BUF_INIT;
	char msg[64];
	snprintf(msg, sizeof(msg), "tix %s: %s", tix_entity_type_name(t), n ? "ops" : "merge");
	tix_status st = tix_pack_encode(&p, &b, err);
	if (!st)
		st = tix_store_commit_write(c->store, parents, np, b.data, b.len, msg, out, err);
	tix_buf_free(&b);
	return st;
}

tix_status tix_sim_create(tix_sim_clone *c, tix_entity_type t, const tix_op *ops, size_t n,
			  tix_oid *id_out, tix_error *err)
{
	char ref[128];
	const tix_oid *author = NULL;
	if (t != TIX_ENTITY_IDENTITY) {
		TIX_REQUIRE(!tix_oid_is_null(&c->me));
		author = &c->me;
	}
	tix_status st = write_pack(c, t, author, NULL, 0, ops, n, id_out, err);
	return st ? st : tix_store_ref_cas(c->store, tix_sim_ref(t, id_out, ref), NULL, id_out, err);
}

tix_status tix_sim_apply(tix_sim_clone *c, tix_entity_type t, const tix_oid *id, const tix_op *ops,
			 size_t n, tix_error *err)
{
	char ref[128];
	tix_oid head, commit;
	tix_entity *e;
	TIX_REQUIRE(!tix_oid_is_null(&c->me));
	tix_status st = tix_store_ref_read(c->store, tix_sim_ref(t, id, ref), &head, err);
	if (st || (st = load_at(c, t, &head, &e, err)))
		return st;
	tix_entity_free(e);
	st = write_pack(c, t, &c->me, &head, 1, ops, n, &commit, err);
	return st ? st : tix_store_ref_cas(c->store, ref, &head, &commit, err);
}

/* RETURNS true if a is b or an ancestor of b. */
static bool is_ancestor(tix_store *s, const tix_oid *a, const tix_oid *b)
{
	tix_sim_dag d;
	if (tix_sim_dag_load(s, b, &d, NULL))
		return false;
	bool found = seen_oid(&d, a);
	tix_sim_dag_free(&d);
	return found;
}

static tix_status sync_one(tix_sim_clone *dst, const char *local, tix_entity_type t, const tix_oid *remote,
			   tix_error *err)
{
	tix_entity *re = NULL, *le = NULL;
	tix_oid head, merge;
	if (load_at(dst, t, remote, &re, err)) {
		dst->rejected++;
		return TIX_OK; /* never adopt invalid remote data */
	}
	tix_entity_free(re);
	tix_status st = tix_store_ref_read(dst->store, local, &head, err);
	if (st == TIX_ERR_NOT_FOUND)
		return tix_store_ref_cas(dst->store, local, NULL, remote, err);
	if (st)
		return st;
	if (tix_oid_eq(&head, remote) || is_ancestor(dst->store, remote, &head))
		return TIX_OK;
	if (is_ancestor(dst->store, &head, remote))
		return tix_store_ref_cas(dst->store, local, &head, remote, err);
	if ((st = load_at(dst, t, &head, &le, err)))
		return st;
	tix_entity_free(le);
	tix_oid parents[2] = {head, *remote};
	const tix_oid *author = tix_oid_is_null(&dst->me) ? NULL : &dst->me;
	if (t != TIX_ENTITY_IDENTITY)
		TIX_REQUIRE(author);
	if ((st = write_pack(dst, t, author, parents, 2, NULL, 0, &merge, err)))
		return st;
	dst->merges++;
	return tix_store_ref_cas(dst->store, local, &head, &merge, err);
}

tix_status tix_sim_sync(tix_sim_clone *dst, tix_sim_clone *src, tix_error *err)
{
	char prefix[64];
	tix_refs refs;
	tix_store_mem_fetch(dst->store, src->store, src->name);
	snprintf(prefix, sizeof(prefix), "refs/remotes/%s/tix/", src->name);
	tix_status st = tix_store_ref_list(dst->store, prefix, &refs, err);
	if (st)
		return st;
	size_t plen = strlen(prefix);
	for (size_t i = 0; i < refs.n && !st; i++) {
		const char *rest = refs.v[i].name + plen; /* "tickets/<id>" */
		tix_entity_type t = 0;
		for (size_t k = 0; k < TIX_ENTITY_NTYPES; k++) {
			const char *dir = tix_entity_ref_prefix(tix_entity_types[k]) + strlen("refs/tix/");
			if (!strncmp(rest, dir, strlen(dir)))
				t = tix_entity_types[k];
		}
		if (!t)
			continue;
		char local[160];
		snprintf(local, sizeof(local), "refs/tix/%s", rest);
		st = sync_one(dst, local, t, &refs.v[i].oid, err);
	}
	tix_refs_free(&refs);
	return st;
}
