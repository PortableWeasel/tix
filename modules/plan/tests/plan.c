/* plan: golden CPM cases, WBS, calendars, rollups, cycles, load, determinism. */
#include "tix/plan.h"
#include "tix/testkit.h"

#include <stdio.h>
#include <stdlib.h>

static tix_error e = TIX_ERROR_INIT;
static uint64_t order_seq = 1;

static tix_oid oid(int n)
{
	tix_oid o;
	memset(&o, 0, sizeof(o));
	o.algo = TIX_HASH_SHA1;
	o.bytes[0] = (uint8_t)(n >> 8);
	o.bytes[1] = (uint8_t)n;
	return o;
}

static tix_date D(const char *s)
{
	tix_date d;
	TIX_REQUIRE(tix_date_parse(s, &d));
	return d;
}

static const char *S(tix_date d, char *buf)
{
	if (d == TIX_DATE_NONE)
		return "none";
	return tix_date_str(d, buf);
}

static tix_order next_order(void)
{
	tix_order o;
	memset(&o, 0, sizeof(o));
	o.lamport = order_seq++;
	o.id.commit = oid(0xfff);
	o.id.line = 2;
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

static void parent(tix_entity *child, const tix_entity *par)
{
	child->u.ticket.plan.parent = par->id;
	child->u.ticket.plan.parent_order = next_order();
}

static void dep(tix_entity *succ, const tix_entity *pred, tix_dep_kind kind, int32_t lag)
{
	tix_ticket_plan *p = &succ->u.ticket.plan;
	p->deps = tix_xreallocarray(p->deps, p->ndeps + 1, sizeof(tix_dep));
	p->deps[p->ndeps++] = (tix_dep){pred->id, kind, lag, next_order()};
}

static void assign(tix_entity *t, const tix_entity *res, int32_t units)
{
	tix_ticket_plan *p = &t->u.ticket.plan;
	p->assignments = tix_xreallocarray(p->assignments, p->nassignments + 1, sizeof(tix_assignment));
	p->assignments[p->nassignments++] = (tix_assignment){res->id, units, -1};
}

static tix_entity *resource(int n, const char *name, int32_t capacity)
{
	tix_entity *r = tix_xcalloc(1, sizeof(*r));
	r->type = TIX_ENTITY_RESOURCE;
	r->id = oid(n);
	r->u.resource.name = tix_xstrdup(name);
	r->u.resource.capacity = capacity;
	r->u.resource.rate = -1;
	r->u.resource.active = true;
	return r;
}

static tix_entity *calendar(int n, unsigned weekdays)
{
	tix_entity *c = tix_xcalloc(1, sizeof(*c));
	c->type = TIX_ENTITY_CALENDAR;
	c->id = oid(n);
	c->u.calendar.name = tix_xstrdup("cal");
	c->u.calendar.weekdays = weekdays;
	c->u.calendar.hours = 8;
	return c;
}

static void holiday(tix_entity *c, tix_date from, tix_date to, bool working)
{
	tix_calendar *cal = &c->u.calendar;
	cal->exceptions = tix_xreallocarray(cal->exceptions, cal->nexceptions + 1, sizeof(tix_cal_exception));
	cal->exceptions[cal->nexceptions++] = (tix_cal_exception){from, to, working, tix_xstrdup(""), next_order()};
}

typedef struct world {
	tix_entity *t[32], *r[4], *c[4];
	size_t nt, nr, nc;
} world;

static tix_entity *add(world *w, tix_entity *x)
{
	if (x->type == TIX_ENTITY_TICKET)
		w->t[w->nt++] = x;
	else if (x->type == TIX_ENTITY_RESOURCE)
		w->r[w->nr++] = x;
	else
		w->c[w->nc++] = x;
	return x;
}

static void world_free(world *w)
{
	for (size_t i = 0; i < w->nt; i++)
		tix_entity_free(w->t[i]);
	for (size_t i = 0; i < w->nr; i++)
		tix_entity_free(w->r[i]);
	for (size_t i = 0; i < w->nc; i++)
		tix_entity_free(w->c[i]);
	memset(w, 0, sizeof(*w));
}

static tix_schedule *run(world *w, const tix_oid *root, const char *today)
{
	tix_plan_input in = {(const tix_entity *const *)w->t, w->nt, (const tix_entity *const *)w->r, w->nr,
			     (const tix_entity *const *)w->c, w->nc, root, D(today)};
	tix_schedule *s = NULL;
	TK_OK(tix_plan_compute(&in, &s, &e), &e);
	return s;
}

static const tix_plan_task *get(const tix_schedule *s, const tix_entity *t)
{
	size_t i = tix_schedule_find(s, &t->id);
	TIX_REQUIRE(i != SIZE_MAX);
	return &s->tasks[i];
}

/* "start..finish f<float>[*]" */
static const char *line(const tix_schedule *s, const tix_entity *t, char *out)
{
	char a[TIX_DATE_STR], b[TIX_DATE_STR];
	const tix_plan_task *x = get(s, t);
	snprintf(out, 64, "%s..%s f%d%s", S(x->start, a), S(x->finish, b), (int)x->total_float, x->critical ? "*" : "");
	return out;
}

#define EXPECT(s, t, want)                                \
	do {                                              \
		char buf_[64];                            \
		TK_STR_EQ(line((s), (t), buf_), (want)); \
	} while (0)

static bool has_diag(const tix_schedule *s, const char *needle)
{
	if (getenv("PLAN_DEBUG"))
		for (size_t i = 0; i < s->ndiags; i++)
			fprintf(stderr, "diag: %s\n", s->diags[i].msg);
	for (size_t i = 0; i < s->ndiags; i++)
		if (strstr(s->diags[i].msg, needle))
			return true;
	return false;
}

/* 2026-11-09 is a Monday. */
static void test_chain_and_float(void)
{
	world w = {0};
	tix_entity *a = add(&w, task(1, "A", 3)), *b = add(&w, task(2, "B", 2)), *m = add(&w, task(3, "M", -1));
	tix_entity *d = add(&w, task(4, "D", 1));
	m->u.ticket.plan.kind = TIX_KIND_MILESTONE;
	dep(b, a, TIX_DEP_FS, 0);
	dep(b, d, TIX_DEP_FS, 0);
	dep(m, b, TIX_DEP_FS, 0);
	tix_schedule *s = run(&w, NULL, "2026-11-09");
	EXPECT(s, a, "2026-11-09..2026-11-11 f0*");
	EXPECT(s, d, "2026-11-09..2026-11-09 f2");
	EXPECT(s, b, "2026-11-12..2026-11-13 f0*");
	EXPECT(s, m, "2026-11-13..2026-11-13 f0*"); /* milestone at B's finish, not the next Monday */
	TK_CHECK(get(s, m)->milestone && s->start == D("2026-11-09") && s->finish == D("2026-11-13"));
	TK_CHECK(get(s, d)->late_finish == D("2026-11-11"));
	TK_CHECK(s->ndiags == 0);
	tix_schedule_free(s);
	world_free(&w);
}

static void test_dependency_kinds(void)
{
	world w = {0};
	tix_entity *a = add(&w, task(1, "A", 3));
	tix_entity *fs2 = add(&w, task(2, "FS+2", 1)), *fsm1 = add(&w, task(3, "FS-1", 1));
	tix_entity *ss1 = add(&w, task(4, "SS+1", 2)), *ff = add(&w, task(5, "FF", 1)), *sf = add(&w, task(6, "SF", 1));
	tix_entity *long7 = add(&w, task(7, "7 days", 7));
	dep(fs2, a, TIX_DEP_FS, 2);
	dep(fsm1, a, TIX_DEP_FS, -1);
	dep(ss1, a, TIX_DEP_SS, 1);
	dep(ff, a, TIX_DEP_FF, 0);
	dep(sf, a, TIX_DEP_SF, 0);
	tix_schedule *s = run(&w, NULL, "2026-11-09");
	char buf[64];
	TK_STR_EQ(line(s, fs2, buf), "2026-11-16..2026-11-16 f1"); /* Thu, Fri lag; Mon. The 7-day task sets the end */
	TK_STR_EQ(line(s, fsm1, buf), "2026-11-11..2026-11-11 f4");
	TK_STR_EQ(line(s, ss1, buf), "2026-11-10..2026-11-11 f4");
	TK_STR_EQ(line(s, ff, buf), "2026-11-11..2026-11-11 f4");
	TK_STR_EQ(line(s, sf, buf), "2026-11-09..2026-11-09 f6"); /* can't go before the project start */
	TK_STR_EQ(line(s, long7, buf), "2026-11-09..2026-11-17 f0*"); /* across a weekend */
	tix_schedule_free(s);
	world_free(&w);
}

static void test_calendars(void)
{
	world w = {0};
	tix_entity *office = add(&w, calendar(100, TIX_WEEKDAYS_MON_FRI)), *site = add(&w, calendar(101, 0x3f));
	tix_entity *none = add(&w, calendar(102, 0));
	holiday(office, D("2026-11-11"), D("2026-11-11"), false);
	holiday(office, D("2026-11-14"), D("2026-11-14"), true); /* a working Saturday */
	tix_entity *p = add(&w, task(1, "Project", -1));
	tix_entity *a = add(&w, task(2, "A", 3)), *b = add(&w, task(3, "B", 3)), *c = add(&w, task(4, "C", 6));
	tix_entity *x = add(&w, task(5, "X", 2));
	parent(a, p);
	parent(b, p);
	parent(c, p);
	parent(x, p);
	p->u.ticket.plan.calendar = office->id; /* inherited by A, B, X */
	c->u.ticket.plan.calendar = site->id;
	x->u.ticket.plan.calendar = none->id; /* unusable: falls back to the inherited one */
	dep(b, a, TIX_DEP_FS, 0);
	tix_schedule *s = run(&w, NULL, "2026-11-09");
	char buf[64];
	TK_STR_EQ(line(s, a, buf), "2026-11-09..2026-11-12 f0*");  /* skips Wed 11 */
	TK_STR_EQ(line(s, b, buf), "2026-11-13..2026-11-16 f0*");  /* Fri, Sat 14 (working), Mon */
	TK_STR_EQ(line(s, c, buf), "2026-11-09..2026-11-14 f1");   /* six-day week */
	TK_CHECK(get(s, x)->calendar == office && has_diag(s, "no working weekdays"));
	TK_CHECK(get(s, a)->calendar == office && get(s, c)->calendar == site);
	TK_CHECK(tix_plan_is_working(office, D("2026-11-14")) && !tix_plan_is_working(office, D("2026-11-11")));
	TK_CHECK(!tix_plan_is_working(NULL, D("2026-11-14")) && tix_plan_is_working(NULL, D("2026-11-13")));
	tix_schedule_free(s);
	world_free(&w);
}

static void test_wbs_and_rollups(void)
{
	world w = {0};
	tix_entity *p = add(&w, task(1, "Project", -1)), *q = add(&w, task(2, "Second root", 1));
	tix_entity *x = add(&w, task(10, "X", 2)), *y = add(&w, task(11, "Y", 4)), *z = add(&w, task(12, "Z", 1));
	tix_entity *z1 = add(&w, task(13, "Z.1", 1));
	parent(x, p);
	parent(y, p);
	parent(z, p);
	parent(z1, z);
	x->u.ticket.plan.rank = tix_xstrdup("a");
	z->u.ticket.plan.rank = tix_xstrdup("b"); /* Y is unranked: last */
	p->u.ticket.plan.rank = tix_xstrdup("m");
	x->u.ticket.plan.effort = 16;
	y->u.ticket.plan.effort = 48;
	x->u.ticket.plan.progress = 100;
	y->u.ticket.plan.progress = 50;
	p->u.ticket.plan.start = D("2026-11-10"); /* constrains everything below */
	tix_schedule *s = run(&w, NULL, "2026-11-02");
	TK_CHECK(s->ntasks == 6);
	if (s->ntasks == 6) {
		const char *want[] = {"1 Project", "1.1 X", "1.2 Z", "1.2.1 Z.1", "1.3 Y", "2 Second root"};
		for (size_t i = 0; i < 6; i++) {
			char got[128];
			snprintf(got, sizeof(got), "%s %s", s->tasks[i].wbs, s->tasks[i].ticket->u.ticket.title);
			TK_STR_EQ(got, want[i]);
		}
	}
	const tix_plan_task *pt = get(s, p);
	char buf[64];
	TK_CHECK(pt->summary && pt->scheduled && !pt->milestone && get(s, z)->summary && !get(s, z1)->summary);
	TK_STR_EQ(line(s, p, buf), "2026-11-10..2026-11-13 f0*");
	TK_CHECK(pt->duration == 4 && pt->effort == 64 && pt->progress == (16 * 100 + 48 * 50 + 8 * 0) / 72);
	TK_CHECK(get(s, q)->start == D("2026-11-10")); /* project start = the earliest start given */
	tix_schedule_free(s);

	/* a sub-tree plan */
	s = run(&w, &z->id, "2026-11-02");
	TK_CHECK(s->ntasks == 2 && !strcmp(s->tasks[0].wbs, "1") && !strcmp(s->tasks[1].wbs, "1.1"));
	tix_schedule_free(s);
	tix_oid nope = oid(999);
	tix_plan_input in = {(const tix_entity *const *)w.t, w.nt, NULL, 0, NULL, 0, &nope, D("2026-11-02")};
	tix_schedule *bad = NULL;
	TK_STATUS(tix_plan_compute(&in, &bad, &e), TIX_ERR_NOT_FOUND, &e);
	world_free(&w);
}

static void test_cycles(void)
{
	world w = {0};
	tix_entity *a = add(&w, task(1, "A", 1)), *b = add(&w, task(2, "B", 1)), *c = add(&w, task(3, "C", 1));
	parent(a, b); /* earlier */
	parent(b, a); /* later: this link is dropped */
	dep(c, a, TIX_DEP_FS, 0);
	tix_entity *x = add(&w, task(4, "X", 2)), *y = add(&w, task(5, "Y", 3)), *z = add(&w, task(6, "Z", 1));
	dep(y, x, TIX_DEP_FS, 0);
	dep(z, y, TIX_DEP_FS, 0);
	dep(x, z, TIX_DEP_FS, 0); /* latest: dropped */
	tix_entity *self = add(&w, task(7, "Self", 1));
	dep(self, self, TIX_DEP_FS, 0);
	tix_entity *orphan = add(&w, task(8, "Orphan", 1));
	tix_oid gone = oid(777);
	orphan->u.ticket.plan.parent = gone;
	tix_schedule *s = run(&w, NULL, "2026-11-09");
	TK_CHECK(has_diag(s, "WBS cycle: dropped the parent link of 0002000"));
	TK_CHECK(get(s, b)->parent == SIZE_MAX && get(s, b)->summary && get(s, a)->depth == 1);
	TK_CHECK(has_diag(s, "dependency cycle: dropped 0006000 -> 0004000"));
	char buf[64];
	TK_STR_EQ(line(s, x, buf), "2026-11-09..2026-11-10 f0*");
	TK_STR_EQ(line(s, z, buf), "2026-11-16..2026-11-16 f0*");
	TK_CHECK(has_diag(s, "depends on itself") && has_diag(s, "parent 0309000 not found"));
	TK_CHECK(get(s, orphan)->parent == SIZE_MAX);
	tix_schedule_free(s);
	world_free(&w);
}

static void test_manual_actuals_deadline(void)
{
	world w = {0};
	tix_entity *a = add(&w, task(1, "A", 2)), *m = add(&w, task(2, "Manual", 2)), *b = add(&w, task(3, "B", 1));
	tix_entity *done = add(&w, task(4, "Done", 5)), *u = add(&w, task(5, "Unscheduled", -1));
	tix_entity *after_u = add(&w, task(6, "After U", 1));
	m->u.ticket.plan.mode = TIX_MODE_MANUAL;
	m->u.ticket.plan.start = D("2026-11-16");
	dep(m, a, TIX_DEP_FS, 0); /* manual tasks don't move */
	dep(b, m, TIX_DEP_FS, 0);
	b->u.ticket.plan.deadline = D("2026-11-17");
	done->u.ticket.plan.actual_start = D("2026-11-03");
	done->u.ticket.plan.actual_finish = D("2026-11-04");
	done->u.ticket.status = TIX_TICKET_DONE;
	dep(after_u, u, TIX_DEP_FS, 0);
	tix_schedule *s = run(&w, NULL, "2026-11-09");
	char buf[64];
	TK_STR_EQ(line(s, m, buf), "2026-11-16..2026-11-17 f0*");
	TK_STR_EQ(line(s, b, buf), "2026-11-18..2026-11-18 f0*");
	TK_CHECK(get(s, b)->late && has_diag(s, "after its deadline 2026-11-17"));
	TK_CHECK(get(s, m)->manual && get(s, done)->manual && get(s, done)->duration == 2 && get(s, done)->progress == 100);
	TK_STR_EQ(line(s, done, buf), "2026-11-03..2026-11-04 f10");
	TK_CHECK(s->start == D("2026-11-03")); /* the earliest actual start */
	TK_CHECK(!get(s, u)->scheduled && get(s, u)->start == TIX_DATE_NONE && has_diag(s, "unscheduled; ignored"));
	TK_CHECK(get(s, after_u)->scheduled);
	tix_schedule_free(s);
	world_free(&w);
}

/* No duration or effort, but both ends fixed: scheduled on them (found dogfooding). */
static void test_fixed_span_without_duration(void)
{
	world w = {0};
	tix_entity *hist = add(&w, task(1, "History", -1)), *next = add(&w, task(2, "After history", -1));
	tix_entity *man = add(&w, task(3, "Manual span", -1)), *open = add(&w, task(4, "Start only", -1));
	hist->u.ticket.plan.actual_start = D("2026-11-02");
	hist->u.ticket.plan.actual_finish = D("2026-11-04");
	next->u.ticket.plan.actual_start = next->u.ticket.plan.actual_finish = D("2026-11-05");
	dep(next, hist, TIX_DEP_FS, 0);
	man->u.ticket.plan.mode = TIX_MODE_MANUAL;
	man->u.ticket.plan.start = D("2026-11-05");
	man->u.ticket.plan.finish = D("2026-11-06");
	open->u.ticket.plan.actual_start = D("2026-11-02");
	tix_schedule *s = run(&w, NULL, "2026-11-09");
	char buf[64];
	TK_STR_EQ(line(s, hist, buf), "2026-11-02..2026-11-04 f1");
	TK_CHECK(get(s, hist)->duration == 3 && !get(s, hist)->milestone);
	TK_STR_EQ(line(s, next, buf), "2026-11-05..2026-11-05 f1");
	TK_CHECK(get(s, next)->duration == 1 && !has_diag(s, "unscheduled; ignored"));
	TK_STR_EQ(line(s, man, buf), "2026-11-05..2026-11-06 f0*");
	TK_CHECK(!get(s, open)->scheduled); /* one fixed end is not enough */
	tix_schedule_free(s);
	world_free(&w);
}

static void test_effort_and_load(void)
{
	world w = {0};
	tix_entity *dev = add(&w, resource(200, "Dev", 100)), *team = add(&w, resource(201, "Team", 300));
	tix_entity *a = add(&w, task(1, "A", -1)), *b = add(&w, task(2, "B", 2)), *c = add(&w, task(3, "C", -1));
	tix_entity *ghost = add(&w, task(4, "Ghost", -1));
	a->u.ticket.plan.effort = 24;
	assign(a, dev, 50); /* 24h at 4h/day */
	assign(b, dev, 100); /* in parallel with A: 150% */
	c->u.ticket.plan.effort = 48;
	assign(c, team, 100); /* three people: 2 days */
	ghost->u.ticket.plan.effort = 8;
	tix_entity fake = *dev;
	fake.id = oid(299);
	assign(ghost, &fake, 100);
	tix_schedule *s = run(&w, NULL, "2026-11-09");
	char buf[64];
	TK_STR_EQ(line(s, a, buf), "2026-11-09..2026-11-16 f0*");
	TK_STR_EQ(line(s, c, buf), "2026-11-09..2026-11-10 f4");
	TK_CHECK(!get(s, ghost)->scheduled && has_diag(s, "resource 012b000 not found"));
	TK_CHECK(s->nloads == 2 && s->ndays == 8);
	if (s->nloads == 2) {
		const tix_plan_load *l = &s->loads[0];
		TK_CHECK(l->resource == dev && l->peak == 150 && l->over_days == 2 && l->units[0] == 150 &&
			 l->units[2] == 50 && l->units[5] == 0 && !l->working[5]);
		TK_CHECK(s->loads[1].resource == team && s->loads[1].over_days == 0);
	}
	TK_CHECK(has_diag(s, "(Dev) is over-allocated on 2 day(s), peak 150%"));
	tix_schedule_free(s);
	world_free(&w);
}

static void dump(const tix_schedule *s, tix_buf *out)
{
	char a[TIX_DATE_STR], b[TIX_DATE_STR];
	for (size_t i = 0; i < s->ntasks; i++) {
		const tix_plan_task *t = &s->tasks[i];
		tix_buf_addf(out, "%s %s %s %s %d %d\n", t->wbs, t->ticket->u.ticket.title, S(t->start, a), S(t->finish, b),
			     (int)t->total_float, t->critical);
	}
	for (size_t i = 0; i < s->ndiags; i++)
		tix_buf_addf(out, "! %s\n", s->diags[i].msg);
}

static void test_determinism(void)
{
	world w = {0};
	uint64_t rng = 42;
	for (int i = 0; i < 24; i++)
		add(&w, task(i + 1, "t", (int64_t)tk_rand_below(&rng, 5)));
	for (int i = 0; i < 24; i++) {
		if (tk_rand_below(&rng, 3) == 0)
			parent(w.t[i], w.t[tk_rand_below(&rng, 24)]);
		for (int k = 0; k < 2; k++)
			dep(w.t[i], w.t[tk_rand_below(&rng, 24)], (tix_dep_kind)tk_rand_below(&rng, 4),
			    (int32_t)tk_rand_below(&rng, 5) - 2);
	}
	tix_buf want = TIX_BUF_INIT, got = TIX_BUF_INIT;
	tix_schedule *s = run(&w, NULL, "2026-11-09");
	dump(s, &want);
	tix_schedule_free(s);
	for (int round = 0; round < 20; round++) {
		for (size_t i = w.nt; i > 1; i--) {
			size_t j = tk_rand_below(&rng, i);
			tix_entity *t = w.t[i - 1];
			w.t[i - 1] = w.t[j];
			w.t[j] = t;
		}
		s = run(&w, NULL, "2026-11-09");
		tix_buf_reset(&got);
		dump(s, &got);
		TK_STR_EQ(tix_buf_cstr(&got), tix_buf_cstr(&want));
		tix_schedule_free(s);
	}
	tix_buf_free(&want);
	tix_buf_free(&got);
	world_free(&w);
}

int main(void)
{
	test_chain_and_float();
	test_dependency_kinds();
	test_calendars();
	test_wbs_and_rollups();
	test_cycles();
	test_manual_actuals_deadline();
	test_fixed_span_without_duration();
	test_effort_and_load();
	test_determinism();
	return tk_finish();
}
