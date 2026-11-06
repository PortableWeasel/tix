/*
 * Forms (docs/architecture.md sections 13.3 and 13.5). Every form carries the value each
 * field had when the page was drawn (hidden o.<field>) and the entity's head. On POST a
 * field is written only when its value differs from o.<field>, so a stale tab never
 * overwrites a field it didn't touch. If the head moved and someone else changed a field
 * this form also changed, the form comes back with both values instead of applying.
 * Create forms use the defaults as their "originals", so the same rule applies.
 * Values go through tix/input.h, so the web accepts exactly what the CLI accepts.
 */
#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum widget { W_TEXT, W_AREA, W_DATE, W_SELECT, W_CAL, W_IDENTITY } widget;

typedef struct fdef {
	const char *name, *label;
	widget w;
	const char *const *choices; /* W_SELECT */
	const char *dflt;           /* the value of a new entity */
	const char *hint;
	bool nullable;              /* "" means none */
	bool plain;                 /* not a tix/input field (ticket title, body, status, labels) */
} fdef;

static const char *const statuses[] = {"open", "in-progress", "blocked", "done", "cancelled", NULL};
static const char *const kinds[] = {"task", "milestone", NULL};
static const char *const modes[] = {"auto", "manual", NULL};
static const char *const res_kinds[] = {"person", "role", "equipment", NULL};
static const char *const yes_no[] = {"yes", "no", NULL};

static const fdef ticket_fields[] = {
	{"title", "Title", W_TEXT, NULL, "", NULL, false, true},
	{"body", "Description", W_AREA, NULL, "", NULL, false, true},
	{"status", "Status", W_SELECT, statuses, "open", NULL, false, true},
	{"labels", "Labels", W_TEXT, NULL, "", "comma-separated", false, true},
	{"parent", "Parent", W_TEXT, NULL, "", "ticket id, empty for none", true, false},
	{"rank", "Position among siblings", W_TEXT, NULL, "", "first, last, before:ID or after:ID; empty keeps it",
	 false, false},
	{"kind", "Kind", W_SELECT, kinds, "task", NULL, false, false},
	{"mode", "Scheduling", W_SELECT, modes, "auto", "manual: start and finish are fixed", false, false},
	{"calendar", "Calendar", W_CAL, NULL, "", NULL, true, false},
	{"start", "Start no earlier than", W_DATE, NULL, "", NULL, true, false},
	{"finish", "Finish (manual tasks)", W_DATE, NULL, "", NULL, true, false},
	{"deadline", "Deadline", W_DATE, NULL, "", NULL, true, false},
	{"duration", "Duration", W_TEXT, NULL, "", "working days, like 3", true, false},
	{"effort", "Effort", W_TEXT, NULL, "", "hours, like 16", true, false},
	{"progress", "Progress", W_TEXT, NULL, "0", "percent", false, false},
	{"actual-start", "Actual start", W_DATE, NULL, "", NULL, true, false},
	{"actual-finish", "Actual finish", W_DATE, NULL, "", NULL, true, false},
};

static const fdef res_fields[] = {
	{"name", "Name", W_TEXT, NULL, "", NULL, false, false},
	{"kind", "Kind", W_SELECT, res_kinds, "person", NULL, false, false},
	{"capacity", "Capacity", W_TEXT, NULL, "100", "percent of one full-time unit", false, false},
	{"calendar", "Calendar", W_CAL, NULL, "", "empty: each task's own", true, false},
	{"identity", "Identity", W_IDENTITY, NULL, "", NULL, true, false},
	{"rate", "Rate", W_TEXT, NULL, "", "per hour, like 95.50 EUR", true, false},
	{"active", "Active", W_SELECT, yes_no, "yes", NULL, false, false},
};

static const fdef cal_fields[] = {
	{"name", "Name", W_TEXT, NULL, "", NULL, false, false},
	{"weekdays", "Working days", W_TEXT, NULL, "mon-fri", "like mon-fri or mon,wed,fri", false, false},
	{"hours", "Hours per day", W_TEXT, NULL, "8", NULL, false, false},
};

