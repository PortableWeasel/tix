/* Ticket pages: the list (/tickets) and one ticket (/t/ID). */
#include "app.h"

#include <string.h>

#define NSTATUS 5

/* ---- /tickets?q&status&label&assignee&under ------------------------------------------- */

void page_tickets(web_ctx *x)
{
	const web_request *req = x->req;
	tix_repo *repo = x->app->cfg.repo;
	tix_ticket_query q = {0};
	q.text = web_kvs_get(&req->query, "q");
	q.label = web_kvs_get(&req->query, "label");
	if (q.label && !*q.label)
		q.label = NULL;
	const char *st[NSTATUS];
	size_t nst = web_kvs_all(&req->query, "status", st, NSTATUS);
	bool filtered = req->query.n > 0;
	for (size_t i = 0; i < nst; i++) {
		tix_ticket_status s;
		if (tix_ticket_status_parse(st[i], &s))
			q.status_mask |= 1u << s;
	}
	if (!filtered)
		q.status_mask = TIX_STATUS_MASK_ACTIVE; /* the default view: what's still open */
	tix_oid assignee, under;
	bool have_assignee, have_under;
	if (!query_id(x, "assignee", TIX_ENTITY_RESOURCE, &assignee, &have_assignee) ||
	    !query_id(x, "under", TIX_ENTITY_TICKET, &under, &have_under))
		return;
	q.assignee = have_assignee ? &assignee : NULL;
	q.under = have_under ? &under : NULL;

	tix_entity_list l = {0}, res = {0};
	if (tix_ticket_list(repo, &q, &l, &x->app->err) || tix_resource_list(repo, &res, &x->app->err)) {
		page_error(x, 500, "Repository problem", x->app->err.msg);
		tix_entity_list_free(&l);
		return;
	}
	page_begin(x, "Tickets");
	html *h = &x->h;

	HL(h, "<form method=\"get\" action=\"/tickets\" class=\"filters\"><label>Text <input name=\"q\" value=\"");
	h_text(h, q.text);
	HL(h, "\"></label> ");
	for (int s = 0; s < NSTATUS; s++) {
		HL(h, "<label><input type=\"checkbox\" name=\"status\" value=\"");
		h_text(h, tix_ticket_status_name((tix_ticket_status)s));
		HL(h, "\"");
		if (q.status_mask & (1u << s))
			HL(h, " checked");
		HL(h, "> ");
		h_text(h, tix_ticket_status_name((tix_ticket_status)s));
		HL(h, "</label>");
	}
	HL(h, " <label>Label <input name=\"label\" size=\"10\" value=\"");
	h_text(h, q.label);
	HL(h, "\"></label> <label>Assignee <select name=\"assignee\"><option value=\"\">anyone</option>");
	for (size_t i = 0; i < res.n; i++) {
		HL(h, "<option value=\"");
		h_id(h, &res.v[i]->id);
		HL(h, "\"");
		if (have_assignee && tix_oid_eq(&assignee, &res.v[i]->id))
			HL(h, " selected");
		HL(h, ">");
		h_text(h, res.v[i]->u.resource.name);
		HL(h, "</option>");
	}
	HL(h, "</select></label> <label>Under <input name=\"under\" size=\"10\" placeholder=\"ticket id\" value=\"");
	if (have_under)
		h_short(h, &under);
	HL(h, "\"></label> <button>Filter</button> <a href=\"/tickets\">reset</a></form>\n");
	if (!x->app->cfg.read_only)
		HL(h, "<p><a href=\"/new\">New ticket</a></p>\n");

	HL(h, "<table class=\"tickets\"><thead><tr><th>ID</th><th>Status</th><th>Title</th><th>Labels</th>"
	      "<th>Updated</th></tr></thead><tbody>\n");
	for (size_t i = 0; i < l.n; i++) {
		const tix_entity *e = l.v[i];
		HL(h, "<tr><td class=\"id\"><a href=\"/t/");
		h_id(h, &e->id);
		HL(h, "\">");
		h_short(h, &e->id);
		HL(h, "</a></td><td>");
		h_status(h, e->u.ticket.status);
		HL(h, "</td><td><a href=\"/t/");
		h_id(h, &e->id);
		HL(h, "\">");
		h_text(h, e->u.ticket.title);
		HL(h, "</a></td><td>");
		h_labels(h, &e->u.ticket.labels);
		HL(h, "</td><td>");
		h_time(h, e->updated);
		HL(h, "</td></tr>\n");
	}
	HL(h, "</tbody></table>\n<p class=\"note\">");
	h_int(h, (long long)l.n);
	if (l.n == 1)
		HL(h, " ticket");
	else
		HL(h, " tickets");
	if (l.nbroken) {
		HL(h, "; ");
		h_int(h, (long long)l.nbroken);
		HL(h, " invalid entries skipped");
	}
	HL(h, "</p>\n");
	page_end(x);
	tix_entity_list_free(&l);
	tix_entity_list_free(&res);
}

