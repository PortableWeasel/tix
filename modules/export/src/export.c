/* Schedule renderers (see tix/export.h). */
#include "export_internal.h"
#include "tix/base_json.h"

#include <stdio.h>
#include <string.h>

bool tix_export_format_parse(const char *s, tix_export_format *out)
{
	static const char *const names[] = {"mermaid", "csv", "json", "mspdi"};
	for (int i = 0; i < 4; i++)
		if (!strcmp(s, names[i])) {
			*out = (tix_export_format)i;
			return true;
		}
	return false;
}

static const char *hex7(const tix_oid *id, char *out)
{
	char hex[TIX_OID_MAX_HEX + 1];
	snprintf(out, 8, "%.7s", tix_oid_hex(id, hex));
	return out;
}

static const char *kind_name(const tix_plan_task *t)
{
	return t->summary ? "summary" : t->milestone ? "milestone" : "task";
}

/* ---- Mermaid ----------------------------------------------------------------- */

/* Mermaid task lines are "name :meta"; ':' ';' '#' and leading/trailing blanks
 * would break the syntax, so they become spaces / are trimmed. */
static void mermaid_text(tix_buf *out, const char *s)
{
	while (*s == ' ')
		s++;
	size_t start = out->len;
	for (; *s; s++)
		tix_buf_addc(out, (*s == ':' || *s == ';' || *s == '#' || *s == '\t') ? ' ' : *s);
	while (out->len > start && out->data[out->len - 1] == ' ')
		out->data[--out->len] = '\0';
	if (out->len == start)
		tix_buf_adds(out, "untitled");
}

static void mermaid(const tix_schedule *s, const char *title, tix_buf *out)
{
	char d1[TIX_DATE_STR], d2[TIX_DATE_STR], id[8];
	tix_buf_adds(out, "gantt\n    title ");
	mermaid_text(out, title ? title : "tix plan");
	tix_buf_adds(out, "\n    dateFormat YYYY-MM-DD\n    axisFormat %Y-%m-%d\n");
	for (size_t top = 0; top < s->ntasks; top++) {
		if (s->tasks[top].depth != 0 || !s->tasks[top].scheduled)
			continue;
		tix_buf_addf(out, "    section %s ", s->tasks[top].wbs);
		mermaid_text(out, s->tasks[top].ticket->u.ticket.title);
		tix_buf_addc(out, '\n');
		for (size_t i = top; i < s->ntasks && (i == top || s->tasks[i].depth > 0); i++) {
			const tix_plan_task *t = &s->tasks[i];
			if (!t->scheduled)
				continue;
			tix_buf_addf(out, "    %s ", t->wbs);
			mermaid_text(out, t->ticket->u.ticket.title);
			tix_buf_adds(out, " :");
			if (t->critical)
				tix_buf_adds(out, "crit, ");
			if (t->progress >= 100)
				tix_buf_adds(out, "done, ");
			else if (t->ticket->u.ticket.status == TIX_TICKET_IN_PROGRESS)
				tix_buf_adds(out, "active, ");
			if (t->milestone)
				tix_buf_addf(out, "milestone, t%s, %s, 0d\n", hex7(&t->ticket->id, id), tix_date_str(t->start, d1));
			else /* Mermaid's end date is exclusive */
				tix_buf_addf(out, "t%s, %s, %s\n", hex7(&t->ticket->id, id), tix_date_str(t->start, d1),
					     tix_date_str(t->finish + 1, d2));
		}
	}
}

/* ---- CSV -------------------------------------------------------------------------- */

static void csv_cell(tix_buf *out, const char *s, bool text)
{
	bool quote = strpbrk(s, ",\"\r\n") || (*s && (s[0] == ' ' || s[strlen(s) - 1] == ' '));
	bool formula = text && *s && strchr("=+-@\t\r", s[0]);
	if (!quote && !formula) {
		tix_buf_adds(out, s);
		return;
	}
	tix_buf_addc(out, '"');
	if (formula)
		tix_buf_addc(out, '\'');
	for (; *s; s++) {
		if (*s == '"')
			tix_buf_addc(out, '"');
		tix_buf_addc(out, *s);
	}
	tix_buf_addc(out, '"');
}

