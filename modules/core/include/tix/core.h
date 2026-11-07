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

#include "tix/index.h"
#include "tix/model.h"
#include "tix/model_replay.h"
#include "tix/plan.h"
#include "tix/store.h"

#define TIX_ID_MIN_PREFIX 7

typedef struct tix_env {
	int64_t (*now)(void *ctx);                         /* unix seconds */
	void (*random)(void *ctx, uint8_t *out, size_t n); /* fresh randomness (nonces) */
	void *ctx;
} tix_env;

/*
 * Signature policy for commits arriving by pull (architecture §14.3). Each commit is checked
 * against the keys of the identity it is authored by (for an identity's own history, the
 * incoming version of that identity). History already here isn't re-checked.
 *   OFF: no checks.  WARN: report unsigned and bad commits, adopt anyway.
 *   REQUIRE: an unsigned or badly signed commit rejects the whole incoming ref.
 */
typedef enum tix_verify_policy { TIX_VERIFY_OFF = 0, TIX_VERIFY_WARN, TIX_VERIFY_REQUIRE } tix_verify_policy;
/* "off"/"warn"/"require" (also true/false/yes/no); *ok = false for anything else. */
tix_verify_policy tix_verify_policy_parse(const char *s, bool *ok);


typedef struct tix_repo_config {
	tix_store *store; /* BORROWS: must outlive the repo */
	tix_env env;
	tix_oid identity; /* the local user's identity, or null if none chosen yet */
	tix_index *index; /* BORROWS, optional: cache of replayed entities (M5); NULL = none */
	tix_verify_policy verify;
} tix_repo_config;

typedef struct tix_repo tix_repo;

/*
 * Opens core on a store. If cfg->identity is set it must load as a valid identity.
 * RETURNS TIX_ERR_INVALID for a corrupt local clock or a bad identity.
 * ENSURES on TIX_OK: *out owned by caller (tix_repo_close).
 */
tix_status tix_repo_open(tix_repo **out, const tix_repo_config *cfg, tix_error *err);
void tix_repo_close(tix_repo *r);

/* For long-running frontends: forget the store's caches and re-read the local Lamport
 * clock (another process may have advanced it), so the next calls see changes other
 * processes made. The clock never moves backwards. RETURNS TIX_ERR_INVALID for a corrupt
 * clock (state is unchanged). */
tix_status tix_repo_refresh(tix_repo *r, tix_error *err);

/* A fingerprint of every entity ref (refs/tix/(all): names and heads), as 16 lowercase hex
 * digits plus NUL. It changes whenever any entity is created or moves, so a frontend can
 * tell a page is stale (M7 live refresh). Not stored; not a security property. Call
 * tix_repo_refresh first to see other processes' changes. */
#define TIX_STAMP_LEN 17
tix_status tix_repo_stamp(tix_repo *r, char out[TIX_STAMP_LEN], tix_error *err);

/* The store the repo was opened on (BORROWED; for listing refs, e.g. ticket heads). */
tix_store *tix_repo_store(tix_repo *r);

/* Changes the signature policy of an open repo (e.g. one `tix pull --verify warn` to trust
 * a new clone's existing history on first use). */
void tix_repo_set_verify(tix_repo *r, tix_verify_policy v);
tix_verify_policy tix_repo_verify(const tix_repo *r);

/* RETURNS false (and leaves *out alone) when no identity is set. */
bool tix_repo_identity(const tix_repo *r, tix_oid *out);
/* Makes id the identity new ops are written as (in memory; persisting the choice,
 * e.g. git config tix.identity, is the frontend's job). The identity must load. */
tix_status tix_repo_set_identity(tix_repo *r, const tix_oid *id, tix_error *err);
/* No identity: writes fail with TIX_ERR_NOT_FOUND until one is set (hosted web mode sets
 * the requester's per request, §13.8). */
void tix_repo_clear_identity(tix_repo *r);

