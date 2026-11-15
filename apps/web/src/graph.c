/*
 * Dependency graph (docs/architecture.md section 13.10): GET /graph?at=ID&page=N&show=ID.
 *
 * A block diagram drawn by the server, in sheets. One sheet shows the tickets directly
 * below one ticket (`at`; none = the WBS roots) and how they depend on each other. There
 * is no free-form layout, only fixed templates on a grid:
 *
 *   ticket block   a ticket without sub-tickets; links to the ticket
 *   group card     a ticket with sub-tickets, drawn as a stack; links to its own sheet
 *   details block  the ticket the sheet is about, across the top of the frame
 *   connector tag  under a block: a dependency that leaves the sheet, with the block at
 *                  its far end; links to the sheet that shows that block
 *
 * Dependencies are lifted to the blocks of the sheet: a link between tickets somewhere
 * below two blocks is one arrow between the blocks, and a link to anything outside the
 * sheet is a tag naming the nearest block that is not an ancestor of this sheet. Blocks are
 * placed in columns by dependency order ("step 1", "step 2", ...), at most ROWS per column.
 * COLS columns make a band; bands are stacked on a sheet while it has at most ROWS rows of
 * blocks (so a long chain snakes down the sheet), and what doesn't fit continues on the
 * next sheet, with tags for the arrows that cross. Blocks without links on the sheet follow
 * in WBS order.
 *
 * Everything is deterministic in the plan (WBS order and ticket ids), so every clone draws
 * the same sheets. Styling is by class, geometry by presentation attributes (the CSP
 * forbids style attributes). Text is clipped by nested <svg> viewports, so a hostile or
 * wide title cannot run over its neighbours. A table under the drawing says the same in
 * text (w3m, screen readers).
 */
#include "app.h"

#include <stdlib.h>
#include <string.h>

enum {
	COLS = 4,       /* columns per band */
	ROWS = 4,       /* blocks per column, and rows of blocks per sheet */
	BW = 200,       /* block */
	BH = 78,
	CHAN = 48,      /* between columns: where arrows run up and down */
	GUT = 30,       /* between rows: where arrows run sideways */
	CHIP_H = 20,    /* one connector tag */
	CHIPS = 3,      /* tags under one block; more become "+N more" */
	HEAD_H = 76,    /* details block */
	LAB_H = 22,     /* column labels */
	FOOT_H = 30,    /* previous/next sheet connectors */
	TITLE_CPS = 27, /* code points per title line in a block */
	CHIP_CPS = 20,
};
#define NONE SIZE_MAX

typedef struct raw {
	size_t a, b; /* edge: node -> node; tag: node, the task at the far end */
	bool out;    /* tag: the dependency leaves this block (else it arrives) */
	bool sheet;  /* tag: the far end is on another sheet of the same parent */
	tix_dep_kind kind;
	int32_t lag;
} raw;

typedef struct edge {
	size_t a, b, n; /* n links between tickets below a and b */
	tix_dep_kind kind;
	int32_t lag;    /* of the link when n == 1 */
	bool back;      /* closes a cycle among the blocks: drawn dashed, ignored for the order */
} edge;

typedef struct chip {
	size_t node, other, n;
	bool out, sheet;
} chip;

typedef struct node {
	size_t task;
	size_t rank, col, row; /* row: within its column, then on its sheet */
	size_t page;
	bool linked;        /* has an arrow on this sheet */
	size_t below, done; /* tickets without sub-tickets below it, and how many are finished */
	size_t inner;       /* links between tickets below it (seen on its own sheet) */
	size_t chip0, nchips;
} node;

typedef struct graph {
	const tix_schedule *s;
	size_t focus; /* task the sheet is about, NONE = top level */
	node *nodes;  /* m blocks in WBS order, then one entry for the details block */
	size_t m;
	size_t *nodeof; /* task -> node, NONE */
	edge *edges;    /* sorted by (a, b) */
	size_t ne;
	chip *chips; /* sorted by (node, in before out, other) */
	size_t nc;
	size_t *colrank; /* per column: its step, NONE for blocks without links */
	size_t *colpage; /* per column: its sheet, and the sheet row its band starts at */
	size_t *colrow;
	size_t ncols, npages;
	size_t free0; /* the first column of blocks without links (the columns before it that have no step are padding) */
} graph;

typedef TIX_VEC(raw) raws;

static void graph_free(graph *g)
{
	free(g->nodes);
	free(g->nodeof);
	free(g->edges);
	free(g->chips);
	free(g->colrank);
	free(g->colpage);
	free(g->colrow);
}

/* ---- the model: blocks, arrows and tags of one sheet ----------------------------------- */

typedef struct idx {
	const tix_oid *id;
	size_t task;
} idx;

static int idx_cmp(const void *a, const void *b)
{
	return tix_oid_cmp(((const idx *)a)->id, ((const idx *)b)->id);
}

static size_t idx_find(const idx *v, size_t n, const tix_oid *id)
{
	size_t lo = 0, hi = n;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		int c = tix_oid_cmp(v[mid].id, id);
		if (!c)
			return v[mid].task;
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return NONE;
}

static int raw_cmp(const void *pa, const void *pb)
{
	const raw *a = pa, *b = pb;
	if (a->a != b->a)
		return a->a < b->a ? -1 : 1;
	if (a->out != b->out)
		return a->out ? 1 : -1;
	if (a->b != b->b)
		return a->b < b->b ? -1 : 1;
	if (a->kind != b->kind)
		return a->kind < b->kind ? -1 : 1;
	return a->lag < b->lag ? -1 : a->lag > b->lag;
}

static int rank_cmp(const void *pa, const void *pb)
{
	const node *a = *(const node *const *)pa, *b = *(const node *const *)pb;
	if (a->rank != b->rank)
		return a->rank < b->rank ? -1 : 1;
	return a->task < b->task ? -1 : a->task > b->task;
}

