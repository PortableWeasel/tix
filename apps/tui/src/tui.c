/*
 * tix-tui views (architecture §14.5): tickets, one ticket, WBS and a Gantt chart. A frontend
 * like the CLI and the web UI: every rule is core's or input's, every edit an op through core.
 */
#include "tui.h"

#include "editor.h"
#include "term.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef enum { V_LIST, V_DETAIL, V_WBS, V_GANTT, V_HELP } view;

typedef struct tui {
	const tui_config *cfg;
	view v, back; /* back: where Esc/q returns to from a ticket or help */
	bool quit;
	/* tickets */
	tix_entity_list list;
	size_t sel, top;
	char filter[256];
	bool all;
	/* one ticket */
	tix_entity *cur;
	TIX_VEC(char *) lines;
	TIX_VEC(tattr) line_attr;
	size_t dtop;
	/* plan */
	tix_plan_result plan;
	bool have_plan;
	size_t psel, ptop;
	int gscroll;
	/* status line */
	char msg[400];
	bool msg_err;
	char stamp[TIX_STAMP_LEN];
	tix_error err;
} tui;

static void say(tui *t, bool err, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void say(tui *t, bool err, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(t->msg, sizeof(t->msg), fmt, ap);
	va_end(ap);
	t->msg_err = err;
}

static const char *short_id(const tix_oid *id, char *out)
{
	char hex[TIX_OID_MAX_HEX + 1];
	snprintf(out, 8, "%.7s", tix_oid_hex(id, hex));
	return out;
}

/* ---- loading -------------------------------------------------------------------------- */

static void load_list(tui *t)
{
	tix_entity_list_free(&t->list);
	tix_ticket_query q = {.text = t->filter, .sort = TIX_SORT_STATUS,
			      .status_mask = t->all ? 0 : TIX_STATUS_MASK_ACTIVE};
	if (tix_ticket_list(t->cfg->repo, &q, &t->list, &t->err)) {
		say(t, true, "%s", t->err.msg);
		memset(&t->list, 0, sizeof(t->list));
	}
	if (t->sel >= t->list.n)
		t->sel = t->list.n ? t->list.n - 1 : 0;
}

static void load_plan(tui *t)
{
	if (t->have_plan)
		tix_plan_result_free(&t->plan);
	t->have_plan = !tix_plan(t->cfg->repo, NULL, t->cfg->today(), &t->plan, &t->err);
	if (!t->have_plan)
		say(t, true, "%s", t->err.msg);
	else if (t->psel >= t->plan.schedule->ntasks)
		t->psel = t->plan.schedule->ntasks ? t->plan.schedule->ntasks - 1 : 0;
}

static void clear_lines(tui *t)
{
	for (size_t i = 0; i < t->lines.n; i++)
		free(t->lines.v[i]);
	t->lines.n = 0;
	t->line_attr.n = 0;
}

static void add_line(tui *t, tattr a, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void add_line(tui *t, tattr a, const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	TIX_VEC_PUSH(&t->lines, tix_xstrdup(buf));
	TIX_VEC_PUSH(&t->line_attr, a);
}

/* Text, wrapped to width cells (on character boundaries), one line per row. */
static void add_text(tui *t, tattr a, const char *text, int width, const char *indent)
{
	const char *p = text ? text : "";
	int iw = (int)strlen(indent);
	do {
		const char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);
		char *para = tix_xstrndup(p, len);
		const char *q = para;
		do {
			/* take characters while they fit */
			size_t take = 0;
			int cells = 0;
			while (q[take]) {
				size_t n = 1;
				while (q[take + n] && ((unsigned char)q[take + n] & 0xc0) == 0x80)
					n++;
				char one[8] = {0};
				memcpy(one, q + take, n < 7 ? n : 7);
				int w = term_width(one);
				if (cells + w > width - iw && take)
					break;
				cells += w;
				take += n;
			}
			char *row = tix_xstrndup(q, take);
			add_line(t, a, "%s%s", indent, row);
			free(row);
			q += take;
		} while (*q);
		free(para);
		p = nl ? nl + 1 : NULL;
	} while (p);
}

static const char *who(tui *t, const tix_oid *id, char *buf, size_t n)
{
	tix_entity *e;
	if (!tix_entity_load(t->cfg->repo, TIX_ENTITY_IDENTITY, id, &e, NULL)) {
		snprintf(buf, n, "%s%s", e->u.identity.name, e->u.identity.kind == TIX_ID_AGENT ? " (agent)" : "");
		tix_entity_free(e);
	} else {
		short_id(id, buf);
	}
	return buf;
}

static void date_or(char *out, tix_date d, const char *none)
{
	if (d == TIX_DATE_NONE)
		snprintf(out, TIX_DATE_STR, "%s", none);
	else
		tix_date_str(d, out);
}

static void build_detail(tui *t, int width)
{
	clear_lines(t);
	if (!t->cur)
		return;
	const tix_entity *e = t->cur;
	const tix_ticket *k = &e->u.ticket;
	const tix_ticket_plan *p = &k->plan;
	char hex[TIX_OID_MAX_HEX + 1], buf[300], d1[TIX_DATE_STR], d2[TIX_DATE_STR];
	add_text(t, T_BOLD, k->title, width, "");
	add_line(t, T_NORMAL, "id:       %s", tix_oid_hex(&e->id, hex));
	char st[TIX_STATE_STR];
	add_line(t, T_NORMAL, "status:   %s", tix_state_str(k->status, k->stage, st));
	if (k->labels.n) {
		tix_buf l = TIX_BUF_INIT;
		for (size_t i = 0; i < k->labels.n; i++)
			tix_buf_addf(&l, "%s%s", i ? ", " : "", k->labels.v[i]);
		add_line(t, T_NORMAL, "labels:   %s", tix_buf_cstr(&l));
		tix_buf_free(&l);
	}
	add_line(t, T_NORMAL, "author:   %s", who(t, &k->author, buf, sizeof(buf)));
	if (!tix_oid_is_null(&p->parent)) {
		tix_entity *par;
		char s[8];
		if (!tix_entity_load(t->cfg->repo, TIX_ENTITY_TICKET, &p->parent, &par, NULL)) {
			add_line(t, T_NORMAL, "parent:   %s %s", short_id(&p->parent, s), par->u.ticket.title);
			tix_entity_free(par);
		} else {
			add_line(t, T_NORMAL, "parent:   %s", short_id(&p->parent, s));
		}
	}
	if (p->external)
		add_line(t, T_NORMAL, "issue:    %s", p->external);
	if (p->start != TIX_DATE_NONE || p->finish != TIX_DATE_NONE) {
		date_or(d1, p->start, "-");
		date_or(d2, p->finish, "-");
		add_line(t, T_NORMAL, "dates:    %s .. %s%s", d1, d2, p->mode == TIX_MODE_MANUAL ? " (manual)" : "");
	}
	if (p->duration >= 0)
		add_line(t, T_NORMAL, "duration: %lldd", (long long)p->duration);
	if (p->effort >= 0)
		add_line(t, T_NORMAL, "effort:   %lldh", (long long)p->effort);
	if (p->deadline != TIX_DATE_NONE) {
		date_or(d1, p->deadline, "-");
		add_line(t, T_NORMAL, "deadline: %s", d1);
	}
	if (p->progress)
		add_line(t, T_NORMAL, "progress: %d%%", p->progress);
	for (size_t i = 0; i < p->ndeps; i++) {
		char s[8];
		add_line(t, T_NORMAL, "after:    %s (%s%+dd)", short_id(&p->deps[i].pred, s),
			 tix_dep_kind_name(p->deps[i].kind), (int)p->deps[i].lag);
	}
	add_line(t, T_NORMAL, "%s", "");
	if (k->body && *k->body) {
		add_text(t, T_NORMAL, k->body, width, "  ");
		add_line(t, T_NORMAL, "%s", "");
	}
	for (size_t i = 0; i < k->ncomments; i++) {
		const tix_comment *c = &k->comments[i];
		char when[32];
		time_t tt = (time_t)c->time;
		struct tm tm;
		if (!gmtime_r(&tt, &tm) || !strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm))
			snprintf(when, sizeof(when), "?");
		add_line(t, T_DIM, "--- %s  %s%s", who(t, &c->author, buf, sizeof(buf)), when, c->edited ? " (edited)" : "");
		add_text(t, T_NORMAL, c->body, width, "  ");
	}
	if (!k->ncomments)
		add_line(t, T_DIM, "%s", "(no comments; c to add one)");
}

