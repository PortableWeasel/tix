/*
 * Request checks, sessions and routing (docs/architecture.md section 13.6). Order for every
 * request: Host (421), then assets and login (no session needed), then the session cookie
 * (401), then for POSTs the Origin/Referer and the form's csrf token (403).
 */
#include "app.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

static const char hexdig[] = "0123456789abcdef";

static void random_hex(web_app *app, char out[WEB_TOKEN_HEX + 1])
{
	uint8_t b[WEB_TOKEN_HEX / 2];
	app->cfg.random(app->cfg.random_ctx, b, sizeof(b));
	for (size_t i = 0; i < sizeof(b); i++) {
		out[2 * i] = hexdig[b[i] >> 4];
		out[2 * i + 1] = hexdig[b[i] & 15];
	}
	out[WEB_TOKEN_HEX] = '\0';
}

/* Constant time in the length of b (the secret). */
static bool secret_eq(const char *a, const char *b)
{
	size_t na = strlen(a), nb = strlen(b);
	unsigned diff = na != nb;
	for (size_t i = 0; i < nb; i++)
		diff |= (unsigned)(unsigned char)(i < na ? a[i] : 0) ^ (unsigned char)b[i];
	return diff == 0;
}

void web_app_init(web_app *app, const web_app_config *cfg)
{
	memset(app, 0, sizeof(*app));
	app->cfg = *cfg;
	app->err = (tix_error)TIX_ERROR_INIT;
	random_hex(app, app->token);
	snprintf(app->css_url, sizeof(app->css_url), "/assets/%s", asset_css_name());
	snprintf(app->always_headers, sizeof(app->always_headers),
		 "Content-Security-Policy: default-src 'none'; style-src 'self'; script-src 'self'; img-src 'self'; "
		 "connect-src 'self'; form-action 'self'; frame-ancestors 'none'; base-uri 'none'\r\n"
		 "X-Content-Type-Options: nosniff\r\n"
		 "X-Frame-Options: DENY\r\n"
		 "Referrer-Policy: no-referrer\r\n"
		 "Cross-Origin-Opener-Policy: same-origin\r\n"
		 "Cross-Origin-Resource-Policy: same-origin\r\n");
}

void web_app_set_listen(web_app *app, const char *host, int port)
{
	app->nhosts = 0;
	bool v6 = strchr(host, ':') != NULL;
	snprintf(app->hosts[app->nhosts++], sizeof(app->hosts[0]), v6 ? "[%s]:%d" : "%s:%d", host, port);
	snprintf(app->hosts[app->nhosts++], sizeof(app->hosts[0]), "localhost:%d", port);
}

static bool host_ok(const web_app *app, const char *host)
{
	for (size_t i = 0; host && i < app->nhosts; i++)
		if (!strcasecmp(host, app->hosts[i]))
			return true;
	return false;
}

/* ---- sessions ---------------------------------------------------------------------- */

static web_session *session_new(web_app *app)
{
	web_session *s;
	if (app->nsessions < WEB_MAX_SESSIONS) {
		s = &app->sessions[app->nsessions++];
	} else { /* evict the least recently used */
		s = &app->sessions[0];
		for (size_t i = 1; i < app->nsessions; i++)
			if (app->sessions[i].used < s->used)
				s = &app->sessions[i];
	}
	random_hex(app, s->id);
	random_hex(app, s->csrf);
	s->used = ++app->clock;
	return s;
}

static web_session *session_find(web_app *app, const web_request *req)
{
	const char *id = web_kvs_get(&req->cookies, "tix_session");
	if (!id)
		return NULL;
	for (size_t i = 0; i < app->nsessions; i++)
		if (secret_eq(id, app->sessions[i].id)) {
			app->sessions[i].used = ++app->clock;
			return &app->sessions[i];
		}
	return NULL;
}

/* The request comes from one of our own pages: Origin, or failing that Referer. */
static bool same_origin(const web_request *req)
{
	char want[96];
	snprintf(want, sizeof(want), "http://%s", req->host);
	if (req->origin)
		return !strcasecmp(req->origin, want);
	if (req->referer) {
		size_t n = strlen(want);
		return !strncasecmp(req->referer, want, n) && req->referer[n] == '/';
	}
	return false;
}

/* ---- page frame -------------------------------------------------------------------- */