/* ---- /t/ID ------------------------------------------------------------------------------- */

#define ROW(h, label)                     \
	do {                              \
		HL(h, "<tr><th>" label "</th><td>"); \
	} while (0)
#define END_ROW(h) HL(h, "</td></tr>\n")

static void planning_table(web_ctx *x, const tix_entity *e, const tix_plan_result *pr)
{
	html *h = &x->h;
	const tix_ticket_plan *p = &e->u.ticket.plan;
	size_t ti = tix_schedule_find(pr->schedule, &e->id);
	const tix_plan_task *t = ti == SIZE_MAX ? NULL : &pr->schedule->tasks[ti];

	HL(h, "<h2>Planning</h2>\n<table class=\"fields\"><tbody>\n");
	ROW(h, "Parent");
	if (!tix_oid_is_null(&p->parent))
		h_ticket(x, &p->parent, &pr->tickets);
	END_ROW(h);
	if (t && t->wbs[0]) {
		ROW(h, "WBS");
		h_text(h, t->wbs);
		END_ROW(h);
	}
	ROW(h, "Kind");
	h_text(h, p->kind == TIX_KIND_MILESTONE ? "milestone" : "task");
	if (p->mode == TIX_MODE_MANUAL)
		HL(h, ", scheduled manually");
	END_ROW(h);
	ROW(h, "Calendar");
	if (!tix_oid_is_null(&p->calendar)) {
		h_named(x, TIX_ENTITY_CALENDAR, &p->calendar, &pr->calendars);
	} else if (t && t->calendar) {
		h_named(x, TIX_ENTITY_CALENDAR, &t->calendar->id, &pr->calendars);
		HL(h, " <span class=\"note\">(inherited)</span>");
	} else {
		HL(h, "<span class=\"note\">Mon-Fri, 8 hours (built in)</span>");
	}
	END_ROW(h);
	ROW(h, "Start no earlier than");
	h_date(h, p->start);
	END_ROW(h);
	if (p->mode == TIX_MODE_MANUAL || p->finish != TIX_DATE_NONE) {
		ROW(h, "Finish");
		h_date(h, p->finish);
		END_ROW(h);
	}
	ROW(h, "Deadline");
	h_date(h, p->deadline);
	END_ROW(h);
	ROW(h, "Duration");
	h_days(h, p->duration);
	END_ROW(h);
	ROW(h, "Effort");
	h_hours(h, p->effort);
	END_ROW(h);
	ROW(h, "Progress");
	h_int(h, p->progress);
	HL(h, "%");
	END_ROW(h);
	ROW(h, "Actual start");
	h_date(h, p->actual_start);
	END_ROW(h);
	ROW(h, "Actual finish");
	h_date(h, p->actual_finish);
	END_ROW(h);
	HL(h, "</tbody></table>\n");

	HL(h, "<h2>Schedule</h2>\n");
	if (!t || !t->scheduled) {
		HL(h, "<p class=\"note\">Unscheduled: give it a duration, or effort with an assignment.</p>\n");
	} else {
		HL(h, "<p>");
		h_date(h, t->start);
		HL(h, " to ");
		h_date(h, t->finish);
		HL(h, " (");
		h_days(h, t->duration);
		HL(h, "), float ");
		h_int(h, t->total_float);
		HL(h, "d");
		if (t->critical)
			HL(h, ", <b class=\"crit\">critical</b>");
		if (t->late)
			HL(h, ", <b class=\"crit\">late</b>");
		HL(h, " &middot; <a href=\"/plan?root=");
		h_id(h, &e->id);
		HL(h, "\">plan from here</a></p>\n");
	}
	for (size_t i = 0; i < pr->schedule->ndiags; i++)
		if (tix_oid_eq(&pr->schedule->diags[i].id, &e->id)) {
			HL(h, "<p class=\"note\">");
			h_text(h, pr->schedule->diags[i].msg);
			HL(h, "</p>\n");
		}

	/* children, in WBS order */
	bool any = false;
	for (size_t i = 0; t && i < pr->schedule->ntasks; i++) {
		const tix_plan_task *c = &pr->schedule->tasks[i];
		if (c->parent != ti)
			continue;
		if (!any)
			HL(h, "<h2>Subtasks</h2>\n<ul>\n");
		any = true;
		HL(h, "<li>");
		h_text(h, c->wbs);
		HL(h, " ");
		h_ticket(x, &c->ticket->id, &pr->tickets);
		HL(h, " ");
		h_status(h, c->ticket->u.ticket.status);
		HL(h, "</li>\n");
	}
	if (any)
		HL(h, "</ul>\n");

	HL(h, "<h2>Dependencies</h2>\n");
	if (!p->ndeps)
		HL(h, "<p class=\"note\">None.</p>\n");
	else
		HL(h, "<ul>\n");
	for (size_t i = 0; i < p->ndeps; i++) {
		const tix_dep *d = &p->deps[i];
		HL(h, "<li>after ");
		h_ticket(x, &d->pred, &pr->tickets);
		HL(h, " <span class=\"id\">");
		h_text(h, tix_dep_kind_name(d->kind));
		if (d->lag) {
			if (d->lag > 0)
				HL(h, "+");
			h_int(h, d->lag);
			HL(h, "d");
		}
		HL(h, "</span></li>\n");
	}
	if (p->ndeps)
		HL(h, "</ul>\n");
	any = false;
	for (size_t i = 0; i < pr->tickets.n; i++) {
		const tix_ticket_plan *op = &pr->tickets.v[i]->u.ticket.plan;
		for (size_t k = 0; k < op->ndeps; k++)
			if (tix_oid_eq(&op->deps[k].pred, &e->id)) {
				if (any)
					HL(h, ", ");
				else
					HL(h, "<p>Followed by ");
				any = true;
				h_ticket(x, &pr->tickets.v[i]->id, &pr->tickets);
			}
	}
	if (any)
		HL(h, "</p>\n");

	HL(h, "<h2>Assignments</h2>\n");
	if (!p->nassignments)
		HL(h, "<p class=\"note\">None.</p>\n");
	else
		HL(h, "<ul>\n");
	for (size_t i = 0; i < p->nassignments; i++) {
		const tix_assignment *a = &p->assignments[i];
		HL(h, "<li>");
		h_named(x, TIX_ENTITY_RESOURCE, &a->resource, &pr->resources);
		HL(h, " at ");
		h_int(h, a->units);
		HL(h, "%");
		if (a->effort >= 0) {
			HL(h, ", ");
			h_hours(h, a->effort);
		}
		HL(h, "</li>\n");
	}
	if (p->nassignments)
		HL(h, "</ul>\n");
}