static void open_ticket(tui *t, const tix_oid *id)
{
	tix_entity *e;
	if (tix_entity_load(t->cfg->repo, TIX_ENTITY_TICKET, id, &e, &t->err)) {
		say(t, true, "%s", t->err.msg);
		return;
	}
	tix_entity_free(t->cur);
	t->cur = e;
	if (t->v != V_DETAIL)
		t->back = t->v;
	t->v = V_DETAIL;
	t->dtop = 0;
}

static void reload_all(tui *t)
{
	load_list(t);
	if (t->have_plan || t->v == V_WBS || t->v == V_GANTT)
		load_plan(t);
	if (t->cur) {
		tix_oid id = t->cur->id;
		tix_entity *e;
		if (!tix_entity_load(t->cfg->repo, TIX_ENTITY_TICKET, &id, &e, NULL)) {
			tix_entity_free(t->cur);
			t->cur = e;
		}
	}
	tix_repo_stamp(t->cfg->repo, t->stamp, NULL);
}

/* ---- drawing ----------------------------------------------------------------------------- */

static void draw_bar(tui *t, int cols)
{
	term_fill(0, 0, cols, T_BAR, ' ');
	static const char *const names[] = {"1 Tickets", "2 WBS", "3 Gantt"};
	int x = term_text(0, 0, cols, T_BAR, " tix ") + 1;
	for (int i = 0; i < 3; i++) {
		bool on = (t->v == (view)(i == 0 ? V_LIST : i == 1 ? V_WBS : V_GANTT)) ||
			  (t->v == V_DETAIL && t->back == (view)(i == 0 ? V_LIST : i == 1 ? V_WBS : V_GANTT));
		x += term_text(0, x, cols - x, on ? T_SELECT : T_BAR, names[i]) + 2;
	}
	if (t->filter[0]) {
		char f[300];
		snprintf(f, sizeof(f), " filter: %s ", t->filter);
		term_text(0, x, cols - x, T_BAR, f);
	}
}

