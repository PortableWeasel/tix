/*
 * tix/core.h -- the API every frontend uses (architecture §8).
 *
 * Core composes model (pure rules) with a tix_store (storage port). It owns the
 * repo-wide Lamport clock and the local identity, validates everything it
 * reads, and never prints, exits, or reads env/clock/randomness itself: those
 * come in through tix_env.
 *
 * Read models are model's tix_entity (plain data). Ids are entity ids (root
 * commit oids). Not thread-safe.
 */
#ifndef TIX_CORE_H
#define TIX_CORE_H

#include "tix/model.h"
#include "tix/model_replay.h"
#include "tix/store.h"

#define TIX_ID_MIN_PREFIX 7

typedef struct tix_env {
	int64_t (*now)(void *ctx);                         /* unix seconds */
	void (*random)(void *ctx, uint8_t *out, size_t n); /* fresh randomness (nonces) */
	void *ctx;
} tix_env;

typedef struct tix_repo_config {
	tix_store *store; /* BORROWS: must outlive the repo */
	tix_env env;
	tix_oid identity; /* the local user's identity, or null if none chosen yet */
} tix_repo_config;

typedef struct tix_repo tix_repo;

/*
 * Opens core on a store. If cfg->identity is set it must load as a valid identity.
 * RETURNS TIX_ERR_INVALID for a corrupt local clock or a bad identity.
 * ENSURES on TIX_OK: *out owned by caller (tix_repo_close).
 */
tix_status tix_repo_open(tix_repo **out, const tix_repo_config *cfg, tix_error *err);
void tix_repo_close(tix_repo *r);

/* RETURNS false (and leaves *out alone) when no identity is set. */
bool tix_repo_identity(const tix_repo *r, tix_oid *out);
/* Makes id the identity new ops are written as (in memory; persisting the choice,
 * e.g. git config tix.identity, is the frontend's job). The identity must load. */
tix_status tix_repo_set_identity(tix_repo *r, const tix_oid *id, tix_error *err);

/* ---- generic entity operations ------------------------------------------ */

/*
 * Resolves a hex id or unique prefix (>= TIX_ID_MIN_PREFIX chars, lowercase).
 * RETURNS TIX_ERR_INVALID (malformed or too short), TIX_ERR_NOT_FOUND,
 * TIX_ERR_AMBIGUOUS.
 */
tix_status tix_entity_resolve(tix_repo *r, tix_entity_type t, const char *prefix, tix_oid *out,
			      tix_error *err);

/* Loads, validates and replays. ENSURES on TIX_OK: *out owned by caller. */
tix_status tix_entity_load(tix_repo *r, tix_entity_type t, const tix_oid *id, tix_entity **out,
			   tix_error *err);

/*
 * Creates an entity. REQUIRES n >= 1. ops[0] must be the type's create op and no
 * other op may be one (else TIX_ERR_INVALID). A NULL nonce in ops[0] is filled
 * with fresh randomness (ops[0] is modified). Tickets need an identity set
 * (else TIX_ERR_NOT_FOUND); identities are self-authored.
 */
tix_status tix_entity_create(tix_repo *r, tix_entity_type t, tix_op *ops, size_t n, tix_oid *out,
			     tix_error *err);

/*
 * Appends ops as one op-pack, authored by the current identity, then advances
 * the clock. Retries automatically if another writer moves the ref meanwhile.
 * Identities may only be edited by themselves, and comments only by their
 * author (TIX_ERR_INVALID otherwise; an unknown comment is TIX_ERR_NOT_FOUND).
 * BORROWS ops.
 */
tix_status tix_entity_apply(tix_repo *r, tix_entity_type t, const tix_oid *id, const tix_op *ops, size_t n,
			    tix_error *err);

/* ---- listing --------------------------------------------------------------- */

typedef struct tix_entity_list {
	tix_entity **v; /* OWNS */
	size_t n;
	size_t nbroken; /* refs that failed validation and were skipped */
} tix_entity_list;

void tix_entity_list_free(tix_entity_list *l);

typedef struct tix_ticket_query {
	unsigned status_mask;  /* bit (1u << status); 0 = any */
	const char *label;     /* NULL = any */
	const tix_oid *author; /* NULL = any */
} tix_ticket_query;

/* Statuses still "open" in the broad sense: open, in-progress, blocked. */
#define TIX_STATUS_MASK_ACTIVE \
	((1u << TIX_TICKET_OPEN) | (1u << TIX_TICKET_IN_PROGRESS) | (1u << TIX_TICKET_BLOCKED))

/* All tickets matching q (NULL = all), newest first (created desc, then id). */
tix_status tix_ticket_list(tix_repo *r, const tix_ticket_query *q, tix_entity_list *out, tix_error *err);
/* All identities, ordered by name, then id. */
tix_status tix_identity_list(tix_repo *r, tix_entity_list *out, tix_error *err);

/* ---- conveniences ----------------------------------------------------------- */

tix_status tix_identity_create(tix_repo *r, const char *name, const char *email, tix_oid *out,
			       tix_error *err);
tix_status tix_ticket_create(tix_repo *r, const char *title, const char *body, const tix_strs *labels,
			     tix_oid *out, tix_error *err);

#endif
