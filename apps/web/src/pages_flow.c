/* The agent workflow's pages (architecture §15.6): /inbox and /activity, the autonomy
 * banner, and typed comments with their answer forms. */
#include "app.h"

#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- helpers ------------------------------------------------------------------------------ */

/* Is t's stage tagged human on the list that applies to it? */
static bool waits_for_person(web_ctx *x, const tix_entity *t)
{
	if (!t->u.ticket.stage)
		return false;
	tix_entity *owner = NULL;
	if (tix_ticket_stages(x->app->cfg.repo, &t->id, NULL, &owner, NULL))
		return false;
	const tix_stage_def *d = tix_stage_find(owner, t->u.ticket.stage);
	bool human = d && d->human;
	tix_entity_free(owner);
	return human;
}

/* The number of questions in typed comments on t that no later comment replies to. */
static size_t open_questions(const tix_ticket *t)
{
	size_t n = 0;
	for (size_t i = 0; i < t->ncomments; i++) {
		const tix_comment *c = &t->comments[i];
		if (!c->data || !strstr(c->data, "\"questions\""))
			continue;
		bool answered = false;
		for (size_t k = i + 1; k < t->ncomments && !answered; k++)
			answered = !tix_opid_cmp(&t->comments[k].reply_to, &c->id);
		if (answered)
			continue;
		cJSON *o = cJSON_Parse(c->data);
		n += (size_t)cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(o, "questions"));
		cJSON_Delete(o);
	}
	return n;
}

static void ticket_cells(web_ctx *x, const tix_entity *e)
{
	html *h = &x->h;
	HL(h, "<td class=\"id\"><a href=\"/t/");
	h_id(h, &e->id);
	HL(h, "\">");
	h_short(h, &e->id);
	HL(h, "</a></td><td>");
	h_state(h, e->u.ticket.status, e->u.ticket.stage);
	HL(h, "</td><td><a href=\"/t/");
	h_id(h, &e->id);
	HL(h, "\">");
	h_text(h, e->u.ticket.title);
	HL(h, "</a></td>");
}

/* ---- /inbox ------------------------------------------------------------------------------- */

void page_inbox(web_ctx *x)
{
	html *h = &x->h;
	tix_entity_list l;
	tix_ticket_query q = {.sort = TIX_SORT_WBS};
	if (tix_ticket_list(x->app->cfg.repo, &q, &l, &x->app->err)) {
		page_error(x, 500, "Repository problem", x->app->err.msg);
		return;
	}
	int64_t now = web_now(x);
	page_begin(x, "Inbox");
	HL(h, "<p class=\"note\">What waits for a person, and what agents are working on. "
	      "<button id=\"tix-notify\" type=\"button\" hidden>Notify me when a ticket waits for me</button></p>\n");
	HL(h, "<h2>Waiting for you</h2>\n<table class=\"tickets inbox\"><thead><tr><th>ID</th><th>Status</th>"
	      "<th>Title</th><th>Why</th><th>Updated</th></tr></thead><tbody>\n");
	size_t waiting = 0;
	for (size_t i = 0; i < l.n; i++) {
		const tix_entity *e = l.v[i];
		if (e->u.ticket.status == TIX_TICKET_CANCELLED)
			continue;
		bool human = waits_for_person(x, e);
		size_t qn = open_questions(&e->u.ticket);
		if (!human && !qn)
			continue;
		waiting++;
		HL(h, "<tr>");
		ticket_cells(x, e);
		HL(h, "<td>");
		if (human)
			HL(h, "its stage waits for a person");
		if (human && qn)
			HL(h, "; ");
		if (qn) {
			h_int(h, (long long)qn);
			if (qn == 1)
				HL(h, " open question");
			else
				HL(h, " open questions");
		}
		HL(h, "</td><td>");
		h_time(h, e->updated);
		HL(h, "</td></tr>\n");
	}
	HL(h, "</tbody></table>\n");
	if (!waiting)
		HL(h, "<p class=\"note\">Nothing waits for you.</p>\n");

	HL(h, "<h2>Being worked on</h2>\n<table class=\"tickets claims\"><thead><tr><th>ID</th><th>Status</th>"
	      "<th>Title</th><th>Who</th><th>For</th><th>Left</th></tr></thead><tbody>\n");
	size_t claimed = 0;
	for (size_t i = 0; i < l.n; i++) {
		const tix_entity *e = l.v[i];
		const tix_claim *c = &e->u.ticket.claim;
		if (!tix_claim_live(c, now))
			continue;
		claimed++;
		HL(h, "<tr>");
		ticket_cells(x, e);
		HL(h, "<td>");
		h_who(x, &c->identity);
		HL(h, "</td><td>");
		h_int(h, (now - c->since) / 60);
		HL(h, " min</td><td>");
		h_int(h, (c->until - now + 59) / 60);
		HL(h, " min</td></tr>\n");
	}
	HL(h, "</tbody></table>\n");
	if (!claimed)
		HL(h, "<p class=\"note\">Nobody holds a claim.</p>\n");
	tix_entity_list_free(&l);
	page_end(x);
}

