/* Plan pages: /wbs, /plan and /load, all with ?root=ID (default: every ticket). */
#include "app.h"
#include "tix/export.h"

#include <string.h>

/* The root picker shared by the three pages. */
static void root_form(web_ctx *x, const char *action_lit_path, const tix_oid *root, bool with_views)
{
	html *h = &x->h;
	HL(h, "<form method=\"get\" class=\"filters\" action=\"");
	h_text(h, action_lit_path);
	HL(h, "\"><label>Root <input name=\"root\" size=\"12\" placeholder=\"ticket id\" value=\"");
	if (root)
		h_short(h, root);
	HL(h, "\"></label> <label>Today <input type=\"date\" name=\"today\" value=\"");
	h_date(h, page_today(x));
	HL(h, "\"></label>");
	if (with_views) {
		HL(h, " <label><input type=\"checkbox\" name=\"critical\" value=\"1\"");
		if (web_kvs_get(&x->req->query, "critical"))
			HL(h, " checked");
		HL(h, "> critical path only</label>");
	}
	HL(h, " <button>Show</button></form>\n");
}

static void diagnostics(web_ctx *x, const tix_schedule *s)
{
	html *h = &x->h;
	if (!s->ndiags)
		return;
	HL(h, "<h2>Notes</h2>\n<ul class=\"notes\">\n");
	for (size_t i = 0; i < s->ndiags; i++) {
		HL(h, "<li>");
		if (!tix_oid_is_null(&s->diags[i].id) && tix_schedule_find(s, &s->diags[i].id) != SIZE_MAX) {
			HL(h, "<a href=\"/t/");
			h_id(h, &s->diags[i].id);
			HL(h, "\">");
			h_short(h, &s->diags[i].id);
			HL(h, "</a>: ");
		}
		h_text(h, s->diags[i].msg);
		HL(h, "</li>\n");
	}
	HL(h, "</ul>\n");
}

static bool begin(web_ctx *x, const char *title, tix_oid *root, bool *have, tix_plan_result *pr)
{
	if (!query_id(x, "root", TIX_ENTITY_TICKET, root, have))
		return false;
	if (!page_plan(x, *have ? root : NULL, pr))
		return false;
	page_begin(x, title);
	return true;
}

static void task_cells(web_ctx *x, const tix_plan_task *t)
{
	html *h = &x->h;
	HL(h, "<td>");
	h_text(h, t->wbs);
	HL(h, "</td><td class=\"id\"><a href=\"/t/");
	h_id(h, &t->ticket->id);
	HL(h, "\">");
	h_short(h, &t->ticket->id);
	HL(h, "</a></td><td class=\"d");
	h_int(h, (long long)(t->depth > 8 ? 8 : t->depth));
	HL(h, "\">");
	if (t->summary)
		HL(h, "<b>");
	h_text(h, t->ticket->u.ticket.title);
	if (t->summary)
		HL(h, "</b>");
	HL(h, "</td><td>");
	h_status(h, t->ticket->u.ticket.status);
	HL(h, "</td><td>");
	if (t->scheduled)
		h_date(h, t->start);
	else
		HL(h, "<span class=\"note\">unscheduled</span>");
	HL(h, "</td><td>");
	h_date(h, t->finish);
	HL(h, "</td><td class=\"num\">");
	h_days(h, t->duration);
	HL(h, "</td>");
}

void page_wbs(web_ctx *x)
{
	tix_oid root;
	bool have;
	tix_plan_result pr;
	if (!begin(x, "Work breakdown", &root, &have, &pr))
		return;
	html *h = &x->h;
	root_form(x, "/wbs", have ? &root : NULL, false);
	const tix_schedule *s = pr.schedule;
	HL(h, "<table class=\"plan\"><thead><tr><th>WBS</th><th>ID</th><th>Title</th><th>Status</th><th>Start</th>"
	      "<th>Finish</th><th class=\"num\">Duration</th><th class=\"num\">Effort</th>"
	      "<th class=\"num\">Done</th></tr></thead><tbody>\n");
	for (size_t i = 0; i < s->ntasks; i++) {
		const tix_plan_task *t = &s->tasks[i];
		HL(h, "<tr>");
		task_cells(x, t);
		HL(h, "<td class=\"num\">");
		h_hours(h, t->effort);
		HL(h, "</td><td class=\"num\">");
		h_int(h, t->progress);
		HL(h, "%</td></tr>\n");
	}
	HL(h, "</tbody></table>\n");
	diagnostics(x, s);
	page_end(x);
	tix_plan_result_free(&pr);
}