static bool finished(const tix_plan_task *t)
{
	tix_ticket_status st = t->ticket->u.ticket.status;
	return st == TIX_TICKET_DONE || st == TIX_TICKET_CANCELLED;
}

/* Steps: a block comes one column after the last block it waits for. Kahn's algorithm in
 * WBS order. When only loops (and what waits for them) are left, one block of a loop starts
 * anyway: the one found by walking back from the first block still waiting, always to its
 * first unplaced predecessor, until a block comes up twice. The arrows that then point
 * back at a placed block are marked. No recursion (the input may be a hostile chain of
 * any length). */
static void rank(graph *g)
{
	size_t m = g->m;
	size_t *indeg = tix_xcalloc(m ? m : 1, sizeof(size_t)), *first = tix_xcalloc(m + 1, sizeof(size_t));
	size_t *queue = tix_xcalloc(m ? m : 1, sizeof(size_t));
	bool *placed = tix_xcalloc(m ? m : 1, sizeof(bool));
	/* in: edge indices by target (then source, as the edges are sorted) */
	size_t *infirst = tix_xcalloc(m + 2, sizeof(size_t)), *in = tix_xcalloc(g->ne ? g->ne : 1, sizeof(size_t));
	size_t *seen = tix_xcalloc(m ? m : 1, sizeof(size_t)), walk = 0;
	for (size_t k = 0; k < g->ne; k++) {
		indeg[g->edges[k].b]++;
		first[g->edges[k].a + 1]++;
		infirst[g->edges[k].b + 2]++;
		g->nodes[g->edges[k].a].linked = g->nodes[g->edges[k].b].linked = true;
	}
	for (size_t i = 0; i < m; i++) {
		first[i + 1] += first[i];
		infirst[i + 2] += infirst[i + 1];
	}
	for (size_t k = 0; k < g->ne; k++)
		in[infirst[g->edges[k].b + 1]++] = k; /* infirst[b] is now where b's edges start */
	size_t head = 0, tail = 0, cursor = 0;
	for (size_t i = 0; i < m; i++)
		if (!indeg[i]) {
			placed[i] = true;
			queue[tail++] = i;
		}
	for (;;) {
		while (head < tail) {
			size_t u = queue[head++];
			for (size_t k = first[u]; k < first[u + 1]; k++) {
				edge *e = &g->edges[k];
				if (placed[e->b]) {
					e->back = true;
					continue;
				}
				if (g->nodes[e->b].rank < g->nodes[u].rank + 1)
					g->nodes[e->b].rank = g->nodes[u].rank + 1;
				if (!--indeg[e->b]) {
					placed[e->b] = true;
					queue[tail++] = e->b;
				}
			}
		}
		while (cursor < m && placed[cursor])
			cursor++;
		if (cursor == m)
			break;
		/* every unplaced block still waits for an unplaced one, so this walk ends on a loop */
		size_t v = cursor;
		for (walk++; seen[v] != walk;) {
			seen[v] = walk;
			for (size_t k = infirst[v]; k < infirst[v + 1]; k++)
				if (!placed[g->edges[in[k]].a]) {
					v = g->edges[in[k]].a;
					break;
				}
		}
		placed[v] = true;
		queue[tail++] = v;
	}
	free(infirst);
	free(in);
	free(seen);
	free(indeg);
	free(first);
	free(queue);
	free(placed);
}

/* Columns: the linked blocks by step (a step with more than ROWS blocks takes several
 * columns), then the rest in WBS order, one row of COLS to a band. Then the bands are
 * dealt onto sheets. */
static void place(graph *g)
{
	size_t m = g->m, nl = 0, c = 0;
	node **order = tix_xcalloc(m ? m : 1, sizeof(*order));
	g->colrank = tix_xcalloc(m + COLS + 1, sizeof(size_t));
	for (size_t i = 0; i < m; i++)
		if (g->nodes[i].linked)
			order[nl++] = &g->nodes[i];
	qsort(order, nl, sizeof(*order), rank_cmp);
	for (size_t i = 0, j = 0; i < nl; i++, j++) {
		if (i && (order[i]->rank != order[i - 1]->rank || j == ROWS)) {
			c++;
			j = 0;
		}
		order[i]->col = c;
		order[i]->row = j;
		g->colrank[c] = order[i]->rank;
	}
	if (nl)
		c++;
	size_t ncols = c;
	c = (c + COLS - 1) / COLS * COLS;
	g->free0 = c;
	for (size_t i = 0; i < m; i++) {
		node *nd = &g->nodes[i];
		if (nd->linked)
			continue;
		nd->col = c++;
		ncols = c;
	}
	for (size_t i = nl ? order[nl - 1]->col + 1 : 0; i < ncols; i++)
		g->colrank[i] = NONE;
	g->ncols = ncols;
	free(order);

	size_t *height = tix_xcalloc(ncols + 1, sizeof(size_t));
	g->colpage = tix_xcalloc(ncols + 1, sizeof(size_t));
	g->colrow = tix_xcalloc(ncols + 1, sizeof(size_t));
	for (size_t i = 0; i < m; i++)
		if (g->nodes[i].row + 1 > height[g->nodes[i].col])
			height[g->nodes[i].col] = g->nodes[i].row + 1;
	size_t page = 0, used = 0;
	for (size_t b = 0; b < ncols; b += COLS) {
		size_t bh = 0;
		for (size_t i = b; i < ncols && i < b + COLS; i++)
			if (height[i] > bh)
				bh = height[i];
		if (used && used + bh > ROWS) {
			page++;
			used = 0;
		}
		for (size_t i = b; i < ncols && i < b + COLS; i++) {
			g->colpage[i] = page;
			g->colrow[i] = used;
		}
		used += bh;
	}
	g->npages = page + 1;
	for (size_t i = 0; i < m; i++) {
		g->nodes[i].page = g->colpage[g->nodes[i].col];
		g->nodes[i].row += g->colrow[g->nodes[i].col];
	}
	free(height);
}

