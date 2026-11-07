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
	tix_verify_policy verify;
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

/* What ticket lists filter and sort on (summary.c). id/head come first and survive
 * core_facets_free; everything from status on is owned. */
typedef struct core_facets {
	tix_oid id, head;
	tix_ticket_status status;
	char *stage;            /* NULL: none (§15.1) */
	tix_oid author, parent; /* parent null: top level */
	char *rank, *title;     /* rank NULL: unranked */
	int64_t created, updated;
	uint64_t max_lamport;
	tix_strs labels;
	TIX_VEC(tix_oid) assignees;
	tix_entity *e;          /* the loaded entity, once there is one (OWNS) */
} core_facets;

/* REQUIRES e is a ticket. Replaces out's contents. */
void core_summary_encode(const tix_entity *e, tix_buf *out);
/* RETURNS false (f's owned part left empty) for anything but a well-formed current summary. */
bool core_summary_decode(const char *data, size_t len, tix_hash_algo algo, core_facets *f);
/* Takes ownership of e (f->e). */
void core_facets_from_entity(tix_entity *e, core_facets *f);
void core_facets_free(core_facets *f);

/* Identities looked up while verifying, loaded once per run (verify.c). */
typedef struct core_key_entry {
	tix_oid id;
	tix_entity *identity; /* OWNS; NULL if it didn't load */
} core_key_entry;
typedef TIX_VEC(core_key_entry) core_keys;
void core_keys_free(core_keys *k);

/* Verifies commit c of entity (t, entity) against its author identity's keys. For an
 * identity's own history pass self = that identity as of the history being checked (the
 * incoming one on pull). *out is BAD for an author that doesn't load locally. */
tix_status core_verify_commit(tix_repo *r, tix_entity_type t, const tix_oid *entity, const tix_commit *c,
			      const tix_entity *self, core_keys *cache, tix_sig_status *out, tix_error *err);

/* "refs/tix/<type>/<hex id>" into out[n]. */
void core_ref_of(tix_entity_type t, const tix_oid *id, char *out, size_t n);

/* Encodes and commits one pack with lamport = clock + 1 (n == 0: a header-only
 * merge pack). Does not move any ref. *lamport receives the pack's value. */
tix_status core_write_pack(tix_repo *r, tix_entity_type t, const tix_oid *author, const tix_oid *parents,
			   size_t nparents, const tix_op *ops, size_t n, tix_oid *out, uint64_t *lamport,
			   tix_error *err);

#endif
