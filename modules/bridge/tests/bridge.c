/* The bridge algorithm against an in-memory tracker (architecture §14.4). */
#include "tix/bridge.h"
#include "tix/store_mem.h"
#include "tix/testkit.h"

#include <stdlib.h>
#include <string.h>

static tix_error e = TIX_ERROR_INIT;

/* ---- a fake tracker ------------------------------------------------------------------ */

typedef struct fcomment {
	int64_t issue, id;
	char *author, *body, *marker;
} fcomment;

typedef struct fake {
	tix_tracker base;
	TIX_VEC(tix_issue) issues;
	tix_strs universe;
	TIX_VEC(fcomment) comments;
	int64_t next_comment;
	int creates, updates;
} fake;

static fake *F(tix_tracker *t) { return (fake *)t; }

static tix_issue *find(fake *f, int64_t n)
{
	for (size_t i = 0; i < f->issues.n; i++)
		if (f->issues.v[i].number == n)
			return &f->issues.v[i];
	return NULL;
}

static void copy_issue(const tix_issue *in, tix_issue *out)
{
	out->number = in->number;
	out->title = tix_xstrdup(in->title);
	out->body = tix_xstrdup(in->body);
	out->closed = in->closed;
	tix_strs_copy(&out->labels, &in->labels);
	out->marker = in->marker ? tix_xstrdup(in->marker) : NULL;
}

static tix_status f_list(tix_tracker *t, tix_issues *out, tix_error *err)
{
	(void)err;
	fake *f = F(t);
	out->n = f->issues.n;
	out->v = tix_xcalloc(out->n ? out->n : 1, sizeof(tix_issue));
	for (size_t i = 0; i < out->n; i++)
		copy_issue(&f->issues.v[i], &out->v[i]);
	return TIX_OK;
}

static tix_status f_labels(tix_tracker *t, tix_strs *out, tix_error *err)
{
	(void)err;
	tix_strs_copy(out, &F(t)->universe);
	return TIX_OK;
}

static void apply_update(fake *f, tix_issue *i, const tix_issue_update *u)
{
	if (u->title) {
		free(i->title);
		i->title = tix_xstrdup(u->title);
	}
	if (u->body) {
		free(i->body);
		i->body = tix_xstrdup(u->body);
	}
	if (u->marker) {
		free(i->marker);
		i->marker = tix_xstrdup(u->marker);
	}
	if (u->set_closed)
		i->closed = u->closed;
	if (u->set_labels) {
		tix_strs_free(&i->labels);
		for (size_t k = 0; k < u->labels->n; k++)
			if (tix_strs_contains(&f->universe, u->labels->v[k]))
				tix_strs_add(&i->labels, u->labels->v[k]);
	}
}

static tix_status f_create(tix_tracker *t, const tix_issue_update *in, int64_t *number, tix_error *err)
{
	(void)err;
	fake *f = F(t);
	tix_issue n = {.number = (int64_t)f->issues.n + 1, .title = tix_xstrdup(""), .body = tix_xstrdup("")};
	apply_update(f, &n, in);
	TIX_VEC_PUSH(&f->issues, n);
	*number = n.number;
	f->creates++;
	return TIX_OK;
}

static tix_status f_update(tix_tracker *t, int64_t number, const tix_issue_update *u, tix_error *err)
{
	tix_issue *i = find(F(t), number);
	if (!i)
		return tix_error_set(err, TIX_ERR_NOT_FOUND, "no issue %lld", (long long)number);
	apply_update(F(t), i, u);
	F(t)->updates++;
	return TIX_OK;
}

static tix_status f_comments(tix_tracker *t, int64_t number, tix_issue_comments *out, tix_error *err)
{
	(void)err;
	fake *f = F(t);
	out->v = tix_xcalloc(f->comments.n ? f->comments.n : 1, sizeof(tix_issue_comment));
	for (size_t i = 0; i < f->comments.n; i++)
		if (f->comments.v[i].issue == number) {
			tix_issue_comment *c = &out->v[out->n++];
			c->id = f->comments.v[i].id;
			c->author = tix_xstrdup(f->comments.v[i].author);
			c->body = tix_xstrdup(f->comments.v[i].body);
			c->marker = f->comments.v[i].marker ? tix_xstrdup(f->comments.v[i].marker) : NULL;
		}
	return TIX_OK;
}

