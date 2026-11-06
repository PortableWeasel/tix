/* Shared page helpers (app.h). */
#include "app.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

bool route_id(web_ctx *x, size_t seg, tix_entity_type t, tix_oid *out)
{
	tix_status st = seg < x->nsegs ? tix_entity_resolve(x->app->cfg.repo, t, x->segs[seg], out, &x->app->err)
				       : TIX_ERR_NOT_FOUND;
	if (st) {
		char title[64];
		snprintf(title, sizeof(title), "No such %s", tix_entity_type_name(t));
		page_error(x, 404, title, x->app->err.msg[0] ? x->app->err.msg : "Not found.");
		return false;
	}
	return true;
}

bool query_id(web_ctx *x, const char *field, tix_entity_type t, tix_oid *out, bool *have)
{
	const char *v = web_kvs_get(&x->req->query, field);
	*have = false;
	if (!v || !*v)
		return true;
	if (tix_entity_resolve(x->app->cfg.repo, t, v, out, &x->app->err)) {
		page_error(x, 404, "Not found", x->app->err.msg);
		return false;
	}
	*have = true;
	return true;
}

tix_date page_today(web_ctx *x)
{
	tix_date today = x->app->cfg.today(x->app->cfg.today_ctx), d;
	const char *v = web_kvs_get(&x->req->query, "today");
	if (v && *v && tix_input_date(v, today, &d))
		return d;
	return today;
}

bool page_plan(web_ctx *x, const tix_oid *root, tix_plan_result *out)
{
	memset(out, 0, sizeof(*out));
	if (tix_plan(x->app->cfg.repo, root, page_today(x), out, &x->app->err)) {
		page_error(x, 500, "Planning failed", x->app->err.msg);
		return false;
	}
	return true;
}

void h_time(html *h, int64_t t)
{
	time_t tt = (time_t)t;
	struct tm tm;
	char buf[32];
	if (gmtime_r(&tt, &tm) && strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M UTC", &tm))
		h_text(h, buf);
}

void h_status(html *h, tix_ticket_status s)
{
	HL(h, "<span class=\"status s-");
	h_text(h, tix_ticket_status_name(s));
	HL(h, "\">");
	h_text(h, tix_ticket_status_name(s));
	HL(h, "</span>");
}

const tix_entity *list_find(const tix_entity_list *l, const tix_oid *id)
{
	for (size_t i = 0; l && i < l->n; i++)
		if (tix_oid_eq(&l->v[i]->id, id))
			return l->v[i];
	return NULL;
}

static const char *type_path(tix_entity_type t)
{
	switch (t) {
	case TIX_ENTITY_TICKET: return "/t/";
	case TIX_ENTITY_RESOURCE: return "/res/";
	case TIX_ENTITY_CALENDAR: return "/cal/";
	default: return "/id/";
	}
}

static const char *entity_name(const tix_entity *e)
{
	switch (e->type) {
	case TIX_ENTITY_TICKET: return e->u.ticket.title;
	case TIX_ENTITY_RESOURCE: return e->u.resource.name;
	case TIX_ENTITY_CALENDAR: return e->u.calendar.name;
	default: return e->u.identity.name;
	}
}

void h_named(web_ctx *x, tix_entity_type t, const tix_oid *id, const tix_entity_list *known)
{
	html *h = &x->h;
	const tix_entity *e = list_find(known, id);
	tix_entity *loaded = NULL;
	if (!e && !tix_entity_load(x->app->cfg.repo, t, id, &loaded, NULL))
		e = loaded;
	if (t != TIX_ENTITY_IDENTITY) {
		HL(h, "<a href=\"");
		h_text(h, type_path(t));
		h_id(h, id);
		HL(h, "\" title=\"");
		h_id(h, id);
		HL(h, "\">");
	}
	if (e)
		h_text(h, entity_name(e));
	else {
		HL(h, "<span class=\"id\">");
		h_short(h, id);
		HL(h, "</span> <span class=\"note\">(not here)</span>");
	}
	if (t != TIX_ENTITY_IDENTITY)
		HL(h, "</a>");
	tix_entity_free(loaded);
}

void h_ticket(web_ctx *x, const tix_oid *id, const tix_entity_list *known)
{
	html *h = &x->h;
	HL(h, "<span class=\"id\">");
	h_short(h, id);
	HL(h, "</span> ");
	h_named(x, TIX_ENTITY_TICKET, id, known);
}

void h_who(web_ctx *x, const tix_oid *identity)
{
	h_named(x, TIX_ENTITY_IDENTITY, identity, NULL);
}

void h_labels(html *h, const tix_strs *labels)
{
	for (size_t i = 0; i < labels->n; i++) {
		HL(h, "<a class=\"label\" href=\"/tickets?label=");
		h_url(h, labels->v[i]);
		HL(h, "\">");
		h_text(h, labels->v[i]);
		HL(h, "</a>");
	}
}

void h_days(html *h, int64_t d)
{
	if (d < 0)
		return;
	h_int(h, d);
	HL(h, "d");
}

void h_hours(html *h, int64_t hrs)
{
	if (hrs < 0)
		return;
	h_int(h, hrs);
	HL(h, "h");
}
