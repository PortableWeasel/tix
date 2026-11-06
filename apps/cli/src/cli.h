/*
 * Internal header of the tix CLI (a composition root: it may use any module).
 * Commands parse argv, call core, and format output. Business rules live in core.
 */
#ifndef TIX_CLI_H
#define TIX_CLI_H

#include "tix/core.h"
#include "tix/export.h"
#include "tix/store_libgit.h"

#include <stdio.h>

enum { EXIT_OK = 0, EXIT_ERR = 1, EXIT_USAGE = 2 };

typedef struct cli {
	tix_store *store;
	tix_repo *repo;
	tix_error err;
} cli;

/* Prints "tix: <msg>" for a failed status. RETURNS EXIT_ERR. */
int cli_fail(cli *c, tix_status st);
/* Prints "tix: <fmt>" plus usage hint. RETURNS EXIT_USAGE. */
int cli_usage(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Resolves an id argument; prints errors. RETURNS EXIT_OK on success. */
int cli_resolve(cli *c, tix_entity_type t, const char *arg, tix_oid *out);
/* Short display id (first TIX_ID_MIN_PREFIX hex chars). REQUIRES out has 16 bytes. */
const char *cli_short(const tix_oid *id, char *out);
/* "2026-11-06 14:03 UTC". REQUIRES out has 32 bytes. */
const char *cli_time(int64_t t, char *out);
/* Display name for an identity id ("Ada Lovelace" or the short id if unknown). */
const char *cli_who(cli *c, const tix_oid *id, char *out, size_t n);

/* Opens $TIX_EDITOR / core.editor / $VISUAL / $EDITOR / vi on a template.
 * Lines starting with '#' are dropped. RETURNS EXIT_OK and fills out. */
int cli_edit_text(cli *c, const char *template_text, tix_buf *out);

/* ---- planning values (planopts.c) ---- */

/* Today's local date, or $TIX_TODAY (YYYY-MM-DD) when set. */
tix_date cli_today(void);
/* YYYY-MM-DD, "today" or "tomorrow". */
bool cli_parse_date(const char *s, tix_date *out);
/* "3" or "3<unit>", 0..max. */
bool cli_parse_count(const char *s, const char *unit, int64_t max, int64_t *out);
/* "none" or "-" clear a value. */
bool cli_is_none(const char *s);

/* Planning options collected for one `new` / `edit`. */
typedef struct cli_plan {
	TIX_VEC(tix_op) ops;
	bool rank_set, parent_set;
	tix_rank_pos rank_pos;
	tix_oid rank_anchor;
	tix_oid parent; /* null when set to none */
} cli_plan;

#define CLI_NOT_MINE (-1)
/* Consumes a planning option at argv[*i] (and its value). RETURNS CLI_NOT_MINE if
 * argv[*i] isn't one, EXIT_OK if consumed, else an exit code (message printed). */
int cli_plan_opt(cli *c, int argc, char **argv, int *i, cli_plan *pl);
/* Adds the set-rank op once the parent is known (self NULL while creating). */
int cli_plan_finish(cli *c, cli_plan *pl, const tix_oid *self, const tix_oid *current_parent);
void cli_plan_free(cli_plan *pl);

int cmd_id(cli *c, int argc, char **argv);
int cmd_new(cli *c, int argc, char **argv);
int cmd_ls(cli *c, int argc, char **argv);
int cmd_show(cli *c, int argc, char **argv);
int cmd_comment(cli *c, int argc, char **argv);
int cmd_edit(cli *c, int argc, char **argv);
int cmd_pull(cli *c, int argc, char **argv);
int cmd_push(cli *c, int argc, char **argv);
int cmd_sync(cli *c, int argc, char **argv);
int cmd_assign(cli *c, int argc, char **argv);
int cmd_unassign(cli *c, int argc, char **argv);
int cmd_res(cli *c, int argc, char **argv);
int cmd_cal(cli *c, int argc, char **argv);
int cmd_wbs(cli *c, int argc, char **argv);
int cmd_plan(cli *c, int argc, char **argv);
int cmd_export(cli *c, int argc, char **argv);

#endif
