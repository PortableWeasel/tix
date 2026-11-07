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

/* Every <Task> block's elements must follow the MSPDI schema's sequence (readers are
 * order-sensitive). The subset tix writes, in schema order: */
static const char *const task_order[] = {
	"UID", "ID", "Name", "Active", "Manual", "Type", "IsNull", "WBS", "OutlineNumber", "OutlineLevel", "Priority",
	"Start", "Finish", "Duration", "DurationFormat", "Work", "EffortDriven", "Estimated", "Milestone", "Summary",
	"Critical", "EarlyStart", "EarlyFinish", "LateStart", "LateFinish", "TotalSlack", "PercentComplete", "Cost",
	"ActualStart", "ActualFinish", "ConstraintType", "CalendarUID", "ConstraintDate", "Deadline", "Notes",
	"PredecessorLink", "Baseline", "ManualStart", "ManualFinish", "ManualDuration", NULL};

static int order_of(const char *name, size_t n)
{
	for (int i = 0; task_order[i]; i++)
		if (strlen(task_order[i]) == n && !strncmp(task_order[i], name, n))
			return i;
	return -1;
}

static void check_task_order(const char *xml)
{
	for (const char *t = strstr(xml, "<Task>"); t; t = strstr(t + 1, "<Task>")) {
		const char *end = strstr(t, "</Task>");
		int last = -1;
		for (const char *p = t + 6; p && p < end;) {
			size_t n = strcspn(p + 1, ">");
			int k = order_of(p + 1, n);
			if (k < 0 || k < last)
				tk_fail(__FILE__, __LINE__, "Task element <%.*s> out of schema order", (int)n, p + 1);
			last = k;
			const char *close = strstr(p, "</");
			if (!strncmp(p + 1, "PredecessorLink", 15)) /* skip its children */
				close = strstr(p, "</PredecessorLink>");
			else if (!strncmp(p + 1, "Baseline", 8))
				close = strstr(p, "</Baseline>");
			p = close ? strchr(close, '>') : NULL;
			if (p)
				p++;
			while (p && p < end && *p != '<')
				p++;
		}
	}
}

/* Overlapping tix calendar exceptions (later wins) become disjoint MSPDI runs. */
static void test_mspdi_calendar(void)
{
	tix_entity *cal = tix_xcalloc(1, sizeof(*cal)), *t = task(20, "Under a calendar", 3);
	cal->type = TIX_ENTITY_CALENDAR;
	cal->id = oid(30);
	tix_calendar *c = &cal->u.calendar;
	c->name = tix_xstrdup("Site & <yard>");
	c->weekdays = 0x3f; /* Mon-Sat */
	c->hours = 6;
	c->exceptions = tix_xcalloc(3, sizeof(tix_cal_exception));
	c->nexceptions = 3;
	tix_date d1, d2, d3, d4;
	TIX_REQUIRE(tix_date_parse("2026-12-24", &d1) && tix_date_parse("2027-01-01", &d2) &&
		    tix_date_parse("2026-12-26", &d3) && tix_date_parse("2026-12-31", &d4));
	c->exceptions[0] = (tix_cal_exception){.from = d1, .to = d2, .working = false, .note = tix_xstrdup("Holidays"), .order = {.lamport = 1}};
	c->exceptions[1] = (tix_cal_exception){.from = d3, .to = d3, .working = true, .order = {.lamport = 2}};
	c->exceptions[2] = (tix_cal_exception){.from = d4, .to = d4, .working = false, .note = tix_xstrdup("NYE"), .order = {.lamport = 3}};
	t->u.ticket.plan.calendar = cal->id;
	const tix_entity *tickets[] = {t}, *cals[] = {cal};
	tix_date today;
	TIX_REQUIRE(tix_date_parse("2026-11-09", &today));
	tix_plan_input in = {tickets, 1, NULL, 0, cals, 1, NULL, today};
	tix_schedule *s = NULL;
	TK_OK(tix_plan_compute(&in, &s, &e), &e);
	tix_buf b = TIX_BUF_INIT;
	if (s)
		tix_export(s, TIX_EXPORT_MSPDI, NULL, &b);
	const char *x = tix_buf_cstr(&b);
	TK_CHECK(strstr(x, "<Calendar><UID>2</UID><Name>Site &amp; &lt;yard&gt;</Name>"));
	/* Mon-Sat with 6 hours: Saturday (DayType 7) works 08-12, 13-15; Sunday doesn't */
	TK_CHECK(strstr(x, "<WeekDay><DayType>1</DayType><DayWorking>0</DayWorking></WeekDay>"));
	TK_CHECK(strstr(x, "<WeekDay><DayType>7</DayType><DayWorking>1</DayWorking><WorkingTimes><WorkingTime><FromTime>"
			   "08:00:00</FromTime><ToTime>12:00:00</ToTime></WorkingTime><WorkingTime><FromTime>13:00:00"
			   "</FromTime><ToTime>15:00:00</ToTime></WorkingTime></WorkingTimes></WeekDay>"));
	static const char *const runs[] = {"2026-12-24", "2026-12-25", "0", "Holidays",
					   "2026-12-26", "2026-12-26", "1", "Holidays",
					   "2026-12-27", "2026-12-30", "0", "Holidays",
					   "2026-12-31", "2026-12-31", "0", "NYE",
					   "2027-01-01", "2027-01-01", "0", "Holidays"};
	const char *p = x;
	for (size_t i = 0; i < 20; i += 4) {
		char want[400];
		snprintf(want, sizeof(want),
			 "<Exception><EnteredByOccurrences>0</EnteredByOccurrences><TimePeriod><FromDate>%sT00:00:00"
			 "</FromDate><ToDate>%sT23:59:00</ToDate></TimePeriod><Occurrences>1</Occurrences><Name>%s</Name>"
			 "<Type>1</Type><DayWorking>%s</DayWorking>",
			 runs[i], runs[i + 1], runs[i + 3], runs[i + 2]);
		const char *hit = strstr(p, want);
		if (!hit)
			tk_fail(__FILE__, __LINE__, "missing or out of order: %s", want);
		else
			p = hit + 1;
	}
	TK_CHECK(!strstr(p + 1, "<Exception><Entered")); /* exactly five */
	TK_CHECK(strstr(x, "<CalendarUID>2</CalendarUID>"));
	TK_CHECK(strstr(x, "<Finish>2026-11-11T15:00:00</Finish>")); /* a 6-hour day ends at 15:00 */
	check_task_order(x);
	tix_buf_free(&b);
	tix_schedule_free(s);
	tix_entity_free(t);
	tix_entity_free(cal);
}