/* ---- /activity ---------------------------------------------------------------------------- */

void page_activity(web_ctx *x)
{
	html *h = &x->h;
	int64_t now = web_now(x), since = now - 86400;
	const char *sv = web_kvs_get(&x->req->query, "since");
	bool from_autonomy = !sv || !strcmp(sv, "autonomy");
	if (sv && !from_autonomy) {
		char *end;
		long v = strtol(sv, &end, 10);
		if (v > 0 && v < 100000 && (!strcmp(end, "h") || !strcmp(end, "d")))
			since = now - v * (*end == 'h' ? 3600 : 86400);
	}
	tix_activity_list l;
	tix_activity_query q = {0};
	if (tix_activity_log(x->app->cfg.repo, &q, &l, &x->app->err)) {
		page_error(x, 500, "Repository problem", x->app->err.msg);
		return;
	}
	if (from_autonomy) { /* since autonomy was last switched on, else the last day */
		bool found = false;
		for (size_t i = l.n; i-- > 0 && !found;)
			if (!strcmp(l.v[i].op, "set-autonomy") && !strcmp(l.v[i].summary, "autonomy autonomous")) {
				since = l.v[i].time;
				found = true;
			}
	}
	page_begin(x, "Activity");
	HL(h, "<p class=\"note\">Since ");
	h_time(h, since);
	if (from_autonomy)
		HL(h, " (autonomy last switched on, else the last day)");
	HL(h, " &middot; <a href=\"/activity?since=2h\">2h</a> <a href=\"/activity?since=24h\">24h</a> "
	      "<a href=\"/activity?since=7d\">7d</a> <a href=\"/activity\">since autonomy</a></p>\n");
	/* the overnight summary: how things ended up, and the typed comments by kind */
	size_t done = 0, blocked = 0, shown = 0;
	tix_strs kinds = {0};
	TIX_VEC(size_t) counts = {0};
	for (size_t i = 0; i < l.n; i++) {
		const tix_activity *a = &l.v[i];
		if (a->time < since)
			continue;
		shown++;
		done += !strncmp(a->summary, "status done", 11);
		blocked += !strncmp(a->summary, "status blocked", 14);
		if (!strcmp(a->op, "typed-comment") && a->summary[0] == '[') {
			char kind[TIX_STAGE_MAX + 1];
			size_t n = strcspn(a->summary + 1, "]");
			if (n && n <= TIX_STAGE_MAX) {
				memcpy(kind, a->summary + 1, n);
				kind[n] = '\0';
				size_t k = 0;
				while (k < kinds.n && strcmp(kinds.v[k], kind))
					k++;
				if (k == kinds.n) {
					tix_strs_add(&kinds, kind);
					size_t zero = 0;
					TIX_VEC_PUSH(&counts, zero);
				}
				counts.v[k]++;
			}
		}
	}
	HL(h, "<p class=\"summary\">");
	h_int(h, (long long)shown);
	HL(h, " changes; ");
	h_int(h, (long long)done);
	HL(h, " to done, ");
	h_int(h, (long long)blocked);
	HL(h, " to blocked");
	for (size_t k = 0; k < kinds.n; k++) {
		if (k)
			HL(h, ", ");
		else
			HL(h, "; notes: ");
		h_int(h, (long long)counts.v[k]);
		HL(h, " ");
		h_text(h, kinds.v[k]);
	}
	HL(h, "</p>\n<table class=\"activity\"><thead><tr><th>When</th><th>Ticket</th><th>What</th><th>Who</th>"
	      "</tr></thead><tbody>\n");
	for (size_t i = l.n; i-- > 0;) { /* newest first */
		const tix_activity *a = &l.v[i];
		if (a->time < since)
			continue;
		HL(h, "<tr><td>");
		h_time(h, a->time);
		HL(h, "</td><td><a href=\"/t/");
		h_id(h, &a->ticket);
		HL(h, "\">");
		h_short(h, &a->ticket);
		HL(h, "</a> ");
		h_text(h, a->title);
		HL(h, "</td><td>");
		h_text(h, a->summary);
		HL(h, "</td><td>");
		h_who(x, &a->author);
		HL(h, "</td></tr>\n");
	}
	HL(h, "</tbody></table>\n");
	tix_strs_free(&kinds);
	free(counts.v);
	tix_activity_list_free(&l);
	page_end(x);
}

