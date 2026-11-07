/*
 * Small forms posted from the ticket and calendar pages: comments, dependencies,
 * assignments, calendar exceptions, and sync. Values go through tix/input.h.
 */
#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool ticket_arg(web_ctx *x, tix_oid *id)
{
	return route_id(x, 1, TIX_ENTITY_TICKET, id);
}

static void back_to(web_ctx *x, const char *prefix, const tix_oid *id)
{
	char path[TIX_OID_MAX_HEX + 8], hex[TIX_OID_MAX_HEX + 1];
	snprintf(path, sizeof(path), "%s%s", prefix, tix_oid_hex(id, hex));
	web_redirect(x, path);
}

/* A 422 page with the message and a way back. */
static void refused(web_ctx *x, const char *prefix, const tix_oid *id, const char *msg)
{
	html *h = &x->h;
	tix_buf_reset(&x->res->body);
	tix_buf_reset(&x->res->headers);
	x->res->status = 422;
	page_begin(x, "Not saved");
	HL(h, "<p class=\"error\">");
	h_text(h, msg);
	HL(h, "</p>\n<p><a href=\"");
	h_text(h, prefix);
	h_id(h, id);
	HL(h, "\">Back</a> (your browser's back button keeps what you typed)</p>\n");
	page_end(x);
}

static bool apply_ops(web_ctx *x, tix_entity_type t, const tix_oid *id, tix_op *ops, size_t n)
{
	tix_status st = tix_entity_apply(x->app->cfg.repo, t, id, ops, n, &x->app->err);
	for (size_t i = 0; i < n; i++)
		tix_op_free(&ops[i]);
	return st == TIX_OK;
}

/* POST /t/ID/comment body=...    POST /t/ID/comment/CID body=... (edit your own) */
void post_comment(web_ctx *x)
{
	tix_oid id;
	if (!ticket_arg(x, &id))
		return;
	const char *body = web_kvs_get(&x->req->form, "body");
	if (!body || !body[strspn(body, " \n\t")]) {
		refused(x, "/t/", &id, "A comment needs some text.");
		return;
	}
	tix_op op;
	if (x->nsegs == 4) {
		tix_opid cid;
		if (!tix_opid_parse(&cid, x->segs[3])) {
			page_error(x, 404, "No such comment", "That isn't a comment id.");
			return;
		}
		op = tix_op_edit_comment(&cid, body);
	} else {
		op = tix_op_text(TIX_OP_COMMENT, body);
	}
	if (!apply_ops(x, TIX_ENTITY_TICKET, &id, &op, 1)) {
		refused(x, "/t/", &id, x->app->err.msg);
		return;
	}
	back_to(x, "/t/", &id);
}

/* GET /t/ID/comment/CID: the edit form for one of your comments */
void page_comment_edit(web_ctx *x)
{
	tix_oid id;
	tix_opid cid;
	tix_entity *e;
	if (!ticket_arg(x, &id))
		return;
	if (!tix_opid_parse(&cid, x->segs[3]) ||
	    tix_entity_load(x->app->cfg.repo, TIX_ENTITY_TICKET, &id, &e, &x->app->err)) {
		page_error(x, 404, "No such comment", "Not found.");
		return;
	}
	const tix_comment *c = NULL;
	for (size_t i = 0; i < e->u.ticket.ncomments; i++)
		if (!tix_opid_cmp(&e->u.ticket.comments[i].id, &cid))
			c = &e->u.ticket.comments[i];
	if (!c) {
		page_error(x, 404, "No such comment", "Not found.");
		tix_entity_free(e);
		return;
	}
	page_begin(x, "Edit comment");
	html *h = &x->h;
	char sid[TIX_OID_MAX_HEX + 16];
	HL(h, "<p>On <a href=\"/t/");
	h_id(h, &id);
	HL(h, "\">");
	h_text(h, e->u.ticket.title);
	HL(h, "</a></p>\n<form method=\"post\" action=\"/t/");
	h_id(h, &id);
	HL(h, "/comment/");
	h_text(h, tix_opid_str(&c->id, sid));
	HL(h, "\">");
	form_csrf(x);
	HL(h, "<textarea name=\"body\" rows=\"8\" cols=\"70\" required>");
	h_text(h, c->body);
	HL(h, "</textarea><p><button>Save</button></p></form>\n");
	page_end(x);
	tix_entity_free(e);
}