static void draw_status(tui *t, int rows, int cols)
{
	static const char *const hints[] = {
		[V_LIST] = "Enter open  / filter  a all  n new  s status  r refresh  ? help  q quit",
		[V_DETAIL] = "s status  e title  l labels  c comment  C/E editor  n subtask  p parent  Esc back",
		[V_WBS] = "Enter open  j/k move  2 WBS  3 Gantt  ? help  q quit",
		[V_GANTT] = "Enter open  h/l scroll  j/k move  ? help  q quit",
		[V_HELP] = "any key: back",
	};
	term_fill(rows - 1, 0, cols, T_NORMAL, ' ');
	if (t->msg[0])
		term_text(rows - 1, 0, cols, t->msg_err ? T_ERR : T_OK, t->msg);
	else
		term_text(rows - 1, 0, cols, T_DIM, hints[t->v]);
}

static void keep_visible(size_t sel, size_t *top, size_t height)
{
	if (sel < *top)
		*top = sel;
	if (height && sel >= *top + height)
		*top = sel - height + 1;
}

static void draw_list(tui *t, int rows, int cols)
{
	size_t h = rows > 3 ? (size_t)rows - 3 : 1;
	keep_visible(t->sel, &t->top, h);
	term_text(1, 0, cols, T_DIM, "ID       STATUS       TITLE");
	if (!t->list.n)
		term_text(3, 2, cols - 2, T_DIM, t->filter[0] ? "nothing matches the filter" : "no tickets (n to create one)");
	for (size_t i = t->top; i < t->list.n && i < t->top + h; i++) {
		int row = 2 + (int)(i - t->top);
		const tix_ticket *k = &t->list.v[i]->u.ticket;
		tattr a = i == t->sel ? T_SELECT : T_NORMAL;
		char s[8], line[96], st[TIX_STATE_STR];
		if (i == t->sel)
			term_fill(row, 0, cols, T_SELECT, ' ');
		snprintf(line, sizeof(line), "%s  %-11s  ", short_id(&t->list.v[i]->id, s), tix_state_str(k->status, k->stage, st));
		int x = term_text(row, 0, cols, a, line);
		x += term_text(row, x, cols - x, i == t->sel ? T_SELECT : T_BOLD, k->title);
		if (k->labels.n) {
			tix_buf l = TIX_BUF_INIT;
			tix_buf_adds(&l, "  [");
			for (size_t j = 0; j < k->labels.n; j++)
				tix_buf_addf(&l, "%s%s", j ? ", " : "", k->labels.v[j]);
			tix_buf_adds(&l, "]");
			term_text(row, x, cols - x, i == t->sel ? T_SELECT : T_DIM, tix_buf_cstr(&l));
			tix_buf_free(&l);
		}
	}
}

