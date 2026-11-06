/* pull / push / sync (architecture §6). Remote data is hostile until replayed. */
#include "core_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SYNC_ROUNDS 3
#define PUSH_CHUNK 500 /* refs per push, keeping argv well below ARG_MAX */

static const tix_entity_type sync_types[] = {TIX_ENTITY_IDENTITY, TIX_ENTITY_TICKET};
#define NTYPES (sizeof(sync_types) / sizeof(sync_types[0]))

void tix_sync_report_free(tix_sync_report *rep)
{
	tix_strs_free(&rep->problems);
	memset(rep, 0, sizeof(*rep));
}

static void problem(tix_sync_report *rep, const char *what, const char *ref, const tix_error *why)
{
	char line[512];
	snprintf(line, sizeof(line), "%s %s: %s", what, ref, why->msg[0] ? why->msg : "invalid");
	tix_strs_add(&rep->problems, line);
}

/* Parses "<type dir>/<hex id>" (the part after ".../tix/"). */
static bool parse_entity_ref(const tix_repo *r, const char *rest, tix_entity_type *t, tix_oid *id)
{
	size_t hexlen = tix_oid_rawlen(r->store->algo) * 2;
	for (size_t i = 0; i < NTYPES; i++) {
		const char *dir = tix_entity_ref_prefix(sync_types[i]) + strlen("refs/tix/");
		size_t n = strlen(dir);
		if (strncmp(rest, dir, n))
			continue;
		const char *hex = rest + n;
		if (strlen(hex) != hexlen || !tix_oid_from_hex(id, hex, hexlen))
			return false;
		*t = sync_types[i];
		return true;
	}
	return false;
}

/* RETURNS (via *found) whether target is head or one of its ancestors. */
static tix_status reaches(tix_repo *r, const tix_oid *head, const tix_oid *target, bool *found, tix_error *err)
{
	core_dag d;
	tix_status st = core_dag_load(r, head, &d, err);
	*found = false;
	if (st)
		return st;
	for (size_t i = 0; i < d.commits.n && !*found; i++)
		*found = tix_oid_eq(&d.commits.v[i].oid, target);
	core_dag_free(&d);
	return TIX_OK;
}

/* Integrates remote head `rh` for entity (t, id) into the local ref. */
static tix_status integrate(tix_repo *r, tix_entity_type t, const tix_oid *id, const char *local,
			    const tix_oid *rh, tix_sync_report *rep, tix_error *err)
{
	tix_status st = TIX_ERR_CONFLICT;
	for (int attempt = 0; attempt < CORE_CAS_ATTEMPTS && st == TIX_ERR_CONFLICT; attempt++) {
		tix_oid lh, merge;
		tix_error why = TIX_ERROR_INIT;
		tix_entity *e = NULL;
		bool have;
		uint64_t lamport;

		st = tix_store_ref_read(r->store, local, &lh, err);
		if (st == TIX_ERR_NOT_FOUND) {
			st = tix_store_ref_cas(r->store, local, NULL, rh, err);
			if (!st)
				rep->created++;
			else if (st == TIX_ERR_EXISTS)
				st = TIX_ERR_CONFLICT; /* someone created it meanwhile: retry */
			continue;
		}
		if (st)
			return st;
		if (tix_oid_eq(&lh, rh))
			return TIX_OK;
		/* Ancestry needs the local history to load; a broken local ref is reported. */
		if (reaches(r, &lh, rh, &have, &why)) {
			problem(rep, "local", local, &why);
			rep->unpushable++;
			return TIX_OK;
		}
		if (have)
			return TIX_OK; /* we are ahead */
		if ((st = reaches(r, rh, &lh, &have, err)))
			return st; /* rh validated a moment ago */
		if (have) {
			st = tix_store_ref_cas(r->store, local, &lh, rh, err);
			if (!st)
				rep->forwarded++;
			continue;
		}
		if (core_load_at(r, t, &lh, &e, &why)) {
			problem(rep, "local", local, &why);
			rep->unpushable++;
			return TIX_OK; /* never merge into history that doesn't validate */
		}
		tix_entity_free(e);
		tix_oid parents[2] = {lh, *rh};
		tix_oid me;
		bool has_me = tix_repo_identity(r, &me);
		if ((st = core_write_pack(r, t, has_me ? &me : NULL, parents, 2, NULL, 0, &merge, &lamport, err)))
			return st;
		/* The union must validate too (size limits apply to the merged entity). */
		if (core_load_at(r, t, &merge, &e, &why)) {
			problem(rep, "merge of", local, &why);
			rep->rejected++;
			return TIX_OK;
		}
		bool same_root = tix_oid_eq(&e->id, id);
		tix_entity_free(e);
		TIX_REQUIRE(same_root); /* both sides were checked against id */
		st = tix_store_ref_cas(r->store, local, &lh, &merge, err);
		if (!st) {
			core_clock_see(r, lamport);
			rep->merged++;
		}
	}
	return st;
}

static void tracking_prefix(const char *remote, char *out, size_t n)
{
	snprintf(out, n, "refs/remotes/%s/tix/", remote);
}

