/* Plan pages: /wbs, /plan and /load, all with ?root=ID (default: every ticket). */
#include "app.h"
#include "tix/export.h"

#include <stdlib.h>
#include <string.h>

/* The baseline picker (plan page): every baseline, newest first. */
static void baseline_select(web_ctx *x, const tix_oid *current)
{
	html *h = &x->h;
	tix_entity_list l;
	if (tix_baseline_list(x->app->cfg.repo, &l, NULL) || !l.n) {
		if (!l.n)
			tix_entity_list_free(&l);
		return;
	}
	HL(h, " <label>Baseline <select name=\"baseline\"><option value=\"\">none</option>");
	for (size_t i = 0; i < l.n; i++) {
		HL(h, "<option value=\"");
		h_id(h, &l.v[i]->id);
		HL(h, "\"");
		if (current && tix_oid_eq(current, &l.v[i]->id))
			HL(h, " selected");
		HL(h, ">");
		h_text(h, l.v[i]->u.baseline.name);
		HL(h, " (");
		h_date(h, l.v[i]->u.baseline.taken);
		HL(h, ")</option>");
	}
	HL(h, "</select></label>");
	tix_entity_list_free(&l);
}

/* The root picker shared by the three pages. */
static void root_form(web_ctx *x, const char *action_lit_path, const tix_oid *root, bool with_views,
		      const tix_oid *baseline)
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
		baseline_select(x, baseline);
	}
	HL(h, " <button>Show</button></form>\n");
}

/* <a href="/export?format=F[&root=R][&baseline=B]">label</a>, then a space. */
static void export_link(web_ctx *x, const char *fmt, const char *label, const tix_oid *root, const tix_oid *bl)
{
	html *h = &x->h;
	HL(h, "<a href=\"/export?format=");
	h_text(h, fmt);
	if (root) {
		HL(h, "&amp;root=");
		h_id(h, root);
	}
	if (bl) {
		HL(h, "&amp;baseline=");
		h_id(h, bl);
	}
	HL(h, "\">");
	h_text(h, label);
	HL(h, "</a> ");
}

/* ?baseline=ID: loads it and compares (all left NULL without the parameter). RETURNS false
 * after an error page. */
static bool load_variance(web_ctx *x, const tix_schedule *s, tix_oid *bid, tix_entity **bl,
			  tix_plan_variance **v, size_t *removed)
{
	bool have;
	*bl = NULL;
	*v = NULL;
	*removed = 0;
	if (!query_id(x, "baseline", TIX_ENTITY_BASELINE, bid, &have))
		return false;
	if (!have)
		return true;
	if (tix_entity_load(x->app->cfg.repo, TIX_ENTITY_BASELINE, bid, bl, &x->app->err)) {
		page_error(x, 404, "Not found", x->app->err.msg);
		return false;
	}
	*v = tix_xcalloc(s->ntasks ? s->ntasks : 1, sizeof(**v));
	*removed = tix_plan_compare(s, &(*bl)->u.baseline, *v);
	return true;
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
	h_state(h, t->ticket->u.ticket.status, t->ticket->u.ticket.stage);
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
	root_form(x, "/wbs", have ? &root : NULL, false, NULL);
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
	const tix_schedule *s = pr.schedule;
	tix_oid bid;
	tix_entity *bl = NULL;
	tix_plan_variance *v = NULL;
	size_t removed = 0;
	if (!load_variance(x, s, &bid, &bl, &v, &removed)) {
		tix_plan_result_free(&pr);
		return;
	}
	root_form(x, "/plan", have ? &root : NULL, true, bl ? &bid : NULL);
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
		HL(h, "\">resource load</a> &middot; export ");
		export_link(x, "csv", "CSV", have ? &root : NULL, bl ? &bid : NULL);
		export_link(x, "json", "JSON", have ? &root : NULL, bl ? &bid : NULL);
		export_link(x, "mermaid", "Mermaid", have ? &root : NULL, bl ? &bid : NULL);
		export_link(x, "mspdi", "MS Project", have ? &root : NULL, bl ? &bid : NULL);
		if (bl) {
			HL(h, " (the downloads compare with <b>");
			h_text(h, bl->u.baseline.name);
			HL(h, "</b>)");
		}
		HL(h, "</p>\n");
	}
	if (bl) {
		size_t later = 0, earlier = 0, added = 0;
		for (size_t i = 0; i < s->ntasks; i++) {
			if (!v[i].baselined)
				added++;
			else if (v[i].dates && v[i].finish_slip > 0)
				later++;
			else if (v[i].dates && v[i].finish_slip < 0)
				earlier++;
		}
		HL(h, "<p class=\"note\">Against baseline <b>");
		h_text(h, bl->u.baseline.name);
		HL(h, "</b> (");
		h_date(h, bl->u.baseline.taken);
		HL(h, "): ");
		h_int(h, (long long)later);
		HL(h, " finish later, ");
		h_int(h, (long long)earlier);
		HL(h, " earlier, ");
		h_int(h, (long long)added);
		HL(h, " new, ");
		h_int(h, (long long)removed);
		HL(h, " removed since. Thin bars in the chart are the baseline.</p>\n");
	}
	h_gantt(x, s, page_today(x), critical, v);
	HL(h, "<table class=\"plan\"><thead><tr><th>WBS</th><th>ID</th><th>Title</th><th>Status</th><th>Start</th>"
	      "<th>Finish</th><th class=\"num\">Duration</th><th class=\"num\">Float</th>");
	if (v)
		HL(h, "<th>Baseline finish</th><th class=\"num\">Slip</th>");
	HL(h, "<th></th></tr></thead><tbody>\n");
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
		HL(h, "</td>");
		if (v) {
			HL(h, "<td>");
			if (!v[i].baselined)
				HL(h, "<span class=\"note\">new</span>");
			else
				h_date(h, v[i].finish);
			HL(h, "</td><td class=\"num");
			if (v[i].dates && v[i].finish_slip > 0)
				HL(h, " crit");
			HL(h, "\">");
			if (v[i].dates) {
				if (v[i].finish_slip > 0)
					HL(h, "+");
				h_int(h, v[i].finish_slip);
				HL(h, "d");
			}
			HL(h, "</td>");
		}
		HL(h, "<td>");
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
	free(v);
	tix_entity_free(bl);
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
	root_form(x, "/load", have ? &root : NULL, false, NULL);
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