#define NFIELDS(a) (sizeof(a) / sizeof(*(a)))
#define MAXF 20

/* ---- canonical text of a field (what the form shows and compares) ----------------------- */

static void hexid(tix_buf *b, const tix_oid *id)
{
	char hex[TIX_OID_MAX_HEX + 1];
	if (!tix_oid_is_null(id))
		tix_buf_adds(b, tix_oid_hex(id, hex));
}

static void date(tix_buf *b, tix_date d)
{
	char s[TIX_DATE_STR];
	if (d != TIX_DATE_NONE)
		tix_buf_adds(b, tix_date_str(d, s));
}

static void weekdays(tix_buf *b, unsigned mask)
{
	static const char *const names[] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};
	for (int d = 0; d < 7;) {
		if (!(mask & (1u << d))) {
			d++;
			continue;
		}
		int e = d;
		while (e + 1 < 7 && (mask & (1u << (e + 1))))
			e++;
		if (b->len)
			tix_buf_addc(b, ',');
		tix_buf_adds(b, names[d]);
		if (e > d) {
			tix_buf_addc(b, e == d + 1 ? ',' : '-');
			tix_buf_adds(b, names[e]);
		}
		d = e + 1;
	}
}

static void value_of(const tix_entity *e, const char *f, tix_buf *b)
{
	tix_buf_reset(b);
	if (e->type == TIX_ENTITY_TICKET) {
		const tix_ticket *t = &e->u.ticket;
		const tix_ticket_plan *p = &t->plan;
		if (!strcmp(f, "title"))
			tix_buf_adds(b, t->title);
		else if (!strcmp(f, "body"))
			tix_buf_adds(b, t->body ? t->body : "");
		else if (!strcmp(f, "status"))
			tix_buf_adds(b, tix_ticket_status_name(t->status));
		else if (!strcmp(f, "labels"))
			for (size_t i = 0; i < t->labels.n; i++)
				tix_buf_addf(b, "%s%s", i ? ", " : "", t->labels.v[i]);
		else if (!strcmp(f, "parent"))
			hexid(b, &p->parent);
		else if (!strcmp(f, "kind"))
			tix_buf_adds(b, kinds[p->kind]);
		else if (!strcmp(f, "mode"))
			tix_buf_adds(b, modes[p->mode]);
		else if (!strcmp(f, "calendar"))
			hexid(b, &p->calendar);
		else if (!strcmp(f, "start"))
			date(b, p->start);
		else if (!strcmp(f, "finish"))
			date(b, p->finish);
		else if (!strcmp(f, "deadline"))
			date(b, p->deadline);
		else if (!strcmp(f, "actual-start"))
			date(b, p->actual_start);
		else if (!strcmp(f, "actual-finish"))
			date(b, p->actual_finish);
		else if (!strcmp(f, "duration") && p->duration >= 0)
			tix_buf_addf(b, "%lld", (long long)p->duration);
		else if (!strcmp(f, "effort") && p->effort >= 0)
			tix_buf_addf(b, "%lld", (long long)p->effort);
		else if (!strcmp(f, "progress"))
			tix_buf_addf(b, "%d", p->progress);
	} else if (e->type == TIX_ENTITY_RESOURCE) {
		const tix_resource *r = &e->u.resource;
		if (!strcmp(f, "name"))
			tix_buf_adds(b, r->name);
		else if (!strcmp(f, "kind"))
			tix_buf_adds(b, res_kinds[r->kind]);
		else if (!strcmp(f, "capacity"))
			tix_buf_addf(b, "%d", (int)r->capacity);
		else if (!strcmp(f, "calendar"))
			hexid(b, &r->calendar);
		else if (!strcmp(f, "identity"))
			hexid(b, &r->identity);
		else if (!strcmp(f, "rate") && r->rate >= 0)
			tix_buf_addf(b, "%lld.%02lld %s", (long long)(r->rate / 100), (long long)(r->rate % 100),
				     r->currency ? r->currency : "");
		else if (!strcmp(f, "active"))
			tix_buf_adds(b, r->active ? "yes" : "no");
	} else if (e->type == TIX_ENTITY_CALENDAR) {
		const tix_calendar *c = &e->u.calendar;
		if (!strcmp(f, "name"))
			tix_buf_adds(b, c->name);
		else if (!strcmp(f, "weekdays"))
			weekdays(b, c->weekdays);
		else if (!strcmp(f, "hours"))
			tix_buf_addf(b, "%d", c->hours);
	}
}

