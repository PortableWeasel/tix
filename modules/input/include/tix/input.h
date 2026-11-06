/*
 * input: the text forms of field values, shared by every frontend (CLI options, web forms).
 *
 * A frontend hands over (entity type, field name, text) and gets ops back, so the CLI and the
 * web UI accept and reject exactly the same input with the same messages. Pure apart from
 * id resolution through core: no I/O, no clock ("today" is passed in), no env.
 *
 * Field names are the CLI option names without "--":
 *   ticket:   start finish deadline actual-start actual-finish parent calendar rank kind mode
 *             duration effort progress dep
 *   resource: name kind capacity calendar identity rate active
 *   calendar: name weekdays hours off on clear
 *
 * Errors: TIX_ERR_INVALID with a message "<field>: <what is wrong>" when the text is
 * malformed (tix_input.syntax is then true), or core's status (NOT_FOUND, AMBIGUOUS, ...)
 * when a well-formed id doesn't resolve.
 */
#ifndef TIX_INPUT_H
#define TIX_INPUT_H

#include "tix/core.h"

typedef struct tix_input_ctx {
	tix_repo *repo; /* BORROWS; resolves ids and "me" */
	tix_date today; /* what "today" and "tomorrow" mean */
} tix_input_ctx;

/* Ops collected for one create or edit. Zero-initialise; free with tix_input_free. */
typedef struct tix_input {
	TIX_VEC(tix_op) ops;
	bool syntax;             /* last failure was malformed text (vs. an id that didn't resolve) */
	bool rank_set, parent_set;
	tix_rank_pos rank_pos;
	tix_oid rank_anchor;
	tix_oid parent;          /* null when set to none */
} tix_input;

/* True when `field` is an input field of entity type t. */
bool tix_input_has(tix_entity_type t, const char *field);

/* Parses one field's text and appends its op(s) to in->ops (rank waits for tix_input_finish).
 * REQUIRES tix_input_has(t, field). */
tix_status tix_input_field(const tix_input_ctx *ctx, tix_entity_type t, const char *field, const char *text,
			   tix_input *in, tix_error *err);

/* Adds the set-rank op once the parent is known. self is NULL while creating; current_parent
 * is the ticket's parent before this edit (NULL or null oid for a root). */
tix_status tix_input_finish(const tix_input_ctx *ctx, tix_input *in, const tix_oid *self,
			    const tix_oid *current_parent, tix_error *err);

void tix_input_free(tix_input *in);

/* "RES[:50%]" plus optional effort text ("8h", NULL for none) -> an assign op.
 * Messages start with "assign: "; *syntax as for tix_input.syntax. */
tix_status tix_input_assignment(const tix_input_ctx *ctx, const char *res_units, const char *effort, tix_op *out,
				bool *syntax, tix_error *err);

/* ---- the value grammar on its own ---- */

/* YYYY-MM-DD, "today" or "tomorrow". */
bool tix_input_date(const char *s, tix_date today, tix_date *out);
/* "3" or "3<unit>" (unit may be ""), 0..max. */
bool tix_input_count(const char *s, const char *unit, int64_t max, int64_t *out);
/* "none" or "-" clear a value. */
bool tix_input_is_none(const char *s);
/* "mon-fri", "mon,wed,fri", "mon-thu,sat" -> bit 0 = Monday. */
bool tix_input_weekdays(const char *s, int64_t *mask);
/* "FROM[..TO][=NOTE]"; note may be NULL when not wanted (then "=NOTE" is still accepted). */
bool tix_input_range(const char *s, tix_date today, tix_date *from, tix_date *to, const char **note);
/* "95" or "95.50" -> 9550 (minor units, at most two decimals). */
bool tix_input_amount(const char *s, int64_t *out);

#endif