/* ---- the autonomy banner (in every page's header) ----------------------------------------- */

void h_autonomy_banner(web_ctx *x)
{
	html *h = &x->h;
	tix_entity_list l;
	if (tix_ticket_list(x->app->cfg.repo, NULL, &l, NULL))
		return;
	int64_t now = web_now(x);
	for (size_t i = 0; i < l.n; i++) {
		const tix_ticket *t = &l.v[i]->u.ticket;
		if (!t->autonomy_set || t->autonomy == TIX_AUTO_SUPERVISED)
			continue;
		bool expired = t->autonomy == TIX_AUTO_AUTONOMOUS && t->autonomy_until >= 0 && t->autonomy_until <= now;
		if (expired)
			continue;
		HL(h, "<p class=\"autonomy");
		if (t->autonomy == TIX_AUTO_HALTED)
			HL(h, " halted");
		HL(h, "\"><a href=\"/t/");
		h_id(h, &l.v[i]->id);
		HL(h, "\">");
		h_text(h, t->title);
		HL(h, "</a>: ");
		h_text(h, tix_autonomy_mode_name(t->autonomy));
		if (t->autonomy == TIX_AUTO_AUTONOMOUS && t->autonomy_until >= 0) {
			HL(h, " until ");
			h_time(h, t->autonomy_until);
		}
		HL(h, " &middot; <a href=\"/activity\">activity</a> &middot; <a href=\"/inbox\">inbox</a></p>");
	}
	tix_entity_list_free(&l);
}

/* ---- typed comments ------------------------------------------------------------------------ */

void h_note(web_ctx *x, const tix_entity *e, const tix_comment *c)
{
	html *h = &x->h;
	const tix_ticket *t = &e->u.ticket;
	char opid[TIX_OID_MAX_HEX + 12];
	cJSON *o = c->data ? cJSON_Parse(c->data) : NULL;
	const cJSON *it;
	const cJSON *fields = cJSON_GetObjectItemCaseSensitive(o, "fields");
	if (cJSON_GetArraySize(fields) > 0) {
		HL(h, "<dl class=\"note-fields\">");
		cJSON_ArrayForEach(it, fields)
		{
			HL(h, "<dt>");
			h_text(h, it->string);
			HL(h, "</dt><dd>");
			h_text(h, cJSON_IsString(it) ? it->valuestring : "");
			HL(h, "</dd>");
		}
		HL(h, "</dl>");
	}
	const cJSON *items = cJSON_GetObjectItemCaseSensitive(o, "items");
	if (cJSON_GetArraySize(items) > 0) {
		HL(h, "<ul class=\"note-items\">");
		cJSON_ArrayForEach(it, items)
		{
			const cJSON *id = cJSON_GetObjectItemCaseSensitive(it, "id");
			const cJSON *tx = cJSON_GetObjectItemCaseSensitive(it, "text");
			if (!cJSON_IsString(id) || !cJSON_IsString(tx))
				continue;
			char *st = tix_note_item_state(t, c, id->valuestring);
			HL(h, "<li><span class=\"status\">");
			h_text(h, st ? st : "?");
			HL(h, "</span> <b>");
			h_text(h, id->valuestring);
			HL(h, "</b> ");
			h_text(h, tx->valuestring);
			HL(h, "</li>");
			free(st);
		}
		HL(h, "</ul>");
	}
	const cJSON *qs = cJSON_GetObjectItemCaseSensitive(o, "questions");
	if (cJSON_GetArraySize(qs) > 0) {
		bool answered = false;
		for (size_t k = 0; k < t->ncomments && !answered; k++)
			answered = !tix_opid_cmp(&t->comments[k].reply_to, &c->id);
		bool form = web_can_write(x);
		if (form) {
			HL(h, "<form class=\"answer\" method=\"post\" action=\"/t/");
			h_id(h, &e->id);
			HL(h, "/answer\">");
			form_csrf(x);
			HL(h, "<input type=\"hidden\" name=\"reply-to\" value=\"");
			h_text(h, tix_opid_str(&c->id, opid));
			HL(h, "\">");
		}
		HL(h, "<ol class=\"note-questions\">");
		cJSON_ArrayForEach(it, qs)
		{
			const cJSON *id = cJSON_GetObjectItemCaseSensitive(it, "id");
			const cJSON *tx = cJSON_GetObjectItemCaseSensitive(it, "text");
			if (!cJSON_IsString(id) || !cJSON_IsString(tx))
				continue;
			HL(h, "<li><label>");
			h_text(h, tx->valuestring);
			if (form) {
				HL(h, "<br><textarea rows=\"2\" cols=\"60\" name=\"a.");
				h_text(h, id->valuestring);
				HL(h, "\"></textarea>");
			}
			HL(h, "</label></li>");
		}
		HL(h, "</ol>");
		if (form) {
			HL(h, "<button>");
			HL(h, "Answer");
			HL(h, "</button>");
			if (answered)
				HL(h, " <span class=\"note\">(answered already; answering again adds another reply)</span>");
			HL(h, "</form>");
		} else if (answered) {
			HL(h, "<p class=\"note\">Answered.</p>");
		}
	}
	const cJSON *ups = cJSON_GetObjectItemCaseSensitive(o, "updates");
	if (cJSON_GetArraySize(ups) > 0) {
		HL(h, "<ul class=\"note-updates\">");
		cJSON_ArrayForEach(it, ups)
		{
			const cJSON *ref = cJSON_GetObjectItemCaseSensitive(it, "ref");
			const cJSON *st = cJSON_GetObjectItemCaseSensitive(it, "state");
			if (!cJSON_IsString(ref) || !cJSON_IsString(st))
				continue;
			const char *hash = strchr(ref->valuestring, '#');
			HL(h, "<li>");
			h_text(h, hash ? hash + 1 : ref->valuestring);
			HL(h, " &rarr; <span class=\"status\">");
			h_text(h, st->valuestring);
			HL(h, "</span></li>");
		}
		HL(h, "</ul>");
	}
	cJSON_Delete(o);
}

