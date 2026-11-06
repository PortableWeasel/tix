/*
 * Server-drawn SVG Gantt (docs/architecture.md section 13.3): no JavaScript needed. One
 * column per day (per week beyond 120 days), bars link to their tickets, the critical path
 * and lateness are marked, dependencies are drawn as arrows. Styling is by class (the CSP
 * forbids style attributes); geometry uses SVG presentation attributes.
 */
#include "app.h"

#include <stdlib.h>
#include <string.h>

enum { LABEL_W = 320, ROW_H = 24, HEAD_H = 34, MAX_COLS = 260, LABEL_CHARS = 42 };

typedef struct geo {
	tix_date s0;
	int step, colw;
	int64_t cols;
} geo;

static long x_of(const geo *g, tix_date d)
{
	return LABEL_W + (long)((d - g->s0) / g->step) * g->colw;
}

/* Title cut to n code points (UTF-8 safe), with an ellipsis. */
static void short_title(html *h, const char *s, size_t n)
{
	size_t i = 0, cps = 0;
	while (s[i] && cps < n) {
		i++;
		while (((unsigned char)s[i] & 0xc0) == 0x80)
			i++;
		cps++;
	}
	h_textn(h, s, i);
	if (s[i])
		HL(h, "\xe2\x80\xa6");
}

static void num(html *h, long v)
{
	h_int(h, v);
}

