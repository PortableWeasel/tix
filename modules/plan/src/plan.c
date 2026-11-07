/* The scheduling engine (see tix/plan.h). */
#include "tix/base_json.h"
#include "tix/plan.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NONE SIZE_MAX

typedef TIX_VEC(size_t) idx_vec;

/* ---- calendars ---------------------------------------------------------------- */

bool tix_plan_is_working(const tix_entity *cal, tix_date d)
{
	if (!cal)
		return tix_date_weekday(d) < 5;
	const tix_calendar *c = &cal->u.calendar;
	const tix_cal_exception *best = NULL;
	for (size_t i = 0; i < c->nexceptions; i++) {
		const tix_cal_exception *x = &c->exceptions[i];
		if (d >= x->from && d <= x->to && (!best || tix_order_cmp(&x->order, &best->order) > 0))
			best = x;
	}
	if (best)
		return best->working;
	return (c->weekdays >> tix_date_weekday(d)) & 1u;
}

static tix_date next_work(const tix_entity *c, tix_date d)
{
	if (d < TIX_DATE_MIN)
		d = TIX_DATE_MIN;
	while (d < TIX_DATE_MAX && !tix_plan_is_working(c, d))
		d++;
	return d;
}

static tix_date prev_work(const tix_entity *c, tix_date d)
{
	if (d > TIX_DATE_MAX)
		d = TIX_DATE_MAX;
	while (d > TIX_DATE_MIN && !tix_plan_is_working(c, d))
		d--;
	return d;
}

static tix_date clamp(int64_t d)
{
	return d < TIX_DATE_MIN ? TIX_DATE_MIN : d > TIX_DATE_MAX ? TIX_DATE_MAX : (tix_date)d;
}

/* Moves a start-of-day boundary by lag working days. */
static tix_date shift(const tix_entity *c, tix_date b, int32_t lag)
{
	if (lag > 0) {
		b = next_work(c, b);
		for (int32_t k = 0; k < lag; k++)
			b = next_work(c, clamp((int64_t)b + 1));
	}
	for (int32_t k = 0; k > lag; k--)
		b = prev_work(c, clamp((int64_t)b - 1));
	return b;
}

/* Exclusive end boundary of a task of d working days starting at s. */
static tix_date end_from_start(const tix_entity *c, tix_date s, int64_t d)
{
	if (d <= 0)
		return s;
	s = next_work(c, s);
	for (int64_t k = 1; k < d; k++)
		s = next_work(c, clamp((int64_t)s + 1));
	return clamp((int64_t)s + 1);
}

/* Start of a task of d working days that ends by boundary e. */
static tix_date start_from_end(const tix_entity *c, tix_date e, int64_t d)
{
	if (d <= 0)
		return e;
	tix_date last = prev_work(c, clamp((int64_t)e - 1));
	for (int64_t k = 1; k < d; k++)
		last = prev_work(c, clamp((int64_t)last - 1));
	return last;
}

/* Working days in [a, b); negative when b < a. */
static int32_t workdays(const tix_entity *c, tix_date a, tix_date b)
{
	int32_t n = 0, sign = 1;
	if (b < a) {
		tix_date t = a;
		a = b;
		b = t;
		sign = -1;
	}
	for (tix_date d = a; d < b; d++)
		n += tix_plan_is_working(c, d);
	return sign * n;
}

static int calendar_hours(const tix_entity *c)
{
	return c ? c->u.calendar.hours : 8;
}

/* ---- working state -------------------------------------------------------------- */

typedef struct edge {
	size_t p, s; /* task indices (leaves) */
	tix_dep_kind kind;
	int32_t lag;
	tix_order order; /* of the dep-add: identifies the dependency */
	size_t owner;    /* task that holds the dependency */
	bool dropped;
} edge;

typedef struct leaf {
	int64_t d; /* duration in working days */
	tix_date es, e, ls, le;
	bool fixed, finish_driven;
} leaf;

typedef struct plan {
	const tix_plan_input *in;
	const tix_entity **tk; /* tickets sorted by id */
	size_t nt;
	const tix_entity **res, **cal; /* sorted by id */
	size_t nres, ncal;
	size_t *parent;  /* per ticket index, after cycle breaking */
	size_t *task_of; /* ticket index -> task index (NONE = out of scope) */
	TIX_VEC(tix_plan_task) tasks;
	size_t *ticket_of; /* task index -> ticket index */
	size_t *first_child, *next_sibling; /* per task */
	TIX_VEC(tix_plan_diag) diags;
	TIX_VEC(edge) edges;
	leaf *lf; /* per task */
} plan;