static void draw_detail(tui *t, int rows, int cols)
{
	build_detail(t, cols - 1);
	size_t h = rows > 2 ? (size_t)rows - 2 : 1;
	if (t->dtop + h > t->lines.n)
		t->dtop = t->lines.n > h ? t->lines.n - h : 0;
	for (size_t i = t->dtop; i < t->lines.n && i < t->dtop + h; i++)
		term_text(1 + (int)(i - t->dtop), 0, cols, t->line_attr.v[i], t->lines.v[i]);
}

static void draw_wbs(tui *t, int rows, int cols)
{
	if (!t->have_plan)
		return;
	const tix_schedule *s = t->plan.schedule;
	size_t h = rows > 3 ? (size_t)rows - 3 : 1;
	keep_visible(t->psel, &t->ptop, h);
	term_text(1, 0, cols, T_DIM, "WBS        ID       TITLE                                    STATUS       DATES");
	for (size_t i = t->ptop; i < s->ntasks && i < t->ptop + h; i++) {
		const tix_plan_task *k = &s->tasks[i];
		int row = 2 + (int)(i - t->ptop);
		bool on = i == t->psel;
		if (on)
			term_fill(row, 0, cols, T_SELECT, ' ');
		char line[256], sid[8], d1[TIX_DATE_STR], d2[TIX_DATE_STR];
		snprintf(line, sizeof(line), "%-10s %s  %*s", k->wbs, short_id(&k->ticket->id, sid),
			 (int)(k->depth > 10 ? 20 : k->depth * 2), "");
		int x = term_text(row, 0, cols, on ? T_SELECT : T_NORMAL, line);
		term_text(row, x, 60 - x > 0 ? 60 - x : 1, on ? T_SELECT : k->summary ? T_SUM : T_NORMAL,
			  k->ticket->u.ticket.title);
		if (cols > 62) {
			date_or(d1, k->start, "-");
			date_or(d2, k->finish, "-");
			snprintf(line, sizeof(line), "%-11s  %s..%s%s", tix_ticket_status_name(k->ticket->u.ticket.status), d1, d2,
				 k->critical ? "  critical" : "");
			term_text(row, 61, cols - 61, on ? T_SELECT : k->critical ? T_CRIT : T_NORMAL, line);
		}
	}
}

static void draw_gantt(tui *t, int rows, int cols)
{
	if (!t->have_plan)
		return;
	const tix_schedule *s = t->plan.schedule;
	if (s->start == TIX_DATE_NONE) {
		term_text(2, 2, cols - 2, T_DIM, "nothing is scheduled");
		return;
	}
	const int left = 36;
	int days = cols - left - 1;
	if (days < 5)
		return;
	if (t->gscroll < 0)
		t->gscroll = 0;
	tix_date d0 = (tix_date)(s->start + t->gscroll), today = t->cfg->today();
	char d[TIX_DATE_STR];
	term_text(1, 0, left, T_DIM, tix_date_str(d0, d));
	for (int c = 0; c < days; c++) { /* Mondays */
		if (tix_date_weekday((tix_date)(d0 + c)) == 0)
			term_text(1, left + c, 1, T_DIM, "|");
	}
	size_t h = rows > 3 ? (size_t)rows - 3 : 1;
	keep_visible(t->psel, &t->ptop, h);
	for (size_t i = t->ptop; i < s->ntasks && i < t->ptop + h; i++) {
		const tix_plan_task *k = &s->tasks[i];
		int row = 2 + (int)(i - t->ptop);
		bool on = i == t->psel;
		char line[256];
		snprintf(line, sizeof(line), "%-8s %*s", k->wbs, (int)(k->depth > 6 ? 12 : k->depth * 2), "");
		int x = term_text(row, 0, left, on ? T_SELECT : T_NORMAL, line);
		term_text(row, x, left - x - 1, on ? T_SELECT : k->summary ? T_SUM : T_NORMAL, k->ticket->u.ticket.title);
		for (int c = 0; c < days; c++) {
			tix_date day = (tix_date)(d0 + c);
			const char *ch = day == today ? ":" : " ";
			tattr a = T_DIM;
			if (k->scheduled && day >= k->start && day <= k->finish) {
				if (k->milestone)
					ch = "*", a = T_BOLD;
				else if (k->summary)
					ch = "-", a = T_SUM;
				else if (!tix_plan_is_working(k->calendar, day))
					ch = ".", a = T_DIM;
				else
					ch = k->critical ? "#" : "=", a = k->critical ? T_CRIT : T_OK;
			}
			term_text(row, left + c, 1, on && *ch == ' ' ? T_SELECT : a, ch);
		}
	}
}