/* ---- form state ------------------------------------------------------------------------ */

typedef struct fstate {
	tix_entity_type type;
	const fdef *defs;
	size_t n;
	const tix_entity *cur;   /* NULL when creating */
	const web_kvs *posted;   /* NULL on a fresh GET */
	char errs[MAXF][300];
	bool conflict[MAXF];
	tix_buf curval[MAXF];    /* the entity's current value (conflicts) */
	char general[320];
	const char *prefill[MAXF]; /* for new forms: values from the query (e.g. parent) */
} fstate;

static void fstate_free(fstate *fs)
{
	for (size_t i = 0; i < MAXF; i++)
		tix_buf_free(&fs->curval[i]);
}

/* What the widget shows. */
static const char *shown(fstate *fs, size_t i, tix_buf *tmp)
{
	const char *v = fs->posted ? web_kvs_get(fs->posted, fs->defs[i].name) : NULL;
	if (v)
		return v;
	if (fs->prefill[i])
		return fs->prefill[i];
	if (fs->cur) {
		value_of(fs->cur, fs->defs[i].name, tmp);
		return tix_buf_cstr(tmp);
	}
	return fs->defs[i].dflt;
}

/* What o.<field> carries: the current value after a conflict, else what the page had. */
static const char *original(fstate *fs, size_t i, tix_buf *tmp)
{
	if (fs->conflict[i])
		return tix_buf_cstr(&fs->curval[i]);
	char oname[96];
	snprintf(oname, sizeof(oname), "o.%s", fs->defs[i].name);
	const char *o = fs->posted ? web_kvs_get(fs->posted, oname) : NULL;
	if (o)
		return o;
	if (fs->cur) {
		value_of(fs->cur, fs->defs[i].name, tmp);
		return tix_buf_cstr(tmp);
	}
	return fs->defs[i].dflt;
}

/* ---- rendering --------------------------------------------------------------------------- */

static void options_entities(html *h, const tix_entity_list *l, const char *sel, const char *none_label)
{
	HL(h, "<option value=\"\">");
	h_text(h, none_label);
	HL(h, "</option>");
	for (size_t i = 0; l && i < l->n; i++) {
		char hex[TIX_OID_MAX_HEX + 1];
		tix_oid_hex(&l->v[i]->id, hex);
		HL(h, "<option value=\"");
		h_text(h, hex);
		HL(h, "\"");
		if (!strcmp(sel, hex))
			HL(h, " selected");
		HL(h, ">");
		h_text(h, l->v[i]->type == TIX_ENTITY_CALENDAR ? l->v[i]->u.calendar.name : l->v[i]->u.identity.name);
		HL(h, "</option>");
	}
}

