/* Resources (/res, /res/ID), calendars (/cal, /cal/ID) and identities (/id). */
#include "app.h"

#include <string.h>

static const char *const day_names[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};

static bool load_list(web_ctx *x, tix_entity_type t, tix_entity_list *l)
{
	tix_status st = t == TIX_ENTITY_RESOURCE ? tix_resource_list(x->app->cfg.repo, l, &x->app->err)
			: t == TIX_ENTITY_CALENDAR ? tix_calendar_list(x->app->cfg.repo, l, &x->app->err)
						   : tix_identity_list(x->app->cfg.repo, l, &x->app->err);
	if (st) {
		page_error(x, 500, "Repository problem", x->app->err.msg);
		return false;
	}
	return true;
}

static bool load_one(web_ctx *x, tix_entity_type t, tix_entity **e)
{
	tix_oid id;
	if (!route_id(x, 1, t, &id))
		return false;
	if (tix_entity_load(x->app->cfg.repo, t, &id, e, &x->app->err)) {
		page_error(x, 404, "Not found", x->app->err.msg);
		return false;
	}
	return true;
}

static void h_rate(html *h, const tix_resource *r)
{
	if (r->rate < 0)
		return;
	h_int(h, r->rate / 100);
	HL(h, ".");
	if (r->rate % 100 < 10)
		HL(h, "0");
	h_int(h, r->rate % 100);
	HL(h, " ");
	h_text(h, r->currency);
	HL(h, "/h");
}

static void h_weekdays(html *h, unsigned mask)
{
	bool first = true;
	for (int d = 0; d < 7; d++)
		if (mask & (1u << d)) {
			if (!first)
				HL(h, " ");
			h_text(h, day_names[d]);
			first = false;
		}
}

void page_resources(web_ctx *x)
{
	tix_entity_list l, cals;
	if (!load_list(x, TIX_ENTITY_RESOURCE, &l))
		return;
	if (!load_list(x, TIX_ENTITY_CALENDAR, &cals)) {
		tix_entity_list_free(&l);
		return;
	}
	page_begin(x, "Resources");
	html *h = &x->h;
	if (web_can_write(x))
		HL(h, "<p><a href=\"/res/new\">New resource</a></p>\n");
	HL(h, "<table><thead><tr><th>Name</th><th>Kind</th><th class=\"num\">Capacity</th><th>Calendar</th>"
	      "<th>Rate</th><th>Active</th></tr></thead><tbody>\n");
	for (size_t i = 0; i < l.n; i++) {
		const tix_resource *r = &l.v[i]->u.resource;
		HL(h, "<tr><td>");
		h_named(x, TIX_ENTITY_RESOURCE, &l.v[i]->id, &l);
		HL(h, "</td><td>");
		h_text(h, tix_field_get(TIX_F_RES_KIND)->enums[r->kind]);
		HL(h, "</td><td class=\"num\">");
		h_int(h, r->capacity);
		HL(h, "%</td><td>");
		if (!tix_oid_is_null(&r->calendar))
			h_named(x, TIX_ENTITY_CALENDAR, &r->calendar, &cals);
		HL(h, "</td><td>");
		h_rate(h, r);
		HL(h, "</td><td>");
		if (r->active)
			HL(h, "yes");
		else
			HL(h, "no");
		HL(h, "</td></tr>\n");
	}
	HL(h, "</tbody></table>\n");
	page_end(x);
	tix_entity_list_free(&l);
	tix_entity_list_free(&cals);
}