void page_plan_view(web_ctx *x)
{
	tix_oid root;
	bool have;
	tix_plan_result pr;
	if (!begin(x, "Plan", &root, &have, &pr))
		return;
	html *h = &x->h;
	bool critical = web_kvs_get(&x->req->query, "critical") != NULL;
	root_form(x, "/plan", have ? &root : NULL, true);
	const tix_schedule *s = pr.schedule;
	if (s->start != TIX_DATE_NONE) {
		HL(h, "<p>");
		h_date(h, s->start);
		HL(h, " to ");
		h_date(h, s->finish);
		HL(h, " &middot; <a href=\"/load");
		if (have) {
			HL(h, "?root=");
			h_id(h, &root);
		}
		HL(h, "\">resource load</a> &middot; export <a href=\"/export?format=csv");
		if (have) {
			HL(h, "&amp;root=");
			h_id(h, &root);
		}
		HL(h, "\">CSV</a> <a href=\"/export?format=json");
		if (have) {
			HL(h, "&amp;root=");
			h_id(h, &root);
		}
		HL(h, "\">JSON</a> <a href=\"/export?format=mermaid");
		if (have) {
			HL(h, "&amp;root=");
			h_id(h, &root);
		}
		HL(h, "\">Mermaid</a></p>\n");
	}
	h_gantt(x, s, page_today(x), critical);
	HL(h, "<table class=\"plan\"><thead><tr><th>WBS</th><th>ID</th><th>Title</th><th>Status</th><th>Start</th>"
	      "<th>Finish</th><th class=\"num\">Duration</th><th class=\"num\">Float</th><th></th></tr></thead><tbody>\n");
	for (size_t i = 0; i < s->ntasks; i++) {
		const tix_plan_task *t = &s->tasks[i];
		if (critical && !t->critical)
			continue;
		HL(h, "<tr>");
		task_cells(x, t);
		HL(h, "<td class=\"num\">");
		if (t->scheduled) {
			h_int(h, t->total_float);
			HL(h, "d");
		}
		HL(h, "</td><td>");
		if (t->critical)
			HL(h, "<span class=\"crit\">critical</span> ");
		if (t->late)
			HL(h, "<b class=\"crit\">late</b> ");
		if (t->milestone)
			HL(h, "milestone ");
		if (t->manual)
			HL(h, "fixed");
		HL(h, "</td></tr>\n");
	}
	HL(h, "</tbody></table>\n");
	diagnostics(x, s);
	page_end(x);
	tix_plan_result_free(&pr);
}

void page_load(web_ctx *x)
{
	tix_oid root;
	bool have;
	tix_plan_result pr;
	if (!begin(x, "Resource load", &root, &have, &pr))
		return;
	html *h = &x->h;
	root_form(x, "/load", have ? &root : NULL, false);
	const tix_schedule *s = pr.schedule;
	if (!s->nloads) {
		HL(h, "<p class=\"note\">No resources are assigned in this plan.</p>\n");
		page_end(x);
		tix_plan_result_free(&pr);
		return;
	}
	/* one column per day up to 13 weeks, else per week (the week's peak) */
	int step = s->ndays > 91 ? 7 : 1;
	size_t cols = (s->ndays + (size_t)step - 1) / (size_t)step;
	if (cols > 120)
		cols = 120;
	HL(h, "<p class=\"note\">Percent of each resource's capacity");
	if (step == 7)
		HL(h, ", the busiest day of each week");
	HL(h, ". Over 100%, or any load on a day off, is over-allocated.</p>\n<div class=\"wide\"><table class=\"load\">"
	      "<thead><tr><th>Resource</th><th class=\"num\">Peak</th><th class=\"num\">Over</th>");
	for (size_t c = 0; c < cols; c++) {
		HL(h, "<th class=\"num\">");
		char d[TIX_DATE_STR];
		tix_date_str((tix_date)(s->start + (tix_date)(c * (size_t)step)), d);
		h_text(h, d + 5); /* MM-DD */
		HL(h, "</th>");
	}
	HL(h, "</tr></thead><tbody>\n");
	for (size_t i = 0; i < s->nloads; i++) {
		const tix_plan_load *l = &s->loads[i];
		HL(h, "<tr><td>");
		h_named(x, TIX_ENTITY_RESOURCE, &l->resource->id, &pr.resources);
		HL(h, "</td><td class=\"num\">");
		h_int(h, l->peak);
		HL(h, "%</td><td class=\"num");
		if (l->over_days)
			HL(h, " crit");
		HL(h, "\">");
		h_int(h, (long long)l->over_days);
		HL(h, "d</td>");
		for (size_t c = 0; c < cols; c++) {
			int32_t peak = 0;
			bool over = false;
			for (size_t d = c * (size_t)step; d < (c + 1) * (size_t)step && d < s->ndays; d++) {
				if (l->units[d] > peak)
					peak = l->units[d];
				over |= l->units[d] > 100 || (l->units[d] > 0 && !l->working[d]);
			}
			HL(h, "<td class=\"num");
			if (over)
				HL(h, " crit");
			HL(h, "\">");
			if (peak)
				h_int(h, peak);
			HL(h, "</td>");
		}
		HL(h, "</tr>\n");
	}
	HL(h, "</tbody></table></div>\n");
	diagnostics(x, s);
	page_end(x);
	tix_plan_result_free(&pr);
}

/* GET /export?format=csv|json|mermaid&root=ID: a download, escaped by the export module. */
void page_export(web_ctx *x)
{
	tix_export_format f;
	const char *fmt = web_kvs_get(&x->req->query, "format");
	if (!fmt || !tix_export_format_parse(fmt, &f)) {
		page_error(x, 400, "Export", "format must be csv, json or mermaid.");
		return;
	}
	tix_oid root;
	bool have;
	tix_plan_result pr;
	if (!query_id(x, "root", TIX_ENTITY_TICKET, &root, &have) || !page_plan(x, have ? &root : NULL, &pr))
		return;
	const tix_entity *r = have ? list_find(&pr.tickets, &root) : NULL;
	tix_export(pr.schedule, f, r ? r->u.ticket.title : NULL, &x->res->body);
	static const char *const types[] = {"text/plain; charset=utf-8", "text/csv; charset=utf-8",
					    "application/json"};
	static const char *const names[] = {"attachment; filename=\"plan.mmd\"", "attachment; filename=\"plan.csv\"",
					    "attachment; filename=\"plan.json\""};
	web_header(x->res, "Content-Type", types[f]);
	web_header(x->res, "Content-Disposition", names[f]);
	web_header(x->res, "Cache-Control", "no-store");
	tix_plan_result_free(&pr);
}