/* Fields [from, to) of fs. */
static void render_fields(web_ctx *x, fstate *fs, size_t from, size_t to, const tix_entity_list *cals,
			  const tix_entity_list *idents)
{
	html *h = &x->h;
	tix_buf a = TIX_BUF_INIT, b = TIX_BUF_INIT;
	for (size_t i = from; i < to && i < fs->n; i++) {
		const fdef *d = &fs->defs[i];
		const char *v = shown(fs, i, &a);
		const char *o = original(fs, i, &b);
		HL(h, "<p class=\"field\"><label>");
		h_text(h, d->label);
		HL(h, "<br>");
		switch (d->w) {
		case W_AREA:
			HL(h, "<textarea name=\"");
			h_text(h, d->name);
			HL(h, "\" rows=\"8\" cols=\"70\">");
			h_text(h, v);
			HL(h, "</textarea>");
			break;
		case W_SELECT:
			HL(h, "<select name=\"");
			h_text(h, d->name);
			HL(h, "\">");
			for (size_t k = 0; d->choices[k]; k++) {
				HL(h, "<option");
				if (!strcmp(v, d->choices[k]))
					HL(h, " selected");
				HL(h, ">");
				h_text(h, d->choices[k]);
				HL(h, "</option>");
			}
			HL(h, "</select>");
			break;
		case W_CAL:
		case W_IDENTITY:
			HL(h, "<select name=\"");
			h_text(h, d->name);
			HL(h, "\">");
			options_entities(h, d->w == W_CAL ? cals : idents, v, "none");
			HL(h, "</select>");
			break;
		case W_DATE:
		case W_TEXT:
			HL(h, "<input name=\"");
			h_text(h, d->name);
			HL(h, "\" value=\"");
			h_text(h, v);
			HL(h, "\"");
			if (d->w == W_DATE)
				HL(h, " type=\"date\"");
			else if (!strcmp(d->name, "title") || !strcmp(d->name, "name"))
				HL(h, " size=\"60\" required");
			else
				HL(h, " size=\"30\"");
			HL(h, ">");
			break;
		}
		HL(h, "</label>");
		if (d->hint) {
			HL(h, " <span class=\"note\">");
			h_text(h, d->hint);
			HL(h, "</span>");
		}
		HL(h, "<input type=\"hidden\" name=\"o.");
		h_text(h, d->name);
		HL(h, "\" value=\"");
		h_text(h, o);
		HL(h, "\">");
		if (fs->errs[i][0]) {
			HL(h, "<br><span class=\"error\">");
			h_text(h, fs->errs[i]);
			HL(h, "</span>");
		}
		if (fs->conflict[i]) {
			HL(h, "<br><span class=\"error\">Changed by someone else meanwhile, to: ");
			h_text(h, tix_buf_cstr(&fs->curval[i]));
			if (!tix_buf_cstr(&fs->curval[i])[0])
				HL(h, "(empty)");
			HL(h, ". Send again to replace it with yours, or change yours.</span>");
		}
		HL(h, "</p>\n");
	}
	tix_buf_free(&a);
	tix_buf_free(&b);
}

/* ---- saving -------------------------------------------------------------------------------- */

typedef TIX_VEC(tix_op) op_vec;

static void ops_free(op_vec *ops)
{
	for (size_t i = 0; i < ops->n; i++)
		tix_op_free(&ops->v[i]);
	free(ops->v);
	memset(ops, 0, sizeof(*ops));
}

static void split_labels(const char *s, tix_strs *out)
{
	while (*s) {
		while (*s == ' ' || *s == ',')
			s++;
		size_t n = strcspn(s, ",");
		size_t m = n;
		while (m && s[m - 1] == ' ')
			m--;
		if (m) {
			char *one = tix_xstrndup(s, m);
			if (!tix_strs_contains(out, one))
				tix_strs_add(out, one);
			free(one);
		}
		s += n;
	}
}

static int find_field(const fstate *fs, const char *name)
{
	for (size_t i = 0; i < fs->n; i++)
		if (!strcmp(fs->defs[i].name, name))
			return (int)i;
	return -1;
}

/*
 * Turns the posted, changed fields into ops (plain ones into ops, input fields into in).
 * RETURNS false when any field has an error or a conflict (messages in fs).
 */
