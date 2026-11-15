/*
 * Request checks, sessions and routing (docs/architecture.md section 13.6). Order for every
 * request: Host (421), then assets and login (no session needed), then the session cookie
 * (401), then for POSTs the Origin/Referer and the form's csrf token (403).
 */
#include "app.h"

#include <stdio.h>
#include <stdlib.h>
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

void web_app_free(web_app *app)
{
	free(app->heads);
	app->heads = NULL;
	app->nheads = 0;
}

void web_app_init(web_app *app, const web_app_config *cfg)
{
	memset(app, 0, sizeof(*app));
	app->cfg = *cfg;
	if (!app->cfg.remote)
		app->cfg.remote = "origin"; /* as the CLI defaults */
	app->err = (tix_error)TIX_ERROR_INIT;
	random_hex(app, app->token);
	snprintf(app->css_url, sizeof(app->css_url), "/assets/%s", asset_css_name());
	snprintf(app->js_url, sizeof(app->js_url), "/assets/%s", asset_js_name());
	snprintf(app->always_headers, sizeof(app->always_headers),
		 "Content-Security-Policy: default-src 'none'; style-src 'self'; script-src 'self'; img-src 'self'; "
		 "connect-src 'self'; form-action 'self'; frame-ancestors 'none'; base-uri 'none'\r\n"
		 "X-Content-Type-Options: nosniff\r\n"
		 "X-Frame-Options: DENY\r\n"
		 "Referrer-Policy: same-origin\r\n" /* no-referrer makes browsers send Origin: null */
		 "Cross-Origin-Opener-Policy: same-origin\r\n"
		 "Cross-Origin-Resource-Policy: same-origin\r\n");
}