static const char *csv_date(tix_date d, char *buf)
{
	if (d == TIX_DATE_NONE)
		return "";
	return tix_date_str(d, buf);
}

static const char *csv_int(int64_t v, char *buf)
{
	if (v < 0) {
		buf[0] = '\0';
		return buf;
	}
	snprintf(buf, 24, "%lld", (long long)v);
	return buf;
}

/* Hundredths as a decimal ("4000.00"); tix stores money in hundredths of the currency. */
static void csv_money(tix_buf *out, bool known, int64_t v)
{
	if (!known) {
		tix_buf_addc(out, ',');
		return;
	}
	long long a = v < 0 ? -(long long)v : (long long)v;
	tix_buf_addf(out, ",%s%lld.%02lld", v < 0 ? "-" : "", a / 100, a % 100);
}

static void csv_delta(tix_buf *out, bool known, int64_t v)
{
	if (known)
		tix_buf_addf(out, ",%lld", (long long)v);
	else
		tix_buf_addc(out, ',');
}

static void csv(const tix_schedule *s, const tix_plan_variance *v, tix_buf *out)
{
	tix_buf_adds(out, "wbs,id,title,kind,status,start,finish,duration_days,effort_hours,progress,total_float,"
			  "critical,late_start,late_finish,deadline,actual_start,actual_finish,assignments,cost,currency");
	if (v)
		tix_buf_adds(out, ",baseline_start,baseline_finish,start_slip_days,finish_slip_days,duration_delta_days,"
				  "effort_delta_hours,cost_delta");
	tix_buf_adds(out, "\r\n");
	for (size_t i = 0; i < s->ntasks; i++) {
		const tix_plan_task *t = &s->tasks[i];
		const tix_ticket *tk = &t->ticket->u.ticket;
		char hex[TIX_OID_MAX_HEX + 1], d[TIX_DATE_STR], n[24], fl[24];
		tix_buf as = TIX_BUF_INIT;
		for (size_t k = 0; k < tk->plan.nassignments; k++) {
			const tix_assignment *a = &tk->plan.assignments[k];
			const tix_entity *r = tix_schedule_resource(s, &a->resource);
			char id[8];
			tix_buf_addf(&as, "%s%s:%d%%", k ? "; " : "", r ? r->u.resource.name : hex7(&a->resource, id),
				     (int)a->units);
		}
		csv_cell(out, t->wbs, false);
		tix_buf_addc(out, ',');
		csv_cell(out, tix_oid_hex(&t->ticket->id, hex), false);
		tix_buf_addc(out, ',');
		csv_cell(out, tk->title, true);
		tix_buf_addf(out, ",%s,%s,", kind_name(t), tix_ticket_status_name(tk->status));
		csv_cell(out, csv_date(t->start, d), false);
		tix_buf_addc(out, ',');
		csv_cell(out, csv_date(t->finish, d), false);
		tix_buf_addf(out, ",%s,", csv_int(t->duration, n));
		tix_buf_addf(out, "%s,%d,", csv_int(t->effort, n), t->progress);
		snprintf(fl, sizeof(fl), "%d", (int)t->total_float);
		tix_buf_addf(out, "%s,%s,", t->scheduled ? fl : "", t->scheduled ? (t->critical ? "yes" : "no") : "");
		tix_buf_addf(out, "%s,", csv_date(t->late_start, d));
		tix_buf_addf(out, "%s,", csv_date(t->late_finish, d));
		tix_buf_addf(out, "%s,", csv_date(tk->plan.deadline, d));
		tix_buf_addf(out, "%s,", csv_date(t->actual_start, d));
		tix_buf_addf(out, "%s,", csv_date(t->actual_finish, d));
		csv_cell(out, tix_buf_cstr(&as), true);
		csv_money(out, t->cost >= 0, t->cost);
		tix_buf_addc(out, ',');
		csv_cell(out, t->currency ? t->currency : "", false);
		if (v) {
			const tix_plan_variance *x = &v[i];
			tix_buf_addf(out, ",%s", csv_date(x->start, d));
			tix_buf_addf(out, ",%s", csv_date(x->finish, d));
			csv_delta(out, x->dates, x->start_slip);
			csv_delta(out, x->dates, x->finish_slip);
			csv_delta(out, x->has_duration, x->duration_delta);
			csv_delta(out, x->has_effort, x->effort_delta);
			csv_money(out, x->has_cost, x->cost_delta);
		}
		tix_buf_adds(out, "\r\n");
		tix_buf_free(&as);
	}
}

