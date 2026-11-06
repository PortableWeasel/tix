/* export: golden Mermaid / CSV / JSON output for a small plan. */
#include "tix/base_json.h"
#include "tix/export.h"
#include "tix/testkit.h"

#include <stdlib.h>

static tix_error e = TIX_ERROR_INIT;

static tix_oid oid(int n)
{
	tix_oid o;
	memset(&o, 0, sizeof(o));
	o.algo = TIX_HASH_SHA1;
	o.bytes[0] = (uint8_t)n;
	return o;
}

static tix_entity *task(int n, const char *title, int64_t duration)
{
	tix_entity *t = tix_xcalloc(1, sizeof(*t));
	t->type = TIX_ENTITY_TICKET;
	t->id = oid(n);
	t->u.ticket.title = tix_xstrdup(title);
	t->u.ticket.body = tix_xstrdup("");
	tix_ticket_plan *p = &t->u.ticket.plan;
	p->start = p->finish = p->deadline = p->actual_start = p->actual_finish = TIX_DATE_NONE;
	p->duration = duration;
	p->effort = -1;
	p->started_at = p->done_at = -1;
	return t;
}

int main(void)
{
	tix_entity *proj = task(1, "Build: phase #1", -1), *a = task(2, "=SUM(A1)", 2), *m = task(3, "Done, \"really\"", 0);
	tix_entity *u = task(4, "Someday", -1), *r = tix_xcalloc(1, sizeof(*r));
	r->type = TIX_ENTITY_RESOURCE;
	r->id = oid(9);
	r->u.resource.name = tix_xstrdup("Ada");
	r->u.resource.capacity = 100;
	a->u.ticket.plan.parent = proj->id;
	m->u.ticket.plan.parent = proj->id;
	m->u.ticket.plan.kind = TIX_KIND_MILESTONE;
	m->u.ticket.plan.deps = tix_xcalloc(1, sizeof(tix_dep));
	m->u.ticket.plan.deps[0] = (tix_dep){a->id, TIX_DEP_FS, 0, {1, {oid(50), 2}}};
	m->u.ticket.plan.ndeps = 1;
	a->u.ticket.plan.assignments = tix_xcalloc(1, sizeof(tix_assignment));
	a->u.ticket.plan.assignments[0] = (tix_assignment){r->id, 50, 8};
	a->u.ticket.plan.nassignments = 1;
	a->u.ticket.status = TIX_TICKET_IN_PROGRESS;
	a->u.ticket.plan.rank = tix_xstrdup("a");
	const tix_entity *tickets[] = {u, m, a, proj}, *resources[] = {r};
	tix_date today;
	TIX_REQUIRE(tix_date_parse("2026-11-12", &today)); /* a Thursday */
	tix_plan_input in = {tickets, 4, resources, 1, NULL, 0, NULL, today};
	tix_schedule *s = NULL;
	TK_OK(tix_plan_compute(&in, &s, &e), &e);
	if (!s)
		return tk_finish();
	tix_buf b = TIX_BUF_INIT;
	tix_export_format f;
	TK_CHECK(tix_export_format_parse("csv", &f) && f == TIX_EXPORT_CSV && !tix_export_format_parse("xml", &f));

	tix_export(s, TIX_EXPORT_MERMAID, "Plan: Q4", &b);
	TK_STR_EQ(tix_buf_cstr(&b),
		  "gantt\n"
		  "    title Plan  Q4\n"
		  "    dateFormat YYYY-MM-DD\n"
		  "    axisFormat %Y-%m-%d\n"
		  "    section 1 Build  phase  1\n"
		  "    1 Build  phase  1 :crit, t0100000, 2026-11-12, 2026-11-14\n"
		  "    1.1 =SUM(A1) :crit, active, t0200000, 2026-11-12, 2026-11-14\n"
		  "    1.2 Done, \"really\" :crit, milestone, t0300000, 2026-11-13, 0d\n");

	tix_buf_reset(&b);
	tix_export(s, TIX_EXPORT_CSV, NULL, &b);
	TK_STR_EQ(tix_buf_cstr(&b),
		  "wbs,id,title,kind,status,start,finish,duration_days,effort_hours,progress,total_float,critical,"
		  "late_start,late_finish,deadline,actual_start,actual_finish,assignments\r\n"
		  "1,0100000000000000000000000000000000000000,Build: phase #1,summary,open,2026-11-12,2026-11-13,2,8,0,0,yes,"
		  "2026-11-12,2026-11-13,,,,\r\n"
		  "1.1,0200000000000000000000000000000000000000,\"'=SUM(A1)\",task,in-progress,2026-11-12,2026-11-13,2,8,0,0,yes,"
		  "2026-11-12,2026-11-13,,,,Ada:50%\r\n"
		  "1.2,0300000000000000000000000000000000000000,\"Done, \"\"really\"\"\",milestone,open,2026-11-13,2026-11-13,0,,0,0,yes,"
		  "2026-11-13,2026-11-13,,,,\r\n"
		  "2,0400000000000000000000000000000000000000,Someday,task,open,,,,,0,,,,,,,,\r\n");

	tix_buf_reset(&b);
	tix_export(s, TIX_EXPORT_JSON, NULL, &b);
	TK_CHECK(b.len > 0 && b.data[b.len - 1] == '\n');
	TK_CHECK(strstr(b.data, "{\"start\":\"2026-11-12\",\"finish\":\"2026-11-13\",\"tasks\":[{\"wbs\":\"1\",") == b.data);
	TK_CHECK(strstr(b.data, "\"assignments\":[{\"resource\":\"0900000000000000000000000000000000000000\",\"name\":\"Ada\","
				"\"units\":50,\"effort\":8}]"));
	TK_CHECK(strstr(b.data, "\"load\":[{\"resource\":\"0900000000000000000000000000000000000000\",\"name\":\"Ada\","
				"\"capacity\":100,\"peak\":50,\"over_days\":0,\"units\":[50,50],\"working\":[true,true]}]"));
	TK_CHECK(strstr(b.data, "\"title\":\"Someday\",\"kind\":\"task\",\"status\":\"open\",\"scheduled\":false,\"start\":null"));
	TK_CHECK(tix_utf8_valid(b.data, b.len));
	tix_buf_free(&b);
	tix_schedule_free(s);
	tix_entity_free(proj);
	tix_entity_free(a);
	tix_entity_free(m);
	tix_entity_free(u);
	tix_entity_free(r);
	return tk_finish();
}