/* GET /export?format=csv|json|mermaid|mspdi&root=ID&baseline=ID: a download, escaped by the export module. */
void page_export(web_ctx *x)
{
	tix_export_format f;
	const char *fmt = web_kvs_get(&x->req->query, "format");
	if (!fmt || !tix_export_format_parse(fmt, &f)) {
		page_error(x, 400, "Export", "format must be csv, json, mermaid or mspdi.");
		return;
	}
	tix_oid root;
	bool have;
	tix_plan_result pr;
	if (!query_id(x, "root", TIX_ENTITY_TICKET, &root, &have) || !page_plan(x, have ? &root : NULL, &pr))
		return;
	const tix_entity *r = have ? list_find(&pr.tickets, &root) : NULL;
	tix_oid bid;
	tix_entity *bl;
	tix_plan_variance *v;
	size_t removed;
	if (!load_variance(x, pr.schedule, &bid, &bl, &v, &removed)) {
		tix_plan_result_free(&pr);
		return;
	}
	tix_export_compare(pr.schedule, f, r ? r->u.ticket.title : NULL, v, &x->res->body);
	free(v);
	tix_entity_free(bl);
	static const char *const types[] = {
		[TIX_EXPORT_MERMAID] = "text/plain; charset=utf-8", [TIX_EXPORT_CSV] = "text/csv; charset=utf-8",
		[TIX_EXPORT_JSON] = "application/json", [TIX_EXPORT_MSPDI] = "application/xml"};
	static const char *const names[] = {[TIX_EXPORT_MERMAID] = "attachment; filename=\"plan.mmd\"",
					    [TIX_EXPORT_CSV] = "attachment; filename=\"plan.csv\"",
					    [TIX_EXPORT_JSON] = "attachment; filename=\"plan.json\"",
					    [TIX_EXPORT_MSPDI] = "attachment; filename=\"plan.xml\""};
	_Static_assert(sizeof(types) / sizeof(*types) == TIX_EXPORT_MSPDI + 1, "a type per format");
	web_header(x->res, "Content-Type", types[f]);
	web_header(x->res, "Content-Disposition", names[f]);
	web_header(x->res, "Cache-Control", "no-store");
	tix_plan_result_free(&pr);
}