/* ---- generic entity operations ------------------------------------------ */

/*
 * Resolves a hex id or unique prefix (>= TIX_ID_MIN_PREFIX chars, lowercase).
 * RETURNS TIX_ERR_INVALID (malformed or too short), TIX_ERR_NOT_FOUND,
 * TIX_ERR_AMBIGUOUS.
 */
tix_status tix_entity_resolve(tix_repo *r, tix_entity_type t, const char *prefix, tix_oid *out,
			      tix_error *err);

/* Loads, validates and replays. With an index, an entity whose ref still points at
 * a head that validated before comes from the cache instead (a cache entry that
 * doesn't decode, or describes another entity, is ignored and replaced).
 * ENSURES on TIX_OK: *out owned by caller. */
tix_status tix_entity_load(tix_repo *r, tix_entity_type t, const tix_oid *id, tix_entity **out,
			   tix_error *err);

/*
 * Creates an entity. REQUIRES n >= 1. ops[0] must be the type's create op and no
 * other op may be one (else TIX_ERR_INVALID). A NULL nonce in ops[0] is filled
 * with fresh randomness (ops[0] is modified). Every type but identity needs an
 * identity set (else TIX_ERR_NOT_FOUND); identities are self-authored. The other
 * ops get the same up-front checks as tix_entity_apply.
 */
tix_status tix_entity_create(tix_repo *r, tix_entity_type t, tix_op *ops, size_t n, tix_oid *out,
			     tix_error *err);

/*
 * Appends ops as one op-pack, authored by the current identity, then advances
 * the clock. Retries automatically if another writer moves the ref meanwhile.
 * BORROWS ops.
 *
 * Up-front checks on local input (remote data is never rejected for these;
 * references to entities not fetched yet are fine there):
 *   - identities may only be edited by themselves, comments only by their
 *     author (TIX_ERR_INVALID; an unknown comment is TIX_ERR_NOT_FOUND);
 *   - parent, dependency, calendar, resource and identity references must name
 *     an existing entity of the right type (TIX_ERR_NOT_FOUND);
 *   - a parent that is the ticket itself or below it, or a dependency that
 *     would close a dependency cycle, is TIX_ERR_INVALID;
 *   - removing a dependency, assignment, calendar exception or stage definition
 *     that isn't there is TIX_ERR_NOT_FOUND;
 *   - a set-state whose stage isn't on the ticket's effective stage list (or is
 *     listed under another phase) is TIX_ERR_INVALID; with no list, any stage goes;
 *   - a claim while someone else's claim is live is TIX_ERR_CONFLICT; a claim must
 *     end after now and at most TIX_CLAIM_MAX_SECS later (TIX_ERR_INVALID); releasing
 *     an unclaimed ticket is TIX_ERR_NOT_FOUND, someone else's live claim
 *     TIX_ERR_CONFLICT (it ends with its lease);
 *   - a typed comment replying to a comment the ticket doesn't have is TIX_ERR_NOT_FOUND;
 *   - an agent identity may only set autonomy to halted (TIX_ERR_REFUSED);
 *   - the autonomy guard (§15.5): while a ticket's effective autonomy is autonomous, an
 *     agent identity may not put it into a stage tagged human, except in the blocked
 *     and done phases, which never hold up other work (TIX_ERR_REFUSED; decide and
 *     record an assumption, or abort instead).
 * Each set-state op is written right after a set-status op of the same phase
 * (added here when the caller didn't), so older readers see the phase (§15.1).
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

typedef enum tix_ticket_sort {
	TIX_SORT_NEWEST = 0, /* created desc, then id */
	TIX_SORT_UPDATED,    /* last change desc, then id */
	TIX_SORT_WBS,        /* tree order: parents before children, siblings by rank then id */
	TIX_SORT_STATUS,     /* in-progress, blocked, open, done, cancelled; then WBS order */
} tix_ticket_sort;