static void draw_help(int rows, int cols)
{
	static const char *const text[] = {
		"tix tui: tickets, WBS and Gantt in the terminal. Edits are saved at once, like the CLI.",
		"",
		"Anywhere:   1 tickets   2 WBS   3 Gantt   ? this help   q quit (back, from a ticket)",
		"Moving:     j/k or arrows, PgUp/PgDn, Home/End; Enter opens a ticket, Esc goes back",
		"Tickets:    / filter by id or title (live)   a active/all   n new ticket   s status",
		"A ticket:   s status   e title   l labels (+add -remove)   c comment   C comment in $EDITOR",
		"            E body in $EDITOR   n new subtask   p parent (id or none)",
		"Gantt:      h/l or left/right scroll by a week   # critical  = task  - summary  * milestone",
		"",
		"Changes made elsewhere (the CLI, the web UI, a sync) show up within a couple of seconds.",
	};
	for (size_t i = 0; i < sizeof(text) / sizeof(*text) && (int)i + 2 < rows; i++)
		term_text(2 + (int)i, 2, cols - 2, i == 0 ? T_BOLD : T_NORMAL, text[i]);
}

static void draw(tui *t)
{
	int rows, cols;
	term_size(&rows, &cols);
	term_clear();
	draw_bar(t, cols);
	switch (t->v) {
	case V_LIST: draw_list(t, rows, cols); break;
	case V_DETAIL: draw_detail(t, rows, cols); break;
	case V_WBS: draw_wbs(t, rows, cols); break;
	case V_GANTT: draw_gantt(t, rows, cols); break;
	case V_HELP: draw_help(rows, cols); break;
	}
	draw_status(t, rows, cols);
	term_flush();
}

/* ---- input ---------------------------------------------------------------------------- */

