/*
 * Internal header of the tix CLI (a composition root: it may use any module).
 * Commands parse argv, call core, and format output. Business rules live in core.
 */
#ifndef TIX_CLI_H
#define TIX_CLI_H

#include "tix/core.h"
#include "tix/export.h"
#include "tix/index_sqlite.h"
#include "tix/input.h"
#include "tix/store_libgit.h"
#include "tix/base_json.h"

#include <stdio.h>

enum { EXIT_OK = 0, EXIT_ERR = 1, EXIT_USAGE = 2, EXIT_NO_WORK = 3 };

typedef struct cli {
	tix_store *store;
	tix_index *index; /* NULL when disabled ($TIX_NO_INDEX) or unavailable */
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

/* ---- field values (planopts.c; the grammar itself is tix/input.h) ---- */

/* Today's local date, or $TIX_TODAY (YYYY-MM-DD) when set. */
tix_date cli_today(void);
tix_input_ctx cli_input_ctx(cli *c);
/* Reports a failed tix_input call: malformed text is a usage error printed as
 * "tix: <prefix><message>" (EXIT_USAGE), anything else goes to cli_fail (EXIT_ERR). */
int cli_input_fail(cli *c, tix_status st, bool syntax, const char *prefix);

#define CLI_NOT_MINE (-1)
/* Consumes "--FIELD VALUE" at argv[*i] when FIELD is an input field of t (while creating,
 * name, active and clear are not). RETURNS CLI_NOT_MINE if it isn't one, EXIT_OK if
 * consumed, else an exit code (message printed). */
int cli_input_opt(cli *c, tix_entity_type t, bool creating, int argc, char **argv, int *i, tix_input *in);

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
int cmd_warnings(cli *c, int argc, char **argv);
int cmd_export(cli *c, int argc, char **argv);
int cmd_index(cli *c, int argc, char **argv);
int cmd_baseline(cli *c, int argc, char **argv);
int cmd_verify(cli *c, int argc, char **argv);
int cmd_bridge(cli *c, int argc, char **argv);
int cmd_stage(cli *c, int argc, char **argv);
int cmd_claim(cli *c, int argc, char **argv);
int cmd_note(cli *c, int argc, char **argv);
int cmd_autonomy(cli *c, int argc, char **argv);
int cmd_log(cli *c, int argc, char **argv);
int cmd_watch(cli *c, int argc, char **argv);
/* {"id","short","title","status","stage","labels","parent","claim":{..}|null,"updated"} */
void cli_json_ticket(cli *c, tix_jw *w, const tix_entity *e, int64_t now);
/* "<hex prefix>:<line>" against e's comments; prints errors. RETURNS EXIT_OK on success. */
int cli_resolve_comment(const tix_entity *e, const char *arg, tix_opid *out);
/* Prints a typed comment's fields, items (with their current state), questions and updates. */
void cli_print_note(const tix_ticket *t, const tix_comment *cm);

#endif