typedef struct tix_ticket_query {
	unsigned status_mask;    /* bit (1u << status); 0 = any */
	const char *stage;       /* only tickets in this stage (§15.1); NULL = any */
	const char *label;       /* NULL = any */
	const tix_oid *author;   /* NULL = any */
	const tix_oid *assignee; /* resource id; NULL = any */
	const tix_oid *under;    /* only tickets below this one in the WBS; NULL = any */
	const char *text;        /* id prefix (lowercase hex) or title substring, ASCII
				    case-insensitive; NULL or "" = any (M6) */
	tix_ticket_sort sort;    /* order of the result */
} tix_ticket_query;

/* Statuses still "open" in the broad sense: open, in-progress, blocked. */
#define TIX_STATUS_MASK_ACTIVE \
	((1u << TIX_TICKET_OPEN) | (1u << TIX_TICKET_IN_PROGRESS) | (1u << TIX_TICKET_BLOCKED))

/* All tickets matching q (NULL = all), ordered by q->sort (NULL: newest first). WBS
 * order uses every ticket's parent chain, also for parents the query filters out. */
tix_status tix_ticket_list(tix_repo *r, const tix_ticket_query *q, tix_entity_list *out, tix_error *err);
/* All identities, ordered by name, then id. */
tix_status tix_identity_list(tix_repo *r, tix_entity_list *out, tix_error *err);
/* All resources / calendars, ordered by name, then id. */
tix_status tix_resource_list(tix_repo *r, tix_entity_list *out, tix_error *err);
tix_status tix_calendar_list(tix_repo *r, tix_entity_list *out, tix_error *err);

/*
 * The stage list that applies to a ticket (architecture §15.1): its own, else that of
 * the nearest ancestor that has one. ENSURES on TIX_OK: *owner is that ticket (owned by
 * the caller), or NULL when no ticket up the chain has a list (any stage goes then).
 * parent: when not NULL, start the search there instead of at id's own parent (for a
 * ticket being created or moved); id may then be NULL.
 */
tix_status tix_ticket_stages(tix_repo *r, const tix_oid *id, const tix_oid *parent, tix_entity **owner,
			     tix_error *err);
/* The definition of phase/stage in owner's list, or NULL. */
const tix_stage_def *tix_stage_find(const tix_entity *owner, tix_ticket_status phase, const char *stage);

/* ---- autonomy (architecture §15.5) ---------------------------------------------- */

typedef struct tix_autonomy {
	bool set;               /* false: nothing up the chain sets it (supervised) */
	tix_autonomy_mode mode; /* effective: autonomous past its until reads as supervised */
	int64_t until;          /* -1 = no expiry */
	tix_oid owner;          /* the ticket carrying the setting */
} tix_autonomy;

/* The autonomy that applies to ticket id at now: its own setting, else the nearest
 * ancestor's. Never fails for an unresolved parent (supervised then). */
tix_status tix_ticket_autonomy(tix_repo *r, const tix_oid *id, int64_t now, tix_autonomy *out, tix_error *err);

/* ---- claims (architecture §15.3) ------------------------------------------------ */

/* A state to look for: a phase, and a stage (NULL = any stage of that phase, "" = none). */
typedef struct tix_ticket_state {
	tix_ticket_status status;
	const char *stage;
} tix_ticket_state;

typedef struct tix_claim_query {
	const tix_ticket_state *states; /* NULL/0: the open and in-progress phases */
	size_t nstates;
	const tix_oid *under; /* only below this ticket; NULL = anywhere */
	int64_t lease;        /* seconds, 1..TIX_CLAIM_MAX_SECS */
	const char *remote;   /* pull before, sync after; NULL = this repository only */
} tix_claim_query;