static void utf8_put(tix_buf *b, int cp)
{
	char s[5] = {0};
	if (cp < 0x80) {
		s[0] = (char)cp;
	} else if (cp < 0x800) {
		s[0] = (char)(0xc0 | (cp >> 6));
		s[1] = (char)(0x80 | (cp & 0x3f));
	} else if (cp < 0x10000) {
		s[0] = (char)(0xe0 | (cp >> 12));
		s[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
		s[2] = (char)(0x80 | (cp & 0x3f));
	} else {
		s[0] = (char)(0xf0 | (cp >> 18));
		s[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
		s[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
		s[3] = (char)(0x80 | (cp & 0x3f));
	}
	tix_buf_adds(b, s);
}

/* A one-line prompt on the status row. on_change (optional) runs after every edit (the live
 * filter). RETURNS false on Esc. */
static bool prompt(tui *t, const char *label, const char *initial, tix_buf *out, void (*on_change)(tui *, const char *))
{
	tix_buf_reset(out);
	tix_buf_adds(out, initial ? initial : "");
	for (;;) {
		if (on_change)
			on_change(t, tix_buf_cstr(out));
		t->msg[0] = '\0';
		draw(t);
		int rows, cols;
		term_size(&rows, &cols);
		term_fill(rows - 1, 0, cols, T_NORMAL, ' ');
		int x = term_text(rows - 1, 0, cols, T_BOLD, label);
		x += term_text(rows - 1, x, 2, T_BOLD, ": ");
		int w = term_width(tix_buf_cstr(out));
		const char *shown = tix_buf_cstr(out);
		while (w > cols - x - 1 && *shown) { /* scroll long input */
			shown++;
			while (((unsigned char)*shown & 0xc0) == 0x80)
				shown++;
			w = term_width(shown);
		}
		x += term_text(rows - 1, x, cols - x, T_NORMAL, shown);
		term_cursor(rows - 1, x < cols ? x : cols - 1, true);
		term_flush();
		int k = term_key(-1);
		term_cursor(0, 0, false);
		if (k == K_ESC)
			return false;
		if (k == K_ENTER)
			return true;
		if (k == K_BACKSPACE) {
			while (out->len && ((unsigned char)out->data[out->len - 1] & 0xc0) == 0x80)
				out->len--;
			if (out->len)
				out->len--;
			out->data[out->len] = '\0';
		} else if (k >= 0x20 && k < 0x110000 && k != 0x7f && out->len < 4000) {
			utf8_put(out, k);
		}
	}
}

static void live_filter(tui *t, const char *text)
{
	snprintf(t->filter, sizeof(t->filter), "%s", text);
	t->sel = t->top = 0;
	load_list(t);
}

/* The ticket the current view points at. */
static const tix_oid *target(tui *t)
{
	if (t->v == V_DETAIL && t->cur)
		return &t->cur->id;
	if (t->v == V_LIST && t->sel < t->list.n)
		return &t->list.v[t->sel]->id;
	if ((t->v == V_WBS || t->v == V_GANTT) && t->have_plan && t->psel < t->plan.schedule->ntasks)
		return &t->plan.schedule->tasks[t->psel].ticket->id;
	return NULL;
}

static void apply(tui *t, const tix_oid *id, tix_op *ops, size_t n, const char *done)
{
	tix_status st = tix_entity_apply(t->cfg->repo, TIX_ENTITY_TICKET, id, ops, n, &t->err);
	for (size_t i = 0; i < n; i++)
		tix_op_free(&ops[i]);
	if (st) {
		say(t, true, "not saved: %s", t->err.msg);
		return;
	}
	say(t, false, "%s", done);
	reload_all(t);
}

static void set_status(tui *t)
{
	const tix_oid *id = target(t);
	if (!id)
		return;
	tix_oid copy = *id;
	say(t, false, "status: o open  i in-progress  b blocked  d done  c cancelled  (Esc: keep)");
	draw(t);
	int k = term_key(-1);
	static const struct { int key; tix_ticket_status s; } m[] = {
		{'o', TIX_TICKET_OPEN}, {'i', TIX_TICKET_IN_PROGRESS}, {'b', TIX_TICKET_BLOCKED},
		{'d', TIX_TICKET_DONE}, {'c', TIX_TICKET_CANCELLED}};
	for (size_t i = 0; i < sizeof(m) / sizeof(*m); i++)
		if (m[i].key == k) {
			tix_op op = tix_op_set_status(m[i].s);
			char done[64];
			snprintf(done, sizeof(done), "status: %s", tix_ticket_status_name(m[i].s));
			apply(t, &copy, &op, 1, done);
			return;
		}
	t->msg[0] = '\0';
}

static void edit_title(tui *t)
{
	if (!t->cur)
		return;
	tix_buf b = TIX_BUF_INIT;
	if (prompt(t, "title", t->cur->u.ticket.title, &b, NULL) && b.len && strcmp(b.data, t->cur->u.ticket.title)) {
		tix_op op = tix_op_text(TIX_OP_SET_TITLE, b.data);
		apply(t, &t->cur->id, &op, 1, "title saved");
	}
	tix_buf_free(&b);
}

static void edit_labels(tui *t)
{
	if (!t->cur)
		return;
	tix_buf b = TIX_BUF_INIT;
	if (prompt(t, "labels (+add -remove)", "", &b, NULL) && b.len) {
		tix_strs add = {0}, rm = {0};
		char *s = tix_xstrdup(b.data), *save = NULL;
		bool bad = false;
		for (char *w = strtok_r(s, " ,", &save); w; w = strtok_r(NULL, " ,", &save)) {
			if (w[0] == '-' && w[1])
				tix_strs_add(&rm, w + 1);
			else if (w[0] == '+' && w[1])
				tix_strs_add(&add, w + 1);
			else if (w[0] != '-' && w[0] != '+')
				tix_strs_add(&add, w);
			else
				bad = true;
		}
		free(s);
		if (bad || (!add.n && !rm.n)) {
			say(t, true, "labels: like +bug -wontfix");
		} else {
			tix_op op = tix_op_label(&add, &rm);
			apply(t, &t->cur->id, &op, 1, "labels saved");
		}
		tix_strs_free(&add);
		tix_strs_free(&rm);
	}
	tix_buf_free(&b);
}

static bool editor(tui *t, const char *template_text, tix_buf *out)
{
	char why[256];
	term_suspend();
	int rc = tix_edit_text(t->cfg->editor, template_text, out, why, sizeof(why));
	term_resume();
	if (rc)
		say(t, true, "%s", why);
	return !rc;
}

static void trim_trailing(tix_buf *b)
{
	while (b->len && (b->data[b->len - 1] == '\n' || b->data[b->len - 1] == ' '))
		b->data[--b->len] = '\0';
}

static void add_comment(tui *t, bool use_editor)
{
	if (!t->cur)
		return;
	tix_buf b = TIX_BUF_INIT;
	bool ok = use_editor ? editor(t, "\n# Your comment. Lines starting with '#' are ignored; empty cancels.\n", &b)
			     : prompt(t, "comment", "", &b, NULL);
	trim_trailing(&b);
	if (ok && b.len) {
		tix_op op = tix_op_text(TIX_OP_COMMENT, b.data);
		apply(t, &t->cur->id, &op, 1, "comment added");
	}
	tix_buf_free(&b);
}

static void edit_body(tui *t)
{
	if (!t->cur)
		return;
	tix_buf tmpl = TIX_BUF_INIT, b = TIX_BUF_INIT;
	tix_buf_adds(&tmpl, t->cur->u.ticket.body ? t->cur->u.ticket.body : "");
	tix_buf_adds(&tmpl, "\n# The description. Lines starting with '#' are ignored.\n");
	if (editor(t, tix_buf_cstr(&tmpl), &b)) {
		trim_trailing(&b);
		if (strcmp(tix_buf_cstr(&b), t->cur->u.ticket.body ? t->cur->u.ticket.body : "")) {
			tix_op op = tix_op_text(TIX_OP_SET_BODY, tix_buf_cstr(&b));
			apply(t, &t->cur->id, &op, 1, "description saved");
		}
	}
	tix_buf_free(&tmpl);
	tix_buf_free(&b);
}

/* A planning field through tix/input.h, exactly as `tix edit --FIELD TEXT`. */
static void input_field(tui *t, const tix_oid *id, const tix_oid *cur_parent, const char *field, const char *text,
			const char *done)
{
	tix_input_ctx ctx = {t->cfg->repo, t->cfg->today()};
	tix_input in = {0};
	tix_status st = tix_input_field(&ctx, TIX_ENTITY_TICKET, field, text, &in, &t->err);
	if (!st)
		st = tix_input_finish(&ctx, &in, id, cur_parent, &t->err);
	if (!st)
		st = tix_entity_apply(t->cfg->repo, TIX_ENTITY_TICKET, id, in.ops.v, in.ops.n, &t->err);
	tix_input_free(&in);
	if (st)
		say(t, true, "not saved: %s", t->err.msg);
	else {
		say(t, false, "%s", done);
		reload_all(t);
	}
}

static void edit_parent(tui *t)
{
	if (!t->cur)
		return;
	tix_buf b = TIX_BUF_INIT;
	if (prompt(t, "parent (ticket id, or none)", "", &b, NULL) && b.len) {
		tix_oid id = t->cur->id, par = t->cur->u.ticket.plan.parent;
		input_field(t, &id, &par, "parent", b.data, "parent saved");
	}
	tix_buf_free(&b);
}

static void new_ticket(tui *t, bool sub)
{
	tix_buf b = TIX_BUF_INIT;
	if (prompt(t, sub ? "new subtask title" : "new ticket title", "", &b, NULL) && b.len) {
		tix_oid id, parent = sub && t->cur ? t->cur->id : (tix_oid){0};
		if (tix_ticket_create(t->cfg->repo, b.data, "", NULL, &id, &t->err)) {
			say(t, true, "not saved: %s", t->err.msg);
		} else {
			char s[8], hex[TIX_OID_MAX_HEX + 1];
			tix_oid none = {0};
			if (sub) /* the parent goes through input like `tix edit --parent` (rank: last) */
				input_field(t, &id, &none, "parent", tix_oid_hex(&parent, hex), "subtask created");
			if (!t->msg_err)
				say(t, false, "created %s", short_id(&id, s));
			reload_all(t);
			open_ticket(t, &id);
		}
	}
	tix_buf_free(&b);
}

/* ---- the loop -------------------------------------------------------------------------- */

static void move_sel(size_t *sel, size_t n, long by)
{
	if (!n) {
		*sel = 0;
		return;
	}
	long v = (long)*sel + by;
	*sel = v < 0 ? 0 : v >= (long)n ? n - 1 : (size_t)v;
}

static void go(tui *t, view v)
{
	if ((v == V_WBS || v == V_GANTT) && !t->have_plan)
		load_plan(t);
	t->v = v;
}

static void key(tui *t, int k)
{
	int rows, cols;
	term_size(&rows, &cols);
	long page = rows > 4 ? rows - 4 : 1;
	if (t->v == V_HELP) {
		t->v = t->back;
		return;
	}
	if (k == '?') {
		t->back = t->v == V_DETAIL ? t->back : t->v;
		t->v = V_HELP;
		return;
	}
	if (k == '1' || k == 't') {
		t->v = V_LIST;
		return;
	}
	if (k == '2' || k == 'w') {
		go(t, V_WBS);
		return;
	}
	if (k == '3' || k == 'g') {
		go(t, V_GANTT);
		return;
	}
	if (k == 'r') {
		tix_repo_refresh(t->cfg->repo, NULL);
		reload_all(t);
		say(t, false, "reloaded");
		return;
	}
	if (t->v == V_DETAIL) {
		switch (k) {
		case 'q':
		case K_ESC:
		case K_LEFT: t->v = t->back; return;
		case 'j':
		case K_DOWN: t->dtop++; return;
		case 'k':
		case K_UP: if (t->dtop) t->dtop--; return;
		case K_PGDN: case ' ': t->dtop += (size_t)page; return;
		case K_PGUP: t->dtop = t->dtop > (size_t)page ? t->dtop - (size_t)page : 0; return;
		case 's': set_status(t); return;
		case 'e': edit_title(t); return;
		case 'l': edit_labels(t); return;
		case 'c': add_comment(t, false); return;
		case 'C': add_comment(t, true); return;
		case 'E': edit_body(t); return;
		case 'p': edit_parent(t); return;
		case 'n': new_ticket(t, true); return;
		default: return;
		}
	}
	size_t *sel = t->v == V_LIST ? &t->sel : &t->psel;
	size_t n = t->v == V_LIST ? t->list.n : t->have_plan ? t->plan.schedule->ntasks : 0;
	switch (k) {
	case 'q': t->quit = true; return;
	case 'j':
	case K_DOWN: move_sel(sel, n, 1); return;
	case 'k':
	case K_UP: move_sel(sel, n, -1); return;
	case K_PGDN: move_sel(sel, n, page); return;
	case K_PGUP: move_sel(sel, n, -page); return;
	case K_HOME: *sel = 0; return;
	case K_END:
	case 'G': move_sel(sel, n, (long)n); return;
	case K_ENTER: case K_RIGHT: {
		const tix_oid *id = target(t);
		if (id) {
			tix_oid copy = *id;
			open_ticket(t, &copy);
		}
		return;
	}
	case 's': set_status(t); return;
	case 'h': if (t->v == V_GANTT) t->gscroll -= 7; return;
	case 'l': if (t->v == V_GANTT) t->gscroll += 7; return;
	default: break;
	}
	if (t->v == V_GANTT && (k == K_LEFT || k == K_RIGHT))
		t->gscroll += k == K_LEFT ? -7 : 7;
	if (t->v == V_LIST) {
		if (k == '/') {
			tix_buf b = TIX_BUF_INIT;
			char before[256];
			snprintf(before, sizeof(before), "%s", t->filter);
			if (!prompt(t, "filter (id or title)", t->filter, &b, live_filter))
				live_filter(t, before);
			tix_buf_free(&b);
		} else if (k == 'a') {
			t->all = !t->all;
			load_list(t);
			say(t, false, "%s", t->all ? "showing every ticket" : "showing active tickets");
		} else if (k == 'n') {
			new_ticket(t, false);
		} else if (k == K_ESC && t->filter[0]) {
			live_filter(t, "");
		}
	}
}

int tui_run(const tui_config *cfg)
{
	tui t = {.cfg = cfg, .v = V_LIST, .back = V_LIST};
	if (!term_init()) {
		fprintf(stderr, "tix tui: cannot start the terminal\n");
		return 1;
	}
	reload_all(&t);
	while (!t.quit) {
		draw(&t);
		int k = term_key(1000);
		if (k == K_TIMEOUT) { /* someone else may have changed something */
			char now[TIX_STAMP_LEN];
			if (!tix_repo_refresh(cfg->repo, NULL) && !tix_repo_stamp(cfg->repo, now, NULL) &&
			    strcmp(now, t.stamp)) {
				reload_all(&t);
				say(&t, false, "updated (changed elsewhere)");
			}
			continue;
		}
		if (k == K_RESIZE)
			continue;
		t.msg[0] = '\0';
		key(&t, k);
	}
	term_end();
	tix_entity_list_free(&t.list);
	tix_entity_free(t.cur);
	if (t.have_plan)
		tix_plan_result_free(&t.plan);
	clear_lines(&t);
	free(t.lines.v);
	free(t.line_attr.v);
	return 0;
}