/* ---- JSON ------------------------------------------------------------------------- */

static void j_date(tix_jw *w, const char *k, tix_date d)
{
	char s[TIX_DATE_STR];
	tix_jw_key(w, k);
	if (d == TIX_DATE_NONE)
		tix_jw_null(w);
	else
		tix_jw_str(w, tix_date_str(d, s));
}

static void j_id(tix_jw *w, const char *k, const tix_oid *id)
{
	char hex[TIX_OID_MAX_HEX + 1];
	tix_jw_key(w, k);
	if (!id || tix_oid_is_null(id))
		tix_jw_null(w);
	else
		tix_jw_str(w, tix_oid_hex(id, hex));
}

static void j_int(tix_jw *w, const char *k, int64_t v, bool present)
{
	tix_jw_key(w, k);
	if (present)
		tix_jw_int(w, v);
	else
		tix_jw_null(w);
}

static void json(const tix_schedule *s, const tix_plan_variance *v, tix_buf *out)
{
	tix_jw w;
	tix_jw_init(&w, out);
	tix_jw_obj_begin(&w);
	j_date(&w, "start", s->start);
	j_date(&w, "finish", s->finish);
	tix_jw_key(&w, "tasks");
	tix_jw_arr_begin(&w);
	for (size_t i = 0; i < s->ntasks; i++) {
		const tix_plan_task *t = &s->tasks[i];
		const tix_ticket *tk = &t->ticket->u.ticket;
		tix_jw_obj_begin(&w);
		tix_jw_key(&w, "wbs");
		tix_jw_str(&w, t->wbs);
		j_id(&w, "id", &t->ticket->id);
		j_id(&w, "parent", t->parent == SIZE_MAX ? NULL : &s->tasks[t->parent].ticket->id);
		tix_jw_key(&w, "title");
		tix_jw_str(&w, tk->title);
		tix_jw_key(&w, "kind");
		tix_jw_str(&w, kind_name(t));
		tix_jw_key(&w, "status");
		tix_jw_str(&w, tix_ticket_status_name(tk->status));
		tix_jw_key(&w, "stage");
		if (tk->stage)
			tix_jw_str(&w, tk->stage);
		else
			tix_jw_null(&w);
		tix_jw_key(&w, "scheduled");
		tix_jw_bool(&w, t->scheduled);
		j_date(&w, "start", t->start);
		j_date(&w, "finish", t->finish);
		j_date(&w, "late_start", t->late_start);
		j_date(&w, "late_finish", t->late_finish);
		j_int(&w, "duration", t->duration, t->duration >= 0);
		j_int(&w, "effort", t->effort, t->effort >= 0);
		j_int(&w, "progress", t->progress, true);
		j_int(&w, "total_float", t->total_float, t->scheduled);
		tix_jw_key(&w, "critical");
		tix_jw_bool(&w, t->critical);
		tix_jw_key(&w, "late");
		tix_jw_bool(&w, t->late);
		j_date(&w, "deadline", tk->plan.deadline);
		j_date(&w, "actual_start", t->actual_start);
		j_date(&w, "actual_finish", t->actual_finish);
		j_int(&w, "cost", t->cost, t->cost >= 0);
		tix_jw_key(&w, "currency");
		if (t->currency)
			tix_jw_str(&w, t->currency);
		else
			tix_jw_null(&w);
		if (v) {
			const tix_plan_variance *x = &v[i];
			tix_jw_key(&w, "baseline");
			if (!x->baselined) {
				tix_jw_null(&w);
			} else {
				tix_jw_obj_begin(&w);
				j_date(&w, "start", x->start);
				j_date(&w, "finish", x->finish);
				j_int(&w, "duration", x->duration, x->duration >= 0);
				j_int(&w, "effort", x->effort, x->effort >= 0);
				j_int(&w, "cost", x->cost, x->cost >= 0);
				j_int(&w, "start_slip", x->start_slip, x->dates);
				j_int(&w, "finish_slip", x->finish_slip, x->dates);
				j_int(&w, "duration_delta", x->duration_delta, x->has_duration);
				j_int(&w, "effort_delta", x->effort_delta, x->has_effort);
				j_int(&w, "cost_delta", x->cost_delta, x->has_cost);
				tix_jw_obj_end(&w);
			}
		}
		tix_jw_key(&w, "deps");
		tix_jw_arr_begin(&w);
		for (size_t k = 0; k < tk->plan.ndeps; k++) {
			tix_jw_obj_begin(&w);
			j_id(&w, "pred", &tk->plan.deps[k].pred);
			tix_jw_key(&w, "kind");
			tix_jw_str(&w, tix_dep_kind_name(tk->plan.deps[k].kind));
			j_int(&w, "lag", tk->plan.deps[k].lag, true);
			tix_jw_obj_end(&w);
		}
		tix_jw_arr_end(&w);
		tix_jw_key(&w, "assignments");
		tix_jw_arr_begin(&w);
		for (size_t k = 0; k < tk->plan.nassignments; k++) {
			const tix_assignment *a = &tk->plan.assignments[k];
			const tix_entity *r = tix_schedule_resource(s, &a->resource);
			tix_jw_obj_begin(&w);
			j_id(&w, "resource", &a->resource);
			tix_jw_key(&w, "name");
			if (r)
				tix_jw_str(&w, r->u.resource.name);
			else
				tix_jw_null(&w);
			j_int(&w, "units", a->units, true);
			j_int(&w, "effort", a->effort, a->effort >= 0);
			tix_jw_obj_end(&w);
		}
		tix_jw_arr_end(&w);
		tix_jw_obj_end(&w);
	}
	tix_jw_arr_end(&w);
	tix_jw_key(&w, "load");
	tix_jw_arr_begin(&w);
	for (size_t i = 0; i < s->nloads; i++) {
		const tix_plan_load *l = &s->loads[i];
		tix_jw_obj_begin(&w);
		j_id(&w, "resource", &l->resource->id);
		tix_jw_key(&w, "name");
		tix_jw_str(&w, l->resource->u.resource.name);
		j_int(&w, "capacity", l->resource->u.resource.capacity, true);
		j_int(&w, "peak", l->peak, true);
		j_int(&w, "over_days", (int64_t)l->over_days, true);
		tix_jw_key(&w, "units");
		tix_jw_arr_begin(&w);
		for (size_t d = 0; d < s->ndays; d++)
			tix_jw_int(&w, l->units[d]);
		tix_jw_arr_end(&w);
		tix_jw_key(&w, "working");
		tix_jw_arr_begin(&w);
		for (size_t d = 0; d < s->ndays; d++)
			tix_jw_bool(&w, l->working[d]);
		tix_jw_arr_end(&w);
		tix_jw_obj_end(&w);
	}
	tix_jw_arr_end(&w);
	tix_jw_key(&w, "diagnostics");
	tix_jw_arr_begin(&w);
	for (size_t i = 0; i < s->ndiags; i++) {
		tix_jw_obj_begin(&w);
		j_id(&w, "id", &s->diags[i].id);
		tix_jw_key(&w, "message");
		tix_jw_str(&w, s->diags[i].msg);
		tix_jw_obj_end(&w);
	}
	tix_jw_arr_end(&w);
	tix_jw_obj_end(&w);
	tix_buf_addc(out, '\n');
}

void tix_export_compare(const tix_schedule *s, tix_export_format f, const char *title, const tix_plan_variance *v,
			tix_buf *out)
{
	TIX_REQUIRE(s && out);
	switch (f) {
	case TIX_EXPORT_MERMAID: mermaid(s, title, out); break;
	case TIX_EXPORT_CSV: csv(s, v, out); break;
	case TIX_EXPORT_JSON: json(s, v, out); break;
	case TIX_EXPORT_MSPDI: tix_export_mspdi(s, title, v, out); break;
	}
}

void tix_export(const tix_schedule *s, tix_export_format f, const char *title, tix_buf *out)
{
	tix_export_compare(s, f, title, NULL, out);
}
