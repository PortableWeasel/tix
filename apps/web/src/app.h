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

/* Page handlers. Each handles GET (and POST where it has a form). */
void page_home(web_ctx *x);

#endif