void page_begin(web_ctx *x, const char *title)
{
	html *h = &x->h;
	web_header(x->res, "Content-Type", "text/html; charset=utf-8");
	web_header(x->res, "Cache-Control", "no-store");
	HL(h, "<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">"
	      "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
	      "<link rel=\"stylesheet\" href=\"");
	h_text(h, x->app->css_url);
	HL(h, "\"><title>");
	h_text(h, title);
	HL(h, " · tix</title></head>\n<body>\n");
	if (x->session) {
		HL(h, "<header><nav><a href=\"/tickets\"><b>tix</b></a> <a href=\"/tickets\">Tickets</a> "
		      "<a href=\"/wbs\">WBS</a> <a href=\"/plan\">Plan</a> <a href=\"/load\">Load</a> "
		      "<a href=\"/res\">Resources</a> <a href=\"/cal\">Calendars</a> <a href=\"/sync\">Sync</a> "
		      "<a href=\"/id\">Identity</a></nav>"
		      "<form class=\"search\" method=\"get\" action=\"/tickets\" role=\"search\">"
		      "<label>Find <input type=\"search\" name=\"q\" placeholder=\"id or title\"");
		const char *q = web_kvs_get(&x->req->query, "q");
		if (q) {
			HL(h, " value=\"");
			h_text(h, q);
			HL(h, "\"");
		}
		HL(h, "></label> <button>Go</button></form>"
		      "<form class=\"logout\" method=\"post\" action=\"/logout\">");
		form_csrf(x);
		HL(h, "<button>Log out</button></form>");
		if (x->app->cfg.read_only)
			HL(h, "<p class=\"note\">Read-only: this server doesn't accept changes.</p>");
		HL(h, "</header>\n");
	}
	HL(h, "<main>\n<h1>");
	h_text(h, title);
	HL(h, "</h1>\n");
}

void page_end(web_ctx *x)
{
	HL(&x->h, "</main>\n</body></html>\n");
}

void page_error(web_ctx *x, int status, const char *title, const char *msg)
{
	tix_buf_reset(&x->res->body);
	tix_buf_reset(&x->res->headers);
	x->res->status = status;
	page_begin(x, title);
	HL(&x->h, "<p>");
	h_text(&x->h, msg);
	HL(&x->h, "</p>\n");
	page_end(x);
}

void web_redirect(web_ctx *x, const char *path)
{
	x->res->status = 303;
	web_header(x->res, "Location", path);
	web_header(x->res, "Cache-Control", "no-store");
}

void form_csrf(web_ctx *x)
{
	HL(&x->h, "<input type=\"hidden\" name=\"csrf\" value=\"");
	h_text(&x->h, x->session ? x->session->csrf : "");
	HL(&x->h, "\">");
}

/* ---- login --------------------------------------------------------------------------- */

static void login_form(web_ctx *x, int status, const char *msg)
{
	x->res->status = status;
	page_begin(x, "Log in");
	html *h = &x->h;
	if (msg) {
		HL(h, "<p class=\"error\">");
		h_text(h, msg);
		HL(h, "</p>\n");
	}
	HL(h, "<p>Open the URL that <code>tix web</code> printed when it started, or paste its token here. "
	      "Restarting <code>tix web</code> prints a new one.</p>\n"
	      "<form method=\"post\" action=\"/login\"><label>Token <input name=\"t\" size=\"66\" "
	      "autocomplete=\"off\" required></label> <button>Log in</button></form>\n");
	page_end(x);
}

static void login(web_ctx *x)
{
	const web_request *req = x->req;
	bool post = !strcmp(req->method, "POST");
	const char *t = post ? web_kvs_get(&req->form, "t") : web_kvs_get(&req->query, "t");
	if (post && !same_origin(req)) {
		page_error(x, 403, "Forbidden", "This form must be sent from this server's own page.");
		return;
	}
	if (!t) {
		login_form(x, 200, NULL);
		return;
	}
	if (!secret_eq(t, x->app->token)) {
		login_form(x, 403, "That token is not this server's.");
		return;
	}
	web_session *s = session_new(x->app);
	char cookie[160];
	snprintf(cookie, sizeof(cookie), "tix_session=%s; HttpOnly; SameSite=Strict; Path=/", s->id);
	web_header(x->res, "Set-Cookie", cookie);
	web_redirect(x, "/");
}

static void logout(web_ctx *x)
{
	web_session *s = (web_session *)x->session;
	memset(s->id, 0, sizeof(s->id)); /* never matches again (ids are 64 hex chars) */
	web_header(x->res, "Set-Cookie", "tix_session=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0");
	web_redirect(x, "/login");
}

/* ---- routing ------------------------------------------------------------------------- */

