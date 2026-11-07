/* The bridge algorithm (see tix/bridge.h, architecture §14.4). */
#include "tix/base_json.h"
#include "tix/bridge.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- helpers shared with adapters ---------------------------------------------------- */

void tix_issue_free(tix_issue *i)
{
	free(i->title);
	free(i->body);
	free(i->marker);
	tix_strs_free(&i->labels);
	memset(i, 0, sizeof(*i));
}

void tix_issues_free(tix_issues *l)
{
	for (size_t i = 0; i < l->n; i++)
		tix_issue_free(&l->v[i]);
	free(l->v);
	memset(l, 0, sizeof(*l));
}

void tix_issue_comments_free(tix_issue_comments *l)
{
	for (size_t i = 0; i < l->n; i++) {
		free(l->v[i].author);
		free(l->v[i].body);
		free(l->v[i].marker);
	}
	free(l->v);
	memset(l, 0, sizeof(*l));
}

void tix_bridge_report_free(tix_bridge_report *rep)
{
	tix_strs_free(&rep->actions);
	tix_strs_free(&rep->conflicts);
	memset(rep, 0, sizeof(*rep));
}

void tix_bridge_split_marker(const char *text, char **body, char **marker)
{
	const char *t = text ? text : "";
	*marker = NULL;
	const char *m = NULL;
	for (const char *p = strstr(t, TIX_BRIDGE_MARKER_OPEN); p; p = strstr(p + 1, TIX_BRIDGE_MARKER_OPEN))
		m = p; /* the last one */
	if (m) {
		const char *v = m + strlen(TIX_BRIDGE_MARKER_OPEN), *close = strstr(v, TIX_BRIDGE_MARKER_CLOSE);
		const char *after = close ? close + strlen(TIX_BRIDGE_MARKER_CLOSE) : NULL;
		size_t vlen = close ? (size_t)(close - v) : 0;
		bool tail = after && after[strspn(after, " \t\r\n")] == '\0';
		bool sane = vlen >= 7 && vlen <= TIX_OID_MAX_HEX + 12 && strspn(v, "0123456789abcdef:") == vlen;
		if (tail && sane) {
			*marker = tix_xstrndup(v, vlen);
			size_t keep = (size_t)(m - t);
			while (keep && (t[keep - 1] == '\n' || t[keep - 1] == '\r' || t[keep - 1] == ' '))
				keep--;
			*body = tix_xstrndup(t, keep);
			return;
		}
	}
	*body = tix_xstrdup(t);
}

void tix_bridge_add_marker(const char *text, const char *marker, tix_buf *out)
{
	tix_buf_reset(out);
	tix_buf_adds(out, text ? text : "");
	if (out->len)
		tix_buf_adds(out, "\n\n");
	tix_buf_addf(out, "%s%s%s", TIX_BRIDGE_MARKER_OPEN, marker, TIX_BRIDGE_MARKER_CLOSE);
}

/* ---- remote text into tix values ------------------------------------------------------- */

/* Valid UTF-8 (bad bytes become '?'), no NUL, CR dropped; single_line also turns line
 * breaks and tabs into spaces; cut to max bytes on a character boundary. */
static char *clean_text(const char *in, size_t max, bool single_line)
{
	tix_buf b = TIX_BUF_INIT;
	const unsigned char *s = (const unsigned char *)(in ? in : "");
	while (*s && b.len < max) {
		size_t n = 1;
		if (*s >= 0x80) {
			n = (*s & 0xe0) == 0xc0 ? 2 : (*s & 0xf0) == 0xe0 ? 3 : (*s & 0xf8) == 0xf0 ? 4 : 0;
			if (!n || !tix_utf8_valid((const char *)s, n) || b.len + n > max) {
				if (!n || !tix_utf8_valid((const char *)s, n))
					tix_buf_addc(&b, '?');
				s += n ? n : 1;
				if (b.len + 1 > max)
					break;
				continue;
			}
			tix_buf_add(&b, (const char *)s, n);
		} else if (*s == '\r') {
			/* dropped */
		} else if (single_line && (*s == '\n' || *s == '\t')) {
			tix_buf_addc(&b, ' ');
		} else if (*s < 0x20 && *s != '\n' && *s != '\t') {
			tix_buf_addc(&b, ' ');
		} else {
			tix_buf_addc(&b, (char)*s);
		}
		s += n;
	}
	char *out = tix_xstrdup(tix_buf_cstr(&b));
	tix_buf_free(&b);
	if (single_line) { /* trim */
		size_t len = strlen(out);
		while (len && out[len - 1] == ' ')
			out[--len] = '\0';
		size_t lead = strspn(out, " ");
		memmove(out, out + lead, len - lead + 1);
		if (!*out) {
			free(out);
			out = tix_xstrdup("(untitled)");
		}
	}
	return out;
}