static bool collect(web_ctx *x, fstate *fs, op_vec *ops, tix_input *in, bool *any)
{
	const web_kvs *f = fs->posted;
	tix_input_ctx ctx = {x->app->cfg.repo, page_today(x)};
	const char *head = web_kvs_get(f, "head");
	char curhead[TIX_OID_MAX_HEX + 1] = "";
	if (fs->cur)
		tix_oid_hex(&fs->cur->head, curhead);
	bool moved = fs->cur && head && strcmp(head, curhead);
	bool ok = true;
	tix_buf tmp = TIX_BUF_INIT;
	*any = false;
	for (size_t i = 0; i < fs->n; i++) {
		const fdef *d = &fs->defs[i];
		const char *v = web_kvs_get(f, d->name);
		if (!v)
			continue; /* not on this form */
		const char *o = original(fs, i, &tmp);
		if (!strcmp(v, o))
			continue; /* unchanged: never written */
		if (moved) {
			value_of(fs->cur, d->name, &fs->curval[i]);
			const char *now = tix_buf_cstr(&fs->curval[i]);
			if (strcmp(now, o) && strcmp(now, v)) { /* they changed it too, differently */
				fs->conflict[i] = true;
				ok = false;
				continue;
			}
		}
		*any = true;
		if (d->plain) {
			if (!strcmp(d->name, "title")) {
				if (!*v) {
					snprintf(fs->errs[i], sizeof(fs->errs[i]), "A title is required.");
					ok = false;
				} else if (fs->cur) {
					TIX_VEC_PUSH(ops, tix_op_text(TIX_OP_SET_TITLE, v));
				}
			} else if (!strcmp(d->name, "body")) {
				if (fs->cur)
					TIX_VEC_PUSH(ops, tix_op_text(TIX_OP_SET_BODY, v));
			} else if (!strcmp(d->name, "status")) {
				tix_ticket_status s;
				if (!tix_ticket_status_parse(v, &s)) {
					snprintf(fs->errs[i], sizeof(fs->errs[i]), "Unknown status.");
					ok = false;
				} else {
					TIX_VEC_PUSH(ops, tix_op_set_status(s));
				}
			} else if (!strcmp(d->name, "labels") && fs->cur) {
				tix_strs was = {0}, now = {0}, add = {0}, rm = {0};
				split_labels(o, &was);
				split_labels(v, &now);
				for (size_t k = 0; k < now.n; k++)
					if (!tix_strs_contains(&was, now.v[k]))
						tix_strs_add(&add, now.v[k]);
				for (size_t k = 0; k < was.n; k++)
					if (!tix_strs_contains(&now, was.v[k]))
						tix_strs_add(&rm, was.v[k]);
				if (add.n || rm.n)
					TIX_VEC_PUSH(ops, tix_op_label(&add, &rm));
				tix_strs_free(&was);
				tix_strs_free(&now);
				tix_strs_free(&add);
				tix_strs_free(&rm);
			}
			continue;
		}
		if (!fs->cur && (!strcmp(d->name, "name") || (fs->type == TIX_ENTITY_RESOURCE && !strcmp(d->name, "kind"))))
			continue; /* part of the create op */
		if (!strcmp(d->name, "rank") && !*v)
			continue;
		const char *text = d->nullable && !*v ? "none" : v;
		if (tix_input_field(&ctx, fs->type, d->name, text, in, &x->app->err)) {
			snprintf(fs->errs[i], sizeof(fs->errs[i]), "%s", x->app->err.msg);
			ok = false;
		}
	}
	tix_buf_free(&tmp);
	return ok;
}

/* ---- the ticket form (/new, /t/ID/edit, POST /t/ID) ---------------------------------------- */

static void ticket_form_page(web_ctx *x, fstate *fs, int status)
{
	tix_entity_list cals = {0};
	if (tix_calendar_list(x->app->cfg.repo, &cals, &x->app->err)) {
		page_error(x, 500, "Repository problem", x->app->err.msg);
		return;
	}
	x->res->status = status;
	html *h = &x->h;
	if (fs->cur) {
		page_begin(x, "Edit ticket");
		HL(h, "<p><a href=\"/t/");
		h_id(h, &fs->cur->id);
		HL(h, "\">");
		h_text(h, fs->cur->u.ticket.title);
		HL(h, "</a></p>\n");
	} else {
		page_begin(x, "New ticket");
	}
	if (fs->general[0]) {
		HL(h, "<p class=\"error\">");
		h_text(h, fs->general);
		HL(h, "</p>\n");
	}
	HL(h, "<form method=\"post\" action=\"");
	if (fs->cur) {
		HL(h, "/t/");
		h_id(h, &fs->cur->id);
	} else {
		HL(h, "/new");
	}
	HL(h, "\">");
	form_csrf(x);
	if (fs->cur) {
		HL(h, "<input type=\"hidden\" name=\"head\" value=\"");
		h_id(h, &fs->cur->head);
		HL(h, "\">");
	}
	HL(h, "\n<fieldset><legend>Ticket</legend>\n");
	render_fields(x, fs, 0, 4, &cals, NULL); /* title, body, status, labels */
	HL(h, "</fieldset>\n<fieldset><legend>Planning</legend>\n");
	render_fields(x, fs, 4, fs->n, &cals, NULL);
	HL(h, "</fieldset>\n<p><button>Save</button>");
	if (fs->cur) {
		HL(h, " <a href=\"/t/");
		h_id(h, &fs->cur->id);
		HL(h, "\">cancel</a>");
	}
	HL(h, "</p></form>\n");
	page_end(x);
	tix_entity_list_free(&cals);
}