static void build(graph *g, const tix_schedule *s, size_t focus)
{
	size_t n = s->ntasks, m = 0;
	memset(g, 0, sizeof(*g));
	g->s = s;
	g->focus = focus;
	/* rep: the block of the sheet a task is (or is below), NONE outside the sheet */
	size_t *rep = tix_xcalloc(n ? n : 1, sizeof(size_t));
	bool *anc = tix_xcalloc(n ? n : 1, sizeof(bool)); /* the focus and its ancestors */
	idx *byid = tix_xcalloc(n ? n : 1, sizeof(idx));
	g->nodeof = tix_xcalloc(n ? n : 1, sizeof(size_t));
	for (size_t i = focus; i != NONE; i = s->tasks[i].parent)
		anc[i] = true;
	for (size_t i = 0; i < n; i++) { /* pre-order: a parent comes before its children */
		size_t p = s->tasks[i].parent;
		rep[i] = p == focus ? i : p == NONE ? NONE : rep[p];
		g->nodeof[i] = NONE;
		if (p == focus)
			g->nodeof[i] = m++;
		byid[i] = (idx){&s->tasks[i].ticket->id, i};
	}
	qsort(byid, n, sizeof(*byid), idx_cmp);
	g->m = m;
	g->nodes = tix_xcalloc(m + 1, sizeof(node));
	g->nodes[m].task = focus;
	for (size_t i = 0; i < n; i++) {
		if (g->nodeof[i] != NONE)
			g->nodes[g->nodeof[i]].task = i;
		if (rep[i] == NONE || s->tasks[i].summary)
			continue;
		g->nodes[m].below++; /* the details block counts the whole sheet */
		g->nodes[m].done += finished(&s->tasks[i]);
		if (rep[i] != i) {
			g->nodes[g->nodeof[rep[i]]].below++;
			g->nodes[g->nodeof[rep[i]]].done += finished(&s->tasks[i]);
		}
	}

	raws re = {0}, rc = {0};
	for (size_t i = 0; i < n; i++) {
		const tix_ticket_plan *pl = &s->tasks[i].ticket->u.ticket.plan;
		for (size_t k = 0; k < pl->ndeps; k++) {
			size_t p = idx_find(byid, n, &pl->deps[k].pred);
			if (p == NONE || p == i)
				continue;
			size_t a = rep[p], b = rep[i], from = NONE, other = NONE;
			bool out = false;
			if (a != NONE && a == b) {
				g->nodes[g->nodeof[a]].inner++;
			} else if (a != NONE && b != NONE) {
				raw r = {g->nodeof[a], g->nodeof[b], false, false, pl->deps[k].kind, pl->deps[k].lag};
				TIX_VEC_PUSH(&re, r);
			} else if (a != NONE) {
				from = g->nodeof[a], other = i, out = true;
			} else if (b != NONE) {
				from = g->nodeof[b], other = p;
			} else if (focus != NONE && i == focus) {
				from = m, other = p;
			} else if (focus != NONE && p == focus) {
				from = m, other = i, out = true;
			}
			if (from == NONE)
				continue;
			/* name the largest block around the far end that doesn't contain this sheet */
			while (!anc[other] && s->tasks[other].parent != NONE && !anc[s->tasks[other].parent])
				other = s->tasks[other].parent;
			raw r = {from, other, out, false, pl->deps[k].kind, pl->deps[k].lag};
			TIX_VEC_PUSH(&rc, r);
		}
	}
	free(rep);
	free(anc);
	free(byid);

	if (re.n)
		qsort(re.v, re.n, sizeof(raw), raw_cmp);
	g->edges = tix_xcalloc(re.n ? re.n : 1, sizeof(edge));
	for (size_t i = 0; i < re.n; i++) {
		if (g->ne && g->edges[g->ne - 1].a == re.v[i].a && g->edges[g->ne - 1].b == re.v[i].b)
			g->edges[g->ne - 1].n++;
		else
			g->edges[g->ne++] = (edge){re.v[i].a, re.v[i].b, 1, re.v[i].kind, re.v[i].lag, false};
	}
	free(re.v);
	rank(g);
	place(g);

	/* arrows between two sheets of this parent become a tag at each end */
	for (size_t k = 0; k < g->ne; k++) {
		const node *a = &g->nodes[g->edges[k].a], *b = &g->nodes[g->edges[k].b];
		if (a->page == b->page)
			continue;
		for (size_t j = 0; j < g->edges[k].n; j++) {
			raw ro = {g->edges[k].a, b->task, true, true, TIX_DEP_FS, 0};
			raw ri = {g->edges[k].b, a->task, false, true, TIX_DEP_FS, 0};
			TIX_VEC_PUSH(&rc, ro);
			TIX_VEC_PUSH(&rc, ri);
		}
	}
	if (rc.n)
		qsort(rc.v, rc.n, sizeof(raw), raw_cmp);
	g->chips = tix_xcalloc(rc.n ? rc.n : 1, sizeof(chip));
	for (size_t i = 0; i < rc.n; i++) {
		chip *last = g->nc ? &g->chips[g->nc - 1] : NULL;
		if (last && last->node == rc.v[i].a && last->out == rc.v[i].out && last->other == rc.v[i].b) {
			last->n++;
			continue;
		}
		node *nd = &g->nodes[rc.v[i].a];
		if (!nd->nchips++)
			nd->chip0 = g->nc;
		g->chips[g->nc++] = (chip){rc.v[i].a, rc.v[i].b, 1, rc.v[i].out, rc.v[i].sheet};
	}
	free(rc.v);
}

/* ---- drawing ----------------------------------------------------------------------- */

#define A(h, name, v)                         \
	do {                                  \
		HL(h, " " name "=\"");        \
		h_int(h, (long long)(v));     \
		HL(h, "\"");                  \
	} while (0)

