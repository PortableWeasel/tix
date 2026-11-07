/*
 * What app.js talks to (M7, architecture section 13.4): a JSON ticket search and the
 * live-refresh event stream. Both are behind the same session check as every page, and
 * neither changes anything: edits from JavaScript post the ordinary forms.
 */
#include "app.h"

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
	web_header(x->res, "Content-Type", "text/event-stream");
	web_header(x->res, "Cache-Control", "no-store");
	tix_buf_adds(&x->res->body, "retry: 5000\n");
	stamp_event(&x->res->body, stamp);
	x->res->stream = true;
}

void web_app_tick(void *vapp, tix_buf *event)
{
	web_app *app = vapp;
	char stamp[TIX_STAMP_LEN];
	if (tix_repo_refresh(app->cfg.repo, NULL) || tix_repo_stamp(app->cfg.repo, stamp, NULL))
		return; /* try again next tick */
	if (!strcmp(stamp, app->stamp))
		return;
	memcpy(app->stamp, stamp, sizeof(stamp));
	stamp_event(event, stamp);
}
