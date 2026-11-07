/*
 * What app.js talks to (M7, architecture section 13.4): a JSON ticket search and the
 * live-refresh event stream. Both are behind the same session check as every page, and
 * neither changes anything: edits from JavaScript post the ordinary forms.
 */
#include "app.h"
#include "tix/base_sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define API_LIMIT_DEFAULT 20
#define API_LIMIT_MAX 50

static void json_headers(web_ctx *x)
{
	web_header(x->res, "Content-Type", "application/json");
	web_header(x->res, "Cache-Control", "no-store");
}

/*
 * GET /api/tickets?q=TEXT&limit=N&status=S...  ->
 *   {"tickets":[{"id":"<hex>","short":"<7>","title":"...","status":"open","stage":null},...],"more":false}
 * Same matching as the ticket list (id prefix or title substring), active first, then WBS
 * order. Without status, every status matches (pickers need done tickets too).
 */
void page_api_tickets(web_ctx *x)
{
	const web_kvs *qs = &x->req->query;
	tix_ticket_query q = {.text = web_kvs_get(qs, "q"), .sort = TIX_SORT_STATUS};
	const char *st[8];
	size_t nst = web_kvs_all(qs, "status", st, 8);
	for (size_t i = 0; i < nst; i++) {
		tix_ticket_status s;
		if (tix_ticket_status_parse(st[i], &s))
			q.status_mask |= 1u << s;
	}
	long limit = API_LIMIT_DEFAULT;
	const char *ls = web_kvs_get(qs, "limit");
	if (ls && *ls) {
		char *end;
		limit = strtol(ls, &end, 10);
		if (*end || limit < 1)
			limit = API_LIMIT_DEFAULT;
		if (limit > API_LIMIT_MAX)
			limit = API_LIMIT_MAX;
	}
	tix_entity_list l;
	if (tix_ticket_list(x->app->cfg.repo, &q, &l, &x->app->err)) {
		x->res->status = 500;
		json_headers(x);
		tix_jw w;
		tix_jw_init(&w, &x->res->body);
		tix_jw_obj_begin(&w);
		tix_jw_key(&w, "error");
		tix_jw_str(&w, x->app->err.msg); /* our own message: valid UTF-8 */
		tix_jw_obj_end(&w);
		return;
	}
	json_headers(x);
	tix_jw w;
	tix_jw_init(&w, &x->res->body);
	tix_jw_obj_begin(&w);
	tix_jw_key(&w, "tickets");
	tix_jw_arr_begin(&w);
	char hex[TIX_OID_MAX_HEX + 1];
	for (size_t i = 0; i < l.n && i < (size_t)limit; i++) {
		const tix_entity *e = l.v[i];
		tix_oid_hex(&e->id, hex);
		tix_jw_obj_begin(&w);
		tix_jw_key(&w, "id");
		tix_jw_str(&w, hex);
		hex[7] = '\0';
		tix_jw_key(&w, "short");
		tix_jw_str(&w, hex);
		tix_jw_key(&w, "title");
		tix_jw_str(&w, e->u.ticket.title); /* validated UTF-8 (model) */
		tix_jw_key(&w, "status");
		tix_jw_str(&w, tix_ticket_status_name(e->u.ticket.status));
		tix_jw_key(&w, "stage");
		if (e->u.ticket.stage)
			tix_jw_str(&w, e->u.ticket.stage);
		else
			tix_jw_null(&w);
		tix_jw_obj_end(&w);
	}
	tix_jw_arr_end(&w);
	tix_jw_key(&w, "more");
	tix_jw_bool(&w, l.n > (size_t)limit);
	tix_jw_obj_end(&w);
	tix_entity_list_free(&l);
}

static void stamp_event(tix_buf *b, const char *stamp)
{
	tix_buf_adds(b, "event: stamp\ndata: ");
	tix_buf_adds(b, stamp);
	tix_buf_adds(b, "\n\n");
}

/* Every ticket's head now. RETURNS false if the store can't list them. */
static bool snapshot_heads(web_app *app, web_head **out, size_t *n)
{
	tix_refs refs;
	const char *prefix = tix_entity_ref_prefix(TIX_ENTITY_TICKET);
	tix_store *s = tix_repo_store(app->cfg.repo);
	if (tix_store_ref_list(s, prefix, &refs, NULL))
		return false;
	size_t plen = strlen(prefix);
	*out = tix_xcalloc(refs.n + 1, sizeof(**out));
	*n = 0;
	for (size_t i = 0; i < refs.n; i++) {
		const char *hex = refs.v[i].name + plen;
		if (!tix_oid_from_hex(&(*out)[*n].id, hex, strlen(hex)))
			continue;
		(*out)[(*n)++].head = refs.v[i].oid;
	}
	tix_refs_free(&refs);
	return true;
}

static bool head_unchanged(const web_app *app, const web_head *h)
{
	for (size_t i = 0; i < app->nheads; i++)
		if (tix_oid_eq(&app->heads[i].id, &h->id))
			return tix_oid_eq(&app->heads[i].head, &h->head);
	return false;
}

/* event: tickets  data: {"changed":[{"id","title","human"}]}: which tickets moved since the
 * last event, and whether each now waits for a person (§15.6). Ticket pages refresh only
 * for their own ticket; app.js may notify for human ones. */
