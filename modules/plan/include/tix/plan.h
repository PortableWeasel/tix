/*
 * tix/plan.h -- the scheduling engine (architecture §5). Pure: no I/O, no clock.
 *
 * Input: replayed tickets, resources and calendars (any order), an optional
 * WBS root and "today". Output: a tix_schedule with, per ticket, its WBS code,
 * computed dates, float and critical flag, summary rollups, per-resource daily
 * load, and diagnostics for everything that had to be ignored or broken.
 *
 * Conventions (v1: whole working days):
 *   - A task of duration D >= 1 starting on working day S covers D working days of
 *     its calendar; `finish` is the last of them (inclusive).
 *   - Milestones (kind milestone, or duration 0) have start == finish.
 *   - Lags are working days of the successor's calendar, may be negative.
 *   - `start` on a ticket means "start no earlier than" (auto) or the fixed start
 *     (manual); a summary's start constrains every task below it.
 *   - Explicit actual_start / actual_finish fix a task where it really happened.
 *   - Duration: `duration` if set; else effort / sum(units x capacity x hours/day)
 *     over the assignments, rounded up to whole days; else unscheduled.
 *   - Dependencies on or of summary tasks apply to every task below them.
 *   - Cycles (WBS parents or dependencies) are broken by dropping the edge whose
 *     op is greatest in total order, so every clone drops the same edge.
 *
 * Determinism: the result depends only on the inputs' content, never on their
 * order in the input arrays.
 */
#ifndef TIX_PLAN_H
#define TIX_PLAN_H

#include "tix/model_replay.h"

#define TIX_PLAN_MAX_LOAD_DAYS 3660 /* resource load is computed for at most ~10 years */
#define TIX_PLAN_WBS_MAX 96

typedef struct tix_plan_input {
	const tix_entity *const *tickets; /* BORROWS (must outlive the schedule) */
	size_t ntickets;
	const tix_entity *const *resources;
	size_t nresources;
	const tix_entity *const *calendars;
	size_t ncalendars;
	const tix_oid *root; /* NULL = every ticket (all WBS roots) */
	tix_date today;      /* project start when no task has a start date */
} tix_plan_input;

typedef struct tix_plan_task {
	const tix_entity *ticket;   /* BORROWS */
	const tix_entity *calendar; /* effective (inherited) calendar, NULL = built-in Mon-Fri 8h */
	size_t parent;              /* index into tasks; SIZE_MAX for the plan's top level */
	size_t depth;               /* 0 at the top level */
	char wbs[TIX_PLAN_WBS_MAX]; /* "1.3.2" */
	bool summary;               /* has children (derived, never stored) */
	bool milestone;
	bool manual;     /* fixed by manual mode or actual dates */
	bool scheduled;  /* has dates (a duration could be determined) */
	bool critical;   /* total float <= 0 */
	bool late;       /* finishes after its deadline */
	tix_date start, finish;           /* TIX_DATE_NONE when unscheduled */
	tix_date late_start, late_finish; /* from the backward pass */
	int32_t total_float;              /* working days */
	int64_t duration;                 /* working days, -1 unknown */
	int64_t effort;                   /* hours (summaries: rolled up), -1 unknown */
	int progress;                     /* percent (summaries: effort-weighted) */
	tix_date actual_start, actual_finish; /* explicit, else derived from status changes */
	int64_t cost;          /* minor currency units from rated assignments (summaries: summed); -1 unknown */
	const char *currency;  /* of cost (BORROWED from a resource); NULL when unknown or mixed */
} tix_plan_task;

typedef struct tix_plan_diag {
	tix_oid id; /* the ticket (or resource/calendar) concerned; null for plan-wide */
	char *msg;  /* OWNS */
} tix_plan_diag;

typedef struct tix_plan_load {
	const tix_entity *resource; /* BORROWS */
	int32_t *units;  /* OWNS; per day of the schedule span: sum of assigned percent */
	bool *working;   /* OWNS; per day: a working day in the resource's calendar */
	size_t over_days; /* days where units > 100 (or > 0 on a non-working day) */
	int32_t peak;
} tix_plan_load;

typedef struct tix_schedule {
	tix_plan_task *tasks; /* WBS pre-order */
	size_t ntasks;
	tix_plan_diag *diags; /* in discovery order, deterministic */
	size_t ndiags;
	tix_plan_load *loads; /* one per assigned resource, sorted by resource id */
	size_t nloads;
	tix_date start, finish; /* span of scheduled tasks (inclusive), NONE if none */
	size_t ndays;           /* length of each load array (<= TIX_PLAN_MAX_LOAD_DAYS) */
	const tix_entity **resources; /* the input's, sorted by id (array OWNED, entities BORROWED) */
	size_t nresources;
	const tix_entity **calendars; /* likewise (exports that carry calendars, e.g. MSPDI) */
	size_t ncalendars;
} tix_schedule;

/*
 * RETURNS TIX_ERR_NOT_FOUND if in->root is set but not among the tickets.
 * Everything else (cycles, dangling references, missing calendars) is a
 * diagnostic, never an error. ENSURES on TIX_OK: *out owned by the caller.
 */
tix_status tix_plan_compute(const tix_plan_input *in, tix_schedule **out, tix_error *err);
void tix_schedule_free(tix_schedule *s);

/* How a task moved since a baseline (architecture §14.2). Slips are in the task's
 * working days, positive = later than baselined. */
typedef struct tix_plan_variance {
	bool baselined;      /* the baseline has this task (false: added since) */
	bool dates;          /* both were scheduled: the slips are valid */
	int32_t start_slip, finish_slip;
	tix_date start, finish;     /* the baseline's dates (TIX_DATE_NONE if it had none) */
	int64_t duration, effort, cost; /* the baseline's values (-1 unknown) */
	const char *currency;           /* the baseline's (BORROWED from it) */
	bool has_duration, has_effort, has_cost; /* both sides known (cost: same currency) */
	int64_t duration_delta, effort_delta, cost_delta;
} tix_plan_variance;

/*
 * Compares s with baseline b: out[i] describes s->tasks[i] (REQUIRES out has s->ntasks
 * entries). RETURNS how many baselined tasks are no longer in the plan. Pure.
 */
size_t tix_plan_compare(const tix_schedule *s, const tix_baseline *b, tix_plan_variance *out);

/* Working-day test for a calendar entity (NULL = built-in Mon-Fri). Later
 * exceptions win where they overlap. */
bool tix_plan_is_working(const tix_entity *calendar, tix_date d);
/* RETURNS the resource entity with that id, or NULL. */
const tix_entity *tix_schedule_resource(const tix_schedule *s, const tix_oid *id);
/* RETURNS the index of the task for ticket id, or SIZE_MAX. */
size_t tix_schedule_find(const tix_schedule *s, const tix_oid *id);

#endif
