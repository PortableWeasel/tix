/*
 * Internal header of the tix CLI (a composition root: it may use any module).
 * Commands parse argv, call core, and format output. Business rules live in core.
 */
#ifndef TIX_CLI_H
#define TIX_CLI_H

#include "tix/core.h"
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

int cmd_id(cli *c, int argc, char **argv);
int cmd_new(cli *c, int argc, char **argv);
int cmd_ls(cli *c, int argc, char **argv);
int cmd_show(cli *c, int argc, char **argv);
int cmd_comment(cli *c, int argc, char **argv);
int cmd_edit(cli *c, int argc, char **argv);

#endif