typedef struct geo {
	size_t page, cols; /* columns drawn on this sheet */
	long w, h, top;    /* top: where the grid's label line starts */
	long rowy[ROWS];
	bool band[ROWS];   /* a band starts at this row: its column labels go above it */
	size_t rows;
} geo;

static long col_x(const geo *q, const node *nd)
{
	(void)q;
	return CHAN + (long)(nd->col % COLS) * (BW + CHAN);
}

static bool on_page(const geo *q, const node *nd)
{
	return nd->page == q->page;
}

/* Tag lines under a block: every tag, or CHIPS - 1 of them and a "+N more". */
static size_t chip_lines(const node *nd)
{
	return nd->nchips > CHIPS ? CHIPS : nd->nchips;
}

static const tix_plan_task *task_of(const graph *g, const node *nd)
{
	return &g->s->tasks[nd->task];
}

/* "/graph?show=ID": the sheet that has this task as a block. */
static void href_show(html *h, const tix_plan_task *t)
{
	HL(h, "/graph?show=");
	h_id(h, &t->ticket->id);
}

static void href_sheet(html *h, const graph *g, size_t page)
{
	HL(h, "/graph?");
	if (g->focus != NONE) {
		HL(h, "at=");
		h_id(h, &g->s->tasks[g->focus].ticket->id);
		HL(h, "&amp;");
	}
	HL(h, "page=");
	h_int(h, (long long)page + 1);
}

static void clip_open(html *h, long x, long y, long w, long ht)
{
	HL(h, "<svg");
	A(h, "x", x);
	A(h, "y", y);
	A(h, "width", w);
	A(h, "height", ht);
	HL(h, ">");
}

static void state_class(html *h, const tix_plan_task *t)
{
	HL(h, " s-");
	h_text(h, tix_ticket_status_name(t->ticket->u.ticket.status));
	if (t->critical && !finished(t))
		HL(h, " crit");
}

static void dates(html *h, const tix_plan_task *t)
{
	if (!t->scheduled) {
		HL(h, "unscheduled");
		return;
	}
	h_date(h, t->start);
	if (t->finish != t->start) {
		HL(h, " \xe2\x86\x92 ");
		h_date(h, t->finish);
	}
}

static void plural(html *h, size_t n, const char *one, const char *many)
{
	h_int(h, (long long)n);
	HL(h, " ");
	h_text(h, n == 1 ? one : many);
}

/* The tooltip (and accessible name) of a block. */
static void describe(html *h, const tix_plan_task *t, const node *nd)
{
	char st[TIX_STATE_STR];
	h_text(h, t->wbs);
	HL(h, " ");
	h_text(h, t->ticket->u.ticket.title);
	HL(h, ": ");
	h_text(h, tix_state_str(t->ticket->u.ticket.status, t->ticket->u.ticket.stage, st));
	HL(h, ", ");
	dates(h, t);
	if (t->critical && !finished(t))
		HL(h, ", critical");
	if (t->late)
		HL(h, ", late");
	if (t->summary) {
		HL(h, "; ");
		plural(h, nd->below, "ticket", "tickets");
		HL(h, " below, ");
		h_int(h, (long long)nd->done);
		HL(h, " finished");
		if (nd->inner) {
			HL(h, ", ");
			plural(h, nd->inner, "link", "links");
			HL(h, " inside");
		}
	}
}

static void progress_bar(html *h, long x, long y, int percent)
{
	HL(h, "<rect class=\"track\"");
	A(h, "x", x);
	A(h, "y", y);
	HL(h, " width=\"36\" height=\"4\" rx=\"2\"/>");
	if (percent > 0) {
		HL(h, "<rect class=\"done\"");
		A(h, "x", x);
		A(h, "y", y);
		A(h, "width", percent >= 100 ? 36 : percent * 36 / 100 + 1);
		HL(h, " height=\"4\" rx=\"2\"/>");
	}
}

/* A title on two lines of at most n code points: broken at the last space that leaves the
 * first line at least half full, the second line cut with an ellipsis. */
static void title_lines(html *h, const char *s, long x, long y)
{
	size_t i = 0, cps = 0, space = 0;
	while (s[i] && cps < TITLE_CPS) {
		if (s[i] == ' ' && cps >= TITLE_CPS / 2)
			space = i;
		i++;
		while (((unsigned char)s[i] & 0xc0) == 0x80)
			i++;
		cps++;
	}
	size_t cut = i, next = i;
	if (s[i] && s[i] != ' ' && space)
		cut = space, next = space + 1;
	while (s[next] == ' ')
		next++;
	HL(h, "<text class=\"title\"");
	A(h, "x", x);
	A(h, "y", y);
	HL(h, ">");
	h_textn(h, s, cut);
	HL(h, "</text>");
	if (s[next]) {
		HL(h, "<text class=\"title\"");
		A(h, "x", x);
		A(h, "y", y + 15);
		HL(h, ">");
		h_short_title(h, s + next, TITLE_CPS - 1);
		HL(h, "</text>");
	}
}

/* Template: connector tag. Incoming ones have a notch, outgoing ones a point. */
static void draw_chip(html *h, const graph *g, const chip *c, long x, long y, long w)
{
	const tix_plan_task *o = &g->s->tasks[c->other];
	HL(h, "<a class=\"chip");
	if (c->out)
		HL(h, " out");
	HL(h, "\" href=\"");
	href_show(h, o);
	HL(h, "\"><title>");
	plural(h, c->n, "link", "links");
	if (c->out)
		HL(h, " to ");
	else
		HL(h, " from ");
	h_text(h, o->wbs);
	HL(h, " ");
	h_text(h, o->ticket->u.ticket.title);
	if (c->sheet)
		HL(h, " (on another sheet of this group)");
	HL(h, "</title><path class=\"tag\" d=\"M");
	h_int(h, x);
	HL(h, ",");
	h_int(h, y);
	if (c->out) {
		HL(h, " h");
		h_int(h, w - 8);
		HL(h, " l8,8 l-8,8 h");
		h_int(h, -(w - 8));
		HL(h, " z\"/>");
	} else {
		HL(h, " h");
		h_int(h, w);
		HL(h, " v16 h");
		h_int(h, -w);
		HL(h, " l8,-8 z\"/>");
	}
	clip_open(h, x + (c->out ? 0 : 8), y, w - 9, 16);
	HL(h, "<text class=\"meta\" x=\"4\" y=\"12\">");
	if (c->out)
		HL(h, "to ");
	else
		HL(h, "from ");
	h_text(h, o->wbs);
	HL(h, " ");
	h_short_title(h, o->ticket->u.ticket.title, CHIP_CPS);
	HL(h, "</text></svg></a>");
}

