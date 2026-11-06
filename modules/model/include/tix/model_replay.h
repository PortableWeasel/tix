/*
 * tix/model_replay.h -- rebuild entity state from its commit DAG (pure).
 *
 * Input: every commit reachable from one entity head, in any order.
 * Validation (all failures are TIX_ERR_INVALID; untrusted input never aborts):
 *   - one root (the entity id), one head, no missing parents, no cycles,
 *     <= TIX_ENTITY_MAX_COMMITS commits, one hash algorithm throughout;
 *   - every pack decodes and has the entity's type;
 *   - lamport strictly greater than every parent's (so the total order below
 *     extends causality);
 *   - the root pack starts with "create"; no other pack contains "create";
 *     merge commits (2+ parents) carry no ops;
 *   - authors: ticket packs always name an identity; an identity's root pack has
 *     no author and its other op-carrying packs are authored by itself.
 *
 * Total order of ops: (lamport, commit oid, line). Merge rules (architecture §3.4):
 *   LWW registers fold in total order; label and key sets are observed-remove
 *   sets, where a remove cancels exactly the adds that are causal ancestors of
 *   it (earlier line in the same commit, or an ancestor commit); comments append;
 *   an edit-comment applies only if its target exists earlier in the order and has
 *   the same author.
 */
#ifndef TIX_MODEL_REPLAY_H
#define TIX_MODEL_REPLAY_H

#include "tix/model.h"

/* BORROWS every pointer for the duration of tix_replay. */
typedef struct tix_dag_node {
	tix_oid oid;
	const tix_oid *parents;
	size_t nparents;
	const char *pack;
	size_t pack_len;
} tix_dag_node;

typedef struct tix_comment {
	tix_opid id;
	tix_oid author;
	int64_t time;
	char *body;
	bool edited;
} tix_comment;

typedef struct tix_ticket {
	tix_oid author; /* creator */
	char *title;
	char *body;
	tix_ticket_status status;
	tix_strs labels;
	tix_comment *comments;
	size_t ncomments;
} tix_ticket;

typedef struct tix_key {
	char *kind, *fingerprint, *pubkey;
} tix_key;

typedef struct tix_identity {
	char *name;
	char *email;
	tix_key *keys; /* sorted by fingerprint */
	size_t nkeys;
} tix_identity;

typedef struct tix_entity {
	tix_entity_type type;
	tix_oid id;           /* root commit */
	tix_oid head;         /* the one commit with no children */
	uint64_t max_lamport; /* over all commits, merges included: advance the clock past it */
	int64_t created;      /* root pack time */
	int64_t updated;      /* latest time among packs carrying ops */
	size_t nops;          /* known ops applied or ignored by rule */
	size_t nunknown;      /* unknown op types, preserved but ignored */
	union {
		tix_ticket ticket;
		tix_identity identity;
	} u;
} tix_entity;

/* ENSURES on TIX_OK: *out is owned by the caller (tix_entity_free). */
tix_status tix_replay(tix_entity_type type, const tix_dag_node *nodes, size_t n,
		      tix_entity **out, tix_error *err);

void tix_entity_free(tix_entity *e);

/*
 * Appends a canonical JSON snapshot of the semantic state. Two clones holding the
 * same op commits produce byte-identical snapshots, even when their merge commits
 * (and thus heads and max_lamport, which are excluded) differ.
 */
void tix_entity_snapshot(const tix_entity *e, tix_buf *out);

#endif
