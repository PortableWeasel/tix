/*
 * tix/bridge.h -- issue-tracker bridges (after v1, architecture §14.4).
 *
 * The port: a tix_tracker is one repository's issues in another tracker (Forgejo first).
 * The algorithm (tix_bridge_run) links tickets to issues through the ticket field
 * `external` ("<tracker key>#<number>"), which syncs with tix, so every clone knows the
 * links. Issues tix creates end with a marker comment "<!-- tix:<ticket id> -->".
 *
 * Changes are detected against a per-clone base (what both sides looked like after the
 * last bridge run here, stored as local state): a field changed on one side is copied to
 * the other; changed on both sides (or never synced from this clone and different) it is
 * a conflict, reported and left alone on both sides. Nothing is ever deleted.
 */
#ifndef TIX_BRIDGE_H
#define TIX_BRIDGE_H

#include "tix/core.h"
#include "tix/store.h"

typedef struct tix_issue {
	int64_t number;
	char *title;
	char *body;     /* without tix's marker line */
	bool closed;
	tix_strs labels;
	char *marker;   /* ticket id from tix's marker, NULL if none (OWNS) */
} tix_issue;

typedef struct tix_issues {
	tix_issue *v;
	size_t n;
} tix_issues;

typedef struct tix_issue_comment {
	int64_t id;
	char *author;
	char *body;     /* without tix's marker line */
	char *marker;   /* tix comment op id ("<hex>:<line>") if tix posted it, else NULL */
} tix_issue_comment;

typedef struct tix_issue_comments {
	tix_issue_comment *v;
	size_t n;
} tix_issue_comments;

void tix_issue_free(tix_issue *i);
void tix_issues_free(tix_issues *l);
void tix_issue_comments_free(tix_issue_comments *l);

/* What to change on an issue; NULL / unset members stay as they are. */
typedef struct tix_issue_update {
	const char *title, *body;  /* body: the text before the marker; the adapter keeps a marker */
	const char *marker;        /* ticket id to (re)write as the marker, NULL = leave */
	bool set_closed, closed;
	bool set_labels;
	const tix_strs *labels;    /* names; ones the tracker doesn't have are skipped */
} tix_issue_update;

typedef struct tix_tracker tix_tracker;

typedef struct tix_tracker_vt {
	/* Every issue (not pull requests), any state. */
	tix_status (*list)(tix_tracker *t, tix_issues *out, tix_error *err);
	/* The label names this repository has (tickets' other labels are not bridged). */
	tix_status (*labels)(tix_tracker *t, tix_strs *out, tix_error *err);
	tix_status (*create)(tix_tracker *t, const tix_issue_update *in, int64_t *number, tix_error *err);
	tix_status (*update)(tix_tracker *t, int64_t number, const tix_issue_update *u, tix_error *err);
	tix_status (*comments)(tix_tracker *t, int64_t number, tix_issue_comments *out, tix_error *err);
	/* Posts body with marker (a tix op id) appended as a marker comment. */
	tix_status (*comment)(tix_tracker *t, int64_t number, const char *body, const char *marker, int64_t *id,
			      tix_error *err);
	void (*destroy)(tix_tracker *t);
} tix_tracker_vt;

struct tix_tracker {
	const tix_tracker_vt *vt;
	const char *key; /* "forgejo:host/owner/repo": prefix of every ticket's `external` */
};

static inline void tix_tracker_destroy(tix_tracker *t)
{
	if (t)
		t->vt->destroy(t);
}

/* Markers: "<!-- tix:<id> -->" on its own last line. */
#define TIX_BRIDGE_MARKER_OPEN "<!-- tix:"
#define TIX_BRIDGE_MARKER_CLOSE " -->"
/* Splits text into the part before a trailing marker and the marker's value (OWNS both;
 * *marker NULL when there is none). Adapters use it on issue and comment bodies. */
void tix_bridge_split_marker(const char *text, char **body, char **marker);
/* text + "\n\n<!-- tix:<marker> -->" into out. */
void tix_bridge_add_marker(const char *text, const char *marker, tix_buf *out);

typedef enum tix_bridge_mode { TIX_BRIDGE_PULL = 1, TIX_BRIDGE_PUSH = 2, TIX_BRIDGE_SYNC = 3 } tix_bridge_mode;

typedef struct tix_bridge_opts {
	tix_bridge_mode mode;
	/* Which unlinked tickets push may export (linked ones always sync): those below
	 * `under`, or with `label`, or every ticket when `all`. None set: none are exported. */
	const tix_oid *under;
	const char *label;
	bool all;
	bool comments; /* also copy new comments both ways */
	bool dry_run;  /* report what would happen; change nothing anywhere */
} tix_bridge_opts;

typedef struct tix_bridge_report {
	size_t tickets_created, issues_created, tickets_updated, issues_updated;
	size_t comments_in, comments_out;
	tix_strs actions;   /* one line per change (or planned change, in a dry run) */
	tix_strs conflicts; /* one line per field left alone because both sides changed */
} tix_bridge_report;

void tix_bridge_report_free(tix_bridge_report *rep);

/*
 * One bridge run between the repo and tracker t. store holds the per-clone base (local
 * state "bridge-<hash of t->key>"). RETURNS the first tracker or repo error (what was done
 * before it stays done and is in the report; the base records only completed work).
 */
tix_status tix_bridge_run(tix_repo *r, tix_store *store, tix_tracker *t, const tix_bridge_opts *o,
			  tix_bridge_report *rep, tix_error *err);

#endif