void web_app_set_listen(web_app *app, const char *host, int port)
{
	if (app->cfg.hosted) { /* the proxy's public name, not where we listen */
		const char *o = app->cfg.hosted->origin, *h = strstr(o, "://");
		h = h ? h + 3 : o;
		app->nhosts = 0;
		snprintf(app->hosts[app->nhosts++], sizeof(app->hosts[0]), "%.*s", (int)strcspn(h, "/"), h);
		return;
	}
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
	/* (hosted sessions also go through here; their id is never sent anywhere) */
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

/* ---- hosted mode (§13.8) ---------------------------------------------------------- */

static bool peer_ok(const web_app *app, const char *peer)
{
	for (size_t i = 0; peer && i < app->cfg.hosted->nfrom; i++)
		if (!strcmp(peer, app->cfg.hosted->from[i]))
			return true;
	return false;
}

/* A usable login: 1-128 bytes of printable text (the header is the proxy's, but be strict). */
static bool login_ok(const char *u)
{
	size_t n = u ? strlen(u) : 0;
	if (!n || n > WEB_MAX_USER)
		return false;
	for (size_t i = 0; i < n; i++)
		if ((unsigned char)u[i] < 0x20 || u[i] == 0x7f)
			return false;
	return true;
}

/* The session of the login the proxy vouches for (made on first sight; LRU like any). */
static web_session *hosted_session(web_app *app, const char *user)
{
	for (size_t i = 0; i < app->nsessions; i++)
		if (!strcmp(app->sessions[i].user, user)) {
			app->sessions[i].used = ++app->clock;
			return &app->sessions[i];
		}
	web_session *s = session_new(app);
	snprintf(s->user, sizeof(s->user), "%s", user);
	s->mapped = false;
	memset(&s->identity, 0, sizeof(s->identity));
	for (size_t i = 0; i < app->cfg.hosted->nusers; i++)
		if (!strcmp(app->cfg.hosted->users[i].login, user)) {
			s->mapped = true;
			s->identity = app->cfg.hosted->users[i].identity;
		}
	return s;
}

/* The request comes from one of our own pages: Origin, or failing that Referer. */
static bool same_origin(const web_app *app, const web_request *req)
{
	char want[160];
	if (app->cfg.hosted)
		snprintf(want, sizeof(want), "%s", app->cfg.hosted->origin);
	else
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
	HL(h, "\"><script defer src=\"");
	h_text(h, x->app->js_url);
	HL(h, "\"></script><title>");
	h_text(h, title);
	HL(h, " · tix</title></head>\n<body");
	char stamp[TIX_STAMP_LEN];
	if (x->session && !tix_repo_stamp(x->app->cfg.repo, stamp, NULL)) {
		HL(h, " data-stamp=\"");
		h_text(h, stamp);
		HL(h, "\"");
	}
	if (!web_can_write(x))
		HL(h, " data-read-only");
	if (x->page_ticket) {
		HL(h, " data-ticket=\"");
		h_id(h, x->page_ticket);
		HL(h, "\"");
	}
	HL(h, ">\n");
	if (x->session) {
		HL(h, "<header><nav><a href=\"/tickets\"><b>tix</b></a> <a href=\"/tickets\">Tickets</a> "
		      "<a href=\"/inbox\">Inbox</a> <a href=\"/activity\">Activity</a> "
		      "<a href=\"/wbs\">WBS</a> <a href=\"/plan\">Plan</a> <a href=\"/baselines\">Baselines</a> "
		      "<a href=\"/load\">Load</a> "
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
		HL(h, "></label> <button>Go</button></form>");
		if (x->app->cfg.hosted) {
			/* the proxy signs people in and out; the token rides along for app.js */
			HL(h, "<span class=\"user\">");
			h_text(h, x->session->user);
			HL(h, "</span><form class=\"logout\" hidden>");
			form_csrf(x);
			HL(h, "</form>");
			if (x->app->cfg.hosted->logout_url) {
				HL(h, " <a href=\"");
				h_text(h, x->app->cfg.hosted->logout_url);
				HL(h, "\">Sign out</a>");
			}
		} else {
			HL(h, "<form class=\"logout\" method=\"post\" action=\"/logout\">");
			form_csrf(x);
			HL(h, "<button>Log out</button></form>");
		}
		h_autonomy_banner(x);
		if (x->app->cfg.read_only)
			HL(h, "<p class=\"note\">Read-only: this server doesn't accept changes.</p>");
		else if (x->app->cfg.hosted && !x->session->mapped)
			HL(h, "<p class=\"note\">Your login isn't linked to a tix identity: you can read, not change.</p>");
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

bool web_can_write(const web_ctx *x)
{
	return !x->app->cfg.read_only && (!x->app->cfg.hosted || (x->session && x->session->mapped));
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
	if (post && !same_origin(x->app, req)) {
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

static void res_edit(web_ctx *x) { page_named_edit(x, TIX_ENTITY_RESOURCE); }
static void cal_edit(web_ctx *x) { page_named_edit(x, TIX_ENTITY_CALENDAR); }
static void res_post(web_ctx *x) { post_named(x, TIX_ENTITY_RESOURCE); }
static void cal_post(web_ctx *x) { post_named(x, TIX_ENTITY_CALENDAR); }

typedef struct route_def {
	const char *seg0;
	size_t nsegs;
	const char *seg_at;  /* NULL: any; else segs[at] must equal it */
	size_t at;
	void (*fn)(web_ctx *);
} route_def;

/* First match wins: specific paths (/res/new) come before patterns (/res/ID). */
static const route_def get_routes[] = {
	{"tickets", 1, NULL, 0, page_tickets},  {"new", 1, NULL, 0, page_new},
	{"t", 3, "edit", 2, page_ticket_edit},  {"t", 4, "comment", 2, page_comment_edit},
	{"t", 3, "warnings", 2, page_ticket_warnings},
	{"t", 2, NULL, 0, page_ticket},         {"wbs", 1, NULL, 0, page_wbs},
	{"plan", 1, NULL, 0, page_plan_view},   {"load", 1, NULL, 0, page_load},
	{"export", 1, NULL, 0, page_export},    {"res", 1, NULL, 0, page_resources},
	{"res", 2, "new", 1, res_edit},         {"res", 3, "edit", 2, res_edit},
	{"res", 2, NULL, 0, page_resource},     {"cal", 1, NULL, 0, page_calendars},
	{"cal", 2, "new", 1, cal_edit},         {"cal", 3, "edit", 2, cal_edit},
	{"cal", 2, NULL, 0, page_calendar},     {"id", 1, NULL, 0, page_identities},
	{"sync", 1, NULL, 0, page_sync},       {"api", 2, "tickets", 1, page_api_tickets},
	{"events", 1, NULL, 0, page_events},     {"baselines", 1, NULL, 0, page_baselines},
	{"inbox", 1, NULL, 0, page_inbox},       {"activity", 1, NULL, 0, page_activity},
};
static const route_def post_routes[] = {
	{"new", 1, NULL, 0, post_new},            {"t", 2, NULL, 0, post_ticket},
	{"t", 3, "comment", 2, post_comment},     {"t", 4, "comment", 2, post_comment},
	{"t", 3, "dep", 2, post_dep},             {"t", 3, "assign", 2, post_assign},
	{"t", 3, "answer", 2, post_answer},
	{"res", 2, NULL, 0, res_post},            {"cal", 2, NULL, 0, cal_post},
	{"cal", 3, "exception", 2, post_exception}, {"sync", 1, NULL, 0, post_sync},
	{"baselines", 1, NULL, 0, post_baseline_take}, {"baselines", 2, NULL, 0, post_baseline_rename},
};

static const route_def *match(const web_ctx *x, const route_def *r, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (is(x, 0, r[i].seg0) && x->nsegs == r[i].nsegs && (!r[i].seg_at || is(x, r[i].at, r[i].seg_at)))
			return &r[i];
	return NULL;
}

/* RETURNS false when nothing matched (the caller answers 404). */
static bool route(web_ctx *x, bool get, bool post)
{
	const route_def *g = match(x, get_routes, sizeof(get_routes) / sizeof(*get_routes));
	const route_def *p = match(x, post_routes, sizeof(post_routes) / sizeof(*post_routes));
	if (get && g) {
		g->fn(x);
		return true;
	}
	if (post && p) {
		p->fn(x);
		return true;
	}
	if (g || p) {
		page_error(x, 405, "Method not allowed", get ? "This address only takes forms." : "This page has no form to send.");
		web_header(x->res, "Allow", g && p ? "GET, HEAD, POST" : g ? "GET, HEAD" : "POST");
		return true;
	}
	return false;
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
	if (app->cfg.hosted && !peer_ok(app, req->peer)) {
		page_error(&x, 403, "Forbidden", "This server only answers through its proxy.");
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
	if (!strcmp(req->path, WEB_HOOK_PATH)) {
		if (!app->cfg.hook_secret) {
			page_error(&x, 404, "Not found", "No such page.");
		} else if (!post) {
			page_error(&x, 405, "Method not allowed", "This address only takes webhooks.");
			web_header(res, "Allow", "POST");
		} else {
			web_hook_forgejo(&x);
		}
		return;
	}
	if (is(&x, 0, "login") && x.nsegs == 1 && !app->cfg.hosted) {
		login(&x);
		return;
	}
	if (app->cfg.hosted) {
		if (!login_ok(req->extra)) {
			page_error(&x, 401, "Sign in", "Sign in through this site's login (the proxy didn't say who you are).");
			return;
		}
		x.session = hosted_session(app, req->extra);
		/* every write is authored by the requester's identity, or refused without one */
		if (x.session->mapped)
			tix_repo_set_identity(app->cfg.repo, &x.session->identity, NULL);
		else
			tix_repo_clear_identity(app->cfg.repo);
	} else {
		x.session = session_find(app, req);
	}
	if (!x.session) {
		login_form(&x, 401, "Log in first.");
		return;
	}
	if (post) {
		const char *csrf = web_kvs_get(&req->form, "csrf");
		if (!same_origin(app, req) || !csrf || !secret_eq(csrf, x.session->csrf)) {
			page_error(&x, 403, "Forbidden",
				   "This form was not sent from this server's own page, or the page is too old. "
				   "Go back, reload and try again.");
			return;
		}
		if (is(&x, 0, "logout") && x.nsegs == 1 && !app->cfg.hosted) {
			logout(&x);
			return;
		}
		if (app->cfg.read_only) {
			page_error(&x, 405, "Read-only", "This server was started with --read-only.");
			return;
		}
		if (app->cfg.hosted && !x.session->mapped) {
			char msg[300];
			snprintf(msg, sizeof(msg),
				 "Your login (%s) isn't linked to a tix identity, so you can read but not change anything. "
				 "Ask whoever runs this server to add you to its users file.", x.session->user);
			page_error(&x, 403, "Read only for you", msg);
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
	if (route(&x, get, post))
		return;
	page_error(&x, 404, "Not found", "No such page.");
}