/* The tags of nd in a column of width w whose first line starts at y. */
static void draw_chips(html *h, const graph *g, const node *nd, long x, long y, long w)
{
	size_t lines = chip_lines(nd), shown = nd->nchips > CHIPS ? CHIPS - 1 : nd->nchips;
	for (size_t k = 0; k < shown; k++)
		draw_chip(h, g, &g->chips[nd->chip0 + k], x, y + (long)k * CHIP_H, w);
	if (shown == lines)
		return;
	/* the rest: on the group's own sheet, or on the ticket's page */
	const tix_plan_task *t = task_of(g, nd);
	size_t more = 0;
	for (size_t k = shown; k < nd->nchips; k++)
		more += g->chips[nd->chip0 + k].n;
	HL(h, "<a class=\"chip more\" href=\"");
	if (t->summary && nd != &g->nodes[g->m])
		HL(h, "/graph?at=");
	else
		HL(h, "/t/");
	h_id(h, &t->ticket->id);
	HL(h, "\"><rect class=\"tag\" rx=\"3\" height=\"16\"");
	A(h, "x", x);
	A(h, "y", y + (long)shown * CHIP_H);
	A(h, "width", w);
	HL(h, "/><text class=\"meta\"");
	A(h, "x", x + 6);
	A(h, "y", y + (long)shown * CHIP_H + 12);
	HL(h, ">+");
	plural(h, more, "more link", "more links");
	HL(h, " elsewhere</text></a>");
}

/* Templates: ticket block and group card, at (x, y). */
static void draw_block(html *h, const graph *g, const node *nd, long x, long y, bool show)
{
	const tix_plan_task *t = task_of(g, nd);
	const tix_ticket *tk = &t->ticket->u.ticket;
	char st[TIX_STATE_STR];
	long w = t->summary ? BW - 6 : BW, ht = t->summary ? BH - 6 : BH;
	HL(h, "<g class=\"node");
	if (t->summary)
		HL(h, " group");
	state_class(h, t);
	if (show)
		HL(h, " show");
	HL(h, "\" id=\"n-");
	h_id(h, &t->ticket->id);
	HL(h, "\" data-n=\"");
	h_int(h, (long long)(nd - g->nodes));
	HL(h, "\"><a href=\"");
	if (t->summary)
		HL(h, "/graph?at=");
	else
		HL(h, "/t/");
	h_id(h, &t->ticket->id);
	HL(h, "\"><title>");
	describe(h, t, nd);
	HL(h, "</title>");
	if (t->summary) { /* a stack of cards: there is more behind this one */
		for (int k = 2; k >= 1; k--) {
			HL(h, "<rect class=\"stack\" rx=\"6\"");
			A(h, "x", x + 3 * k);
			A(h, "y", y + 3 * k);
			A(h, "width", w);
			A(h, "height", ht);
			HL(h, "/>");
		}
	}
	HL(h, "<rect class=\"box\" rx=\"6\"");
	A(h, "x", x);
	A(h, "y", y);
	A(h, "width", w);
	A(h, "height", ht);
	HL(h, "/>");
	clip_open(h, x, y, w, ht);
	HL(h, "<rect class=\"stripe\" x=\"1\" y=\"6\" width=\"4\"");
	A(h, "height", ht - 12);
	HL(h, "/><text class=\"meta\" x=\"12\" y=\"16\">");
	if (t->milestone)
		HL(h, "\xe2\x97\x86 ");
	h_text(h, t->wbs);
	HL(h, " \xc2\xb7 ");
	h_short(h, &t->ticket->id);
	HL(h, "</text><text class=\"state\" text-anchor=\"end\" y=\"16\"");
	A(h, "x", w - 8);
	HL(h, ">");
	h_text(h, tix_state_str(tk->status, tk->stage, st));
	HL(h, "</text>");
	title_lines(h, tk->title, 12, 33);
	HL(h, "<text class=\"meta\" x=\"12\"");
	A(h, "y", ht - 7);
	HL(h, ">");
	if (t->summary) {
		plural(h, nd->below, "ticket", "tickets");
		HL(h, " \xc2\xb7 ");
		h_int(h, (long long)nd->done);
		HL(h, " done \xe2\x96\xb8");
	} else {
		dates(h, t);
	}
	HL(h, "</text>");
	progress_bar(h, w - 44, ht - 12, t->progress);
	HL(h, "</svg></a>");
	if (nd->nchips) {
		long lines = (long)chip_lines(nd);
		HL(h, "<path class=\"spine\" d=\"M");
		h_int(h, x + 5);
		HL(h, ",");
		h_int(h, y + BH);
		HL(h, " V");
		h_int(h, y + BH + 4 + (lines - 1) * CHIP_H + 8);
		HL(h, "\"/>");
		draw_chips(h, g, nd, x + 10, y + BH + 4, BW - 10);
	}
	HL(h, "</g>\n");
}