/*
 * Hands out work: the first ticket in WBS order that is in one of the states, isn't
 * claimed by anyone else (live), isn't in a stage tagged human, isn't under a halted
 * root (§15.5), and whose predecessors are all done or cancelled. A ticket you already hold that matches comes first (it is
 * renewed). With a remote: pulls first, then pushes the claim. The push decides a race:
 * when it is refused and the pull shows someone else's live claim on the same ticket,
 * this claim is released (handing the ticket to theirs, see tix_claim) and the next
 * candidate is tried (up to 5 tries).
 * RETURNS TIX_ERR_NOT_FOUND when there is no work; transport errors from sync.
 */
tix_status tix_claim_next(tix_repo *r, const tix_claim_query *q, tix_oid *out, tix_error *err);

/* RETURNS true if every predecessor of t is done or cancelled (an unknown one counts as
 * not done). */
bool tix_ticket_deps_done(tix_repo *r, const tix_entity *t);

/* ---- activity (architecture §15.6) ------------------------------------------------ */

/* One ticket op, for feeds. OWNS op, summary and title. */
typedef struct tix_activity {
	int64_t time;     /* pack time (as written by its author) */
	uint64_t lamport;
	tix_opid id;
	tix_oid ticket, author;
	char *title;      /* the ticket's current title */
	char *op;         /* wire name: "set-state", "typed-comment", ... */
	char *summary;    /* one line for people: "status open/ready", "[review] Two findings" */
} tix_activity;

typedef struct tix_activity_list {
	tix_activity *v;
	size_t n;
} tix_activity_list;

void tix_activity_list_free(tix_activity_list *l);

typedef struct tix_activity_query {
	int64_t since;            /* only ops whose pack time is >= since; 0 = all */
	const tix_oid *under;     /* only tickets below this one (and it); NULL = all */
	const tix_oid *ticket;    /* only this ticket; NULL = all */
	const tix_oid *after;     /* with ticket: only commits not reachable from this head */
	size_t limit;             /* keep the last N; 0 = all */
} tix_activity_query;

/* Ticket ops in total order (lamport, commit, line), oldest first. Tickets whose history
 * doesn't validate are skipped. */
tix_status tix_activity_log(tix_repo *r, const tix_activity_query *q, tix_activity_list *out, tix_error *err);

/* ---- planning (architecture §5) --------------------------------------------- */

typedef enum tix_rank_pos { TIX_RANK_FIRST, TIX_RANK_LAST, TIX_RANK_BEFORE, TIX_RANK_AFTER } tix_rank_pos;

/*
 * Computes a rank that puts `self` at pos among the children of `parent` (NULL =
 * the top level), relative to `anchor` for BEFORE/AFTER. Siblings are never
 * rewritten. Unranked siblings sort after ranked ones, so LAST means "after the
 * last ranked sibling". RETURNS TIX_ERR_INVALID if the anchor isn't a sibling
 * or has no rank yet, TIX_ERR_CONFLICT if there is no room (see tix_rank_between).
 */
tix_status tix_ticket_rank(tix_repo *r, const tix_oid *parent, const tix_oid *self, tix_rank_pos pos,
			   const tix_oid *anchor, tix_buf *out, tix_error *err);

/*
 * Baselines (architecture §14.2): snapshots of the computed plan that sync like tickets.
 * tix_baseline_take plans from root (NULL = everything) as of today and stores every
 * task's dates, duration, effort and cost. Large plans span several packs, written as a
 * chain and published with one ref update. *ntasks (optional) receives the task count.
 * RETURNS TIX_ERR_NOT_FOUND without an identity or for an unknown root.
 * Rename with tix_entity_apply(TIX_ENTITY_BASELINE, ..., tix_op_text(TIX_OP_BASELINE_SET_NAME, ..)).
 */
tix_status tix_baseline_take(tix_repo *r, const char *name, const tix_oid *root, tix_date today, tix_oid *out,
			     size_t *ntasks, tix_error *err);