static void add_comment(fake *f, int64_t issue, const char *author, const char *body, const char *marker)
{
	fcomment c = {issue, ++f->next_comment, tix_xstrdup(author), tix_xstrdup(body), marker ? tix_xstrdup(marker) : NULL};
	TIX_VEC_PUSH(&f->comments, c);
}

static tix_status f_comment(tix_tracker *t, int64_t number, const char *body, const char *marker, int64_t *id,
			    tix_error *err)
{
	(void)err;
	add_comment(F(t), number, "tix-bot", body, marker);
	*id = F(t)->next_comment;
	return TIX_OK;
}

static void f_destroy(tix_tracker *t)
{
	fake *f = F(t);
	for (size_t i = 0; i < f->issues.n; i++)
		tix_issue_free(&f->issues.v[i]);
	free(f->issues.v);
	tix_strs_free(&f->universe);
	for (size_t i = 0; i < f->comments.n; i++) {
		free(f->comments.v[i].author);
		free(f->comments.v[i].body);
		free(f->comments.v[i].marker);
	}
	free(f->comments.v);
}

static const tix_tracker_vt fake_vt = {f_list, f_labels, f_create, f_update, f_comments, f_comment, f_destroy};

static void add_issue(fake *f, const char *title, const char *body, bool closed, const char *label, const char *marker)
{
	tix_issue n = {.number = (int64_t)f->issues.n + 1, .title = tix_xstrdup(title), .body = tix_xstrdup(body),
		       .closed = closed, .marker = marker ? tix_xstrdup(marker) : NULL};
	if (label)
		tix_strs_add(&n.labels, label);
	TIX_VEC_PUSH(&f->issues, n);
}

/* ---- repo plumbing --------------------------------------------------------------------- */

typedef struct fake_env {
	int64_t now;
	uint64_t seed;
} fake_env;

static int64_t fake_now(void *ctx) { return ((fake_env *)ctx)->now += 60; }
static void fake_random(void *ctx, uint8_t *out, size_t n)
{
	fake_env *f = ctx;
	for (size_t i = 0; i < n; i++) {
		f->seed = f->seed * 6364136223846793005ull + 1442695040888963407ull;
		out[i] = (uint8_t)(f->seed >> 56);
	}
}

static tix_entity *ticket(tix_repo *r, const tix_oid *id)
{
	tix_entity *t = NULL;
	TK_OK(tix_entity_load(r, TIX_ENTITY_TICKET, id, &t, &e), &e);
	return t;
}

/* The ticket linked to issue n ("fake:x/y#n"), or NULL. */
static tix_entity *linked(tix_repo *r, int64_t n)
{
	tix_entity_list l;
	char want[64];
	snprintf(want, sizeof(want), "fake:x/y#%lld", (long long)n);
	TK_OK(tix_ticket_list(r, NULL, &l, &e), &e);
	tix_entity *found = NULL;
	for (size_t i = 0; i < l.n; i++)
		if (l.v[i]->u.ticket.plan.external && !strcmp(l.v[i]->u.ticket.plan.external, want) && !found) {
			found = l.v[i];
			l.v[i] = NULL;
		}
	for (size_t i = 0; i < l.n; i++)
		tix_entity_free(l.v[i]);
	free(l.v);
	return found;
}

static bool has_line(const tix_strs *s, const char *needle)
{
	for (size_t i = 0; i < s->n; i++)
		if (strstr(s->v[i], needle))
			return true;
	return false;
}

static tix_bridge_report run_bridge(tix_repo *r, tix_store *s, fake *f, tix_bridge_opts o)
{
	tix_bridge_report rep = {0};
	TK_OK(tix_bridge_run(r, s, &f->base, &o, &rep, &e), &e);
	if (getenv("BRIDGE_DEBUG")) {
		for (size_t i = 0; i < rep.actions.n; i++)
			fprintf(stderr, "  action: %s\n", rep.actions.v[i]);
		for (size_t i = 0; i < rep.conflicts.n; i++)
			fprintf(stderr, "  conflict: %s\n", rep.conflicts.v[i]);
		fprintf(stderr, "--\n");
	}
	return rep;
}