/* Template: details block, the ticket this sheet is about. */
static void draw_details(html *h, const graph *g, const geo *q)
{
	const node *nd = &g->nodes[g->m];
	const tix_plan_task *t = task_of(g, nd);
	const tix_ticket *tk = &t->ticket->u.ticket;
	char st[TIX_STATE_STR];
	long w = q->w - 12, textw = nd->nchips ? w - BW - 16 : w;
	HL(h, "<g class=\"node details");
	state_class(h, t);
	HL(h, "\"><a href=\"/t/");
	h_id(h, &t->ticket->id);
	HL(h, "\"><title>");
	describe(h, t, nd);
	HL(h, "</title><rect class=\"box\" x=\"6\" y=\"6\" rx=\"6\"");
	A(h, "width", w);
	A(h, "height", HEAD_H);
	HL(h, "/>");
	clip_open(h, 6, 6, textw, HEAD_H);
	HL(h, "<rect class=\"stripe\" x=\"1\" y=\"6\" width=\"4\"");
	A(h, "height", HEAD_H - 12);
	HL(h, "/><text class=\"meta\" x=\"12\" y=\"18\">");
	if (t->milestone)
		HL(h, "\xe2\x97\x86 ");
	h_text(h, t->wbs);
	HL(h, " \xc2\xb7 ");
	h_short(h, &t->ticket->id);
	HL(h, " \xc2\xb7 <tspan class=\"state\">");
	h_text(h, tix_state_str(tk->status, tk->stage, st));
	HL(h, "</tspan></text><text class=\"big\" x=\"12\" y=\"40\">");
	h_text(h, tk->title);
	HL(h, "</text><text class=\"meta\" x=\"12\" y=\"60\">");
	if (g->m) {
		plural(h, g->m, "block", "blocks");
		HL(h, " on ");
		plural(h, g->npages, "sheet", "sheets");
		HL(h, " \xc2\xb7 ");
		plural(h, nd->below, "ticket", "tickets");
		HL(h, " below, ");
		h_int(h, (long long)nd->done);
		HL(h, " done \xc2\xb7 ");
	}
	dates(h, t);
	HL(h, "</text>");
	progress_bar(h, 12, 66, t->progress);
	HL(h, "</svg></a>");
	if (nd->nchips)
		draw_chips(h, g, nd, 6 + w - BW - 6, 6 + 9, BW);
	HL(h, "</g>\n");
}

static void measure(const graph *g, geo *q, size_t page)
{
	size_t lines[ROWS] = {0};
	memset(q, 0, sizeof(*q));
	q->page = page;
	for (size_t i = 0; i < g->m; i++) {
		const node *nd = &g->nodes[i];
		if (!on_page(q, nd))
			continue;
		if (nd->col % COLS + 1 > q->cols)
			q->cols = nd->col % COLS + 1;
		q->band[g->colrow[nd->col]] = true;
		if (nd->row + 1 > q->rows)
			q->rows = nd->row + 1;
		if (chip_lines(nd) > lines[nd->row])
			lines[nd->row] = chip_lines(nd);
	}
	if (q->cols < 2)
		q->cols = 2; /* room for the details block and the sheet connectors */
	q->w = CHAN + (long)q->cols * (BW + CHAN);
	q->top = g->focus != NONE ? 6 + HEAD_H : 6;
	long y = q->top;
	for (size_t r = 0; r < q->rows; r++) {
		y += GUT + (q->band[r] ? LAB_H : 0);
		q->rowy[r] = y;
		y += BH + (lines[r] ? 4 + (long)lines[r] * CHIP_H : 0);
	}
	q->h = (q->rows ? y + 16 : q->top + LAB_H + 24) + (g->npages > 1 ? FOOT_H : 0) + 6;
}

/* An arrow from the right of a to the left of b, in the channels and gutters only: out
 * into the channel after a's column, along the gutter above b's row, down the channel
 * before b's column. Lanes are offset by where the arrow starts, so arrows from different
 * blocks stay apart and arrows into one block join. The same path leads from the end of
 * one band to the start of the next, and back along a loop. */
static void draw_edge(html *h, const graph *g, const geo *q, const edge *e)
{
	const node *a = &g->nodes[e->a], *b = &g->nodes[e->b];
	const tix_plan_task *ta = task_of(g, a), *tb = task_of(g, b);
	long x1 = col_x(q, a) + BW, y1 = q->rowy[a->row] + BH / 2 + ((long)b->row - (long)a->row) * 5;
	long x2 = col_x(q, b), y2 = q->rowy[b->row] + BH / 2;
	long cx1 = x1 + 8 + (long)a->row * 6, cx2 = x2 - 16 + (long)b->row * 3;
	long gy = q->rowy[b->row] - GUT / 2 - 10 + (long)((a->col % COLS * ROWS + a->row) % 8) * 3;
	HL(h, "<path class=\"edge");
	if (e->n > 1)
		HL(h, " multi");
	if (e->back)
		HL(h, " back");
	HL(h, "\" marker-end=\"url(#garr)\" data-a=\"");
	h_int(h, (long long)e->a);
	HL(h, "\" data-b=\"");
	h_int(h, (long long)e->b);
	HL(h, "\" d=\"M");
	h_int(h, x1);
	HL(h, ",");
	h_int(h, y1);
	HL(h, " H");
	h_int(h, cx1);
	if (b->col != a->col + 1 || b->col % COLS == 0) {
		HL(h, " V");
		h_int(h, gy);
		HL(h, " H");
		h_int(h, cx2);
	}
	HL(h, " V");
	h_int(h, y2);
	HL(h, " H");
	h_int(h, x2);
	HL(h, "\"><title>");
	h_text(h, ta->wbs);
	HL(h, " ");
	h_text(h, ta->ticket->u.ticket.title);
	HL(h, " \xe2\x86\x92 ");
	h_text(h, tb->wbs);
	HL(h, " ");
	h_text(h, tb->ticket->u.ticket.title);
	HL(h, ": ");
	if (e->n > 1) {
		plural(h, e->n, "link", "links");
	} else {
		h_text(h, tix_dep_kind_name(e->kind));
		if (e->lag) {
			if (e->lag > 0)
				HL(h, "+");
			h_int(h, e->lag);
			HL(h, "d");
		}
	}
	if (e->back)
		HL(h, " (closes a loop between these blocks)");
	HL(h, "</title></path>\n");
}