/* All baselines, newest first (taken date, then creation time, then id). */
tix_status tix_baseline_list(tix_repo *r, tix_entity_list *out, tix_error *err);

/* Signature audit of one entity's whole local history (architecture §14.3). */
typedef struct tix_verify_report {
	size_t good, unsigned_commits, bad;
} tix_verify_report;
/* Adds to *rep (callers may sum over many entities). RETURNS TIX_ERR_NOT_FOUND for an
 * unknown entity, TIX_ERR_IO when a verifier (ssh-keygen, gpg) can't run. */
tix_status tix_entity_verify(tix_repo *r, tix_entity_type t, const tix_oid *id, tix_verify_report *rep,
			     tix_error *err);

/* A schedule plus the entities it borrows. Free with tix_plan_result_free. */
typedef struct tix_plan_result {
	tix_schedule *schedule;
	tix_entity_list tickets, resources, calendars;
} tix_plan_result;

void tix_plan_result_free(tix_plan_result *res);

/* Loads everything and runs the planner for root (NULL = all tickets). `today`
 * comes from the caller (core never reads the clock). */
tix_status tix_plan(tix_repo *r, const tix_oid *root, tix_date today, tix_plan_result *out, tix_error *err);

/* ---- sync (architecture §6) ------------------------------------------------- */

typedef struct tix_sync_report {
	size_t created;   /* entities new to this clone */
	size_t forwarded; /* local ref fast-forwarded to the remote's */
	size_t merged;    /* diverged histories joined by a merge commit */
	size_t rejected;  /* remote refs that failed validation (never adopted) */
	size_t skipped;   /* refs of a type this version doesn't know */
	size_t pushed;    /* refs sent by the last push */
	size_t unpushable; /* local refs that failed validation (never pushed) */
	size_t unsigned_commits, bad_signatures; /* incoming, when verifying (warn or require) */
	tix_strs problems; /* one line per rejected/unpushable ref, sorted */
} tix_sync_report;

void tix_sync_report_free(tix_sync_report *rep);

/*
 * Fetches `remote` into refs/tix-remotes/<remote>/, validates every fetched
 * entity, and integrates each valid one into refs/tix/: create, fast-forward,
 * or a merge commit (header-only pack, authored by the identity if one is set).
 * Never adopts history that fails validation; never rewrites local history.
 * The clock is advanced past everything adopted.
 * REQUIRES rep zero-initialised (counts accumulate across calls).
 * RETURNS transport errors from the store; a rejected ref is not an error (it is
 * counted in rep).
 */
tix_status tix_pull(tix_repo *r, const char *remote, tix_sync_report *rep, tix_error *err);

/*
 * Pushes every valid local entity ref that differs from the last-known remote
 * value (refs/tix-remotes/<remote>/), never forced, then records the pushed
 * values there. A ref whose last-known remote value is not in its history is
 * not pushed (reported in rep: pull first; after a pull it means the remote's
 * history was rejected). RETURNS TIX_ERR_CONFLICT when the remote moved since
 * the last fetch (pull, then push again).
 */
tix_status tix_push(tix_repo *r, const char *remote, tix_sync_report *rep, tix_error *err);

/* pull, then push; on TIX_ERR_CONFLICT pull and push again (up to 3 rounds). */
tix_status tix_sync(tix_repo *r, const char *remote, tix_sync_report *rep, tix_error *err);

/* ---- conveniences ----------------------------------------------------------- */

tix_status tix_identity_create(tix_repo *r, const char *name, const char *email, tix_oid *out,
			       tix_error *err);
tix_status tix_ticket_create(tix_repo *r, const char *title, const char *body, const tix_strs *labels,
			     tix_oid *out, tix_error *err);
tix_status tix_resource_create(tix_repo *r, const char *name, tix_resource_kind kind, tix_oid *out,
			       tix_error *err);
tix_status tix_calendar_create(tix_repo *r, const char *name, tix_oid *out, tix_error *err);

#endif