static void edit_text(tix_repo *r, const tix_oid *id, tix_op_kind k, const char *v)
{
	tix_op op = tix_op_text(k, v);
	TK_OK(tix_entity_apply(r, TIX_ENTITY_TICKET, id, &op, 1, &e), &e);
	tix_op_free(&op);
}

static void set_issue_title(fake *f, int64_t n, const char *t)
{
	tix_issue *i = find(f, n);
	free(i->title);
	i->title = tix_xstrdup(t);
}

int main(void)
{
	fake_env env = {1700000000, 7};
	tix_store *s = tix_store_mem_new(TIX_HASH_SHA1);
	tix_repo *r = NULL;
	tix_repo_config cfg = {.store = s, .env = {fake_now, fake_random, &env}};
	TK_OK(tix_repo_open(&r, &cfg, &e), &e);
	tix_oid me;
	TK_OK(tix_identity_create(r, "Ada", "a@x", &me, &e), &e);
	TK_OK(tix_repo_set_identity(r, &me, &e), &e);

	fake f = {.base = {&fake_vt, "fake:x/y"}};
	tix_strs_add(&f.universe, "bug");
	tix_strs_add(&f.universe, "export");
	add_issue(&f, "Crash on save", "Steps:\n1. save", false, "bug", NULL);
	add_issue(&f, "Old one", "", true, NULL, NULL);
	/* hostile: control characters, a line break and invalid UTF-8 in the title, a label tix can't hold */
	add_issue(&f, "Bad\x01title\nsecond line \xff", "body\r\nwith CRLF", false, "has space", NULL);
	tix_strs_add(&f.universe, "has space");
	add_comment(&f, 1, "bob", "I see it too", NULL);

	/* dry run: nothing changes anywhere */
	tix_bridge_report rep = run_bridge(r, s, &f, (tix_bridge_opts){.mode = TIX_BRIDGE_PULL, .dry_run = true});
	TK_CHECK(has_line(&rep.actions, "would create a ticket for fake:x/y#1") && rep.tickets_created == 0);
	TK_CHECK(!linked(r, 1));
	tix_bridge_report_free(&rep);

	/* pull: import */
	rep = run_bridge(r, s, &f, (tix_bridge_opts){.mode = TIX_BRIDGE_PULL, .comments = true});
	TK_CHECK(rep.tickets_created == 3 && rep.issues_created == 0 && rep.conflicts.n == 0 && rep.comments_in == 1);
	tix_bridge_report_free(&rep);
	tix_entity *t1 = linked(r, 1), *t2 = linked(r, 2), *t3 = linked(r, 3);
	TK_CHECK(t1 && t2 && t3);
	if (!t1 || !t2 || !t3)
		return tk_finish();
	tix_oid id1 = t1->id, id2 = t2->id, id3 = t3->id;
	TK_STR_EQ(t1->u.ticket.title, "Crash on save");
	TK_STR_EQ(t1->u.ticket.body, "Steps:\n1. save");
	TK_CHECK(tix_strs_contains(&t1->u.ticket.labels, "bug") && t1->u.ticket.ncomments == 1);
	TK_CHECK(t1->u.ticket.ncomments == 1 &&
		 !strncmp(t1->u.ticket.comments[0].body, "> bob wrote on fake:x/y#1 (comment 1):\n\nI see it too", 60));
	TK_CHECK(t2->u.ticket.status == TIX_TICKET_DONE);
	TK_STR_EQ(t3->u.ticket.title, "Bad title second line ?");
	TK_STR_EQ(t3->u.ticket.body, "body\nwith CRLF");
	TK_CHECK(t3->u.ticket.labels.n == 0); /* "has space" can't be a tix label */
	tix_entity_free(t1);
	tix_entity_free(t2);
	tix_entity_free(t3);

	/* nothing changed: nothing happens (and comments aren't imported twice) */
	rep = run_bridge(r, s, &f, (tix_bridge_opts){.mode = TIX_BRIDGE_SYNC, .comments = true});
	TK_CHECK(rep.actions.n == 0 && rep.conflicts.n == 0 && rep.comments_in == 0);
	tix_bridge_report_free(&rep);

	/* one side changed: copied to the other, both ways */
	edit_text(r, &id1, TIX_OP_SET_TITLE, "Crash when saving");
	tix_issue *i1 = find(&f, 1);
	free(i1->body);
	i1->body = tix_xstrdup("Steps:\n1. save\n2. boom");
	rep = run_bridge(r, s, &f, (tix_bridge_opts){.mode = TIX_BRIDGE_SYNC});
	TK_CHECK(rep.issues_updated == 1 && rep.tickets_updated == 1 && rep.conflicts.n == 0);
	tix_bridge_report_free(&rep);
	TK_STR_EQ(find(&f, 1)->title, "Crash when saving");
	tix_entity *x = ticket(r, &id1);
	TK_STR_EQ(x->u.ticket.body, "Steps:\n1. save\n2. boom");
	tix_entity_free(x);

	/* pull only doesn't push, push only doesn't pull: and the pending change waits */
	edit_text(r, &id1, TIX_OP_SET_TITLE, "Crash when saving, again");
	rep = run_bridge(r, s, &f, (tix_bridge_opts){.mode = TIX_BRIDGE_PULL});
	TK_CHECK(rep.issues_updated == 0);
	tix_bridge_report_free(&rep);
	rep = run_bridge(r, s, &f, (tix_bridge_opts){.mode = TIX_BRIDGE_PUSH});
	TK_CHECK(rep.issues_updated == 1);
	tix_bridge_report_free(&rep);
	TK_STR_EQ(find(&f, 1)->title, "Crash when saving, again");

	/* both sides changed: a conflict, nothing overwritten; then they agree again */
	edit_text(r, &id1, TIX_OP_SET_TITLE, "tix side");
	set_issue_title(&f, 1, "tracker side");
	rep = run_bridge(r, s, &f, (tix_bridge_opts){.mode = TIX_BRIDGE_SYNC});
	TK_CHECK(rep.conflicts.n == 1 && has_line(&rep.conflicts, "title changed on both sides"));
	tix_bridge_report_free(&rep);
	TK_STR_EQ(find(&f, 1)->title, "tracker side");
	x = ticket(r, &id1);
	TK_STR_EQ(x->u.ticket.title, "tix side");
	tix_entity_free(x);
	set_issue_title(&f, 1, "tix side");
	rep = run_bridge(r, s, &f, (tix_bridge_opts){.mode = TIX_BRIDGE_SYNC});
	TK_CHECK(rep.conflicts.n == 0 && rep.actions.n == 0);
	tix_bridge_report_free(&rep);

	/* state: closing the ticket closes the issue; reopening the issue reopens the ticket */
	tix_op done = tix_op_set_status(TIX_TICKET_DONE);
	TK_OK(tix_entity_apply(r, TIX_ENTITY_TICKET, &id1, &done, 1, &e), &e);
	tix_op_free(&done);
	rep = run_bridge(r, s, &f, (tix_bridge_opts){.mode = TIX_BRIDGE_SYNC});
	TK_CHECK(find(&f, 1)->closed);
	tix_bridge_report_free(&rep);
	find(&f, 2)->closed = false;
	rep = run_bridge(r, s, &f, (tix_bridge_opts){.mode = TIX_BRIDGE_SYNC});
	tix_bridge_report_free(&rep);
	x = ticket(r, &id2);
	TK_CHECK(x->u.ticket.status == TIX_TICKET_OPEN);
	tix_entity_free(x);

	/* export: only tickets in scope; with a marker; once */
	tix_strs lab = {0};
	tix_strs_add(&lab, "export");
	tix_strs_add(&lab, "tix-only");
	tix_oid ex, quiet;
	TK_OK(tix_ticket_create(r, "Made in tix", "details", &lab, &ex, &e), &e);
	TK_OK(tix_ticket_create(r, "Stays local", "", NULL, &quiet, &e), &e);
	tix_strs_free(&lab);
	edit_text(r, &ex, TIX_OP_COMMENT, "a tix comment");
	rep = run_bridge(r, s, &f, (tix_bridge_opts){.mode = TIX_BRIDGE_PUSH, .label = "export", .comments = true});
	TK_CHECK(rep.issues_created == 1 && rep.comments_out == 1);
	tix_bridge_report_free(&rep);
	tix_issue *made = find(&f, 4);
	char hex[TIX_OID_MAX_HEX + 1];
	tix_oid_hex(&ex, hex);
	TK_CHECK(made && !strcmp(made->title, "Made in tix") && made->marker && !strcmp(made->marker, hex));
	TK_CHECK(made && made->labels.n == 1 && tix_strs_contains(&made->labels, "export")); /* tix-only isn't there */
	x = ticket(r, &ex);
	TK_STR_EQ(x->u.ticket.plan.external, "fake:x/y#4");
	tix_entity_free(x);
	x = ticket(r, &quiet);
	TK_CHECK(!x->u.ticket.plan.external);
	tix_entity_free(x);
	rep = run_bridge(r, s, &f, (tix_bridge_opts){.mode = TIX_BRIDGE_SYNC, .label = "export", .comments = true});
	TK_CHECK(rep.issues_created == 0 && rep.comments_out == 0 && rep.comments_in == 0 && rep.actions.n == 0);
	tix_bridge_report_free(&rep);

	/* a clone with no base: equal fields set the base silently; comments aren't echoed */
	TK_OK(tix_store_local_set(s, "bridge-0000000000000000", "", 0, &e), &e);
	tix_store *s2 = tix_store_mem_new(TIX_HASH_SHA1); /* empty local state, same repo */
	rep = run_bridge(r, s2, &f, (tix_bridge_opts){.mode = TIX_BRIDGE_SYNC, .comments = true});
	TK_CHECK(rep.conflicts.n == 0 && rep.comments_in == 0 && rep.comments_out == 0);
	tix_bridge_report_free(&rep);

	/* a lost link is found again through the issue's marker */
	tix_oid lost;
	TK_OK(tix_ticket_create(r, "Lost link", "", NULL, &lost, &e), &e);
	tix_oid_hex(&lost, hex);
	add_issue(&f, "Lost link", "", false, NULL, hex);
	rep = run_bridge(r, s, &f, (tix_bridge_opts){.mode = TIX_BRIDGE_SYNC});
	TK_CHECK(has_line(&rep.actions, "link ticket") && rep.issues_created == 0);
	tix_bridge_report_free(&rep);
	x = ticket(r, &lost);
	TK_STR_EQ(x->u.ticket.plan.external, "fake:x/y#5");
	tix_entity_free(x);

	/* two tickets on one issue, and a link to an issue that's gone: reported */
	tix_op dupe = tix_op_set(TIX_F_EXTERNAL, tix_value_str("fake:x/y#1"));
	TK_OK(tix_entity_apply(r, TIX_ENTITY_TICKET, &id3, &dupe, 1, &e), &e);
	tix_op_free(&dupe);
	tix_op gone = tix_op_set(TIX_F_EXTERNAL, tix_value_str("fake:x/y#99"));
	TK_OK(tix_entity_apply(r, TIX_ENTITY_TICKET, &quiet, &gone, 1, &e), &e);
	tix_op_free(&gone);
	rep = run_bridge(r, s, &f, (tix_bridge_opts){.mode = TIX_BRIDGE_SYNC});
	TK_CHECK(has_line(&rep.conflicts, "both link to fake:x/y#1") && has_line(&rep.conflicts, "#99, which isn't there"));
	tix_bridge_report_free(&rep);

	/* markers */
	char *body, *marker;
	tix_bridge_split_marker("text\n\n<!-- tix:0123abc -->\n", &body, &marker);
	TK_STR_EQ(body, "text");
	TK_STR_EQ(marker, "0123abc");
	free(body);
	free(marker);
	tix_bridge_split_marker("<!-- tix:0123abc --> then more", &body, &marker); /* not at the end */
	TK_CHECK(!marker && !strcmp(body, "<!-- tix:0123abc --> then more"));
	free(body);
	tix_bridge_split_marker("x <!-- tix:<script> -->", &body, &marker); /* not an id */
	TK_CHECK(!marker);
	free(body);

	f_destroy(&f.base);
	tix_store_destroy(s2);
	tix_repo_close(r);
	tix_store_destroy(s);
	return tk_finish();
}