/* ---- the per-clone base ----------------------------------------------------------------- */

enum { F_TITLE, F_BODY, F_CLOSED, F_LABELS, NF };

typedef struct base_entry {
	int64_t number;
	uint64_t h[NF]; /* 0 = unknown */
} base_entry;

typedef struct base {
	TIX_VEC(base_entry) v;
} base;

static uint64_t hash_str(const char *s)
{
	uint64_t h = 14695981039346656037ull;
	for (; *s; s++)
		h = (h ^ (unsigned char)*s) * 1099511628211ull;
	h ^= 0x9e3779b97f4a7c15ull; /* never 0 for real values */
	return h ? h : 1;
}

static void base_name(const tix_tracker *t, char *out, size_t n)
{
	snprintf(out, n, "bridge-%016" PRIx64, hash_str(t->key));
}

static base_entry *base_find(base *b, int64_t number)
{
	for (size_t i = 0; i < b->v.n; i++)
		if (b->v.v[i].number == number)
			return &b->v.v[i];
	return NULL;
}

static base_entry *base_get(base *b, int64_t number)
{
	base_entry *e = base_find(b, number);
	if (e)
		return e;
	base_entry n = {number, {0}};
	TIX_VEC_PUSH(&b->v, n);
	return &b->v.v[b->v.n - 1];
}

/* Lines "i <number> <h0> <h1> <h2> <h3>" (hex hashes). Anything else is skipped: a damaged
 * base only makes the next run report conflicts instead of copying. */
static void base_load(tix_store *s, const tix_tracker *t, base *b)
{
	char name[40];
	tix_buf data = TIX_BUF_INIT;
	base_name(t, name, sizeof(name));
	if (tix_store_local_get(s, name, &data, NULL)) {
		tix_buf_free(&data);
		return;
	}
	const char *p = tix_buf_cstr(&data);
	while (*p) {
		base_entry e;
		int used = 0;
		if (sscanf(p, "i %" SCNd64 " %" SCNx64 " %" SCNx64 " %" SCNx64 " %" SCNx64 "%n", &e.number, &e.h[0], &e.h[1],
			   &e.h[2], &e.h[3], &used) == 5 &&
		    e.number > 0)
			TIX_VEC_PUSH(&b->v, e);
		const char *nl = strchr(p, '\n');
		if (!nl)
			break;
		p = nl + 1;
	}
	tix_buf_free(&data);
}

static tix_status base_save(tix_store *s, const tix_tracker *t, const base *b, tix_error *err)
{
	char name[40];
	tix_buf data = TIX_BUF_INIT;
	base_name(t, name, sizeof(name));
	for (size_t i = 0; i < b->v.n; i++) {
		const base_entry *e = &b->v.v[i];
		tix_buf_addf(&data, "i %" PRId64 " %" PRIx64 " %" PRIx64 " %" PRIx64 " %" PRIx64 "\n", e->number, e->h[0],
			     e->h[1], e->h[2], e->h[3]);
	}
	tix_status st = tix_store_local_set(s, name, data.data ? data.data : "", data.len, err);
	tix_buf_free(&data);
	return st;
}

/* ---- one run --------------------------------------------------------------------------- */

typedef struct run {
	tix_repo *r;
	tix_tracker *t;
	const tix_bridge_opts *o;
	tix_bridge_report *rep;
	tix_strs universe; /* the tracker's label names */
	base b;
	tix_oid me;
	bool have_me;
} run;