static bool load_ticket(web_ctx *x, tix_entity **e)
{
	tix_oid id;
	if (!route_id(x, 1, TIX_ENTITY_TICKET, &id))
		return false;
	if (tix_entity_load(x->app->cfg.repo, TIX_ENTITY_TICKET, &id, e, &x->app->err)) {
		page_error(x, 404, "No such ticket", x->app->err.msg);
		return false;
	}
	return true;
}

void page_ticket_edit(web_ctx *x)
{
	tix_entity *e;
	if (!load_ticket(x, &e))
		return;
	fstate fs = {.type = TIX_ENTITY_TICKET, .defs = ticket_fields, .n = NFIELDS(ticket_fields), .cur = e};
	ticket_form_page(x, &fs, 200);
	fstate_free(&fs);
	tix_entity_free(e);
}

void page_new(web_ctx *x)
{
	fstate fs = {.type = TIX_ENTITY_TICKET, .defs = ticket_fields, .n = NFIELDS(ticket_fields)};
	const char *parent = web_kvs_get(&x->req->query, "parent");
	if (parent && *parent) {
		fs.prefill[find_field(&fs, "parent")] = parent;
		fs.prefill[find_field(&fs, "rank")] = "last";
	}
	ticket_form_page(x, &fs, 200);
	fstate_free(&fs);
}

static void apply_and_go(web_ctx *x, fstate *fs, op_vec *ops, tix_input *in, const tix_oid *id)
{
	for (size_t k = 0; k < in->ops.n; k++) {
		TIX_VEC_PUSH(ops, in->ops.v[k]); /* moved */
		memset(&in->ops.v[k], 0, sizeof(tix_op));
	}
	tix_status st = ops->n ? tix_entity_apply(x->app->cfg.repo, fs->type, id, ops->v, ops->n, &x->app->err) : TIX_OK;
	if (st) {
		snprintf(fs->general, sizeof(fs->general), "Not saved: %s", x->app->err.msg);
		return;
	}
	char path[TIX_OID_MAX_HEX + 8];
	char hex[TIX_OID_MAX_HEX + 1];
	snprintf(path, sizeof(path), "%s%s", fs->type == TIX_ENTITY_TICKET ? "/t/" : fs->type == TIX_ENTITY_RESOURCE ? "/res/" : "/cal/",
		 tix_oid_hex(id, hex));
	web_redirect(x, path);
}

void post_ticket(web_ctx *x)
{
	tix_entity *e;
	if (!load_ticket(x, &e))
		return;
	fstate fs = {.type = TIX_ENTITY_TICKET, .defs = ticket_fields, .n = NFIELDS(ticket_fields), .cur = e,
		     .posted = &x->req->form};
	op_vec ops = {0};
	tix_input in = {0};
	bool any;
	tix_input_ctx ctx = {x->app->cfg.repo, page_today(x)};
	bool ok = collect(x, &fs, &ops, &in, &any);
	if (ok && tix_input_finish(&ctx, &in, &e->id, &e->u.ticket.plan.parent, &x->app->err)) {
		snprintf(fs.errs[find_field(&fs, "rank")], sizeof(fs.errs[0]), "%s", x->app->err.msg);
		ok = false;
	}
	if (ok)
		apply_and_go(x, &fs, &ops, &in, &e->id);
	if (!ok || fs.general[0]) {
		tix_entity *fresh = NULL; /* show the form against the current state */
		if (!tix_entity_load(x->app->cfg.repo, TIX_ENTITY_TICKET, &e->id, &fresh, NULL)) {
			tix_entity_free(e);
			e = fresh;
			fs.cur = e;
		}
		ticket_form_page(x, &fs, 422);
	}
	ops_free(&ops);
	tix_input_free(&in);
	fstate_free(&fs);
	tix_entity_free(e);
}

