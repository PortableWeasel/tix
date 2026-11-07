/*
 * The tix web app (docs/architecture.md section 13): request checks, sessions, routing and
 * pages on top of core. A composition root like apps/cli: no business rules here.
 */
#ifndef TIX_WEB_APP_H
#define TIX_WEB_APP_H

#include "html.h"
#include "tix/base_json.h"
#include "tix/input.h"
#include "web_http.h"

#define WEB_TOKEN_HEX 64 /* 32 random bytes */
#define WEB_MAX_SESSIONS 64
#define WEB_MAX_HOSTS 3
#define WEB_MAX_USER 128

/* A login the proxy may send, and the tix identity its writes are authored by. */
typedef struct web_user {
	const char *login;
	tix_oid identity;
} web_user;

/* Hosted mode (architecture §13.8): behind a reverse proxy that authenticates (Authentik). */
typedef struct web_hosted {
	const char *header;       /* request header with the login, e.g. "X-authentik-username" */
	const char *origin;       /* "https://tix.example.org": the only Host/Origin accepted */
	const char *const *from;  /* peer addresses the proxy connects from (exact) */
	size_t nfrom;
	const web_user *users;    /* logins that may write; anyone else the proxy lets in may read */
	size_t nusers;
	const char *logout_url;   /* optional "Sign out" link (the proxy's) */
} web_hosted;

typedef struct web_session {
	char id[WEB_TOKEN_HEX + 1];
	char csrf[WEB_TOKEN_HEX + 1];
	uint64_t used; /* last use, as a counter (for eviction) */
	/* hosted mode: whose session this is (one per login; no cookie) */
	char user[WEB_MAX_USER + 1];
	bool mapped;     /* the login maps to a tix identity: may write */
	tix_oid identity;
} web_session;

typedef struct web_app_config {
	tix_repo *repo; /* BORROWS */
	void (*random)(void *ctx, uint8_t *out, size_t n);
	void *random_ctx;
	tix_date (*today)(void *ctx);
	void *today_ctx;
	bool read_only;
	const char *remote; /* for sync: git config tix.remote, else "origin" */
	const web_hosted *hosted; /* NULL: local mode (loopback, login token) */
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
	char js_url[64];           /* "/assets/tix-<hash>.js" */
	char stamp[TIX_STAMP_LEN]; /* last repository stamp sent to event streams */
} web_app;

/* Fills app (makes the login token). */
void web_app_init(web_app *app, const web_app_config *cfg);
/* Sets the accepted Host values once the port is known: "127.0.0.1:P" or "[::1]:P", plus
 * "localhost:P". */
void web_app_set_listen(web_app *app, const char *host, int port);

/* The web_handler. */
void web_app_handle(void *app, const web_request *req, web_response *res);
/* web_server_opts.tick: refreshes, and emits a stamp event when the repository changed. */
void web_app_tick(void *app, tix_buf *event);

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
/* Whether forms that change things should be shown: not --read-only, and in hosted mode
 * only to logins linked to an identity. (The POST checks are in web_app_handle.) */
bool web_can_write(const web_ctx *x);
/* <input type=hidden name=csrf ...> for this session's forms. */
void form_csrf(web_ctx *x);

/* Assets (assets.c). RETURNS false when name isn't one. */
bool asset_find(const char *name, const char **type, const char **body, size_t *len);
const char *asset_css_name(void); /* "tix-<hash>.css" */
const char *asset_js_name(void);  /* "tix-<hash>.js": progressive enhancement only (M7) */

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
/* v: a baseline comparison (one per task) to draw as thin bars under the tasks; NULL = none. */
void h_gantt(web_ctx *x, const tix_schedule *s, tix_date today, bool critical_only, const tix_plan_variance *v);

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

/* Forms (forms.c, actions.c). */
void page_new(web_ctx *x);
void post_new(web_ctx *x);
void page_ticket_edit(web_ctx *x);
void post_ticket(web_ctx *x);
void post_comment(web_ctx *x);
void page_comment_edit(web_ctx *x);
void post_dep(web_ctx *x);
void post_assign(web_ctx *x);
void page_named_edit(web_ctx *x, tix_entity_type t);
void post_named(web_ctx *x, tix_entity_type t);
void post_exception(web_ctx *x);
void page_sync(web_ctx *x);
void post_sync(web_ctx *x);
void page_api_tickets(web_ctx *x);
void page_baselines(web_ctx *x);
void post_baseline_take(web_ctx *x);
void post_baseline_rename(web_ctx *x);
void page_events(web_ctx *x);

#endif