tix_status tix_pull(tix_repo *r, const char *remote, tix_sync_report *rep, tix_error *err)
{
	TIX_REQUIRE(r && remote && rep);
	char prefix[128];
	tix_refs refs;
	tix_status st = tix_store_remote_fetch(r->store, remote, err);
	if (st)
		return st;
	tracking_prefix(remote, prefix, sizeof(prefix));
	if ((st = tix_store_ref_list(r->store, prefix, &refs, err)))
		return st;
	size_t plen = strlen(prefix);
	/* refs are sorted, so identities integrate before tickets */
	for (size_t i = 0; i < refs.n && !st; i++) {
		const char *rest = refs.v[i].name + plen;
		tix_entity_type t;
		tix_oid id;
		tix_entity *e = NULL;
		tix_error why = TIX_ERROR_INIT;
		char local[160];
		if (!parse_entity_ref(r, rest, &t, &id)) {
			rep->skipped++;
			continue;
		}
		if (core_load_at(r, t, &refs.v[i].oid, &e, &why)) {
			problem(rep, "rejected", refs.v[i].name, &why);
			rep->rejected++;
			continue;
		}
		bool same_root = tix_oid_eq(&e->id, &id);
		tix_entity_free(e);
		if (!same_root) {
			tix_error_set(&why, TIX_ERR_INVALID, "history does not belong to that id");
			problem(rep, "rejected", refs.v[i].name, &why);
			rep->rejected++;
			continue;
		}
		core_ref_of(t, &id, local, sizeof(local));
		st = integrate(r, t, &id, local, &refs.v[i].oid, rep, err);
	}
	tix_refs_free(&refs);
	tix_status cst = core_clock_save(r, st ? NULL : err);
	return st ? st : cst;
}

tix_status tix_push(tix_repo *r, const char *remote, tix_sync_report *rep, tix_error *err)
{
	TIX_REQUIRE(r && remote && rep);
	char prefix[128];
	TIX_VEC(char *) names = {0};
	TIX_VEC(tix_oid) heads = {0};
	tix_status st = TIX_OK;
	if (!tix_remote_name_valid(remote))
		return tix_error_set(err, TIX_ERR_INVALID, "bad remote name '%s'", remote);
	tracking_prefix(remote, prefix, sizeof(prefix));

	for (size_t k = 0; k < NTYPES && !st; k++) {
		tix_entity_type t = sync_types[k];
		tix_refs refs;
		if ((st = tix_store_ref_list(r->store, tix_entity_ref_prefix(t), &refs, err)))
			break;
		for (size_t i = 0; i < refs.n; i++) {
			const char *rest = refs.v[i].name + strlen("refs/tix/");
			char track[192];
			tix_oid id, theirs;
			tix_entity *e = NULL;
			tix_error why = TIX_ERROR_INIT;
			if (!parse_entity_ref(r, rest, &t, &id))
				continue; /* not ours to push */
			snprintf(track, sizeof(track), "%s%s", prefix, rest);
			bool known = !tix_store_ref_read(r->store, track, &theirs, NULL);
			if (known && tix_oid_eq(&theirs, &refs.v[i].oid))
				continue; /* remote already has it */
			if (tix_entity_load(r, t, &id, &e, &why)) {
				problem(rep, "not pushed", refs.v[i].name, &why);
				rep->unpushable++;
				continue; /* never spread history that doesn't validate */
			}
			tix_entity_free(e);
			bool contains = true;
			if (known && reaches(r, &refs.v[i].oid, &theirs, &contains, &why) == TIX_OK && !contains) {
				/* After a pull this only happens when the remote's history was rejected. */
				tix_error_set(&why, TIX_ERR_CONFLICT, "remote has changes not merged here (pull first)");
				problem(rep, "not pushed", refs.v[i].name, &why);
				rep->unpushable++;
				continue;
			}
			TIX_VEC_PUSH(&names, tix_xstrdup(refs.v[i].name));
			TIX_VEC_PUSH(&heads, refs.v[i].oid);
		}
		tix_refs_free(&refs);
	}

	rep->pushed = 0;
	for (size_t at = 0; at < names.n && !st; at += PUSH_CHUNK) {
		size_t n = names.n - at < PUSH_CHUNK ? names.n - at : PUSH_CHUNK;
		st = tix_store_remote_push(r->store, remote, (const char *const *)names.v + at, n, err);
		if (st)
			break;
		rep->pushed += n;
		/* Record what the remote now has, so the next push skips it. */
		for (size_t i = at; i < at + n; i++) {
			char track[192];
			tix_oid old;
			snprintf(track, sizeof(track), "%s%s", prefix, names.v[i] + strlen("refs/tix/"));
			bool had = !tix_store_ref_read(r->store, track, &old, NULL);
			tix_store_ref_cas(r->store, track, had ? &old : NULL, &heads.v[i], NULL);
		}
	}
	for (size_t i = 0; i < names.n; i++)
		free(names.v[i]);
	free(names.v);
	free(heads.v);
	return st;
}

tix_status tix_sync(tix_repo *r, const char *remote, tix_sync_report *rep, tix_error *err)
{
	tix_status st = TIX_OK;
	for (int round = 0; round < SYNC_ROUNDS; round++) {
		if ((st = tix_pull(r, remote, rep, err)))
			return st;
		st = tix_push(r, remote, rep, err);
		if (st != TIX_ERR_CONFLICT)
			return st;
	}
	return st;
}