void page_resource(web_ctx *x)
{
	tix_entity *e;
	if (!load_one(x, TIX_ENTITY_RESOURCE, &e))
		return;
	const tix_resource *r = &e->u.resource;
	tix_ticket_query q = {.assignee = &e->id, .status_mask = TIX_STATUS_MASK_ACTIVE};
	tix_entity_list l;
	if (tix_ticket_list(x->app->cfg.repo, &q, &l, &x->app->err)) {
		page_error(x, 500, "Repository problem", x->app->err.msg);
		tix_entity_free(e);
		return;
	}
	page_begin(x, r->name);
	html *h = &x->h;
	HL(h, "<table class=\"fields\"><tbody><tr><th>ID</th><td class=\"id\">");
	h_id(h, &e->id);
	HL(h, "</td></tr>\n<tr><th>Kind</th><td>");
	h_text(h, tix_field_get(TIX_F_RES_KIND)->enums[r->kind]);
	HL(h, "</td></tr>\n<tr><th>Capacity</th><td>");
	h_int(h, r->capacity);
	HL(h, "%</td></tr>\n<tr><th>Calendar</th><td>");
	if (!tix_oid_is_null(&r->calendar))
		h_named(x, TIX_ENTITY_CALENDAR, &r->calendar, NULL);
	else
		HL(h, "<span class=\"note\">each task's own</span>");
	HL(h, "</td></tr>\n<tr><th>Identity</th><td>");
	if (!tix_oid_is_null(&r->identity))
		h_who(x, &r->identity);
	HL(h, "</td></tr>\n<tr><th>Rate</th><td>");
	h_rate(h, r);
	HL(h, "</td></tr>\n<tr><th>Active</th><td>");
	if (r->active)
		HL(h, "yes");
	else
		HL(h, "no");
	HL(h, "</td></tr>\n</tbody></table>\n");
	if (web_can_write(x)) {
		HL(h, "<p><a href=\"/res/");
		h_id(h, &e->id);
		HL(h, "/edit\">Edit</a></p>\n");
	}
	HL(h, "<h2>Open assignments</h2>\n");
	if (!l.n)
		HL(h, "<p class=\"note\">None.</p>\n");
	else
		HL(h, "<ul>\n");
	for (size_t i = 0; i < l.n; i++) {
		HL(h, "<li>");
		h_ticket(x, &l.v[i]->id, &l);
		HL(h, " ");
		h_status(h, l.v[i]->u.ticket.status);
		HL(h, "</li>\n");
	}
	if (l.n)
		HL(h, "</ul>\n");
	HL(h, "<p><a href=\"/load\">Resource load</a> &middot; <a href=\"/tickets?assignee=");
	h_id(h, &e->id);
	HL(h, "\">all its tickets</a></p>\n");
	page_end(x);
	tix_entity_list_free(&l);
	tix_entity_free(e);
}

void page_calendars(web_ctx *x)
{
	tix_entity_list l;
	if (!load_list(x, TIX_ENTITY_CALENDAR, &l))
		return;
	page_begin(x, "Calendars");
	html *h = &x->h;
	if (web_can_write(x))
		HL(h, "<p><a href=\"/cal/new\">New calendar</a></p>\n");
	HL(h, "<p class=\"note\">Tasks without a calendar (and without an ancestor's) use Mon-Fri, 8 hours.</p>\n"
	      "<table><thead><tr><th>Name</th><th>Working days</th><th class=\"num\">Hours</th>"
	      "<th class=\"num\">Exceptions</th></tr></thead><tbody>\n");
	for (size_t i = 0; i < l.n; i++) {
		const tix_calendar *c = &l.v[i]->u.calendar;
		HL(h, "<tr><td>");
		h_named(x, TIX_ENTITY_CALENDAR, &l.v[i]->id, &l);
		HL(h, "</td><td>");
		h_weekdays(h, c->weekdays);
		HL(h, "</td><td class=\"num\">");
		h_int(h, c->hours);
		HL(h, "</td><td class=\"num\">");
		h_int(h, (long long)c->nexceptions);
		HL(h, "</td></tr>\n");
	}
	HL(h, "</tbody></table>\n");
	page_end(x);
	tix_entity_list_free(&l);
}