static void diag(plan *p, const tix_oid *id, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void diag(plan *p, const tix_oid *id, const char *fmt, ...)
{
	tix_plan_diag d;
	tix_buf b = TIX_BUF_INIT;
	char tmp[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	size_t len = strlen(tmp);
	while (len && !tix_utf8_valid(tmp, len)) /* truncation may split a character */
		tmp[--len] = '\0';
	tix_buf_adds(&b, tmp);
	memset(&d.id, 0, sizeof(d.id));
	if (id)
		d.id = *id;
	d.msg = tix_buf_detach(&b);
	TIX_VEC_PUSH(&p->diags, d);
}

static const char *short_id(const tix_oid *id, char *out)
{
	char hex[TIX_OID_MAX_HEX + 1];
	snprintf(out, 8, "%.7s", tix_oid_hex(id, hex));
	return out;
}

static int cmp_entity_id(const void *a, const void *b)
{
	return tix_oid_cmp(&(*(const tix_entity *const *)a)->id, &(*(const tix_entity *const *)b)->id);
}

static const tix_entity **sorted(const tix_entity *const *v, size_t n)
{
	const tix_entity **out = tix_xcalloc(n ? n : 1, sizeof(*out));
	if (n) {
		memcpy(out, v, n * sizeof(*out));
		qsort(out, n, sizeof(*out), cmp_entity_id);
	}
	return out;
}

static size_t find(const tix_entity **v, size_t n, const tix_oid *id)
{
	size_t lo = 0, hi = n;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		int c = tix_oid_cmp(&v[mid]->id, id);
		if (!c)
			return mid;
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return NONE;
}

static const tix_ticket_plan *tpl(const plan *p, size_t ti)
{
	return &p->tk[ti]->u.ticket.plan;
}

/* ---- WBS: parents, cycles, order ----------------------------------------------- */

static void build_parents(plan *p)
{
	char a[8], b[8];
	p->parent = tix_xcalloc(p->nt ? p->nt : 1, sizeof(size_t));
	for (size_t i = 0; i < p->nt; i++) {
		const tix_oid *par = &tpl(p, i)->parent;
		p->parent[i] = NONE;
		if (tix_oid_is_null(par))
			continue;
		size_t k = find(p->tk, p->nt, par);
		if (k == NONE)
			diag(p, &p->tk[i]->id, "%s: parent %s not found; treated as top level", short_id(&p->tk[i]->id, a),
			     short_id(par, b));
		else if (k == i)
			diag(p, &p->tk[i]->id, "%s: is its own parent; treated as top level", short_id(&p->tk[i]->id, a));
		else
			p->parent[i] = k;
	}
	/* Out-degree is at most one, so cycles are disjoint loops: find one, drop the
	 * parent edge whose set-parent op is greatest in total order, repeat. */
	uint8_t *state = tix_xcalloc(p->nt ? p->nt : 1, 1); /* 0 new, 1 on path, 2 done */
	size_t *path = tix_xcalloc(p->nt ? p->nt : 1, sizeof(size_t));
	for (size_t start = 0; start < p->nt; start++) {
		size_t n = 0, v = start;
		while (v != NONE && state[v] == 0) {
			state[v] = 1;
			path[n++] = v;
			v = p->parent[v];
		}
		if (v != NONE && state[v] == 1) { /* cycle: path from v to the end */
			size_t worst = v;
			for (size_t k = 0; k < n; k++) {
				size_t u = path[k];
				bool on_cycle = false;
				for (size_t m = 0; m < n && !on_cycle; m++)
					on_cycle = path[m] == v && m <= k;
				if (on_cycle && tix_order_cmp(&tpl(p, u)->parent_order, &tpl(p, worst)->parent_order) > 0)
					worst = u;
			}
			diag(p, &p->tk[worst]->id, "WBS cycle: dropped the parent link of %s (the latest change)",
			     short_id(&p->tk[worst]->id, a));
			p->parent[worst] = NONE;
		}
		for (size_t k = 0; k < n; k++)
			state[path[k]] = 2;
	}
	free(state);
	free(path);
}

/* Sibling order: ranked first (by rank), then unranked; ties by id. */
static int cmp_sibling(const plan *p, size_t x, size_t y)
{
	const char *rx = tpl(p, x)->rank, *ry = tpl(p, y)->rank;
	if (rx && ry) {
		int c = strcmp(rx, ry);
		if (c)
			return c;
	} else if (rx || ry) {
		return rx ? -1 : 1;
	}
	return tix_oid_cmp(&p->tk[x]->id, &p->tk[y]->id);
}

static const plan *sib_ctx;
static int cmp_sibling_q(const void *a, const void *b)
{
	return cmp_sibling(sib_ctx, *(const size_t *)a, *(const size_t *)b);
}

/* Children lists per ticket, sorted. */
typedef struct kids {
	size_t *start; /* CSR offsets, nt + 1 */
	size_t *v;
} kids;

static void build_kids(plan *p, kids *k)
{
	k->start = tix_xcalloc(p->nt + 1, sizeof(size_t));
	k->v = tix_xcalloc(p->nt ? p->nt : 1, sizeof(size_t));
	for (size_t i = 0; i < p->nt; i++)
		if (p->parent[i] != NONE)
			k->start[p->parent[i] + 1]++;
	for (size_t i = 0; i < p->nt; i++)
		k->start[i + 1] += k->start[i];
	size_t *fill = tix_xcalloc(p->nt ? p->nt : 1, sizeof(size_t));
	for (size_t i = 0; i < p->nt; i++)
		if (p->parent[i] != NONE) {
			size_t par = p->parent[i];
			k->v[k->start[par] + fill[par]++] = i;
		}
	free(fill);
	sib_ctx = p;
	for (size_t i = 0; i < p->nt; i++)
		if (k->start[i + 1] > k->start[i])
			qsort(k->v + k->start[i], k->start[i + 1] - k->start[i], sizeof(size_t), cmp_sibling_q);
}

static const tix_entity *resolve_calendar(plan *p, size_t ti, const tix_entity *inherited)
{
	const tix_oid *cid = &tpl(p, ti)->calendar;
	char a[8], b[8];
	if (tix_oid_is_null(cid))
		return inherited;
	size_t k = find(p->cal, p->ncal, cid);
	if (k == NONE) {
		diag(p, &p->tk[ti]->id, "%s: calendar %s not found; using the inherited one",
		     short_id(&p->tk[ti]->id, a), short_id(cid, b));
		return inherited;
	}
	if (!p->cal[k]->u.calendar.weekdays) {
		diag(p, cid, "calendar %s has no working weekdays; using the inherited one", short_id(cid, a));
		return inherited;
	}
	return p->cal[k];
}

/* Pre-order walk producing tasks with WBS codes. Iterative (deep trees are data). */
static void walk(plan *p, const kids *k, const size_t *tops, size_t ntops, const tix_entity *top_cal)
{
	typedef struct frame {
		size_t ti, task_parent, depth, ordinal;
		const tix_entity *cal;
	} frame;
	TIX_VEC(frame) st = {0};
	for (size_t i = ntops; i-- > 0;) {
		frame f = {tops[i], NONE, 0, i + 1, top_cal};
		TIX_VEC_PUSH(&st, f);
	}
	TIX_VEC(size_t) tick = {0};
	while (st.n) {
		frame f = st.v[--st.n];
		tix_plan_task t;
		memset(&t, 0, sizeof(t));
		t.ticket = p->tk[f.ti];
		t.parent = f.task_parent;
		t.depth = f.depth;
		t.calendar = resolve_calendar(p, f.ti, f.cal);
		char code[TIX_PLAN_WBS_MAX + 24]; /* very deep trees get a truncated code */
		if (f.task_parent == NONE)
			snprintf(code, sizeof(code), "%zu", f.ordinal);
		else
			snprintf(code, sizeof(code), "%s.%zu", p->tasks.v[f.task_parent].wbs, f.ordinal);
		size_t clen = strlen(code) < sizeof(t.wbs) - 1 ? strlen(code) : sizeof(t.wbs) - 1;
		memcpy(t.wbs, code, clen);
		t.wbs[clen] = '\0';
		size_t me = p->tasks.n;
		TIX_VEC_PUSH(&p->tasks, t);
		TIX_VEC_PUSH(&tick, f.ti);
		p->task_of[f.ti] = me;
		size_t a = k->start[f.ti], b = k->start[f.ti + 1];
		p->tasks.v[me].summary = b > a;
		for (size_t c = b; c-- > a;) {
			frame child = {k->v[c], me, f.depth + 1, c - a + 1, p->tasks.v[me].calendar};
			TIX_VEC_PUSH(&st, child);
		}
	}
	free(st.v);
	p->ticket_of = tick.v;
}

/* ---- durations -------------------------------------------------------------------- */

static int64_t leaf_effort(const tix_ticket_plan *pl)
{
	if (pl->effort >= 0)
		return pl->effort;
	int64_t sum = -1;
	for (size_t i = 0; i < pl->nassignments; i++)
		if (pl->assignments[i].effort >= 0)
			sum = (sum < 0 ? 0 : sum) + pl->assignments[i].effort;
	return sum;
}

static int64_t leaf_duration(plan *p, size_t task)
{
	tix_plan_task *t = &p->tasks.v[task];
	const tix_ticket_plan *pl = &t->ticket->u.ticket.plan;
	char a[8], b[8];
	if (pl->kind == TIX_KIND_MILESTONE)
		return 0;
	if (pl->duration >= 0)
		return pl->duration;
	int64_t effort = leaf_effort(pl);
	if (effort < 0) {
		/* No duration or effort, but both ends fixed (actuals, or manual dates): the span. */
		bool manual = pl->mode == TIX_MODE_MANUAL;
		tix_date s = pl->actual_start != TIX_DATE_NONE ? pl->actual_start : manual ? pl->start : TIX_DATE_NONE;
		tix_date f = pl->actual_finish != TIX_DATE_NONE ? pl->actual_finish : manual ? pl->finish : TIX_DATE_NONE;
		if (s == TIX_DATE_NONE || f == TIX_DATE_NONE || f < s)
			return -1;
		int32_t d = workdays(t->calendar, s, clamp((int64_t)f + 1));
		return d < 1 ? 1 : d;
	}
	if (effort == 0)
		return 0;
	int64_t denom = 0; /* sum of units% x capacity% x hours/day, in 1/10000 hour per day */
	for (size_t i = 0; i < pl->nassignments; i++) {
		const tix_assignment *as = &pl->assignments[i];
		size_t r = find(p->res, p->nres, &as->resource);
		if (r == NONE) {
			diag(p, &t->ticket->id, "%s: resource %s not found; assignment ignored",
			     short_id(&t->ticket->id, a), short_id(&as->resource, b));
			continue;
		}
		const tix_resource *res = &p->res[r]->u.resource;
		const tix_entity *rc = NULL;
		if (!tix_oid_is_null(&res->calendar)) {
			size_t k = find(p->cal, p->ncal, &res->calendar);
			rc = k == NONE ? NULL : p->cal[k];
		}
		int hpd = rc ? calendar_hours(rc) : calendar_hours(t->calendar);
		denom += (int64_t)as->units * res->capacity * hpd;
	}
	if (denom <= 0)
		return -1;
	int64_t d = (effort * 10000 + denom - 1) / denom;
	if (d > TIX_DURATION_MAX)
		d = TIX_DURATION_MAX;
	return d < 1 ? 1 : d;
}

/* ---- dependencies ------------------------------------------------------------------- */

/* Leaf tasks under task i (or i itself), in task order. */
static void leaves_under(const plan *p, size_t i, idx_vec *out)
{
	out->n = 0;
	size_t depth = p->tasks.v[i].depth;
	for (size_t k = i; k < p->tasks.n && (k == i || p->tasks.v[k].depth > depth); k++)
		if (!p->tasks.v[k].summary)
			TIX_VEC_PUSH(out, k);
}

static int cmp_edge(const void *pa, const void *pb)
{
	const edge *a = pa, *b = pb;
	if (a->s != b->s)
		return a->s < b->s ? -1 : 1;
	if (a->p != b->p)
		return a->p < b->p ? -1 : 1;
	return tix_order_cmp(&a->order, &b->order);
}

static void build_edges(plan *p)
{
	char a[8], b[8];
	idx_vec ps = {0}, ss = {0};
	for (size_t i = 0; i < p->tasks.n; i++) {
		const tix_entity *t = p->tasks.v[i].ticket;
		const tix_ticket_plan *pl = &t->u.ticket.plan;
		for (size_t k = 0; k < pl->ndeps; k++) {
			const tix_dep *d = &pl->deps[k];
			size_t pt = find(p->tk, p->nt, &d->pred);
			if (pt == NONE || p->task_of[pt] == NONE) {
				diag(p, &t->id, "%s: depends on %s, which is %s; ignored", short_id(&t->id, a),
				     short_id(&d->pred, b), pt == NONE ? "not found" : "outside this plan");
				continue;
			}
			leaves_under(p, p->task_of[pt], &ps);
			leaves_under(p, i, &ss);
			for (size_t x = 0; x < ps.n; x++)
				for (size_t y = 0; y < ss.n; y++) {
					if (ps.v[x] == ss.v[y]) {
						diag(p, &t->id, "%s: depends on itself (via %s); ignored", short_id(&t->id, a),
						     short_id(&d->pred, b));
						continue;
					}
					edge e = {ps.v[x], ss.v[y], d->kind, d->lag, d->order, i, false};
					TIX_VEC_PUSH(&p->edges, e);
				}
		}
	}
	free(ps.v);
	free(ss.v);
	if (p->edges.n)
		qsort(p->edges.v, p->edges.n, sizeof(edge), cmp_edge);
	/* edges touching unscheduled tasks can't constrain anything */
	for (size_t i = 0; i < p->edges.n; i++) {
		edge *e = &p->edges.v[i];
		if (p->lf[e->p].d < 0 || p->lf[e->s].d < 0) {
			e->dropped = true;
			const tix_entity *u = p->tasks.v[p->lf[e->p].d < 0 ? e->p : e->s].ticket;
			diag(p, &p->tasks.v[e->owner].ticket->id, "%s: dependency involves %s, which is unscheduled; ignored",
			     short_id(&p->tasks.v[e->owner].ticket->id, a), short_id(&u->id, b));
		}
	}
}

/* Kahn's algorithm; breaks cycles by dropping the latest dependency on one.
 * ENSURES: order[] holds every leaf task index in topological order. */
static size_t topo(plan *p, size_t *order)
{
	size_t n = p->tasks.n;
	size_t *indeg = tix_xcalloc(n ? n : 1, sizeof(size_t));
	bool *done = tix_xcalloc(n ? n : 1, sizeof(bool));
	size_t nleaves = 0;
	for (size_t i = 0; i < n; i++)
		nleaves += !p->tasks.v[i].summary;
	for (;;) {
		memset(indeg, 0, n * sizeof(size_t));
		memset(done, 0, n * sizeof(bool));
		for (size_t i = 0; i < p->edges.n; i++)
			if (!p->edges.v[i].dropped)
				indeg[p->edges.v[i].s]++;
		size_t qh = 0, qt = 0;
		for (size_t i = 0; i < n; i++)
			if (!p->tasks.v[i].summary && !indeg[i])
				order[qt++] = i, done[i] = true;
		while (qh < qt) {
			size_t v = order[qh++];
			for (size_t i = 0; i < p->edges.n; i++) {
				edge *e = &p->edges.v[i];
				if (!e->dropped && e->p == v && --indeg[e->s] == 0 && !done[e->s])
					order[qt++] = e->s, done[e->s] = true;
			}
		}
		if (qt == nleaves) {
			free(indeg);
			free(done);
			return qt;
		}
		/* Walk backwards along live edges from a stuck leaf until a node repeats. */
		size_t v = 0;
		while (v < n && (p->tasks.v[v].summary || done[v]))
			v++;
		size_t *seen_at = tix_xcalloc(n, sizeof(size_t));
		TIX_VEC(size_t) via = {0}; /* edge indices along the walk */
		for (size_t step = 1;; step++) {
			if (seen_at[v])
				break;
			seen_at[v] = step;
			size_t pick = NONE;
			for (size_t i = 0; i < p->edges.n && pick == NONE; i++) {
				const edge *e = &p->edges.v[i];
				if (!e->dropped && e->s == v && !done[e->p])
					pick = i;
			}
			TIX_REQUIRE(pick != NONE); /* a stuck node always has a live, stuck predecessor */
			TIX_VEC_PUSH(&via, pick);
			v = p->edges.v[pick].p;
		}
		/* The cycle is the walk's edges from where v was first seen. */
		size_t worst = NONE;
		for (size_t k = seen_at[v] - 1; k < via.n; k++)
			if (worst == NONE || tix_order_cmp(&p->edges.v[via.v[k]].order, &p->edges.v[worst].order) > 0)
				worst = via.v[k];
		edge w = p->edges.v[worst];
		char a[8], b[8];
		diag(p, &p->tasks.v[w.owner].ticket->id, "dependency cycle: dropped %s -> %s (the latest change)",
		     short_id(&p->tasks.v[w.p].ticket->id, a), short_id(&p->tasks.v[w.s].ticket->id, b));
		for (size_t i = 0; i < p->edges.n; i++) /* the whole dependency, however expanded */
			if (!tix_order_cmp(&p->edges.v[i].order, &w.order))
				p->edges.v[i].dropped = true;
		free(seen_at);
		free(via.v);
	}
}

/* ---- CPM ------------------------------------------------------------------------------ */

static tix_date project_start(plan *p)
{
	tix_date ps = TIX_DATE_NONE;
	for (size_t i = 0; i < p->tasks.n; i++) {
		const tix_ticket_plan *pl = &p->tasks.v[i].ticket->u.ticket.plan;
		tix_date s = pl->actual_start != TIX_DATE_NONE ? pl->actual_start : pl->start;
		if (s != TIX_DATE_NONE && (ps == TIX_DATE_NONE || s < ps))
			ps = s;
	}
	return ps != TIX_DATE_NONE ? ps : p->in->today;
}

static void forward(plan *p, const size_t *order, size_t n, tix_date ps)
{
	char a[8];
	for (size_t k = 0; k < n; k++) {
		size_t i = order[k];
		tix_plan_task *t = &p->tasks.v[i];
		const tix_ticket_plan *pl = &t->ticket->u.ticket.plan;
		const tix_entity *c = t->calendar;
		leaf *L = &p->lf[i];
		if (L->d < 0)
			continue;
		tix_date fixed_start = pl->actual_start != TIX_DATE_NONE ? pl->actual_start
				       : pl->mode == TIX_MODE_MANUAL ? pl->start
								     : TIX_DATE_NONE;
		tix_date fixed_finish = pl->actual_finish != TIX_DATE_NONE ? pl->actual_finish
					: pl->mode == TIX_MODE_MANUAL ? pl->finish
								      : TIX_DATE_NONE;
		if (pl->mode == TIX_MODE_MANUAL && pl->start == TIX_DATE_NONE && pl->actual_start == TIX_DATE_NONE)
			diag(p, &t->ticket->id, "%s: manual but has no start; scheduled automatically",
			     short_id(&t->ticket->id, a));

		tix_date es = ps;
		bool by_finish = false;
		if (fixed_start != TIX_DATE_NONE) {
			es = fixed_start;
		} else {
			for (size_t up = i; up != NONE; up = p->tasks.v[up].parent) { /* own + ancestors' starts */
				tix_date s = p->tasks.v[up].ticket->u.ticket.plan.start;
				if (s != TIX_DATE_NONE && s > es)
					es = s;
			}
			for (size_t x = 0; x < p->edges.n; x++) {
				const edge *e = &p->edges.v[x];
				if (e->dropped || e->s != i)
					continue;
				const leaf *P = &p->lf[e->p];
				tix_date cand;
				switch (e->kind) {
				case TIX_DEP_FS: cand = shift(c, P->e, e->lag); break;
				case TIX_DEP_SS: cand = shift(c, P->es, e->lag); break;
				case TIX_DEP_FF: cand = start_from_end(c, shift(c, P->e, e->lag), L->d); break;
				default: cand = start_from_end(c, shift(c, P->es, e->lag), L->d); break;
				}
				if (cand > es || (cand == es && (e->kind == TIX_DEP_FS || e->kind == TIX_DEP_FF))) {
					by_finish = e->kind == TIX_DEP_FS || e->kind == TIX_DEP_FF;
					es = cand > es ? cand : es;
				}
			}
		}
		/* a computed start moves to a working day; a fixed one (actual dates, a manual
		 * start) happened or was drawn there, so it stays, even on a weekend */
		if (fixed_start == TIX_DATE_NONE && (L->d > 0 || !by_finish))
			es = next_work(c, es);
		L->es = es;
		L->finish_driven = L->d == 0 && by_finish;
		if (fixed_finish != TIX_DATE_NONE) {
			if (fixed_finish < es) {
				diag(p, &t->ticket->id, "%s: finish before start; finish ignored", short_id(&t->ticket->id, a));
				L->e = end_from_start(c, es, L->d);
			} else {
				L->e = clamp((int64_t)fixed_finish + 1);
				L->d = workdays(c, es, L->e);
			}
			L->fixed = true;
		} else {
			L->e = end_from_start(c, es, L->d);
			L->fixed = fixed_start != TIX_DATE_NONE;
		}
	}
}

static void backward(plan *p, const size_t *order, size_t n, tix_date pe)
{
	for (size_t k = n; k-- > 0;) {
		size_t i = order[k];
		leaf *L = &p->lf[i];
		const tix_entity *c = p->tasks.v[i].calendar;
		if (L->d < 0)
			continue;
		tix_date le = pe;
		for (size_t x = 0; x < p->edges.n; x++) {
			const edge *e = &p->edges.v[x];
			if (e->dropped || e->p != i)
				continue;
			const leaf *S = &p->lf[e->s];
			const tix_entity *cs = p->tasks.v[e->s].calendar;
			tix_date bound;
			switch (e->kind) {
			case TIX_DEP_FS: bound = shift(cs, S->ls, -e->lag); break;
			case TIX_DEP_FF: bound = shift(cs, S->le, -e->lag); break;
			case TIX_DEP_SS: bound = end_from_start(c, prev_work(c, shift(cs, S->ls, -e->lag)), L->d); break;
			default: bound = end_from_start(c, prev_work(c, shift(cs, S->le, -e->lag)), L->d); break;
			}
			if (bound < le)
				le = bound;
		}
		L->le = le;
		L->ls = start_from_end(c, le, L->d);
	}
}

/* ---- rollups and output ------------------------------------------------------------------ */

static void fill_leaf(plan *p, size_t i)
{
	tix_plan_task *t = &p->tasks.v[i];
	const tix_ticket_plan *pl = &t->ticket->u.ticket.plan;
	const leaf *L = &p->lf[i];
	const tix_entity *c = t->calendar;
	t->milestone = pl->kind == TIX_KIND_MILESTONE || L->d == 0;
	t->effort = leaf_effort(pl);
	t->progress = t->ticket->u.ticket.status == TIX_TICKET_DONE ? 100 : pl->progress;
	t->duration = L->d;
	if (L->d < 0) {
		t->start = t->finish = t->late_start = t->late_finish = TIX_DATE_NONE;
		return;
	}
	t->scheduled = true;
	t->manual = L->fixed;
	if (t->milestone) {
		t->start = t->finish = L->finish_driven ? prev_work(c, clamp((int64_t)L->es - 1)) : L->es;
		t->late_start = t->late_finish = L->finish_driven ? prev_work(c, clamp((int64_t)L->ls - 1)) : L->ls;
	} else {
		t->start = L->es;
		t->finish = prev_work(c, clamp((int64_t)L->e - 1));
		t->late_start = L->ls;
		t->late_finish = prev_work(c, clamp((int64_t)L->le - 1));
	}
	t->total_float = workdays(c, L->es, L->ls);
	t->critical = t->total_float <= 0;
}

static void rollup(plan *p)
{
	/* children come after their parent in pre-order, so walk backwards */
	int64_t *wsum = tix_xcalloc(p->tasks.n ? p->tasks.n : 1, sizeof(int64_t));
	int64_t *wp = tix_xcalloc(p->tasks.n ? p->tasks.n : 1, sizeof(int64_t));
	bool *any = tix_xcalloc(p->tasks.n ? p->tasks.n : 1, sizeof(bool));
	for (size_t i = p->tasks.n; i-- > 0;) {
		tix_plan_task *t = &p->tasks.v[i];
		if (t->summary) {
			if (wsum[i])
				t->progress = (int)(wp[i] / wsum[i]);
			if (t->scheduled) {
				t->duration = workdays(t->calendar, t->start, clamp((int64_t)t->finish + 1));
				t->milestone = false;
			}
			if (t->effort < 0 && t->ticket->u.ticket.plan.effort >= 0)
				t->effort = t->ticket->u.ticket.plan.effort;
		}
		size_t par = t->parent;
		if (par == NONE)
			continue;
		tix_plan_task *u = &p->tasks.v[par];
		if (t->scheduled) {
			if (!u->scheduled) {
				u->scheduled = true;
				u->start = t->start;
				u->finish = t->finish;
				u->late_start = t->late_start;
				u->late_finish = t->late_finish;
				u->total_float = t->total_float;
			} else {
				if (t->start < u->start)
					u->start = t->start;
				if (t->finish > u->finish)
					u->finish = t->finish;
				if (t->late_start < u->late_start)
					u->late_start = t->late_start;
				if (t->late_finish > u->late_finish)
					u->late_finish = t->late_finish;
				if (t->total_float < u->total_float)
					u->total_float = t->total_float;
			}
			u->critical |= t->critical;
		}
		if (t->effort >= 0) {
			u->effort = (u->effort < 0 || !any[par]) ? t->effort : u->effort + t->effort;
			any[par] = true;
		}
		int64_t w = t->effort > 0 ? t->effort : t->duration > 0 ? t->duration * 8 : 1;
		wsum[par] += w;
		wp[par] += w * t->progress;
	}
	free(wsum);
	free(wp);
	free(any);
}

/* Hours one assignment works on its task: its own effort, else its share of the task's
 * effort (by units x capacity), else the task's duration at its units. */
static double assignment_hours(const tix_plan_task *t, const tix_assignment *a, double weight, double wsum)
{
	if (a->effort >= 0)
		return (double)a->effort;
	if (t->effort >= 0)
		return wsum > 0 ? (double)t->effort * weight / wsum : 0;
	if (t->duration > 0) {
		int hours = t->calendar ? t->calendar->u.calendar.hours : 8;
		return (double)t->duration * hours * weight / 10000.0;
	}
	return 0;
}

/*
 * Cost per task: hours x hourly rate over the assignments to rated resources (unrated ones
 * cost nothing here). Several currencies on one task, or below one summary, make the cost
 * unknown rather than a meaningless sum. Doubles round once at the end (deterministic).
 */
static void costs(plan *p)
{
	size_t n = p->tasks.n;
	bool *mixed = tix_xcalloc(n ? n : 1, sizeof(bool));
	for (size_t i = 0; i < n; i++) {
		tix_plan_task *t = &p->tasks.v[i];
		t->cost = -1;
		t->currency = NULL;
		if (t->summary)
			continue;
		const tix_ticket_plan *pl = &t->ticket->u.ticket.plan;
		double wsum = 0, total = 0;
		for (size_t k = 0; k < pl->nassignments; k++) {
			size_t r = find(p->res, p->nres, &pl->assignments[k].resource);
			if (r != NONE)
				wsum += (double)pl->assignments[k].units * p->res[r]->u.resource.capacity;
		}
		bool any = false;
		for (size_t k = 0; k < pl->nassignments; k++) {
			size_t r = find(p->res, p->nres, &pl->assignments[k].resource);
			if (r == NONE)
				continue;
			const tix_resource *res = &p->res[r]->u.resource;
			if (res->rate < 0 || !res->currency)
				continue;
			if (t->currency && strcmp(t->currency, res->currency))
				mixed[i] = true;
			t->currency = res->currency;
			double w = (double)pl->assignments[k].units * res->capacity;
			total += assignment_hours(t, &pl->assignments[k], w, wsum) * (double)res->rate;
			any = true;
		}
		if (mixed[i] || !any) {
			t->currency = NULL;
			continue;
		}
		t->cost = total >= (double)TIX_COST_MAX ? TIX_COST_MAX : (int64_t)(total + 0.5);
	}
	/* summaries: children first (pre-order backwards) */
	for (size_t i = n; i-- > 0;) {
		tix_plan_task *t = &p->tasks.v[i];
		size_t par = t->parent;
		if (par == NONE)
			continue;
		tix_plan_task *u = &p->tasks.v[par];
		if (mixed[i])
			mixed[par] = true;
		if (t->cost < 0 || mixed[par])
			continue;
		if (u->currency && strcmp(u->currency, t->currency)) {
			mixed[par] = true;
			u->cost = -1;
			u->currency = NULL;
			continue;
		}
		u->currency = t->currency;
		u->cost = (u->cost < 0 ? 0 : u->cost) + t->cost;
		if (u->cost > TIX_COST_MAX)
			u->cost = TIX_COST_MAX;
	}
	for (size_t i = 0; i < n; i++)
		if (mixed[i]) {
			p->tasks.v[i].cost = -1;
			p->tasks.v[i].currency = NULL;
		}
	free(mixed);
}

static void actuals_and_deadlines(plan *p)
{
	char a[8], s1[TIX_DATE_STR], s2[TIX_DATE_STR];
	for (size_t i = 0; i < p->tasks.n; i++) {
		tix_plan_task *t = &p->tasks.v[i];
		const tix_ticket_plan *pl = &t->ticket->u.ticket.plan;
		t->actual_start = pl->actual_start != TIX_DATE_NONE ? pl->actual_start
				  : pl->started_at >= 0       ? tix_date_from_unix(pl->started_at)
							      : TIX_DATE_NONE;
		t->actual_finish = pl->actual_finish != TIX_DATE_NONE ? pl->actual_finish
				   : pl->done_at >= 0          ? tix_date_from_unix(pl->done_at)
							       : TIX_DATE_NONE;
		if (pl->deadline != TIX_DATE_NONE && t->scheduled && t->finish > pl->deadline) {
			t->late = true;
			diag(p, &t->ticket->id, "%s: finishes %s, after its deadline %s", short_id(&t->ticket->id, a),
			     tix_date_str(t->finish, s1), tix_date_str(pl->deadline, s2));
		}
	}
}

static int cmp_load(const void *a, const void *b)
{
	return tix_oid_cmp(&((const tix_plan_load *)a)->resource->id, &((const tix_plan_load *)b)->resource->id);
}

static void loads(plan *p, tix_schedule *s)
{
	if (s->start == TIX_DATE_NONE)
		return;
	size_t span = (size_t)(s->finish - s->start) + 1;
	if (span > TIX_PLAN_MAX_LOAD_DAYS) {
		diag(p, NULL, "plan spans %zu days; resource load covers the first %d", span, TIX_PLAN_MAX_LOAD_DAYS);
		span = TIX_PLAN_MAX_LOAD_DAYS;
	}
	s->ndays = span;
	TIX_VEC(tix_plan_load) v = {0};
	for (size_t i = 0; i < p->tasks.n; i++) {
		const tix_plan_task *t = &p->tasks.v[i];
		if (t->summary || !t->scheduled || t->milestone)
			continue;
		const tix_ticket_plan *pl = &t->ticket->u.ticket.plan;
		for (size_t k = 0; k < pl->nassignments; k++) {
			size_t r = find(p->res, p->nres, &pl->assignments[k].resource);
			if (r == NONE)
				continue; /* reported with the duration */
			size_t li = NONE;
			for (size_t m = 0; m < v.n && li == NONE; m++)
				if (v.v[m].resource == p->res[r])
					li = m;
			if (li == NONE) {
				tix_plan_load l = {p->res[r], tix_xcalloc(span, sizeof(int32_t)), tix_xcalloc(span, sizeof(bool)), 0, 0};
				const tix_oid *rc = &p->res[r]->u.resource.calendar;
				size_t ck = tix_oid_is_null(rc) ? NONE : find(p->cal, p->ncal, rc);
				const tix_entity *cal = ck == NONE ? NULL : p->cal[ck];
				for (size_t d = 0; d < span; d++)
					l.working[d] = tix_plan_is_working(cal, s->start + (tix_date)d);
				li = v.n;
				TIX_VEC_PUSH(&v, l);
			}
			for (tix_date d = t->start; d <= t->finish; d++) {
				size_t off = (size_t)(d - s->start);
				if (off < span && tix_plan_is_working(t->calendar, d))
					v.v[li].units[off] += pl->assignments[k].units;
			}
		}
	}
	for (size_t i = 0; i < v.n; i++) {
		tix_plan_load *l = &v.v[i];
		for (size_t d = 0; d < span; d++) {
			if (l->units[d] > l->peak)
				l->peak = l->units[d];
			if (l->units[d] > (l->working[d] ? 100 : 0))
				l->over_days++;
		}
		if (l->over_days) {
			char a[8];
			diag(p, &l->resource->id, "resource %s (%s) is over-allocated on %zu day(s), peak %d%%",
			     short_id(&l->resource->id, a), l->resource->u.resource.name, l->over_days, (int)l->peak);
		}
	}
	if (v.n)
		qsort(v.v, v.n, sizeof(*v.v), cmp_load);
	s->loads = v.v;
	s->nloads = v.n;
}

tix_status tix_plan_compute(const tix_plan_input *in, tix_schedule **out, tix_error *err)
{
	TIX_REQUIRE(in && out && (in->tickets || !in->ntickets) && tix_date_valid(in->today));
	*out = NULL;
	plan p;
	memset(&p, 0, sizeof(p));
	p.in = in;
	p.tk = sorted(in->tickets, in->ntickets);
	p.nt = in->ntickets;
	p.res = sorted(in->resources, in->nresources);
	p.nres = in->nresources;
	p.cal = sorted(in->calendars, in->ncalendars);
	p.ncal = in->ncalendars;
	for (size_t i = 0; i < p.nt; i++)
		TIX_REQUIRE(p.tk[i]->type == TIX_ENTITY_TICKET);

	size_t root = NONE;
	if (in->root && (root = find(p.tk, p.nt, in->root)) == NONE) {
		free(p.tk);
		free(p.res);
		free(p.cal);
		char hex[TIX_OID_MAX_HEX + 1];
		return tix_error_set(err, TIX_ERR_NOT_FOUND, "no ticket %s to plan", tix_oid_hex(in->root, hex));
	}
	build_parents(&p);
	kids k;
	build_kids(&p, &k);
	p.task_of = tix_xcalloc(p.nt ? p.nt : 1, sizeof(size_t));
	for (size_t i = 0; i < p.nt; i++)
		p.task_of[i] = NONE;
	idx_vec tops = {0};
	if (root != NONE) {
		TIX_VEC_PUSH(&tops, root);
	} else {
		for (size_t i = 0; i < p.nt; i++)
			if (p.parent[i] == NONE)
				TIX_VEC_PUSH(&tops, i);
		sib_ctx = &p;
		if (tops.n)
			qsort(tops.v, tops.n, sizeof(size_t), cmp_sibling_q);
	}
	const tix_entity *inh = NULL; /* a sub-tree root still inherits from above */
	if (root != NONE) {
		idx_vec chain = {0};
		for (size_t up = p.parent[root]; up != NONE; up = p.parent[up])
			TIX_VEC_PUSH(&chain, up);
		for (size_t c = chain.n; c-- > 0;)
			inh = resolve_calendar(&p, chain.v[c], inh);
		free(chain.v);
	}
	walk(&p, &k, tops.v, tops.n, inh);
	free(tops.v);
	free(k.start);
	free(k.v);

	size_t n = p.tasks.n;
	p.lf = tix_xcalloc(n ? n : 1, sizeof(leaf));
	for (size_t i = 0; i < n; i++)
		p.lf[i].d = p.tasks.v[i].summary ? -1 : leaf_duration(&p, i);
	build_edges(&p);
	size_t *order = tix_xcalloc(n ? n : 1, sizeof(size_t));
	size_t norder = topo(&p, order);
	tix_date ps = project_start(&p);
	forward(&p, order, norder, ps);
	tix_date pe = TIX_DATE_NONE;
	for (size_t i = 0; i < n; i++)
		if (!p.tasks.v[i].summary && p.lf[i].d >= 0 && (pe == TIX_DATE_NONE || p.lf[i].e > pe))
			pe = p.lf[i].e;
	if (pe != TIX_DATE_NONE)
		backward(&p, order, norder, pe);
	for (size_t i = 0; i < n; i++) {
		p.tasks.v[i].effort = -1;
		if (!p.tasks.v[i].summary)
			fill_leaf(&p, i);
	}
	rollup(&p);
	costs(&p);
	actuals_and_deadlines(&p);

	tix_schedule *s = tix_xcalloc(1, sizeof(*s));
	s->start = s->finish = TIX_DATE_NONE;
	for (size_t i = 0; i < n; i++) {
		const tix_plan_task *t = &p.tasks.v[i];
		if (!t->scheduled)
			continue;
		if (s->start == TIX_DATE_NONE || t->start < s->start)
			s->start = t->start;
		if (s->finish == TIX_DATE_NONE || t->finish > s->finish)
			s->finish = t->finish;
	}
	loads(&p, s);
	s->tasks = p.tasks.v;
	s->ntasks = p.tasks.n;
	s->diags = p.diags.v;
	s->ndiags = p.diags.n;
	free(order);
	free(p.lf);
	free(p.edges.v);
	free(p.ticket_of);
	free(p.task_of);
	free(p.parent);
	free(p.tk);
	s->resources = p.res;
	s->nresources = p.nres;
	s->calendars = p.cal;
	s->ncalendars = p.ncal;
	*out = s;
	return TIX_OK;
}

void tix_schedule_free(tix_schedule *s)
{
	if (!s)
		return;
	for (size_t i = 0; i < s->ndiags; i++)
		free(s->diags[i].msg);
	for (size_t i = 0; i < s->nloads; i++) {
		free(s->loads[i].units);
		free(s->loads[i].working);
	}
	free(s->diags);
	free(s->loads);
	free(s->tasks);
	free(s->resources);
	free(s->calendars);
	free(s);
}

size_t tix_schedule_find(const tix_schedule *s, const tix_oid *id)
{
	for (size_t i = 0; i < s->ntasks; i++)
		if (tix_oid_eq(&s->tasks[i].ticket->id, id))
			return i;
	return NONE;
}

const tix_entity *tix_schedule_resource(const tix_schedule *s, const tix_oid *id)
{
	size_t k = find(s->resources, s->nresources, id);
	return k == NONE ? NULL : s->resources[k];
}

/* Working days from base to now in the task's calendar: positive when now is later. */
static int32_t slip(const tix_entity *c, tix_date base, tix_date now)
{
	if (now == base)
		return 0;
	return now > base ? workdays(c, clamp((int64_t)base + 1), clamp((int64_t)now + 1))
			  : -workdays(c, clamp((int64_t)now + 1), clamp((int64_t)base + 1));
}

size_t tix_plan_compare(const tix_schedule *s, const tix_baseline *b, tix_plan_variance *out)
{
	TIX_REQUIRE(s && b && (out || !s->ntasks));
	size_t present = 0;
	for (size_t i = 0; i < s->ntasks; i++) {
		const tix_plan_task *t = &s->tasks[i];
		tix_plan_variance *v = &out[i];
		memset(v, 0, sizeof(*v));
		v->start = v->finish = TIX_DATE_NONE;
		v->duration = v->effort = v->cost = -1;
		const tix_baseline_task *k = tix_baseline_find(b, &t->ticket->id);
		if (!k)
			continue;
		present++;
		v->baselined = true;
		v->start = k->start;
		v->finish = k->finish;
		v->duration = k->duration;
		v->effort = k->effort;
		v->cost = k->cost;
		v->currency = k->currency;
		if (t->scheduled && k->start != TIX_DATE_NONE) {
			v->dates = true;
			v->start_slip = slip(t->calendar, k->start, t->start);
			v->finish_slip = slip(t->calendar, k->finish, t->finish);
		}
		if ((v->has_duration = t->duration >= 0 && k->duration >= 0))
			v->duration_delta = t->duration - k->duration;
		if ((v->has_effort = t->effort >= 0 && k->effort >= 0))
			v->effort_delta = t->effort - k->effort;
		if ((v->has_cost = t->cost >= 0 && k->cost >= 0 && t->currency && k->currency &&
				   !strcmp(t->currency, k->currency)))
			v->cost_delta = t->cost - k->cost;
	}
	return b->ntasks - present;
}