static bool split_path(web_ctx *x)
{
	const char *p = x->req->path;
	x->nsegs = 0;
	while (*p) {
		while (*p == '/')
			p++;
		if (!*p)
			break;
		size_t n = strcspn(p, "/");
		if (x->nsegs == 4 || n >= sizeof(x->segs[0]))
			return false;
		memcpy(x->segs[x->nsegs], p, n);
		x->segs[x->nsegs][n] = '\0';
		x->nsegs++;
		p += n;
	}
	return true;
}

static bool is(const web_ctx *x, size_t i, const char *s)
{
	return i < x->nsegs && !strcmp(x->segs[i], s);
}

static void serve_asset(web_ctx *x)
{
	const char *type, *body;
	size_t len;
	if (x->nsegs != 2 || !asset_find(x->segs[1], &type, &body, &len)) {
		page_error(x, 404, "Not found", "No such file.");
		return;
	}
	web_header(x->res, "Content-Type", type);
	web_header(x->res, "Cache-Control", "public, max-age=31536000, immutable");
	tix_buf_add(&x->res->body, body, len);
}

void web_app_handle(void *vapp, const web_request *req, web_response *res)
{
	web_app *app = vapp;
	web_ctx x = {.app = app, .req = req, .res = res, .h = {&res->body}};
	bool get = !strcmp(req->method, "GET") || !strcmp(req->method, "HEAD");
	bool post = !strcmp(req->method, "POST");
	app->err = (tix_error)TIX_ERROR_INIT;

	if (!req->host) {
		page_error(&x, 400, "Bad request", "A Host header is required.");
		return;
	}
	if (!host_ok(app, req->host)) {
		page_error(&x, 421, "Wrong address", "This server only answers on the address it printed at start.");
		return;
	}
	if (!split_path(&x)) {
		page_error(&x, 404, "Not found", "No such page.");
		return;
	}
	if (!get && !post) {
		page_error(&x, 405, "Method not allowed", "Only GET and POST are used here.");
		web_header(res, "Allow", "GET, HEAD, POST");
		return;
	}
	if (is(&x, 0, "favicon.ico") && x.nsegs == 1) {
		res->status = 204; /* no icon; keeps browsers from logging a 404 */
		web_header(res, "Cache-Control", "public, max-age=86400");
		return;
	}
	if (is(&x, 0, "assets")) {
		serve_asset(&x);
		return;
	}
	if (is(&x, 0, "login") && x.nsegs == 1) {
		login(&x);
		return;
	}
	x.session = session_find(app, req);
	if (!x.session) {
		login_form(&x, 401, "Log in first.");
		return;
	}
	if (post) {
		const char *csrf = web_kvs_get(&req->form, "csrf");
		if (!same_origin(req) || !csrf || !secret_eq(csrf, x.session->csrf)) {
			page_error(&x, 403, "Forbidden",
				   "This form was not sent from this server's own page, or the page is too old. "
				   "Go back, reload and try again.");
			return;
		}
		if (is(&x, 0, "logout") && x.nsegs == 1) {
			logout(&x);
			return;
		}
		if (app->cfg.read_only) {
			page_error(&x, 405, "Read-only", "This server was started with --read-only.");
			return;
		}
	}
	/* other processes may have changed the repository since the last request */
	if (tix_repo_refresh(app->cfg.repo, &app->err)) {
		page_error(&x, 500, "Repository problem", app->err.msg);
		return;
	}
	if (x.nsegs == 0 && get) {
		web_redirect(&x, "/tickets");
		return;
	}
	static const struct {
		const char *seg;
		size_t nsegs;
		void (*get)(web_ctx *);
	} routes[] = {
		{"tickets", 1, page_tickets}, {"t", 2, page_ticket},      {"wbs", 1, page_wbs},
		{"plan", 1, page_plan_view},  {"load", 1, page_load},     {"export", 1, page_export},
		{"res", 1, page_resources},   {"res", 2, page_resource},  {"cal", 1, page_calendars},
		{"cal", 2, page_calendar},    {"id", 1, page_identities},
	};
	for (size_t i = 0; i < sizeof(routes) / sizeof(*routes); i++)
		if (is(&x, 0, routes[i].seg) && x.nsegs == routes[i].nsegs) {
			if (get) {
				routes[i].get(&x);
				return;
			}
			page_error(&x, 405, "Method not allowed", "This page has no form to send.");
			web_header(res, "Allow", "GET, HEAD");
			return;
		}
	page_error(&x, 404, "Not found", "No such page.");
}
