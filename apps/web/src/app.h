/*
 * The tix web app (docs/architecture.md section 13): request checks, sessions, routing and
 * pages on top of core. A composition root like apps/cli: no business rules here.
 */
#ifndef TIX_WEB_APP_H
#define TIX_WEB_APP_H

#include "html.h"
#include "tix/input.h"
#include "web_http.h"

#define WEB_TOKEN_HEX 64 /* 32 random bytes */
#define WEB_MAX_SESSIONS 16
#define WEB_MAX_HOSTS 3

typedef struct web_session {
	char id[WEB_TOKEN_HEX + 1];
	char csrf[WEB_TOKEN_HEX + 1];
	uint64_t used; /* last use, as a counter (for eviction) */
} web_session;

typedef struct web_app_config {
	tix_repo *repo; /* BORROWS */
	void (*random)(void *ctx, uint8_t *out, size_t n);
	void *random_ctx;
	tix_date (*today)(void *ctx);
	void *today_ctx;
	bool read_only;
} web_app_config;

typedef struct web_app {
	web_app_config cfg;
	tix_error err;
	char token[WEB_TOKEN_HEX + 1]; /* the login token printed at start */
	char hosts[WEB_MAX_HOSTS][64]; /* accepted Host header values */
	size_t nhosts;
	web_session sessions[WEB_MAX_SESSIONS];
	size_t nsessions;
	uint64_t clock;
	char always_headers[1024]; /* security headers on every response */
	char css_url[64];          /* "/assets/tix-<hash>.css" */
} web_app;

/* Fills app (makes the login token). */
void web_app_init(web_app *app, const web_app_config *cfg);
/* Sets the accepted Host values once the port is known: "127.0.0.1:P" or "[::1]:P", plus
 * "localhost:P". */
void web_app_set_listen(web_app *app, const char *host, int port);

/* The web_handler. */
void web_app_handle(void *app, const web_request *req, web_response *res);

/* ---- for the page handlers (pages_*.c) ---- */

typedef struct web_ctx {
	web_app *app;
	const web_request *req;
	web_response *res;
	const web_session *session; /* NULL before login */
	html h;
	char segs[4][96]; /* path segments, at most 4 */
	size_t nsegs;
} web_ctx;

/* Standard page frame. title is plain text (escaped). */
void page_begin(web_ctx *x, const char *title);
void page_end(web_ctx *x);
/* A complete error page with status. msg is plain text. */
void page_error(web_ctx *x, int status, const char *title, const char *msg);
/* 303 to a path we built ourselves (no CR/LF). */
void web_redirect(web_ctx *x, const char *path);
/* <input type=hidden name=csrf ...> for this session's forms. */
void form_csrf(web_ctx *x);

/* Assets (assets.c). RETURNS false when name isn't one. */
bool asset_find(const char *name, const char **type, const char **body, size_t *len);
const char *asset_css_name(void); /* "tix-<hash>.css" */

/* ---- shared page helpers (util.c) ---- */

/* Resolves path segment i (a full id or a prefix) of type t. On failure writes a 404 page
 * and RETURNS false. */
bool route_id(web_ctx *x, size_t seg, tix_entity_type t, tix_oid *out);
/* Resolves an optional query field (e.g. root=ID); "" or absent -> *have = false. Writes a
 * 404 page and RETURNS false for an id that doesn't resolve. */
bool query_id(web_ctx *x, const char *field, tix_entity_type t, tix_oid *out, bool *have);
/* today, or the page's ?today=D. */
tix_date page_today(web_ctx *x);
/* Runs the planner (root may be NULL). Writes a 500 page and RETURNS false on failure. */
bool page_plan(web_ctx *x, const tix_oid *root, tix_plan_result *out);

/* "2026-11-06 14:03 UTC" */
void h_time(html *h, int64_t t);
void h_status(html *h, tix_ticket_status s);
/* <a href="/t/ID">short</a> title, from a list (or loaded when list is NULL). */
void h_ticket(web_ctx *x, const tix_oid *id, const tix_entity_list *known);
void h_who(web_ctx *x, const tix_oid *identity);
void h_named(web_ctx *x, tix_entity_type t, const tix_oid *id, const tix_entity_list *known);
const tix_entity *list_find(const tix_entity_list *l, const tix_oid *id);
void h_labels(html *h, const tix_strs *labels);
/* Days, hours, percent; nothing for unknown (< 0). */
void h_days(html *h, int64_t d);
void h_hours(html *h, int64_t hrs);

/* SVG Gantt of a schedule (gantt.c). */
void h_gantt(web_ctx *x, const tix_schedule *s, tix_date today, bool critical_only);

/* Page handlers (pages_*.c). Each handles GET (and POST where it has a form). */
void page_tickets(web_ctx *x);
void page_ticket(web_ctx *x);
void page_wbs(web_ctx *x);
void page_plan_view(web_ctx *x);
void page_load(web_ctx *x);
void page_resources(web_ctx *x);
void page_resource(web_ctx *x);
void page_calendars(web_ctx *x);
void page_calendar(web_ctx *x);
void page_identities(web_ctx *x);
void page_export(web_ctx *x);

#endif