/* Template: the connector to the previous or next sheet, in the frame's bottom band. */
static void draw_turn(html *h, const graph *g, const geo *q, bool next)
{
	long w = 96, x = next ? q->w - 16 - w : 16, y = q->h - 6 - FOOT_H + 4;
	HL(h, "<a class=\"chip turn\" href=\"");
	href_sheet(h, g, next ? q->page + 1 : q->page - 1);
	HL(h, "\"><path class=\"tag\" d=\"M");
	h_int(h, next ? x : x + 9);
	HL(h, ",");
	h_int(h, y);
	if (next)
		HL(h, " h87 l9,10 l-9,10 h-87 z\"/>");
	else
		HL(h, " h87 v20 h-87 l-9,-10 z\"/>");
	HL(h, "<text");
	A(h, "x", x + 14);
	A(h, "y", y + 14);
	HL(h, ">");
	if (next)
		HL(h, "sheet ");
	else
		HL(h, "\xe2\x97\x82 sheet ");
	h_int(h, (long long)(next ? q->page + 2 : q->page));
	if (next)
		HL(h, " \xe2\x96\xb8");
	HL(h, "</text></a>");
}

static void draw(web_ctx *x, const graph *g, size_t page, size_t show)
{
	html *h = &x->h;
	geo q;
	measure(g, &q, page);
	HL(h, "<div class=\"wide\"><svg class=\"dgraph\" xmlns=\"http://www.w3.org/2000/svg\" role=\"img\" "
	      "aria-label=\"Dependency graph\"");
	A(h, "width", q.w);
	A(h, "height", q.h);
	HL(h, " viewBox=\"0 0 ");
	h_int(h, q.w);
	HL(h, " ");
	h_int(h, q.h);
	HL(h, "\"><defs><marker id=\"garr\" viewBox=\"0 0 6 6\" refX=\"6\" refY=\"3\" markerWidth=\"7\" "
	      "markerHeight=\"7\" markerUnits=\"userSpaceOnUse\" orient=\"auto\"><path class=\"head\" d=\"M0,0 L6,3 L0,6 z\"/></marker></defs>\n"
	      "<rect class=\"frame\" x=\"6\" y=\"6\" rx=\"6\"");
	A(h, "width", q.w - 12);
	A(h, "height", q.h - 12);
	HL(h, "/>\n");
	if (g->focus != NONE)
		draw_details(h, g, &q);
	if (!g->m) {
		HL(h, "<text class=\"meta\" x=\"18\"");
		A(h, "y", q.top + LAB_H);
		HL(h, ">No sub-tickets.</text>\n");
	}
	/* column labels */
	bool said_free = false;
	for (size_t c = 0; c < g->ncols; c++) {
		if (g->colpage[c] != page || (g->colrank[c] == NONE && (said_free || c < g->free0)))
			continue;
		if (c && g->colpage[c - 1] == page && g->colrank[c] != NONE && g->colrank[c] == g->colrank[c - 1])
			continue; /* a step that takes several columns is named once */
		HL(h, "<text class=\"meta lab\"");
		A(h, "x", CHAN + (long)(c % COLS) * (BW + CHAN));
		A(h, "y", q.rowy[g->colrow[c]] - GUT - 4);
		HL(h, ">");
		if (g->colrank[c] == NONE) {
			said_free = true;
			if (g->ne)
				HL(h, "no order between these");
			else
				HL(h, "no order on this level");
		} else {
			HL(h, "step ");
			h_int(h, (long long)g->colrank[c] + 1);
		}
		HL(h, "</text>");
	}
	HL(h, "\n");
	for (size_t k = 0; k < g->ne; k++)
		if (on_page(&q, &g->nodes[g->edges[k].a]) && on_page(&q, &g->nodes[g->edges[k].b]))
			draw_edge(h, g, &q, &g->edges[k]);
	for (size_t i = 0; i < g->m; i++)
		if (on_page(&q, &g->nodes[i]))
			draw_block(h, g, &g->nodes[i], col_x(&q, &g->nodes[i]), q.rowy[g->nodes[i].row],
				   g->nodes[i].task == show);
	if (page > 0)
		draw_turn(h, g, &q, false);
	if (page + 1 < g->npages)
		draw_turn(h, g, &q, true);
	HL(h, "</svg></div>\n");
}

/* ---- the page around the drawing ---------------------------------------------------- */

static void crumbs(web_ctx *x, const graph *g)
{
	html *h = &x->h;
	const tix_schedule *s = g->s;
	size_t depth = 0;
	for (size_t i = g->focus; i != NONE; i = s->tasks[i].parent)
		depth++;
	size_t *path = tix_xcalloc(depth ? depth : 1, sizeof(size_t)), k = depth;
	for (size_t i = g->focus; i != NONE; i = s->tasks[i].parent)
		path[--k] = i;
	HL(h, "<nav class=\"crumbs\" aria-label=\"Path\">");
	if (g->focus == NONE)
		HL(h, "<b>All tickets</b>");
	else
		HL(h, "<a href=\"/graph\">All tickets</a>");
	for (k = 0; k < depth; k++) {
		const tix_plan_task *t = &s->tasks[path[k]];
		HL(h, " \xe2\x80\xba ");
		if (k + 1 == depth) {
			HL(h, "<b>");
		} else {
			HL(h, "<a href=\"/graph?at=");
			h_id(h, &t->ticket->id);
			HL(h, "\">");
		}
		h_text(h, t->wbs);
		HL(h, " ");
		h_short_title(h, t->ticket->u.ticket.title, 40);
		if (k + 1 == depth)
			HL(h, "</b>");
		else
			HL(h, "</a>");
	}
	HL(h, "</nav>\n");
	free(path);
}

