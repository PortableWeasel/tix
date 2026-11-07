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
 *   the same author. Dependencies, assignments and calendar exceptions are OR-maps:
 *   the key's surviving adds decide presence, the latest surviving add the value.
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

/* An op's position in the total order (lamport, commit oid, line). */
typedef struct tix_order {
	uint64_t lamport;
	tix_opid id;
} tix_order;

int tix_order_cmp(const tix_order *a, const tix_order *b);

typedef struct tix_dep {
	tix_oid pred;
	tix_dep_kind kind;
	int32_t lag;     /* working days */
	tix_order order; /* of the add that set this value (cycle breaking) */
} tix_dep;

typedef struct tix_assignment {
	tix_oid resource;
	int32_t units;  /* percent of the resource's capacity */
	int64_t effort; /* hours, -1 = unset */
} tix_assignment;

/* WBS / scheduling / assignment state (architecture §4.2). Unset dates are
 * TIX_DATE_NONE, unset numbers -1, unset ids null. */
typedef struct tix_ticket_plan {
	tix_oid parent;
	tix_order parent_order; /* of the winning set-parent (cycle breaking) */
	char *rank;             /* NULL = unranked */
	tix_ticket_kind kind;
	tix_oid calendar;
	tix_sched_mode mode;
	tix_date start, finish, deadline, actual_start, actual_finish;
	int64_t duration; /* working days */
	int64_t effort;   /* hours */
	int progress;     /* 0..100 */
	tix_dep *deps;    /* sorted by pred */
	size_t ndeps;
	tix_assignment *assignments; /* sorted by resource */
	size_t nassignments;
	int64_t started_at; /* time of the first set-status in-progress, or -1 */
	int64_t done_at;    /* time of the set-status that made it done, -1 unless done */
} tix_ticket_plan;

typedef struct tix_ticket {
	tix_oid author; /* creator */
	char *title;
	char *body;
	tix_ticket_status status;
	tix_strs labels;
	tix_comment *comments;
	size_t ncomments;
	tix_ticket_plan plan;
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

typedef struct tix_resource {
	char *name;
	tix_resource_kind kind;
	int32_t capacity; /* percent: 100 = one full-time unit */
	tix_oid calendar; /* null = the task's calendar */
	tix_oid identity; /* null = not linked to a user */
	int64_t rate;     /* minor currency units per hour, -1 = unset */
	char *currency;   /* NULL = unset */
	bool active;
} tix_resource;

typedef struct tix_cal_exception {
	tix_date from, to; /* inclusive */
	bool working;
	char *note;
	tix_order order; /* later exceptions win where ranges overlap */
} tix_cal_exception;

typedef struct tix_calendar {
	char *name;
	unsigned weekdays; /* bit 0 = Monday */
	int hours;         /* working hours per day */
	tix_cal_exception *exceptions; /* sorted by (from, to) */
	size_t nexceptions;
} tix_calendar;

/* One task in a baseline: the last "task" op for that ticket in total order wins. */
typedef struct tix_baseline_task {
	tix_oid ticket;
	tix_date start, finish;         /* TIX_DATE_NONE = unscheduled then */
	int64_t duration, effort, cost; /* -1 = unknown */
	char *currency;                 /* NULL with cost -1 */
} tix_baseline_task;

/* A plan snapshot (architecture §14.2). Immutable apart from its name. */
typedef struct tix_baseline {
	char *name;
	tix_date taken;
	tix_oid root;             /* null = the whole plan */
	tix_baseline_task *tasks; /* sorted by ticket id */
	size_t ntasks;
} tix_baseline;

/* RETURNS the baseline's entry for ticket id, or NULL. */
const tix_baseline_task *tix_baseline_find(const tix_baseline *b, const tix_oid *ticket);

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
		tix_resource resource;
		tix_calendar calendar;
		tix_baseline baseline;
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

/*
 * Lossless serialisation of a replayed entity, for caches (the M5 index). Not a
 * wire format: never synced, never compared across versions. Bump
 * TIX_ENTITY_CODEC_VERSION whenever tix_entity or the replay rules change, so
 * stale cache entries miss instead of lying.
 */
#define TIX_ENTITY_CODEC_VERSION 2

void tix_entity_encode(const tix_entity *e, tix_buf *out);
/* Checks every field (types, ranges, UTF-8, id algo). RETURNS TIX_ERR_INVALID for
 * malformed input, TIX_ERR_UNSUPPORTED for another codec version.
 * ENSURES on TIX_OK: *out owned by the caller. */
tix_status tix_entity_decode(const char *data, size_t len, tix_hash_algo algo, tix_entity **out,
			     tix_error *err);

#endif