void h_gantt(web_ctx *x, const tix_schedule *s, tix_date today, bool critical_only)
{
	html *h = &x->h;
	if (s->start == TIX_DATE_NONE) {
		HL(h, "<p class=\"note\">Nothing is scheduled yet.</p>\n");
		return;
	}
	geo g = {s->start, 1, 14, 0};
	int64_t span = (int64_t)s->finish - s->start + 1;
	if (span <= 45)
		g.colw = 24; /* short plans get room to read */
	if (span > 120) {
		g.step = 7;
		g.colw = 10;
	}
	g.cols = (span + g.step - 1) / g.step;
	if (g.cols > MAX_COLS)
		g.cols = MAX_COLS;
	tix_date last = (tix_date)(g.s0 + g.cols * g.step - 1);

	/* rows: the tasks shown, in WBS order */
	size_t *row = tix_xcalloc(s->ntasks ? s->ntasks : 1, sizeof(size_t));
	size_t nrows = 0;
	for (size_t i = 0; i < s->ntasks; i++) {
		const tix_plan_task *t = &s->tasks[i];
		row[i] = SIZE_MAX;
		if (critical_only && !t->critical)
			continue;
		row[i] = nrows++;
	}
	long width = LABEL_W + (long)g.cols * g.colw + 8, height = HEAD_H + (long)nrows * ROW_H + 4;

	HL(h, "<svg class=\"gantt\" xmlns=\"http://www.w3.org/2000/svg\" role=\"img\" aria-label=\"Gantt chart\" "
	      "width=\"");
	num(h, width);
	HL(h, "\" height=\"");
	num(h, height);
	HL(h, "\" viewBox=\"0 0 ");
	num(h, width);
	HL(h, " ");
	num(h, height);
	HL(h, "\"><defs><marker id=\"arr\" viewBox=\"0 0 6 6\" refX=\"6\" refY=\"3\" markerWidth=\"6\" "
	      "markerHeight=\"6\" orient=\"auto\"><path class=\"head\" d=\"M0,0 L6,3 L0,6 z\"/></marker></defs>\n");

	/* header: month names, then Monday (or week) ticks; weekends shaded in day view */
	for (int64_t c = 0; c < g.cols; c++) {
		tix_date d = (tix_date)(g.s0 + c * g.step);
		int y, m, day;
		tix_date_ymd(d, &y, &m, &day);
		long cx = LABEL_W + (long)c * g.colw;
		if (c == 0 || day <= g.step) {
			static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
							     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
			HL(h, "<text x=\"");
			num(h, cx + 2);
			HL(h, "\" y=\"12\">");
			h_text(h, months[m - 1]);
			if (c == 0 || m == 1) {
				HL(h, " ");
				h_int(h, y);
			}
			HL(h, "</text>");
		}
		if (g.step == 1 && tix_date_weekday(d) >= 5) {
			HL(h, "<rect class=\"off\" x=\"");
			num(h, cx);
			HL(h, "\" y=\"");
			num(h, HEAD_H - 14);
			HL(h, "\" width=\"");
			num(h, g.colw);
			HL(h, "\" height=\"");
			num(h, height - HEAD_H + 14);
			HL(h, "\"/>");
		}
		if (g.step == 7 || tix_date_weekday(d) == 0) {
			HL(h, "<line class=\"grid\" x1=\"");
			num(h, cx);
			HL(h, "\" x2=\"");
			num(h, cx);
			HL(h, "\" y1=\"");
			num(h, HEAD_H - 14);
			HL(h, "\" y2=\"");
			num(h, height);
			HL(h, "\"/><text class=\"day\" x=\"");
			num(h, cx + 2);
			HL(h, "\" y=\"");
			num(h, HEAD_H - 4);
			HL(h, "\">");
			h_int(h, day);
			HL(h, "</text>");
		}
	}
	HL(h, "\n");
	if (today >= g.s0 && today <= last) {
		long tx = x_of(&g, today) + (g.step == 1 ? g.colw / 2 : 0);
		HL(h, "<line class=\"today\" x1=\"");
		num(h, tx);
		HL(h, "\" x2=\"");
		num(h, tx);
		HL(h, "\" y1=\"");
		num(h, HEAD_H - 14);
		HL(h, "\" y2=\"");
		num(h, height);
		HL(h, "\"><title>today</title></line>\n");
	}

	/* dependency arrows first, so bars sit on top */
	for (size_t i = 0; i < s->ntasks; i++) {
		const tix_plan_task *t = &s->tasks[i];
		if (row[i] == SIZE_MAX || !t->scheduled)
			continue;
		const tix_ticket_plan *p = &t->ticket->u.ticket.plan;
		for (size_t k = 0; k < p->ndeps; k++) {
			size_t j = tix_schedule_find(s, &p->deps[k].pred);
			if (j == SIZE_MAX || row[j] == SIZE_MAX || !s->tasks[j].scheduled || s->tasks[j].start > last ||
			    t->start > last)
				continue;
			const tix_plan_task *pt = &s->tasks[j];
			bool from_end = p->deps[k].kind == TIX_DEP_FS || p->deps[k].kind == TIX_DEP_FF;
			bool to_end = p->deps[k].kind == TIX_DEP_FF || p->deps[k].kind == TIX_DEP_SF;
			long x1 = from_end ? x_of(&g, pt->finish > last ? last : pt->finish) + g.colw : x_of(&g, pt->start);
			long x2 = to_end ? x_of(&g, t->finish > last ? last : t->finish) + g.colw : x_of(&g, t->start);
			long y1 = HEAD_H + (long)row[j] * ROW_H + ROW_H / 2, y2 = HEAD_H + (long)row[i] * ROW_H + ROW_H / 2;
			HL(h, "<path class=\"arrow\" marker-end=\"url(#arr)\" d=\"M");
			num(h, x1);
			HL(h, ",");
			num(h, y1);
			HL(h, " h4 V");
			num(h, y2);
			HL(h, " H");
			num(h, x2);
			HL(h, "\"/>\n");
		}
	}

	for (size_t i = 0; i < s->ntasks; i++) {
		const tix_plan_task *t = &s->tasks[i];
		if (row[i] == SIZE_MAX)
			continue;
		long y = HEAD_H + (long)row[i] * ROW_H;
		HL(h, "<a href=\"/t/");
		h_id(h, &t->ticket->id);
		HL(h, "\"><title>");
		h_text(h, t->wbs);
		HL(h, " ");
		h_text(h, t->ticket->u.ticket.title);
		if (t->scheduled) {
			HL(h, ": ");
			h_date(h, t->start);
			HL(h, " to ");
			h_date(h, t->finish);
			if (t->critical)
				HL(h, ", critical");
			if (t->late)
				HL(h, ", late");
		} else {
			HL(h, ": unscheduled");
		}
		HL(h, "</title><text class=\"label\" x=\"");
		num(h, 4 + (long)(t->depth > 10 ? 10 : t->depth) * 12);
		HL(h, "\" y=\"");
		num(h, y + 15);
		HL(h, "\">");
		h_text(h, t->wbs);
		HL(h, " ");
		short_title(h, t->ticket->u.ticket.title, LABEL_CHARS - (t->depth > 10 ? 10 : t->depth) * 2);
		HL(h, "</text>");
		if (t->scheduled && t->start <= last) {
			long x0 = x_of(&g, t->start), x1 = x_of(&g, t->finish > last ? last : t->finish) + g.colw;
			if (t->milestone) {
				long cx = x0 + (g.step == 1 ? g.colw / 2 : 0), cy = y + ROW_H / 2;
				HL(h, "<path class=\"ms");
				if (t->critical)
					HL(h, " crit");
				HL(h, "\" d=\"M");
				num(h, cx);
				HL(h, ",");
				num(h, cy - 7);
				HL(h, " l7,7 l-7,7 l-7,-7 z\"/>");
			} else {
				HL(h, "<rect class=\"bar");
				if (t->summary)
					HL(h, " sum");
				else if (t->critical)
					HL(h, " crit");
				if (t->late)
					HL(h, " late");
				HL(h, "\" x=\"");
				num(h, x0);
				HL(h, "\" y=\"");
				num(h, y + (t->summary ? 8 : 4));
				HL(h, "\" width=\"");
				num(h, x1 - x0 < 3 ? 3 : x1 - x0);
				HL(h, "\" height=\"");
				num(h, t->summary ? 6 : ROW_H - 8);
				HL(h, "\" rx=\"2\"/>");
				if (!t->summary && t->progress > 0) {
					HL(h, "<rect class=\"done\" x=\"");
					num(h, x0);
					HL(h, "\" y=\"");
					num(h, y + ROW_H - 7);
					HL(h, "\" width=\"");
					num(h, (x1 - x0) * t->progress / 100);
					HL(h, "\" height=\"3\"/>");
				}
			}
		}
		HL(h, "</a>\n");
	}
	HL(h, "</svg>\n");
	if ((int64_t)s->finish - s->start + 1 > g.cols * g.step)
		HL(h, "<p class=\"note\">The chart is cut off at its right edge; plan from a smaller root to see the rest.</p>\n");
	free(row);
}