int main(void)
{
	test_mspdi_calendar();
	tix_entity *proj = task(1, "Build: phase #1", -1), *a = task(2, "=SUM(A1)", 2), *m = task(3, "Done, \"really\"", 0);
	tix_entity *u = task(4, "Someday", -1), *r = tix_xcalloc(1, sizeof(*r));
	r->type = TIX_ENTITY_RESOURCE;
	r->id = oid(9);
	r->u.resource.name = tix_xstrdup("Ada");
	r->u.resource.capacity = 100;
	r->u.resource.rate = -1; /* unset, as the model's default */
	r->u.resource.active = true;
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
		  "late_start,late_finish,deadline,actual_start,actual_finish,assignments,cost,currency\r\n"
		  "1,0100000000000000000000000000000000000000,Build: phase #1,summary,open,2026-11-12,2026-11-13,2,8,0,0,yes,"
		  "2026-11-12,2026-11-13,,,,,,\r\n"
		  "1.1,0200000000000000000000000000000000000000,\"'=SUM(A1)\",task,in-progress,2026-11-12,2026-11-13,2,8,0,0,yes,"
		  "2026-11-12,2026-11-13,,,,Ada:50%,,\r\n"
		  "1.2,0300000000000000000000000000000000000000,\"Done, \"\"really\"\"\",milestone,open,2026-11-13,2026-11-13,0,,0,0,yes,"
		  "2026-11-13,2026-11-13,,,,,,\r\n"
		  "2,0400000000000000000000000000000000000000,Someday,task,open,,,,,0,,,,,,,,,,\r\n");

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

	/* MS Project XML */
	tix_buf_reset(&b);
	TK_CHECK(tix_export_format_parse("mspdi", &f) && f == TIX_EXPORT_MSPDI);
	tix_export(s, TIX_EXPORT_MSPDI, "Plan <Q4> & \"co\"\x01", &b);
	const char *x = tix_buf_cstr(&b);
	TK_CHECK(strstr(x, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
			   "<Project xmlns=\"http://schemas.microsoft.com/project\"><SaveVersion>14</SaveVersion>"
			   "<Name>Plan &lt;Q4&gt; &amp; &quot;co&quot; </Name>") == x);
	TK_CHECK(strstr(x, "<Task><UID>0</UID><ID>0</ID>")); /* the project summary */
	TK_CHECK(strstr(x, "<Task><UID>1</UID><ID>1</ID><Name>Build: phase #1</Name>"));
	TK_CHECK(strstr(x, "<Name>=SUM(A1)</Name><Active>1</Active><Manual>0</Manual><Type>0</Type><IsNull>0</IsNull>"
			   "<WBS>1.1</WBS><OutlineNumber>1.1</OutlineNumber><OutlineLevel>2</OutlineLevel>"));
	TK_CHECK(strstr(x, "<Duration>PT16H0M0S</Duration>")); /* 2 days x 8 h */
	TK_CHECK(strstr(x, "<Name>Done, &quot;really&quot;</Name>"));
	TK_CHECK(strstr(x, "<Milestone>1</Milestone>"));
	TK_CHECK(strstr(x, "<PredecessorLink><PredecessorUID>2</PredecessorUID><Type>1</Type><CrossProject>0"
			   "</CrossProject><LinkLag>0</LinkLag><LagFormat>7</LagFormat></PredecessorLink>"));
	TK_CHECK(strstr(x, "<Name>Someday</Name>") && strstr(x, "<Estimated>1</Estimated>"));
	TK_CHECK(strstr(x, "<Resource><UID>1</UID><ID>1</ID><Name>Ada</Name><Type>1</Type><IsNull>0</IsNull>"
			   "<MaxUnits>1.00</MaxUnits><IsInactive>0</IsInactive></Resource>"));
	TK_CHECK(strstr(x, "<Assignment><UID>1</UID><TaskUID>2</TaskUID><ResourceUID>1</ResourceUID>"));
	TK_CHECK(strstr(x, "<Units>0.50</Units><Work>PT8H0M0S</Work></Assignment>"));
	TK_CHECK(strstr(x, "</Assignments>\n</Project>\n"));
	check_task_order(x);
	TK_CHECK(tix_utf8_valid(b.data, b.len));

	/* with a baseline comparison: task 1.1 ('a') was baselined a day earlier */
	tix_date b0, b1;
	TIX_REQUIRE(tix_date_parse("2026-11-11", &b0) && tix_date_parse("2026-11-12", &b1));
	tix_baseline_task bt[1] = {{a->id, b0, b1, 2, 8, -1, NULL}};
	tix_baseline bl = {.name = "v1", .taken = b0, .tasks = bt, .ntasks = 1};
	tix_plan_variance var[4];
	TK_CHECK(tix_plan_compare(s, &bl, var) == 0);
	tix_buf_reset(&b);
	tix_export_compare(s, TIX_EXPORT_CSV, NULL, var, &b);
	TK_CHECK(strstr(b.data, ",cost,currency,baseline_start,baseline_finish,start_slip_days,finish_slip_days,"
				"duration_delta_days,effort_delta_hours,cost_delta\r\n"));
	TK_CHECK(strstr(b.data, "Ada:50%,,,2026-11-11,2026-11-12,1,1,0,0,\r\n"));
	TK_CHECK(strstr(b.data, "Someday,task,open,,,,,0" ",,,,,,,,,," ",,,,,,,\r\n")); /* not baselined: empty */
	tix_buf_reset(&b);
	tix_export_compare(s, TIX_EXPORT_JSON, NULL, var, &b);
	TK_CHECK(strstr(b.data, "\"baseline\":{\"start\":\"2026-11-11\",\"finish\":\"2026-11-12\",\"duration\":2,\"effort\":8,"
				"\"cost\":null,\"start_slip\":1,\"finish_slip\":1,\"duration_delta\":0,\"effort_delta\":0,"
				"\"cost_delta\":null}"));
	TK_CHECK(strstr(b.data, "\"title\":\"Someday\"") && strstr(strstr(b.data, "\"title\":\"Someday\""), "\"baseline\":null"));
	tix_buf_reset(&b);
	tix_export_compare(s, TIX_EXPORT_MSPDI, NULL, var, &b);
	TK_CHECK(strstr(b.data, "<Baseline><Number>0</Number><Start>2026-11-11T08:00:00</Start><Finish>2026-11-12T17:00:00"
				"</Finish><Duration>PT16H0M0S</Duration><DurationFormat>7</DurationFormat><Work>PT8H0M0S</Work>"
				"</Baseline>"));
	check_task_order(b.data);
	tix_buf_free(&b);
	tix_schedule_free(s);
	tix_entity_free(proj);
	tix_entity_free(a);
	tix_entity_free(m);
	tix_entity_free(u);
	tix_entity_free(r);
	return tk_finish();
}