static void act(run *x, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void act(run *x, const char *fmt, ...)
{
	char line[600];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (x->o->dry_run) {
		char d[620];
		snprintf(d, sizeof(d), "would %s", line);
		tix_strs_add(&x->rep->actions, d);
	} else {
		tix_strs_add(&x->rep->actions, line);
	}
}

static void conflict(run *x, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void conflict(run *x, const char *fmt, ...)
{
	char line[600];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	tix_strs_add(&x->rep->conflicts, line);
}

static bool closed_status(tix_ticket_status s)
{
	return s == TIX_TICKET_DONE || s == TIX_TICKET_CANCELLED;
}

/* Ticket labels as tix accepts them: valid ones only (a remote label tix can't hold is
 * reported, not copied). */
static bool label_ok(const char *l)
{
	size_t n = strlen(l);
	if (!n || n > TIX_LABEL_MAX)
		return false;
	for (size_t i = 0; i < n; i++)
		if ((unsigned char)l[i] <= ' ' || l[i] == ',')
			return false;
	return tix_utf8_valid(l, n);
}

/* The labels both sides can hold: the tracker has them and tix accepts them as labels.
 * Only these are bridged; anything else stays where it is (no endless copying). */
static void labels_in(const run *x, const tix_strs *l, tix_strs *out)
{
	memset(out, 0, sizeof(*out));
	for (size_t i = 0; i < l->n; i++)
		if (tix_strs_contains(&x->universe, l->v[i]) && label_ok(l->v[i]))
			tix_strs_add(out, l->v[i]);
}

static char *joined(const tix_strs *l)
{
	tix_buf b = TIX_BUF_INIT;
	for (size_t i = 0; i < l->n; i++)
		tix_buf_addf(&b, "%s\n", l->v[i]);
	char *s = tix_xstrdup(tix_buf_cstr(&b));
	tix_buf_free(&b);
	return s;
}

static void external_of(const run *x, int64_t number, char *out, size_t n)
{
	snprintf(out, n, "%s#%" PRId64, x->t->key, number);
}

static const char *short_hex(const tix_oid *id, char *out)
{
	char hex[TIX_OID_MAX_HEX + 1];
	snprintf(out, 8, "%.7s", tix_oid_hex(id, hex));
	return out;
}

/* Field-by-field for a linked pair. */
static tix_status sync_pair(run *x, const tix_entity *e, const tix_issue *iss, tix_error *err)
{
	const tix_ticket *tk = &e->u.ticket;
	base_entry *b = base_get(&x->b, iss->number);
	bool may_pull = x->o->mode & TIX_BRIDGE_PULL, may_push = x->o->mode & TIX_BRIDGE_PUSH;
	char s[8];
	tix_strs tl, il;
	labels_in(x, &tk->labels, &tl);
	labels_in(x, &iss->labels, &il);
	char *tlj = joined(&tl), *ilj = joined(&il);
	char *ibody = clean_text(iss->body, TIX_TEXT_MAX, false), *ititle = clean_text(iss->title, TIX_TITLE_MAX, true);
	const char *tv[NF] = {tk->title, tk->body ? tk->body : "", closed_status(tk->status) ? "1" : "0", tlj};
	const char *iv[NF] = {ititle, ibody, iss->closed ? "1" : "0", ilj};
	static const char *const names[NF] = {"title", "body", "state", "labels"};
	TIX_VEC(tix_op) ops = {0};
	tix_issue_update up = {0};
	bool push_any = false;
	uint64_t newh[NF];
	for (int f = 0; f < NF; f++) {
		uint64_t ht = hash_str(tv[f]), hi = hash_str(iv[f]);
		newh[f] = b->h[f];
		if (ht == hi) {
			newh[f] = ht;
			continue;
		}
		bool tchg = !b->h[f] || ht != b->h[f], ichg = !b->h[f] || hi != b->h[f];
		if (tchg && ichg) {
			conflict(x, "%s #%" PRId64 " (ticket %s): %s changed on both sides%s; edit one to match the other",
				 x->t->key, iss->number, short_hex(&e->id, s), names[f], b->h[f] ? "" : " (or never bridged here)");
			continue;
		}
		if (ichg && may_pull) { /* issue -> ticket */
			switch (f) {
			case F_TITLE: TIX_VEC_PUSH(&ops, tix_op_text(TIX_OP_SET_TITLE, ititle)); break;
			case F_BODY: TIX_VEC_PUSH(&ops, tix_op_text(TIX_OP_SET_BODY, ibody)); break;
			case F_CLOSED:
				TIX_VEC_PUSH(&ops, tix_op_set_status(iss->closed ? TIX_TICKET_DONE : TIX_TICKET_OPEN));
				break;
			case F_LABELS: {
				tix_strs add = {0}, rm = {0};
				for (size_t i = 0; i < il.n; i++)
					if (!tix_strs_contains(&tl, il.v[i]) && label_ok(il.v[i]))
						tix_strs_add(&add, il.v[i]);
				for (size_t i = 0; i < tl.n; i++)
					if (!tix_strs_contains(&il, tl.v[i]))
						tix_strs_add(&rm, tl.v[i]);
				if (add.n || rm.n)
					TIX_VEC_PUSH(&ops, tix_op_label(&add, &rm));
				tix_strs_free(&add);
				tix_strs_free(&rm);
				break;
			}
			}
			act(x, "update ticket %s %s from %s #%" PRId64, short_hex(&e->id, s), names[f], x->t->key, iss->number);
			newh[f] = hi;
		} else if (tchg && may_push) { /* ticket -> issue */
			switch (f) {
			case F_TITLE: up.title = tk->title; break;
			case F_BODY: up.body = tk->body ? tk->body : ""; break;
			case F_CLOSED: up.set_closed = true, up.closed = closed_status(tk->status); break;
			case F_LABELS: up.set_labels = true, up.labels = &tl; break;
			}
			push_any = true;
			act(x, "update %s #%" PRId64 " %s from ticket %s", x->t->key, iss->number, names[f], short_hex(&e->id, s));
			newh[f] = ht;
		}
	}
	tix_status st = TIX_OK;
	if (!x->o->dry_run && ops.n) {
		st = tix_entity_apply(x->r, TIX_ENTITY_TICKET, &e->id, ops.v, ops.n, err);
		if (!st)
			x->rep->tickets_updated++;
	}
	if (!st && !x->o->dry_run && push_any) {
		st = x->t->vt->update(x->t, iss->number, &up, err);
		if (!st)
			x->rep->issues_updated++;
	}
	if (!st && !x->o->dry_run)
		memcpy(b->h, newh, sizeof(newh));
	for (size_t i = 0; i < ops.n; i++)
		tix_op_free(&ops.v[i]);
	free(ops.v);
	free(tlj);
	free(ilj);
	free(ibody);
	free(ititle);
	tix_strs_free(&tl);
	tix_strs_free(&il);
	return st;
}

/* The first line of an imported comment: "> author wrote on <key>#<n> (comment <id>):" */
static void import_line(const run *x, int64_t number, const tix_issue_comment *c, tix_buf *out)
{
	tix_buf_reset(out);
	char *who = clean_text(c->author ? c->author : "someone", 100, true);
	tix_buf_addf(out, "> %s wrote on %s#%" PRId64 " (comment %" PRId64 "):", who, x->t->key, number, c->id);
	free(who);
}

static bool imported_from(const run *x, const char *body, int64_t number, int64_t *id)
{
	char head[600];
	snprintf(head, sizeof(head), " wrote on %s#%" PRId64 " (comment ", x->t->key, number);
	if (strncmp(body, "> ", 2))
		return false;
	const char *nl = strchr(body, '\n');
	const char *h = strstr(body, head);
	if (!h || (nl && h > nl))
		return false;
	return sscanf(h + strlen(head), "%" SCNd64 "):", id) == 1;
}

static tix_status sync_comments(run *x, const tix_entity *e, int64_t number, tix_error *err)
{
	tix_issue_comments cs = {0};
	tix_status st = x->t->vt->comments(x->t, number, &cs, err);
	if (st)
		return st;
	const tix_ticket *tk = &e->u.ticket;
	char s[8], op[TIX_OID_MAX_HEX + 16];
	tix_buf line = TIX_BUF_INIT, body = TIX_BUF_INIT;
	/* tracker -> tix: comments without a tix marker that no tix comment imported yet */
	for (size_t i = 0; i < cs.n && !st && (x->o->mode & TIX_BRIDGE_PULL); i++) {
		const tix_issue_comment *c = &cs.v[i];
		if (c->marker)
			continue;
		bool have = false;
		for (size_t k = 0; k < tk->ncomments && !have; k++) {
			int64_t id;
			have = imported_from(x, tk->comments[k].body, number, &id) && id == c->id;
		}
		if (have)
			continue;
		import_line(x, number, c, &line);
		char *text = clean_text(c->body, TIX_TEXT_MAX - line.len - 4, false);
		tix_buf_reset(&body);
		tix_buf_addf(&body, "%s\n\n%s", tix_buf_cstr(&line), text);
		free(text);
		act(x, "copy %s #%" PRId64 " comment %" PRId64 " to ticket %s", x->t->key, number, c->id, short_hex(&e->id, s));
		if (!x->o->dry_run) {
			tix_op o = tix_op_text(TIX_OP_COMMENT, tix_buf_cstr(&body));
			st = tix_entity_apply(x->r, TIX_ENTITY_TICKET, &e->id, &o, 1, err);
			tix_op_free(&o);
			if (!st)
				x->rep->comments_in++;
		}
	}
	/* tix -> tracker: comments that aren't imports and no tracker comment carries yet */
	for (size_t k = 0; k < tk->ncomments && !st && (x->o->mode & TIX_BRIDGE_PUSH); k++) {
		const tix_comment *c = &tk->comments[k];
		int64_t ignored;
		if (imported_from(x, c->body, number, &ignored))
			continue;
		tix_opid_str(&c->id, op);
		bool have = false;
		for (size_t i = 0; i < cs.n && !have; i++)
			have = cs.v[i].marker && !strcmp(cs.v[i].marker, op);
		if (have)
			continue;
		act(x, "copy ticket %s comment %.12s to %s #%" PRId64, short_hex(&e->id, s), op, x->t->key, number);
		if (!x->o->dry_run) {
			int64_t id;
			st = x->t->vt->comment(x->t, number, c->body, op, &id, err);
			if (!st)
				x->rep->comments_out++;
		}
	}
	tix_buf_free(&line);
	tix_buf_free(&body);
	tix_issue_comments_free(&cs);
	return st;
}

static tix_status import_issue(run *x, const tix_issue *iss, tix_error *err)
{
	char ext[TIX_EXTERNAL_MAX + 1];
	external_of(x, iss->number, ext, sizeof(ext));
	act(x, "create a ticket for %s", ext);
	if (x->o->dry_run)
		return TIX_OK;
	char *title = clean_text(iss->title, TIX_TITLE_MAX, true), *body = clean_text(iss->body, TIX_TEXT_MAX, false);
	tix_strs labels = {0};
	for (size_t i = 0; i < iss->labels.n; i++)
		if (label_ok(iss->labels.v[i]) && tix_strs_contains(&x->universe, iss->labels.v[i]))
			tix_strs_add(&labels, iss->labels.v[i]);
	tix_op ops[3];
	size_t n = 0;
	ops[n++] = tix_op_ticket_create(NULL, title, body, &labels);
	ops[n++] = tix_op_set(TIX_F_EXTERNAL, tix_value_str(ext));
	if (iss->closed)
		ops[n++] = tix_op_set_status(TIX_TICKET_DONE);
	tix_oid id;
	tix_status st = tix_entity_create(x->r, TIX_ENTITY_TICKET, ops, n, &id, err);
	for (size_t i = 0; i < n; i++)
		tix_op_free(&ops[i]);
	if (!st) {
		x->rep->tickets_created++;
		base_entry *b = base_get(&x->b, iss->number);
		char *lj = joined(&labels);
		b->h[F_TITLE] = hash_str(title);
		b->h[F_BODY] = hash_str(body);
		b->h[F_CLOSED] = hash_str(iss->closed ? "1" : "0");
		b->h[F_LABELS] = hash_str(lj);
		free(lj);
	}
	free(title);
	free(body);
	tix_strs_free(&labels);
	return st;
}

static tix_status export_ticket(run *x, const tix_entity *e, tix_error *err)
{
	const tix_ticket *tk = &e->u.ticket;
	char hex[TIX_OID_MAX_HEX + 1], s[8];
	tix_oid_hex(&e->id, hex);
	act(x, "create an issue in %s for ticket %s", x->t->key, short_hex(&e->id, s));
	if (x->o->dry_run)
		return TIX_OK;
	tix_strs tl;
	labels_in(x, &tk->labels, &tl);
	tix_issue_update in = {.title = tk->title, .body = tk->body ? tk->body : "", .marker = hex, .set_labels = true,
			       .labels = &tl};
	int64_t number;
	tix_status st = x->t->vt->create(x->t, &in, &number, err);
	if (!st && closed_status(tk->status)) {
		tix_issue_update cl = {.set_closed = true, .closed = true};
		st = x->t->vt->update(x->t, number, &cl, err);
	}
	if (!st) {
		x->rep->issues_created++;
		char ext[TIX_EXTERNAL_MAX + 1];
		external_of(x, number, ext, sizeof(ext));
		tix_op op = tix_op_set(TIX_F_EXTERNAL, tix_value_str(ext));
		st = tix_entity_apply(x->r, TIX_ENTITY_TICKET, &e->id, &op, 1, err);
		tix_op_free(&op);
		/* if that failed, the next run links them through the issue's marker */
		if (!st && x->o->comments)
			st = sync_comments(x, e, number, err);
		base_entry *b = base_get(&x->b, number);
		char *lj = joined(&tl);
		b->h[F_TITLE] = hash_str(tk->title);
		b->h[F_BODY] = hash_str(tk->body ? tk->body : "");
		b->h[F_CLOSED] = hash_str(closed_status(tk->status) ? "1" : "0");
		b->h[F_LABELS] = hash_str(lj);
		free(lj);
	}
	tix_strs_free(&tl);
	return st;
}

/* The issue number a ticket links to in this tracker, or 0. */
static int64_t linked_number(const run *x, const tix_entity *e)
{
	const char *ext = e->u.ticket.plan.external;
	size_t kl = strlen(x->t->key);
	if (!ext || strncmp(ext, x->t->key, kl) || ext[kl] != '#')
		return 0;
	char *end;
	long long n = strtoll(ext + kl + 1, &end, 10);
	return *end || n <= 0 ? 0 : (int64_t)n;
}

static bool in_scope(run *x, const tix_entity *e, const tix_entity_list *all)
{
	if (x->o->all)
		return true;
	if (x->o->label && tix_strs_contains(&e->u.ticket.labels, x->o->label))
		return true;
	if (x->o->under) {
		const tix_entity *cur = e;
		for (size_t steps = 0; cur && steps <= all->n; steps++) {
			const tix_oid *p = &cur->u.ticket.plan.parent;
			if (tix_oid_is_null(p))
				return false;
			if (tix_oid_eq(p, x->o->under))
				return true;
			const tix_entity *next = NULL;
			for (size_t i = 0; i < all->n && !next; i++)
				if (tix_oid_eq(&all->v[i]->id, p))
					next = all->v[i];
			cur = next;
		}
	}
	return false;
}

tix_status tix_bridge_run(tix_repo *r, tix_store *store, tix_tracker *t, const tix_bridge_opts *o,
			  tix_bridge_report *rep, tix_error *err)
{
	TIX_REQUIRE(r && store && t && t->key && o && rep && (o->mode & TIX_BRIDGE_SYNC));
	run x = {.r = r, .t = t, .o = o, .rep = rep};
	x.have_me = tix_repo_identity(r, &x.me);
	if (!x.have_me && !o->dry_run)
		return tix_error_set(err, TIX_ERR_NOT_FOUND, "no identity configured (create one with `tix id new`)");
	tix_issues issues = {0};
	tix_entity_list tickets = {0};
	tix_status st = t->vt->labels(t, &x.universe, err);
	if (!st)
		st = t->vt->list(t, &issues, err);
	if (!st)
		st = tix_ticket_list(r, NULL, &tickets, err); /* every status */
	if (st)
		goto out;
	base_load(store, t, &x.b);

	/* links from tickets; two tickets claiming one issue is a conflict for both */
	const tix_entity **by_issue = tix_xcalloc(issues.n ? issues.n : 1, sizeof(*by_issue));
	bool *dup = tix_xcalloc(issues.n ? issues.n : 1, sizeof(*dup));
	char s1[8], s2[8];
	for (size_t k = 0; k < tickets.n; k++) {
		int64_t n = linked_number(&x, tickets.v[k]);
		if (!n)
			continue;
		size_t i = 0;
		while (i < issues.n && issues.v[i].number != n)
			i++;
		if (i == issues.n) {
			conflict(&x, "ticket %s links to %s#%" PRId64 ", which isn't there (deleted or moved?)",
				 short_hex(&tickets.v[k]->id, s1), t->key, n);
			continue;
		}
		if (by_issue[i]) {
			if (!dup[i])
				conflict(&x, "tickets %s and %s both link to %s#%" PRId64 "; close one and clear its link",
					 short_hex(&by_issue[i]->id, s1), short_hex(&tickets.v[k]->id, s2), t->key, n);
			dup[i] = true;
			continue;
		}
		by_issue[i] = tickets.v[k];
	}

	for (size_t i = 0; i < issues.n && !st; i++) {
		const tix_issue *iss = &issues.v[i];
		if (dup[i])
			continue;
		const tix_entity *e = by_issue[i];
		if (!e && iss->marker) { /* created by tix: find its ticket */
			tix_oid id;
			const tix_entity *m = NULL;
			if (!tix_entity_resolve(r, TIX_ENTITY_TICKET, iss->marker, &id, NULL))
				for (size_t k = 0; k < tickets.n && !m; k++)
					if (tix_oid_eq(&tickets.v[k]->id, &id))
						m = tickets.v[k];
			if (!m) {
				conflict(&x, "%s#%" PRId64 " was made for ticket %.12s, which isn't here (tix pull first?)", t->key,
					 iss->number, iss->marker);
				continue;
			}
			if (m->u.ticket.plan.external) {
				conflict(&x, "%s#%" PRId64 " was made for ticket %s, which links to %s instead (a duplicate?)",
					 t->key, iss->number, short_hex(&m->id, s1), m->u.ticket.plan.external);
				continue;
			}
			char ext[TIX_EXTERNAL_MAX + 1];
			external_of(&x, iss->number, ext, sizeof(ext));
			act(&x, "link ticket %s to %s", short_hex(&m->id, s1), ext);
			if (!o->dry_run) {
				tix_op op = tix_op_set(TIX_F_EXTERNAL, tix_value_str(ext));
				st = tix_entity_apply(r, TIX_ENTITY_TICKET, &m->id, &op, 1, err);
				tix_op_free(&op);
			}
			e = m;
		}
		if (!e) {
			if (o->mode & TIX_BRIDGE_PULL)
				st = import_issue(&x, iss, err);
			continue;
		}
		if (!st)
			st = sync_pair(&x, e, iss, err);
		if (!st && o->comments) {
			/* comments against the current ticket (sync_pair may have changed it) */
			tix_entity *fresh = NULL;
			if (!o->dry_run && tix_entity_load(r, TIX_ENTITY_TICKET, &e->id, &fresh, NULL))
				fresh = NULL;
			st = sync_comments(&x, fresh ? fresh : e, iss->number, err);
			tix_entity_free(fresh);
		}
	}
	/* comments of issues imported just now */
	if (!st && o->comments && !o->dry_run && rep->tickets_created) {
		tix_entity_list now = {0};
		if (!tix_ticket_list(r, NULL, &now, err)) {
			for (size_t k = 0; k < now.n && !st; k++) {
				int64_t n = linked_number(&x, now.v[k]);
				bool before = false;
				for (size_t j = 0; j < tickets.n && !before; j++)
					before = tix_oid_eq(&tickets.v[j]->id, &now.v[k]->id);
				if (n && !before)
					st = sync_comments(&x, now.v[k], n, err);
			}
			tix_entity_list_free(&now);
		}
	}
	if (!st && (o->mode & TIX_BRIDGE_PUSH))
		for (size_t k = 0; k < tickets.n && !st; k++) {
			const tix_entity *e = tickets.v[k];
			if (e->u.ticket.plan.external || !in_scope(&x, e, &tickets))
				continue;
			bool claimed = false; /* an issue already carries its marker: linked above */
			char hex[TIX_OID_MAX_HEX + 1];
			tix_oid_hex(&e->id, hex);
			for (size_t i = 0; i < issues.n && !claimed; i++)
				claimed = issues.v[i].marker && !strncmp(hex, issues.v[i].marker, strlen(issues.v[i].marker));
			if (!claimed)
				st = export_ticket(&x, e, err);
		}
	free(by_issue);
	free(dup);
	if (!o->dry_run) {
		tix_error berr = TIX_ERROR_INIT;
		tix_status bst = base_save(store, t, &x.b, st ? &berr : err);
		if (!st)
			st = bst;
	}
out:
	free(x.b.v.v);
	tix_strs_free(&x.universe);
	tix_issues_free(&issues);
	tix_entity_list_free(&tickets);
	return st;
}