void page_calendar(web_ctx *x)
{
	tix_entity *e;
	if (!load_one(x, TIX_ENTITY_CALENDAR, &e))
		return;
	const tix_calendar *c = &e->u.calendar;
	page_begin(x, c->name);
	html *h = &x->h;
	HL(h, "<table class=\"fields\"><tbody><tr><th>ID</th><td class=\"id\">");
	h_id(h, &e->id);
	HL(h, "</td></tr>\n<tr><th>Working days</th><td>");
	h_weekdays(h, c->weekdays);
	HL(h, "</td></tr>\n<tr><th>Hours per day</th><td>");
	h_int(h, c->hours);
	HL(h, "</td></tr>\n</tbody></table>\n");
	bool w = web_can_write(x);
	if (w) {
		HL(h, "<p><a href=\"/cal/");
		h_id(h, &e->id);
		HL(h, "/edit\">Edit</a></p>\n");
	}
	HL(h, "<h2>Exceptions</h2>\n");
	if (!c->nexceptions)
		HL(h, "<p class=\"note\">None.</p>\n");
	else
		HL(h, "<table><thead><tr><th>From</th><th>To</th><th>Day</th><th>Note</th></tr></thead><tbody>\n");
	for (size_t i = 0; i < c->nexceptions; i++) {
		const tix_cal_exception *ex = &c->exceptions[i];
		HL(h, "<tr><td>");
		h_date(h, ex->from);
		HL(h, "</td><td>");
		h_date(h, ex->to);
		HL(h, "</td><td>");
		if (ex->working)
			HL(h, "working");
		else
			HL(h, "off");
		HL(h, "</td><td>");
		h_text(h, ex->note);
		HL(h, "</td><td>");
		if (w) {
			HL(h, "<form class=\"inline\" method=\"post\" action=\"/cal/");
			h_id(h, &e->id);
			HL(h, "/exception\">");
			form_csrf(x);
			HL(h, "<input type=\"hidden\" name=\"op\" value=\"clear\"><input type=\"hidden\" name=\"from\" value=\"");
			h_date(h, ex->from);
			HL(h, "\"><input type=\"hidden\" name=\"to\" value=\"");
			h_date(h, ex->to);
			HL(h, "\"><button>remove</button></form>");
		}
		HL(h, "</td></tr>\n");
	}
	if (c->nexceptions)
		HL(h, "</tbody></table>\n");
	if (w) {
		HL(h, "<form method=\"post\" action=\"/cal/");
		h_id(h, &e->id);
		HL(h, "/exception\">");
		form_csrf(x);
		HL(h, "<label><select name=\"op\"><option value=\"off\">Day off</option><option value=\"on\">Extra working day"
		      "</option></select></label> <label>from <input type=\"date\" name=\"from\" required></label> "
		      "<label>to <input type=\"date\" name=\"to\"></label> <label>note <input name=\"note\" size=\"20\">"
		      "</label> <button>Add</button></form>\n");
	}
	page_end(x);
	tix_entity_free(e);
}

void page_identities(web_ctx *x)
{
	tix_entity_list l;
	if (!load_list(x, TIX_ENTITY_IDENTITY, &l))
		return;
	tix_oid me;
	bool have_me = tix_repo_identity(x->app->cfg.repo, &me);
	page_begin(x, "Identities");
	html *h = &x->h;
	HL(h, "<p>");
	if (have_me) {
		HL(h, "Changes made here are written as <b>");
		h_who(x, &me);
		HL(h, "</b> (git config <code>tix.identity</code>; change it with <code>tix id use</code>).");
	} else {
		HL(h, "<b>No identity is set</b>, so changes can't be written. Create one with <code>tix id new</code>, "
		      "then restart <code>tix web</code>.");
	}
	HL(h, "</p>\n<table><thead><tr><th>Name</th><th>Email</th><th>ID</th><th>Keys</th></tr></thead><tbody>\n");
	for (size_t i = 0; i < l.n; i++) {
		const tix_identity *id = &l.v[i]->u.identity;
		HL(h, "<tr><td>");
		h_text(h, id->name);
		if (have_me && tix_oid_eq(&me, &l.v[i]->id))
			HL(h, " <span class=\"status\">you</span>");
		HL(h, "</td><td>");
		h_text(h, id->email);
		HL(h, "</td><td class=\"id\">");
		h_short(h, &l.v[i]->id);
		HL(h, "</td><td class=\"num\">");
		h_int(h, (long long)id->nkeys);
		HL(h, "</td></tr>\n");
	}
	HL(h, "</tbody></table>\n");
	page_end(x);
	tix_entity_list_free(&l);
}
