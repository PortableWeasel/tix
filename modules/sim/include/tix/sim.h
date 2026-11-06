/*
 * tix/sim.h -- simulated clones for tests (TEST support only, never linked into apps).
 *
 * A clone = a store_mem store + a Lamport clock + the identity it writes as.
 * It mimics what core (M2) and sync (M3) will do, so model behaviour can be
 * tested on realistic, concurrent DAGs before those layers exist. Time is
 * synthetic (derived from the Lamport value) to keep runs deterministic.
 */
#ifndef TIX_SIM_H
#define TIX_SIM_H

#include "tix/model.h"
#include "tix/model_replay.h"
#include "tix/store.h"

typedef struct tix_sim_clone {
	char name[16];    /* also the remote name others fetch it as */
	tix_store *store; /* OWNS */
	uint64_t clock;
	tix_oid me;       /* identity id; null until set by the caller */
	size_t merges;    /* merge commits written by tix_sim_sync */
	size_t rejected;  /* remote refs refused by validation */
	uint64_t nonces;  /* counter behind tix_sim_nonce */
} tix_sim_clone;

void tix_sim_clone_init(tix_sim_clone *c, const char *name, tix_hash_algo algo);
void tix_sim_clone_free(tix_sim_clone *c);

/* All commits reachable from head. OWNS the commits; nodes point into them. */
typedef struct tix_sim_dag {
	tix_commit *commits;
	tix_dag_node *nodes;
	size_t n;
} tix_sim_dag;

tix_status tix_sim_dag_load(tix_store *s, const tix_oid *head, tix_sim_dag *out, tix_error *err);
void tix_sim_dag_free(tix_sim_dag *d);

/* "refs/tix/tickets/<hex id>" etc. REQUIRES out has 128 bytes. RETURNS out. */
char *tix_sim_ref(tix_entity_type t, const tix_oid *id, char *out);

/* Deterministic per-clone nonce for create ops (tests only; real callers use
 * fresh randomness). REQUIRES out has TIX_NONCE_HEX + 1 bytes. RETURNS out. */
char *tix_sim_nonce(tix_sim_clone *c, char *out);

/* Loads and replays the entity at ref; advances the clock past it. */
tix_status tix_sim_load(tix_sim_clone *c, tix_entity_type t, const char *ref, tix_entity **out,
			tix_error *err);

/* Creates an entity from ops (BORROWS ops). Identity roots are written without
 * an author; other types REQUIRE c->me to be set. ENSURES *id_out = root commit. */
tix_status tix_sim_create(tix_sim_clone *c, tix_entity_type t, const tix_op *ops, size_t n,
			  tix_oid *id_out, tix_error *err);

/* Appends one op-pack to an existing entity (BORROWS ops). */
tix_status tix_sim_apply(tix_sim_clone *c, tix_entity_type t, const tix_oid *id, const tix_op *ops,
			 size_t n, tix_error *err);

/*
 * dst fetches src, then for each fetched entity: validate (replay) it, then
 * create / fast-forward / skip / merge (header-only merge commit). Invalid
 * remote entities are counted in dst->rejected and left alone.
 */
tix_status tix_sim_sync(tix_sim_clone *dst, tix_sim_clone *src, tix_error *err);

#endif