static void tickets_event(web_app *app, tix_buf *b, const web_head *now, size_t n)
{
	tix_buf data = TIX_BUF_INIT;
	tix_jw w;
	char hex[TIX_OID_MAX_HEX + 1];
	size_t count = 0;
	tix_jw_init(&w, &data);
	tix_jw_obj_begin(&w);
	tix_jw_key(&w, "changed");
	tix_jw_arr_begin(&w);
	for (size_t i = 0; i < n && count < 50; i++) {
		if (head_unchanged(app, &now[i]))
			continue;
		tix_entity *e;
		if (tix_entity_load(app->cfg.repo, TIX_ENTITY_TICKET, &now[i].id, &e, NULL))
			continue;
		bool human = false;
		tix_entity *owner = NULL;
		if (e->u.ticket.stage && !tix_ticket_stages(app->cfg.repo, &e->id, NULL, &owner, NULL)) {
			const tix_stage_def *d = tix_stage_find(owner, e->u.ticket.status, e->u.ticket.stage);
			human = d && d->human;
		}
		tix_entity_free(owner);
		tix_jw_obj_begin(&w);
		tix_jw_key(&w, "id");
		tix_jw_str(&w, tix_oid_hex(&e->id, hex));
		tix_jw_key(&w, "title");
		tix_jw_str(&w, e->u.ticket.title);
		tix_jw_key(&w, "human");
		tix_jw_bool(&w, human);
		tix_jw_obj_end(&w);
		tix_entity_free(e);
		count++;
	}
	tix_jw_arr_end(&w);
	tix_jw_obj_end(&w);
	if (count) {
		tix_buf_adds(b, "event: tickets\ndata: ");
		tix_buf_add(b, data.data, data.len);
		tix_buf_adds(b, "\n\n");
	}
	tix_buf_free(&data);
}

static void remember_heads(web_app *app, web_head *now, size_t n)
{
	free(app->heads);
	app->heads = now;
	app->nheads = n;
}

/* GET /events: text/event-stream. The first event is the current stamp; then one each
 * time it changes (web_app_tick). Pages carry their stamp in <body data-stamp>. */
void page_events(web_ctx *x)
{
	char stamp[TIX_STAMP_LEN];
	if (tix_repo_stamp(x->app->cfg.repo, stamp, &x->app->err)) {
		page_error(x, 500, "Repository problem", x->app->err.msg);
		return;
	}
	memcpy(x->app->stamp, stamp, sizeof(stamp));
	web_head *now;
	size_t n;
	if (snapshot_heads(x->app, &now, &n))
		remember_heads(x->app, now, n);
	web_header(x->res, "Content-Type", "text/event-stream");
	web_header(x->res, "Cache-Control", "no-store");
	tix_buf_adds(&x->res->body, "retry: 5000\n");
	stamp_event(&x->res->body, stamp);
	x->res->stream = true;
}

/* Pulls from the configured remote (a timed pull or a webhook). RETURNS false on failure. */
static bool pull_now(web_app *app, tix_error *err)
{
	if (!app->cfg.remote)
		return tix_error_set(err, TIX_ERR_NOT_FOUND, "no remote configured"), false;
	tix_sync_report rep = {0};
	tix_status st = tix_pull(app->cfg.repo, app->cfg.remote, &rep, err);
	tix_sync_report_free(&rep);
	return st == TIX_OK;
}

void web_app_tick(void *vapp, tix_buf *event)
{
	web_app *app = vapp;
	char stamp[TIX_STAMP_LEN];
	if (app->cfg.pull_every && app->cfg.now) {
		int64_t now = app->cfg.now(app->cfg.now_ctx);
		if (now - app->last_pull >= app->cfg.pull_every) {
			app->last_pull = now;
			(void)pull_now(app, NULL); /* a failed pull is retried next interval */
		}
	}
	if (tix_repo_refresh(app->cfg.repo, NULL) || tix_repo_stamp(app->cfg.repo, stamp, NULL))
		return; /* try again next tick */
	if (!strcmp(stamp, app->stamp))
		return;
	memcpy(app->stamp, stamp, sizeof(stamp));
	web_head *now;
	size_t n;
	if (snapshot_heads(app, &now, &n)) {
		tickets_event(app, event, now, n);
		remember_heads(app, now, n);
	}
	stamp_event(event, stamp);
}

void web_hook_forgejo(web_ctx *x)
{
	web_app *app = x->app;
	const web_request *req = x->req;
	uint8_t want[TIX_SHA256_LEN], got[TIX_SHA256_LEN];
	bool ok = req->json && req->signature && strlen(req->signature) == 2 * TIX_SHA256_LEN;
	for (size_t i = 0; ok && i < TIX_SHA256_LEN; i++) {
		unsigned v;
		char two[3] = {req->signature[2 * i], req->signature[2 * i + 1], 0};
		ok = strspn(two, "0123456789abcdefABCDEF") == 2 && sscanf(two, "%2x", &v) == 1;
		got[i] = (uint8_t)v;
	}
	if (ok) {
		tix_hmac_sha256(app->cfg.hook_secret, app->cfg.hook_secret_len, req->json, req->json_len, want);
		ok = tix_bytes_eq_ct(want, got, TIX_SHA256_LEN);
	}
	if (!ok) {
		page_error(x, 403, "Forbidden", "Missing or wrong webhook signature.");
		return;
	}
	if (!pull_now(app, &app->err)) {
		page_error(x, 502, "Pull failed", app->err.msg);
		return;
	}
	x->res->status = 204;
}