void page_ticket(web_ctx *x)
{
	tix_oid id;
	if (!route_id(x, 1, TIX_ENTITY_TICKET, &id))
		return;
	tix_plan_result pr;
	if (!page_plan(x, NULL, &pr)) /* the whole plan: dependencies reach outside any subtree */
		return;
	const tix_entity *e = list_find(&pr.tickets, &id);
	if (!e) {
		page_error(x, 404, "No such ticket", "It doesn't validate, so it isn't shown.");
		tix_plan_result_free(&pr);
		return;
	}
	const tix_ticket *t = &e->u.ticket;
	page_begin(x, t->title);
	html *h = &x->h;
	HL(h, "<p>");
	h_status(h, t->status);
	HL(h, " ");
	h_labels(h, &t->labels);
	HL(h, " <span class=\"note\">opened by ");
	h_who(x, &t->author);
	HL(h, " ");
	h_time(h, e->created);
	HL(h, ", updated ");
	h_time(h, e->updated);
	HL(h, "</span></p>\n<p class=\"note\">id <span class=\"id\">");
	h_id(h, &e->id);
	HL(h, "</span></p>\n");
	if (t->body && *t->body) {
		HL(h, "<div class=\"body\">");
		h_text(h, t->body);
		HL(h, "</div>\n");
	}
	planning_table(x, e, &pr);

	HL(h, "<h2>Comments</h2>\n");
	if (!t->ncomments)
		HL(h, "<p class=\"note\">None yet.</p>\n");
	for (size_t i = 0; i < t->ncomments; i++) {
		const tix_comment *c = &t->comments[i];
		HL(h, "<div class=\"comment\"><p class=\"note\">");
		h_who(x, &c->author);
		HL(h, ", ");
		h_time(h, c->time);
		if (c->edited)
			HL(h, " (edited)");
		HL(h, "</p><div class=\"body\">");
		h_text(h, c->body);
		HL(h, "</div></div>\n");
	}
	page_end(x);
	tix_plan_result_free(&pr);
}