/* POST /t/ID/dep op=add pred=ID kind=FS lag=2 | op=remove pred=ID */
void post_dep(web_ctx *x)
{
	tix_oid id;
	if (!ticket_arg(x, &id))
		return;
	const web_kvs *f = &x->req->form;
	const char *op = web_kvs_get(f, "op"), *pred = web_kvs_get(f, "pred");
	const char *kind = web_kvs_get(f, "kind"), *lag = web_kvs_get(f, "lag");
	if (!op || !pred || !*pred) {
		refused(x, "/t/", &id, "Say which ticket this depends on.");
		return;
	}
	char text[200];
	bool add = !strcmp(op, "add");
	if (add) {
		long n = 0;
		char *end = NULL;
		if (lag && *lag) {
			n = strtol(lag, &end, 10);
			if (*end) {
				refused(x, "/t/", &id, "dep: lag is a whole number of working days, like 2 or -1");
				return;
			}
		}
		snprintf(text, sizeof(text), "+%s:%s%+ldd", pred, kind && *kind ? kind : "FS", n);
		if (!n) /* "+0d" is not in the grammar; leave the lag out */
			snprintf(text, sizeof(text), "+%s:%s", pred, kind && *kind ? kind : "FS");
	} else {
		snprintf(text, sizeof(text), "-%s", pred);
	}
	tix_input_ctx ctx = {x->app->cfg.repo, page_today(x)};
	tix_input in = {0};
	tix_status st = tix_input_field(&ctx, TIX_ENTITY_TICKET, "dep", text, &in, &x->app->err);
	if (!st)
		st = tix_entity_apply(x->app->cfg.repo, TIX_ENTITY_TICKET, &id, in.ops.v, in.ops.n, &x->app->err);
	tix_input_free(&in);
	if (st) {
		refused(x, "/t/", &id, x->app->err.msg);
		return;
	}
	back_to(x, "/t/", &id);
}

/* POST /t/ID/assign op=add res=ID units=50 effort=8 | op=remove res=ID */
void post_assign(web_ctx *x)
{
	tix_oid id;
	if (!ticket_arg(x, &id))
		return;
	const web_kvs *f = &x->req->form;
	const char *op = web_kvs_get(f, "op"), *res = web_kvs_get(f, "res");
	const char *units = web_kvs_get(f, "units"), *effort = web_kvs_get(f, "effort");
	if (!op || !res || !*res) {
		refused(x, "/t/", &id, "Choose a resource.");
		return;
	}
	tix_input_ctx ctx = {x->app->cfg.repo, page_today(x)};
	tix_op o;
	tix_status st;
	if (!strcmp(op, "add")) {
		char text[200];
		bool syntax;
		snprintf(text, sizeof(text), "%s:%s", res, units && *units ? units : "100");
		st = tix_input_assignment(&ctx, text, effort && *effort ? effort : NULL, &o, &syntax, &x->app->err);
	} else {
		tix_oid r;
		st = tix_entity_resolve(x->app->cfg.repo, TIX_ENTITY_RESOURCE, res, &r, &x->app->err);
		if (!st)
			o = tix_op_unassign(&r);
	}
	if (!st && !apply_ops(x, TIX_ENTITY_TICKET, &id, &o, 1))
		st = TIX_ERR_INVALID;
	if (st) {
		refused(x, "/t/", &id, x->app->err.msg);
		return;
	}
	back_to(x, "/t/", &id);
}

