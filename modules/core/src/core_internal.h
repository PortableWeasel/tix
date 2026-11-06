/* Shared between core's translation units; not a public header. */
#ifndef TIX_CORE_INTERNAL_H
#define TIX_CORE_INTERNAL_H

#include "tix/core.h"

#define CORE_CAS_ATTEMPTS 5

struct tix_repo {
	tix_store *store;
	tix_env env;
	tix_oid identity;
	uint64_t clock;
	tix_index *index; /* optional */
};

/* Open-addressing set of oids. Zero-initialise; free with core_oidset_free. */
typedef struct core_oidset {
	tix_oid *slots;
	bool *used;
	size_t cap, n;
} core_oidset;

bool core_oidset_add(core_oidset *s, const tix_oid *o); /* RETURNS false if present */
bool core_oidset_has(const core_oidset *s, const tix_oid *o);
void core_oidset_free(core_oidset *s);

/* Every commit reachable from a head. OWNS the commits. */
typedef struct core_dag {
	TIX_VEC(tix_commit) commits;
} core_dag;

/* Loads the history below head. A missing commit or more than
 * TIX_ENTITY_MAX_COMMITS is TIX_ERR_INVALID. */
tix_status core_dag_load(tix_repo *r, const tix_oid *head, core_dag *d, tix_error *err);
void core_dag_free(core_dag *d);

/* Loads, validates and replays the history below head (no ref involved);
 * the clock sees its Lamport values. */
tix_status core_load_at(tix_repo *r, tix_entity_type t, const tix_oid *head, tix_entity **out,
			tix_error *err);

void core_clock_see(tix_repo *r, uint64_t lamport);
tix_status core_clock_save(tix_repo *r, tix_error *err);

/* "refs/tix/<type>/<hex id>" into out[n]. */
void core_ref_of(tix_entity_type t, const tix_oid *id, char *out, size_t n);

/* Encodes and commits one pack with lamport = clock + 1 (n == 0: a header-only
 * merge pack). Does not move any ref. *lamport receives the pack's value. */
tix_status core_write_pack(tix_repo *r, tix_entity_type t, const tix_oid *author, const tix_oid *parents,
			   size_t nparents, const tix_op *ops, size_t n, tix_oid *out, uint64_t *lamport,
			   tix_error *err);

#endif