/* POST /t/ID/answer: reply-to plus a.<question id> fields -> a typed comment of kind
 * "answer" whose fields map question ids to the answers given (empty ones left out). */
void post_answer(web_ctx *x)
{
	tix_oid id;
	tix_entity *e = NULL;
	if (tix_entity_resolve(x->app->cfg.repo, TIX_ENTITY_TICKET, x->segs[1], &id, NULL) ||
	    tix_entity_load(x->app->cfg.repo, TIX_ENTITY_TICKET, &id, &e, NULL)) {
		page_error(x, 404, "No such ticket", "It doesn't validate, so it can't be answered.");
		return;
	}
	const char *rt = web_kvs_get(&x->req->form, "reply-to");
	tix_opid reply;
	if (!rt || !tix_opid_parse(&reply, rt)) {
		tix_entity_free(e);
		page_error(x, 422, "Not saved", "The answer form lost its question.");
		return;
	}
	tix_buf data = TIX_BUF_INIT;
	tix_jw w;
	size_t n = 0;
	tix_jw_init(&w, &data);
	tix_jw_obj_begin(&w);
	tix_jw_key(&w, "fields");
	tix_jw_obj_begin(&w);
	for (size_t i = 0; i < x->req->form.n; i++) {
		const web_kv *kv = &x->req->form.v[i];
		if (strncmp(kv->name, "a.", 2) || !kv->value[0])
			continue;
		tix_jw_key(&w, kv->name + 2);
		tix_jw_str(&w, kv->value);
		n++;
	}
	tix_jw_obj_end(&w);
	tix_jw_obj_end(&w);
	tix_status st = TIX_OK;
	if (!n) {
		st = tix_error_set(&x->app->err, TIX_ERR_INVALID, "Write at least one answer.");
	} else if (!(st = tix_note_data_check(tix_buf_cstr(&data), &x->app->err))) {
		tix_op op = tix_op_typed_comment("answer", "", tix_buf_cstr(&data), &reply);
		st = tix_entity_apply(x->app->cfg.repo, TIX_ENTITY_TICKET, &id, &op, 1, &x->app->err);
		tix_op_free(&op);
	}
	tix_buf_free(&data);
	if (st) {
		char msg[400];
		snprintf(msg, sizeof(msg), "%s", x->app->err.msg);
		tix_entity_free(e);
		page_error(x, 422, "Not saved", msg);
		return;
	}
	char to[TIX_OID_MAX_HEX + 8], hex[TIX_OID_MAX_HEX + 1];
	snprintf(to, sizeof(to), "/t/%s", tix_oid_hex(&id, hex));
	tix_entity_free(e);
	web_redirect(x, to);
}