/* POST /cal/ID/exception op=off|on|clear from=D to=D note=... */
void post_exception(web_ctx *x)
{
	tix_oid id;
	if (!route_id(x, 1, TIX_ENTITY_CALENDAR, &id))
		return;
	const web_kvs *f = &x->req->form;
	const char *op = web_kvs_get(f, "op"), *from = web_kvs_get(f, "from"), *to = web_kvs_get(f, "to");
	const char *note = web_kvs_get(f, "note");
	if (!op || (strcmp(op, "off") && strcmp(op, "on") && strcmp(op, "clear")) || !from || !*from) {
		refused(x, "/cal/", &id, "Give at least a first date.");
		return;
	}
	char text[400];
	snprintf(text, sizeof(text), "%s%s%s%s%s", from, to && *to ? ".." : "", to && *to ? to : "",
		 strcmp(op, "clear") && note && *note ? "=" : "", strcmp(op, "clear") && note ? note : "");
	tix_input_ctx ctx = {x->app->cfg.repo, page_today(x)};
	tix_input in = {0};
	tix_status st = tix_input_field(&ctx, TIX_ENTITY_CALENDAR, op, text, &in, &x->app->err);
	if (!st)
		st = tix_entity_apply(x->app->cfg.repo, TIX_ENTITY_CALENDAR, &id, in.ops.v, in.ops.n, &x->app->err);
	tix_input_free(&in);
	if (st) {
		refused(x, "/cal/", &id, x->app->err.msg);
		return;
	}
	back_to(x, "/cal/", &id);
}

/* GET /sync shows the remote; POST /sync pulls, merges and pushes. */
void page_sync(web_ctx *x)
{
	page_begin(x, "Sync");
	html *h = &x->h;
	HL(h, "<p>Pull from <code>");
	h_text(h, x->app->cfg.remote);
	HL(h, "</code>, merge, and push. The page waits while <code>git</code> talks to the remote.</p>\n");
	if (!x->app->cfg.read_only) {
		HL(h, "<form method=\"post\" action=\"/sync\">");
		form_csrf(x);
		HL(h, "<button>Sync now</button></form>\n");
	}
	page_end(x);
}

void post_sync(web_ctx *x)
{
	tix_sync_report rep = {0};
	tix_status st = tix_sync(x->app->cfg.repo, x->app->cfg.remote, &rep, &x->app->err);
	if (st)
		x->res->status = 502;
	page_begin(x, "Sync");
	html *h = &x->h;
	if (st) {
		HL(h, "<p class=\"error\">");
		h_text(h, x->app->err.msg);
		HL(h, "</p>\n");
	} else {
		HL(h, "<p class=\"ok\">Done.</p>\n");
	}
	HL(h, "<table class=\"fields\"><tbody>");
	static const char *const names[] = {"new here", "caught up", "merged", "rejected (invalid)",
					    "skipped (unknown kind)", "pushed", "not pushed (invalid)",
					    "unsigned incoming commits", "badly signed incoming commits"};
	size_t vals[] = {rep.created, rep.forwarded, rep.merged, rep.rejected, rep.skipped, rep.pushed, rep.unpushable,
			 rep.unsigned_commits, rep.bad_signatures};
	for (size_t i = 0; i < sizeof(vals) / sizeof(*vals); i++) {
		HL(h, "<tr><th>");
		h_text(h, names[i]);
		HL(h, "</th><td class=\"num\">");
		h_int(h, (long long)vals[i]);
		HL(h, "</td></tr>");
	}
	HL(h, "</tbody></table>\n");
	if (rep.problems.n) {
		HL(h, "<h2>Problems</h2>\n<ul>");
		for (size_t i = 0; i < rep.problems.n; i++) {
			HL(h, "<li>");
			h_text(h, rep.problems.v[i]); /* from the remote: escaped like everything else */
			HL(h, "</li>");
		}
		HL(h, "</ul>\n");
	}
	HL(h, "<p><a href=\"/tickets\">Tickets</a></p>\n");
	page_end(x);
	tix_sync_report_free(&rep);
}