void post_new(web_ctx *x)
{
	fstate fs = {.type = TIX_ENTITY_TICKET, .defs = ticket_fields, .n = NFIELDS(ticket_fields),
		     .posted = &x->req->form};
	op_vec ops = {0};
	tix_input in = {0};
	bool any;
	tix_input_ctx ctx = {x->app->cfg.repo, page_today(x)};
	const char *title = web_kvs_get(&x->req->form, "title"), *body = web_kvs_get(&x->req->form, "body");
	const char *labels = web_kvs_get(&x->req->form, "labels");
	bool ok = collect(x, &fs, &ops, &in, &any);
	if (!title || !*title) {
		snprintf(fs.errs[0], sizeof(fs.errs[0]), "A title is required.");
		ok = false;
	}
	if (ok && tix_input_finish(&ctx, &in, NULL, NULL, &x->app->err)) {
		snprintf(fs.errs[find_field(&fs, "rank")], sizeof(fs.errs[0]), "%s", x->app->err.msg);
		ok = false;
	}
	if (ok) {
		tix_strs ls = {0};
		split_labels(labels ? labels : "", &ls);
		op_vec all = {0};
		TIX_VEC_PUSH(&all, tix_op_ticket_create(NULL, title, body ? body : "", &ls));
		tix_strs_free(&ls);
		for (size_t k = 0; k < ops.n; k++)
			TIX_VEC_PUSH(&all, ops.v[k]);
		for (size_t k = 0; k < in.ops.n; k++)
			TIX_VEC_PUSH(&all, in.ops.v[k]);
		tix_oid id;
		tix_status st = tix_entity_create(x->app->cfg.repo, TIX_ENTITY_TICKET, all.v, all.n, &id, &x->app->err);
		tix_op_free(&all.v[0]);
		free(all.v); /* the rest are still owned by ops and in */
		if (st) {
			snprintf(fs.general, sizeof(fs.general), "Not created: %s", x->app->err.msg);
			ok = false;
		} else {
			char path[TIX_OID_MAX_HEX + 4], hex[TIX_OID_MAX_HEX + 1];
			snprintf(path, sizeof(path), "/t/%s", tix_oid_hex(&id, hex));
			web_redirect(x, path);
		}
	}
	if (!ok)
		ticket_form_page(x, &fs, 422);
	ops_free(&ops);
	tix_input_free(&in);
	fstate_free(&fs);
}

/* ---- resources and calendars (/res/new, /res/ID/edit, POST /res/ID; same for /cal) ------- */

static const char *kind_path(tix_entity_type t)
{
	return t == TIX_ENTITY_RESOURCE ? "/res" : "/cal";
}

static void named_form_page(web_ctx *x, fstate *fs, int status)
{
	tix_entity_list cals = {0}, idents = {0};
	if (tix_calendar_list(x->app->cfg.repo, &cals, &x->app->err) ||
	    tix_identity_list(x->app->cfg.repo, &idents, &x->app->err)) {
		page_error(x, 500, "Repository problem", x->app->err.msg);
		tix_entity_list_free(&cals);
		return;
	}
	bool res = fs->type == TIX_ENTITY_RESOURCE;
	x->res->status = status;
	html *h = &x->h;
	if (fs->cur)
		page_begin(x, res ? "Edit resource" : "Edit calendar");
	else
		page_begin(x, res ? "New resource" : "New calendar");
	if (fs->general[0]) {
		HL(h, "<p class=\"error\">");
		h_text(h, fs->general);
		HL(h, "</p>\n");
	}
	HL(h, "<form method=\"post\" action=\"");
	h_text(h, kind_path(fs->type));
	HL(h, "/");
	if (fs->cur)
		h_id(h, &fs->cur->id);
	else
		HL(h, "new");
	HL(h, "\">");
	form_csrf(x);
	if (fs->cur) {
		HL(h, "<input type=\"hidden\" name=\"head\" value=\"");
		h_id(h, &fs->cur->head);
		HL(h, "\">");
	}
	HL(h, "\n");
	render_fields(x, fs, 0, fs->n, &cals, &idents);
	HL(h, "<p><button>Save</button></p></form>\n");
	page_end(x);
	tix_entity_list_free(&cals);
	tix_entity_list_free(&idents);
}