static void pager(web_ctx *x, const graph *g, size_t page)
{
	html *h = &x->h;
	if (g->npages < 2)
		return;
	HL(h, "<p class=\"sheets\">Sheet ");
	h_int(h, (long long)page + 1);
	HL(h, " of ");
	h_int(h, (long long)g->npages);
	HL(h, ":");
	if (page > 0) {
		HL(h, " <a rel=\"prev\" href=\"");
		href_sheet(h, g, page - 1);
		HL(h, "\">previous</a>");
	}
	if (page + 1 < g->npages) {
		HL(h, " <a rel=\"next\" href=\"");
		href_sheet(h, g, page + 1);
		HL(h, "\">next</a>");
	}
	HL(h, "</p>\n");
}

/* One cell of the text table: the blocks an arrow or tag of node i leads to. */
static void list_links(html *h, const graph *g, size_t i, bool out)
{
	const node *nd = &g->nodes[i];
	bool any = false;
	for (size_t k = 0; k < g->ne; k++) {
		const edge *e = &g->edges[k];
		if ((out ? e->a : e->b) != i)
			continue;
		const node *o = &g->nodes[out ? e->b : e->a];
		if (o->page != nd->page)
			continue; /* on another sheet: a tag, below */
		const tix_plan_task *t = task_of(g, o);
		if (any)
			HL(h, ", ");
		any = true;
		HL(h, "<a href=\"#n-");
		h_id(h, &t->ticket->id);
		HL(h, "\">");
		h_text(h, t->wbs);
		HL(h, "</a>");
	}
	for (size_t k = 0; k < nd->nchips; k++) {
		const chip *c = &g->chips[nd->chip0 + k];
		if (c->out != out)
			continue;
		const tix_plan_task *t = &g->s->tasks[c->other];
		if (any)
			HL(h, ", ");
		any = true;
		HL(h, "<a href=\"");
		href_show(h, t);
		HL(h, "\">");
		h_text(h, t->wbs);
		HL(h, " ");
		h_short_title(h, t->ticket->u.ticket.title, CHIP_CPS);
		HL(h, "</a> (off sheet)");
	}
}

static void list(web_ctx *x, const graph *g, size_t page)
{
	html *h = &x->h;
	if (!g->m)
		return;
	HL(h, "<h2>This sheet as a list</h2>\n<table class=\"plan\"><thead><tr><th>WBS</th><th>ID</th><th>Title</th>"
	      "<th>Status</th><th>Below</th><th>After</th><th>Before</th></tr></thead><tbody>\n");
	for (size_t i = 0; i < g->m; i++) {
		const node *nd = &g->nodes[i];
		if (nd->page != page)
			continue;
		const tix_plan_task *t = task_of(g, nd);
		HL(h, "<tr><td>");
		h_text(h, t->wbs);
		HL(h, "</td><td class=\"id\"><a href=\"/t/");
		h_id(h, &t->ticket->id);
		HL(h, "\">");
		h_short(h, &t->ticket->id);
		HL(h, "</a></td><td>");
		if (t->summary) {
			HL(h, "<a href=\"/graph?at=");
			h_id(h, &t->ticket->id);
			HL(h, "\">");
		}
		h_text(h, t->ticket->u.ticket.title);
		if (t->summary)
			HL(h, "</a>");
		HL(h, "</td><td>");
		h_state(h, t->ticket->u.ticket.status, t->ticket->u.ticket.stage);
		HL(h, "</td><td>");
		if (t->summary) {
			h_int(h, (long long)nd->done);
			HL(h, " of ");
			h_int(h, (long long)nd->below);
			HL(h, " done");
		}
		HL(h, "</td><td>");
		list_links(h, g, i, false);
		HL(h, "</td><td>");
		list_links(h, g, i, true);
		HL(h, "</td></tr>\n");
	}
	HL(h, "</tbody></table>\n");
}

void page_graph(web_ctx *x)
{
	tix_oid at, show;
	bool have_at, have_show;
	tix_plan_result pr;
	if (!query_id(x, "at", TIX_ENTITY_TICKET, &at, &have_at) ||
	    !query_id(x, "show", TIX_ENTITY_TICKET, &show, &have_show))
		return;
	if (!page_plan(x, NULL, &pr))
		return;
	const tix_schedule *s = pr.schedule;
	size_t focus = NONE, shown = NONE;
	if (have_show) { /* the sheet that has this ticket as a block */
		shown = tix_schedule_find(s, &show);
		if (shown != NONE)
			focus = s->tasks[shown].parent;
	} else if (have_at) {
		focus = tix_schedule_find(s, &at);
	}
	if ((have_show && shown == NONE) || (!have_show && have_at && focus == NONE)) {
		tix_plan_result_free(&pr);
		page_error(x, 404, "Not found", "That ticket is not in the plan.");
		return;
	}
	graph g;
	build(&g, s, focus);
	size_t page = 0;
	const char *pv = web_kvs_get(&x->req->query, "page");
	if (shown != NONE) {
		page = g.nodes[g.nodeof[shown]].page;
	} else if (pv) {
		char *end;
		unsigned long v = strtoul(pv, &end, 10);
		if (*pv >= '0' && *pv <= '9' && !*end && v >= 1)
			page = v > g.npages ? g.npages - 1 : (size_t)v - 1;
	}

	page_begin(x, "Dependency graph");
	html *h = &x->h;
	crumbs(x, &g);
	if (!s->ntasks) {
		HL(h, "<p class=\"note\">No tickets yet.</p>\n");
	} else {
		HL(h, "<p class=\"note\">Arrows run from a block to what waits for it. A stacked card has sub-tickets: "
		      "select it to open its sheet. A tag under a block is a link that leaves this sheet: select it to "
		      "follow.</p>\n");
		pager(x, &g, page);
		draw(x, &g, page, shown);
		list(x, &g, page);
	}
	page_end(x);
	graph_free(&g);
	tix_plan_result_free(&pr);
}