static void named_setup(fstate *fs, tix_entity_type t)
{
	fs->type = t;
	fs->defs = t == TIX_ENTITY_RESOURCE ? res_fields : cal_fields;
	fs->n = t == TIX_ENTITY_RESOURCE ? NFIELDS(res_fields) : NFIELDS(cal_fields);
}

static bool load_named(web_ctx *x, tix_entity_type t, tix_entity **e)
{
	tix_oid id;
	if (!route_id(x, 1, t, &id))
		return false;
	if (tix_entity_load(x->app->cfg.repo, t, &id, e, &x->app->err)) {
		page_error(x, 404, "Not found", x->app->err.msg);
		return false;
	}
	return true;
}

void page_named_edit(web_ctx *x, tix_entity_type t)
{
	fstate fs = {0};
	named_setup(&fs, t);
	tix_entity *e = NULL;
	if (x->nsegs == 2 && !strcmp(x->segs[1], "new")) {
		named_form_page(x, &fs, 200);
	} else if (load_named(x, t, &e)) {
		fs.cur = e;
		named_form_page(x, &fs, 200);
	}
	fstate_free(&fs);
	tix_entity_free(e);
}

void post_named(web_ctx *x, tix_entity_type t)
{
	fstate fs = {0};
	named_setup(&fs, t);
	fs.posted = &x->req->form;
	tix_entity *e = NULL;
	bool creating = !strcmp(x->segs[1], "new");
	if (!creating) {
		if (!load_named(x, t, &e)) {
			fstate_free(&fs);
			return;
		}
		fs.cur = e;
	}
	op_vec ops = {0};
	tix_input in = {0};
	bool any;
	bool ok = collect(x, &fs, &ops, &in, &any);
	const char *name = web_kvs_get(&x->req->form, "name");
	if (creating && (!name || !*name)) {
		snprintf(fs.errs[0], sizeof(fs.errs[0]), "A name is required.");
		ok = false;
	}
	if (ok && creating) {
		op_vec all = {0};
		if (t == TIX_ENTITY_RESOURCE) {
			int k = tix_field_enum(TIX_F_RES_KIND, web_kvs_get(&x->req->form, "kind") ? web_kvs_get(&x->req->form, "kind") : "person");
			TIX_VEC_PUSH(&all, tix_op_resource_create(NULL, name, k < 0 ? TIX_RES_PERSON : (tix_resource_kind)k));
		} else {
			TIX_VEC_PUSH(&all, tix_op_calendar_create(NULL, name));
		}
		for (size_t k = 0; k < in.ops.n; k++)
			TIX_VEC_PUSH(&all, in.ops.v[k]);
		tix_oid id;
		tix_status st = tix_entity_create(x->app->cfg.repo, t, all.v, all.n, &id, &x->app->err);
		tix_op_free(&all.v[0]);
		free(all.v);
		if (st) {
			snprintf(fs.general, sizeof(fs.general), "Not created: %s", x->app->err.msg);
			ok = false;
		} else {
			char path[TIX_OID_MAX_HEX + 8], hex[TIX_OID_MAX_HEX + 1];
			snprintf(path, sizeof(path), "%s/%s", kind_path(t), tix_oid_hex(&id, hex));
			web_redirect(x, path);
		}
	} else if (ok) {
		apply_and_go(x, &fs, &ops, &in, &e->id);
		ok = !fs.general[0];
	}
	if (!ok)
		named_form_page(x, &fs, 422);
	ops_free(&ops);
	tix_input_free(&in);
	fstate_free(&fs);
	tix_entity_free(e);
}
