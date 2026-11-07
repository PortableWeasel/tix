/*
 * The web app's handlers on core over store_mem, without sockets: request checks
 * (Host, session, CSRF, Origin), login, headers, escaping, and the pages.
 */
#include "app.h"
#include "tix/store_mem.h"
#include "tix/testkit.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static tix_error e = TIX_ERROR_INIT;

typedef struct fake_env {
	int64_t now;
	uint64_t rng;
} fake_env;

static int64_t fake_now(void *ctx)
{
	return ((fake_env *)ctx)->now += 60;
}

static void fake_random(void *ctx, uint8_t *out, size_t n)
{
	fake_env *f = ctx;
	for (size_t i = 0; i < n; i++)
		out[i] = (uint8_t)tk_rand(&f->rng);
}

static tix_date fixed_today(void *ctx)
{
	(void)ctx;
	tix_date d;
	tix_date_parse("2026-11-09", &d);
	return d;
}

/* ---- a test client ---- */

typedef struct client {
	web_app *app;
	char cookie[128];
	char csrf[WEB_TOKEN_HEX + 1];
	const char *host, *origin, *referer;
	web_response res;
	char loc[256];
} client;

typedef struct hdrs {
	const char *names[6], *values[6];
} hdrs;

static const char *hdr(void *ctx, const char *name, size_t *len)
{
	hdrs *h = ctx;
	for (int i = 0; i < 6 && h->names[i]; i++)
		if (!strcasecmp(h->names[i], name)) {
			*len = strlen(h->values[i]);
			return h->values[i];
		}
	return NULL;
}

static void add_hdr(hdrs *h, const char *name, const char *value)
{
	for (int i = 0; i < 6; i++)
		if (!h->names[i]) {
			h->names[i] = name;
			h->values[i] = value;
			return;
		}
}

/* Sends method path?query with a urlencoded body; the response is in c->res. */
static int call(client *c, const char *method, const char *target, const char *body)
{
	web_response_free(&c->res);
	c->res = (web_response)WEB_RESPONSE_INIT;
	const char *q = strchr(target, '?');
	size_t plen = q ? (size_t)(q - target) : strlen(target);
	hdrs h = {{0}, {0}};
	if (c->host)
		add_hdr(&h, "Host", c->host);
	if (c->cookie[0])
		add_hdr(&h, "Cookie", c->cookie);
	if (c->origin)
		add_hdr(&h, "Origin", c->origin);
	if (c->referer)
		add_hdr(&h, "Referer", c->referer);
	if (body)
		add_hdr(&h, "Content-Type", "application/x-www-form-urlencoded");
	web_raw raw = {method, target, q ? q + 1 : "", body ? body : "", strlen(method), plen,
		       q ? strlen(q + 1) : 0, body ? strlen(body) : 0, hdr, &h};
	web_request req;
	char why[128];
	TK_CHECK(web_request_decode(&raw, &req, why, sizeof(why)));
	web_app_handle(c->app, &req, &c->res);
	web_request_free(&req);
	c->loc[0] = '\0';
	const char *l = strstr(tix_buf_cstr(&c->res.headers), "Location: ");
	if (l)
		snprintf(c->loc, sizeof(c->loc), "%.*s", (int)strcspn(l + 10, "\r"), l + 10);
	return c->res.status;
}

static const char *body(const client *c)
{
	return tix_buf_cstr(&c->res.body);
}

#define HAS(c, needle)                                                                                \
	do {                                                                                          \
		if (!strstr(body(c), (needle)))                                                       \
			tk_fail(__FILE__, __LINE__, "expected '%s' in the page:\n%s", (needle), body(c)); \
	} while (0)
#define HASNT(c, needle)                                                                     \
	do {                                                                                 \
		if (strstr(body(c), (needle)))                                               \
			tk_fail(__FILE__, __LINE__, "did not expect '%s' in the page", (needle)); \
	} while (0)
#define HEADER(c, needle) TK_CHECK(strstr(tix_buf_cstr(&(c)->res.headers), (needle)) != NULL)

/* Logs in with the app's token; keeps the cookie and the csrf token from a page. */
static void login(client *c)
{
	char target[160];
	snprintf(target, sizeof(target), "/login?t=%s", c->app->token);
	TK_CHECK(call(c, "GET", target, NULL) == 303);
	TK_STR_EQ(c->loc, "/");
	const char *sc = strstr(tix_buf_cstr(&c->res.headers), "Set-Cookie: ");
	TK_CHECK(sc != NULL);
	if (!sc)
		return;
	TK_CHECK(strstr(sc, "HttpOnly; SameSite=Strict; Path=/") != NULL);
	snprintf(c->cookie, sizeof(c->cookie), "%.*s", (int)strcspn(sc + 12, ";"), sc + 12);
	TK_CHECK(call(c, "GET", "/tickets", NULL) == 200);
	const char *f = strstr(body(c), "name=\"csrf\" value=\"");
	c->csrf[0] = '\0';
	if (f)
		snprintf(c->csrf, sizeof(c->csrf), "%.*s", WEB_TOKEN_HEX, f + 19);
}

typedef struct world {
	fake_env env;
	tix_store *store;
	tix_repo *repo;
	web_app app;
	tix_oid me;
} world;

static void world_open(world *w, bool read_only)
{
	w->env = (fake_env){1700000000, 3};
	w->store = tix_store_mem_new(TIX_HASH_SHA1);
	tix_repo_config cfg = {.store = w->store, .env = {fake_now, fake_random, &w->env}};
	TK_OK(tix_repo_open(&w->repo, &cfg, &e), &e);
	TK_OK(tix_identity_create(w->repo, "Ada <script>", "ada@example.org", &w->me, &e), &e);
	TK_OK(tix_repo_set_identity(w->repo, &w->me, &e), &e);
	web_app_config acfg = {.repo = w->repo, .random = fake_random, .random_ctx = &w->env, .today = fixed_today,
			       .read_only = read_only};
	web_app_init(&w->app, &acfg);
	web_app_set_listen(&w->app, "127.0.0.1", 7480);
}

static void world_close(world *w)
{
	tix_repo_close(w->repo);
	tix_store_destroy(w->store);
}

static client new_client(world *w)
{
	client c = {.app = &w->app, .host = "127.0.0.1:7480", .origin = "http://127.0.0.1:7480",
		    .res = WEB_RESPONSE_INIT};
	return c;
}

static void test_checks(void)
{
	world w;
	world_open(&w, false);
	client c = new_client(&w);

	/* the security headers the server adds to every response */
	TK_CHECK(strstr(w.app.always_headers, "Content-Security-Policy: default-src 'none'; style-src 'self'; "
					      "script-src 'self'") != NULL);
	TK_CHECK(strstr(w.app.always_headers, "frame-ancestors 'none'") && strstr(w.app.always_headers, "nosniff"));
	TK_CHECK(strstr(w.app.always_headers, "Referrer-Policy: same-origin"));

	/* Host first: DNS rebinding and foreign names get 421, no Host 400, even for login */
	c.host = "evil.example:7480";
	TK_CHECK(call(&c, "GET", "/tickets", NULL) == 421);
	TK_CHECK(call(&c, "GET", "/login", NULL) == 421);
	c.host = "127.0.0.1:7481";
	TK_CHECK(call(&c, "GET", "/tickets", NULL) == 421);
	c.host = NULL;
	TK_CHECK(call(&c, "GET", "/tickets", NULL) == 400);
	c.host = "LOCALHOST:7480";
	TK_CHECK(call(&c, "GET", "/tickets", NULL) == 401); /* host ok (case-insensitive), no session */
	c.host = "127.0.0.1:7480";

	/* no session: 401 with the login form, never data */
	TK_CHECK(call(&c, "GET", "/tickets", NULL) == 401);
	HAS(&c, "action=\"/login\"");
	HASNT(&c, "Ada");
	TK_CHECK(call(&c, "GET", "/", NULL) == 401);
	snprintf(c.cookie, sizeof(c.cookie), "tix_session=%064d", 0);
	TK_CHECK(call(&c, "GET", "/tickets", NULL) == 401);
	c.cookie[0] = '\0';

	/* wrong tokens */
	TK_CHECK(call(&c, "GET", "/login?t=nope", NULL) == 403);
	TK_CHECK(call(&c, "GET", "/login?t=", NULL) == 403);
	char almost[WEB_TOKEN_HEX + 2];
	snprintf(almost, sizeof(almost), "%s", w.app.token);
	almost[WEB_TOKEN_HEX - 1] ^= 1;
	char target[160];
	snprintf(target, sizeof(target), "/login?t=%s", almost);
	TK_CHECK(call(&c, "GET", target, NULL) == 403);
	snprintf(target, sizeof(target), "/login?t=%sx", w.app.token);
	TK_CHECK(call(&c, "GET", target, NULL) == 403);
	TK_CHECK(call(&c, "GET", "/login", NULL) == 200); /* the paste form */

	/* POST /login works with the right token from our own page only */
	char form[160];
	snprintf(form, sizeof(form), "t=%s", w.app.token);
	c.origin = "http://evil.example";
	TK_CHECK(call(&c, "POST", "/login", form) == 403);
	c.origin = "http://127.0.0.1:7480";
	TK_CHECK(call(&c, "POST", "/login", form) == 303);

	login(&c);
	TK_CHECK(c.csrf[0] != '\0' && strlen(c.csrf) == WEB_TOKEN_HEX);
	TK_CHECK(call(&c, "GET", "/", NULL) == 303);
	TK_STR_EQ(c.loc, "/tickets");
	TK_CHECK(call(&c, "GET", "/tickets", NULL) == 200);
	HEADER(&c, "Cache-Control: no-store");
	HEADER(&c, "Content-Type: text/html; charset=utf-8");
	TK_CHECK(call(&c, "GET", "/no/such/page", NULL) == 404);
	TK_CHECK(call(&c, "PUT", "/tickets", NULL) == 405);
	HEADER(&c, "Allow: GET, HEAD, POST");
	TK_CHECK(call(&c, "GET", "/a/b/c/d/e", NULL) == 404);

	/* POSTs: csrf token and Origin (or Referer) */
	char good[160], bad[160];
	snprintf(good, sizeof(good), "csrf=%s", c.csrf);
	snprintf(bad, sizeof(bad), "csrf=%.*s0", WEB_TOKEN_HEX - 1, c.csrf);
	TK_CHECK(call(&c, "POST", "/logout", "") == 403);   /* no token */
	TK_CHECK(call(&c, "POST", "/logout", bad) == 403);  /* wrong token */
	c.origin = "http://evil.example";
	TK_CHECK(call(&c, "POST", "/logout", good) == 403); /* foreign origin */
	c.origin = "null";
	TK_CHECK(call(&c, "POST", "/logout", good) == 403);
	c.origin = NULL;
	TK_CHECK(call(&c, "POST", "/logout", good) == 403); /* neither Origin nor Referer */
	c.referer = "http://127.0.0.1:7480.evil.example/";
	TK_CHECK(call(&c, "POST", "/logout", good) == 403);
	c.referer = "http://127.0.0.1:7480/tickets";
	TK_CHECK(call(&c, "POST", "/logout", good) == 303); /* Referer from our own page */
	TK_STR_EQ(c.loc, "/login");
	HEADER(&c, "Max-Age=0");
	TK_CHECK(call(&c, "GET", "/tickets", NULL) == 401); /* the session is gone */
	c.referer = NULL;
	c.origin = "http://127.0.0.1:7480";

	/* another tab's session is separate; sessions are capped, the oldest goes */
	client d = new_client(&w);
	login(&d);
	TK_CHECK(strcmp(d.csrf, c.csrf));
	for (int i = 0; i < WEB_MAX_SESSIONS + 2; i++) {
		client t = new_client(&w);
		login(&t);
		web_response_free(&t.res);
	}
	TK_CHECK(call(&d, "GET", "/tickets", NULL) == 401);

	/* assets: exact names only, cached forever */
	char css[96];
	snprintf(css, sizeof(css), "%s", w.app.css_url);
	TK_CHECK(call(&c, "GET", css, NULL) == 200);
	HEADER(&c, "Content-Type: text/css");
	HEADER(&c, "immutable");
	TK_CHECK(call(&c, "GET", "/assets/../app.c", NULL) == 404);
	TK_CHECK(call(&c, "GET", "/assets/tix-00000000.css", NULL) == 404);

	web_response_free(&c.res);
	web_response_free(&d.res);
	world_close(&w);
}

static void test_read_only(void)
{
	world w;
	world_open(&w, true);
	client c = new_client(&w);
	login(&c);
	HAS(&c, "Read-only");
	char form[160];
	snprintf(form, sizeof(form), "csrf=%s", c.csrf);
	TK_CHECK(call(&c, "POST", "/tickets", form) == 405);
	TK_CHECK(call(&c, "POST", "/logout", form) == 303); /* logging out still works */
	web_response_free(&c.res);
	world_close(&w);
}

static void test_escaping(void)
{
	tix_buf b = TIX_BUF_INIT;
	html h = {&b};
	h_text(&h, "<script>alert('x')</script> & \"q\"");
	TK_STR_EQ(tix_buf_cstr(&b), "&lt;script&gt;alert(&#39;x&#39;)&lt;/script&gt; &amp; &quot;q&quot;");
	tix_buf_reset(&b);
	h_text(&h, "a\xe2\x80\xae" "b\xe2\x81\xa6" "c\xe2\x80\x8d" "d"); /* RLO, LRI replaced; ZWJ kept */
	TK_STR_EQ(tix_buf_cstr(&b), "a\xef\xbf\xbd" "b\xef\xbf\xbd" "c\xe2\x80\x8d" "d");
	tix_buf_reset(&b);
	h_url(&h, "a b/c?d=e&f\"<");
	TK_STR_EQ(tix_buf_cstr(&b), "a%20b%2Fc%3Fd%3De%26f%22%3C");
	tix_buf_reset(&b);
	HL(&h, "<b>");
	h_int(&h, -42);
	TK_STR_EQ(tix_buf_cstr(&b), "<b>-42");
	tix_buf_free(&b);

	/* hostile titles never reach the page raw */
	world w;
	world_open(&w, false);
	tix_oid t;
	TK_OK(tix_ticket_create(w.repo, "<img src=x onerror=alert(1)>\"'", "", NULL, &t, &e), &e);
	client c = new_client(&w);
	login(&c);
	TK_CHECK(call(&c, "GET", "/tickets", NULL) == 200);
	HAS(&c, "&lt;img src=x onerror=alert(1)&gt;&quot;&#39;");
	HASNT(&c, "<img");
	web_response_free(&c.res);
	world_close(&w);
}


/* Ops through core, as the CLI would write them. */
static void apply1(world *w, tix_entity_type t, const tix_oid *id, tix_op op)
{
	TK_OK(tix_entity_apply(w->repo, t, id, &op, 1, &e), &e);
	tix_op_free(&op);
}

static void field(world *w, tix_entity_type t, const tix_oid *id, const char *name, const char *text)
{
	tix_input_ctx ctx = {w->repo, fixed_today(NULL)};
	tix_input in = {0};
	TK_OK(tix_input_field(&ctx, t, name, text, &in, &e), &e);
	TK_OK(tix_input_finish(&ctx, &in, id, NULL, &e), &e);
	TK_OK(tix_entity_apply(w->repo, t, id, in.ops.v, in.ops.n, &e), &e);
	tix_input_free(&in);
}

/* No page may carry markup that came from the data. The one script tag is ours (M7). */
static void clean(client *c, const char *what)
{
	const char *b = body(c), *s = strstr(b, "<script");
	if (s && (strncmp(s, "<script defer src=\"/assets/tix-", strlen("<script defer src=\"/assets/tix-")) || strstr(s + 1, "<script")))
		tk_fail(__FILE__, __LINE__, "unexpected script on %s", what);
	if (strstr(b, "<img") || strstr(b, "\xe2\x80\xae") || strstr(b, "href=\"javascript"))
		tk_fail(__FILE__, __LINE__, "unescaped hostile content on %s", what);
}

static void test_pages(void)
{
	world w;
	world_open(&w, false);
	tix_oid house, dig, pour, hand, crew, cal, other;
	char hex[TIX_OID_MAX_HEX + 1], buf[256];
	TK_OK(tix_calendar_create(w.repo, "Site <script>x</script>", &cal, &e), &e);
	field(&w, TIX_ENTITY_CALENDAR, &cal, "off", "2026-11-11=<img src=x onerror=alert(1)>");
	TK_OK(tix_resource_create(w.repo, "Crew \xe2\x80\xae<img src=x>", TIX_RES_ROLE, &crew, &e), &e);
	field(&w, TIX_ENTITY_RESOURCE, &crew, "capacity", "200%");
	tix_strs lab = {0};
	tix_strs_add(&lab, "<script>");
	TK_OK(tix_ticket_create(w.repo, "House <script>alert(1)</script>", "Body with <b>tags</b>\nand javascript:x",
				&lab, &house, &e), &e);
	tix_strs_free(&lab);
	field(&w, TIX_ENTITY_TICKET, &house, "calendar", tix_oid_hex(&cal, hex));
	TK_OK(tix_ticket_create(w.repo, "Dig", "", NULL, &dig, &e), &e);
	TK_OK(tix_ticket_create(w.repo, "Pour \"slab\" & 'cure'", "", NULL, &pour, &e), &e);
	TK_OK(tix_ticket_create(w.repo, "Handover", "", NULL, &hand, &e), &e);
	TK_OK(tix_ticket_create(w.repo, "Unrelated", "", NULL, &other, &e), &e);
	tix_oid_hex(&house, hex);
	field(&w, TIX_ENTITY_TICKET, &dig, "parent", hex);
	field(&w, TIX_ENTITY_TICKET, &pour, "parent", hex);
	field(&w, TIX_ENTITY_TICKET, &hand, "parent", hex);
	field(&w, TIX_ENTITY_TICKET, &dig, "effort", "32h");
	field(&w, TIX_ENTITY_TICKET, &pour, "duration", "2d");
	field(&w, TIX_ENTITY_TICKET, &hand, "kind", "milestone");
	field(&w, TIX_ENTITY_TICKET, &house, "start", "2026-11-09");
	snprintf(buf, sizeof(buf), "+%s:FS+1d", tix_oid_hex(&dig, hex));
	field(&w, TIX_ENTITY_TICKET, &pour, "dep", buf);
	snprintf(buf, sizeof(buf), "+%s", tix_oid_hex(&pour, hex));
	field(&w, TIX_ENTITY_TICKET, &hand, "dep", buf);
	field(&w, TIX_ENTITY_TICKET, &hand, "deadline", "2026-11-12");
	tix_op as = tix_op_assign(&crew, 100, -1);
	apply1(&w, TIX_ENTITY_TICKET, &dig, as);
	apply1(&w, TIX_ENTITY_TICKET, &dig, tix_op_text(TIX_OP_COMMENT, "<script>steal()</script> looks fine"));
	apply1(&w, TIX_ENTITY_TICKET, &other, tix_op_set_status(TIX_TICKET_DONE));

	client c = new_client(&w);
	login(&c);
	char target[256];

	/* list: active by default, filters, hostile labels */
	TK_CHECK(call(&c, "GET", "/tickets", NULL) == 200);
	clean(&c, "/tickets");
	HAS(&c, "House &lt;script&gt;alert(1)&lt;/script&gt;");
	HASNT(&c, "Unrelated"); /* done: not in the default view */
	HAS(&c, "href=\"/tickets?label=%3Cscript%3E\"");
	TK_CHECK(call(&c, "GET", "/tickets?status=done", NULL) == 200);
	HAS(&c, "Unrelated");
	HASNT(&c, "Dig<");
	TK_CHECK(call(&c, "GET", "/tickets?q=POUR&status=open", NULL) == 200);
	HAS(&c, "Pour &quot;slab&quot; &amp; &#39;cure&#39;");
	HASNT(&c, ">Dig<");
	snprintf(target, sizeof(target), "/tickets?status=open&under=%.8s", tix_oid_hex(&house, hex));
	TK_CHECK(call(&c, "GET", target, NULL) == 200);
	HAS(&c, ">Dig<");
	HASNT(&c, "House &lt;");
	snprintf(target, sizeof(target), "/tickets?status=open&assignee=%s", tix_oid_hex(&crew, hex));
	TK_CHECK(call(&c, "GET", target, NULL) == 200);
	HAS(&c, ">Dig<");
	HASNT(&c, "Pour");
	TK_CHECK(call(&c, "GET", "/tickets?under=deadbeef", NULL) == 404);
	/* sort: active first by default; wbs puts a parent before its children; junk falls back */
	TK_CHECK(call(&c, "GET", "/tickets", NULL) == 200);
	HAS(&c, "<option value=\"status\" selected>");
	TK_CHECK(call(&c, "GET", "/tickets?sort=wbs", NULL) == 200);
	HAS(&c, "<option value=\"wbs\" selected>");
	TK_CHECK(strstr(body(&c), "House &lt;") && strstr(body(&c), ">Dig<") &&
		 strstr(body(&c), "House &lt;") < strstr(body(&c), ">Dig<"));
	TK_CHECK(call(&c, "GET", "/tickets?sort=%3Cx%3E", NULL) == 200);
	HAS(&c, "<option value=\"status\" selected>");

	/* one ticket */
	snprintf(target, sizeof(target), "/t/%s", tix_oid_hex(&dig, hex));
	TK_CHECK(call(&c, "GET", target, NULL) == 200);
	clean(&c, "/t/dig");
	HAS(&c, "&lt;script&gt;steal()&lt;/script&gt; looks fine");
	HAS(&c, "Ada &lt;script&gt;"); /* the comment's author, escaped */
	HAS(&c, "2026-11-09 to 2026-11-10");
	HAS(&c, "at 100%");
	HAS(&c, "Followed by");
	snprintf(target, sizeof(target), "/t/%.7s", tix_oid_hex(&pour, hex)); /* a prefix works too */
	TK_CHECK(call(&c, "GET", target, NULL) == 200);
	HAS(&c, "after <span class=\"id\">");
	HAS(&c, "FS+1d");
	snprintf(target, sizeof(target), "/t/%s", tix_oid_hex(&house, hex));
	TK_CHECK(call(&c, "GET", target, NULL) == 200);
	clean(&c, "/t/house");
	HAS(&c, "Body with &lt;b&gt;tags&lt;/b&gt;");
	HAS(&c, "Subtasks");
	HAS(&c, "Site &lt;script&gt;x&lt;/script&gt;");
	TK_CHECK(call(&c, "GET", "/t/deadbeef", NULL) == 404);
	TK_CHECK(call(&c, "GET", "/t/xyz", NULL) == 404);
	TK_CHECK(call(&c, "POST", target, "") == 403); /* no csrf */

	/* wbs, plan + gantt, load, export */
	TK_CHECK(call(&c, "GET", "/wbs", NULL) == 200);
	clean(&c, "/wbs");
	HAS(&c, "<td>1.1</td>");
	TK_CHECK(call(&c, "GET", "/plan", NULL) == 200);
	clean(&c, "/plan");
	HAS(&c, "<svg class=\"gantt\"");
	snprintf(buf, sizeof(buf), "<a href=\"/t/%s\"><title>", tix_oid_hex(&dig, hex));
	HAS(&c, buf);
	HAS(&c, "class=\"arrow\"");
	HAS(&c, "class=\"ms");     /* the milestone diamond */
	HAS(&c, "class=\"bar sum"); /* the summary bar */
	HAS(&c, "class=\"today\"");
	HAS(&c, "<b class=\"crit\">late</b>");
	HASNT(&c, "style=");       /* the CSP forbids style attributes */
	snprintf(target, sizeof(target), "/plan?root=%s&critical=1&today=2026-11-10", tix_oid_hex(&house, hex));
	TK_CHECK(call(&c, "GET", target, NULL) == 200);
	HASNT(&c, "Unrelated");
	TK_CHECK(call(&c, "GET", "/plan?root=nope", NULL) == 404);
	TK_CHECK(call(&c, "GET", "/load", NULL) == 200);
	clean(&c, "/load");
	HAS(&c, "Crew \xef\xbf\xbd&lt;img src=x&gt;");
	TK_CHECK(call(&c, "GET", "/export?format=csv", NULL) == 200);
	HEADER(&c, "Content-Type: text/csv");
	HEADER(&c, "Content-Disposition: attachment; filename=\"plan.csv\"");
	TK_CHECK(call(&c, "GET", "/export?format=json", NULL) == 200);
	HEADER(&c, "Content-Type: application/json");
	TK_CHECK(call(&c, "GET", "/export?format=exe", NULL) == 400);
	TK_CHECK(call(&c, "GET", "/export", NULL) == 400);

	/* resources, calendars, identities */
	TK_CHECK(call(&c, "GET", "/res", NULL) == 200);
	clean(&c, "/res");
	HAS(&c, "200%");
	snprintf(target, sizeof(target), "/res/%s", tix_oid_hex(&crew, hex));
	TK_CHECK(call(&c, "GET", target, NULL) == 200);
	clean(&c, "/res/crew");
	HAS(&c, ">Dig<");
	TK_CHECK(call(&c, "GET", "/cal", NULL) == 200);
	clean(&c, "/cal");
	snprintf(target, sizeof(target), "/cal/%s", tix_oid_hex(&cal, hex));
	TK_CHECK(call(&c, "GET", target, NULL) == 200);
	clean(&c, "/cal/site");
	HAS(&c, "&lt;img src=x onerror=alert(1)&gt;");
	snprintf(target, sizeof(target), "/res/%s", tix_oid_hex(&cal, hex)); /* wrong type */
	TK_CHECK(call(&c, "GET", target, NULL) == 404);
	TK_CHECK(call(&c, "GET", "/id", NULL) == 200);
	clean(&c, "/id");
	HAS(&c, "written as <b>Ada &lt;script&gt;</b>");

	web_response_free(&c.res);
	world_close(&w);
}


/* application/x-www-form-urlencoded from name, value, ... NULL (csrf added). */
static const char *form(client *c, ...)
{
	static tix_buf b;
	tix_buf_reset(&b);
	tix_buf_addf(&b, "csrf=%s", c->csrf);
	va_list ap;
	va_start(ap, c);
	for (const char *n; (n = va_arg(ap, const char *));) {
		const char *v = va_arg(ap, const char *);
		tix_buf_addf(&b, "&%s=", n);
		for (const unsigned char *q = (const unsigned char *)v; *q; q++) {
			if ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '-' ||
			    *q == '.' || *q == '_')
				tix_buf_addc(&b, (char)*q);
			else
				tix_buf_addf(&b, "%%%02X", *q);
		}
	}
	va_end(ap);
	return tix_buf_cstr(&b);
}

static tix_entity *ticket(world *w, const tix_oid *id)
{
	tix_entity *t = NULL;
	TK_OK(tix_entity_load(w->repo, TIX_ENTITY_TICKET, id, &t, &e), &e);
	return t;
}

/* The id at the end of a redirect to /t/ID (or /res/ID, /cal/ID). */
static bool redirected_id(const client *c, const char *prefix, tix_oid *out)
{
	size_t n = strlen(prefix);
	return !strncmp(c->loc, prefix, n) && tix_oid_from_hex(out, c->loc + n, strlen(c->loc + n));
}

static void test_forms(void)
{
	world w;
	world_open(&w, false);
	client c = new_client(&w);
	login(&c);
	char hex[TIX_OID_MAX_HEX + 1], head[TIX_OID_MAX_HEX + 1], target[256];
	tix_oid house, t, crew, cal;
	TK_OK(tix_ticket_create(w.repo, "House", "", NULL, &house, &e), &e);

	/* the new-ticket form, prefilled from ?parent= */
	snprintf(target, sizeof(target), "/new?parent=%s", tix_oid_hex(&house, hex));
	TK_CHECK(call(&c, "GET", target, NULL) == 200);
	HAS(&c, "name=\"o.parent\" value=\"\"");
	HAS(&c, "name=\"rank\" value=\"last\"");

	/* create: title, body, labels, parent by prefix, planning values in CLI syntax */
	char pre[9];
	snprintf(pre, sizeof(pre), "%.8s", hex);
	TK_CHECK(call(&c, "POST", "/new", form(&c, "title", "Dig <deep>", "body", "line 1\r\nline 2", "labels", "site, p1, site",
					    "o.labels", "", "parent", pre, "o.parent", "", "rank", "last", "o.rank", "",
					    "effort", "16", "o.effort", "", "start", "2026-11-10", "o.start", "",
					    "status", "in-progress", "o.status", "open", "kind", "task", "o.kind", "task",
					    "deadline", "", "o.deadline", "", NULL)) == 303);
	TK_CHECK(redirected_id(&c, "/t/", &t));
	tix_entity *te = ticket(&w, &t);
	TK_STR_EQ(te->u.ticket.title, "Dig <deep>");
	TK_STR_EQ(te->u.ticket.body, "line 1\nline 2");
	TK_CHECK(te->u.ticket.labels.n == 2 && te->u.ticket.status == TIX_TICKET_IN_PROGRESS);
	TK_CHECK(tix_oid_eq(&te->u.ticket.plan.parent, &house) && te->u.ticket.plan.effort == 16);
	TK_CHECK(te->u.ticket.plan.rank != NULL && te->u.ticket.plan.deadline == TIX_DATE_NONE);
	tix_entity_free(te);

	/* refused creates keep what was typed and say why (the CLI's words) */
	TK_CHECK(call(&c, "POST", "/new", form(&c, "title", "", "o.title", "", NULL)) == 422);
	HAS(&c, "A title is required.");
	TK_CHECK(call(&c, "POST", "/new", form(&c, "title", "Keep me", "o.title", "", "duration", "3x", "o.duration", "",
					    NULL)) == 422);
	HAS(&c, "duration: &#39;3x&#39; should look like 3d (or none)");
	HAS(&c, "value=\"Keep me\"");
	HAS(&c, "value=\"3x\"");
	TK_CHECK(call(&c, "POST", "/new", form(&c, "title", "x", "o.title", "", "parent", "deadbeef", "o.parent", "",
					    NULL)) == 422);
	HAS(&c, "no ticket matches");

	/* the edit form shows the originals */
	snprintf(target, sizeof(target), "/t/%s/edit", tix_oid_hex(&t, hex));
	TK_CHECK(call(&c, "GET", target, NULL) == 200);
	HAS(&c, "name=\"o.title\" value=\"Dig &lt;deep&gt;\"");
	HAS(&c, "name=\"o.effort\" value=\"16\"");
	HAS(&c, "name=\"o.labels\" value=\"p1, site\"");
	HAS(&c, "name=\"head\" value=\"");

	/* only changed fields are written: duration changes, the stale effort original doesn't matter */
	snprintf(target, sizeof(target), "/t/%s", hex);
	te = ticket(&w, &t);
	tix_oid_hex(&te->head, head);
	tix_entity_free(te);
	TK_CHECK(call(&c, "POST", target, form(&c, "head", head, "duration", "5", "o.duration", "", "effort", "16",
					    "o.effort", "16", "labels", "site, ui", "o.labels", "p1, site", NULL)) == 303);
	te = ticket(&w, &t);
	TK_CHECK(te->u.ticket.plan.duration == 5 && te->u.ticket.plan.effort == 16);
	TK_CHECK(te->u.ticket.labels.n == 2 && tix_strs_contains(&te->u.ticket.labels, "ui") &&
		 !tix_strs_contains(&te->u.ticket.labels, "p1"));
	tix_oid_hex(&te->head, head);
	tix_entity_free(te);

	/* a stale tab: someone else retitles the ticket meanwhile */
	tix_op other = tix_op_text(TIX_OP_SET_TITLE, "Dig (by Bob)");
	TK_OK(tix_entity_apply(w.repo, TIX_ENTITY_TICKET, &t, &other, 1, &e), &e);
	tix_op_free(&other);
	TK_CHECK(call(&c, "POST", target, form(&c, "head", head, "title", "Dig (by Ada)", "o.title", "Dig <deep>",
					    NULL)) == 422);
	HAS(&c, "Changed by someone else meanwhile, to: Dig (by Bob)");
	HAS(&c, "value=\"Dig (by Ada)\"");
	HAS(&c, "name=\"o.title\" value=\"Dig (by Bob)\"");
	te = ticket(&w, &t);
	TK_STR_EQ(te->u.ticket.title, "Dig (by Bob)"); /* nothing applied */
	char fresh[TIX_OID_MAX_HEX + 1];
	tix_oid_hex(&te->head, fresh);
	tix_entity_free(te);
	/* sending again from the re-shown form (new head, new original) replaces it */
	TK_CHECK(call(&c, "POST", target, form(&c, "head", fresh, "title", "Dig (by Ada)", "o.title", "Dig (by Bob)",
					    NULL)) == 303);
	te = ticket(&w, &t);
	TK_STR_EQ(te->u.ticket.title, "Dig (by Ada)");
	tix_entity_free(te);
	/* a stale tab that changes a field nobody else touched just applies */
	TK_CHECK(call(&c, "POST", target, form(&c, "head", head, "progress", "40", "o.progress", "0", NULL)) == 303);
	te = ticket(&w, &t);
	TK_CHECK(te->u.ticket.plan.progress == 40);
	TK_STR_EQ(te->u.ticket.title, "Dig (by Ada)");
	tix_oid_hex(&te->head, head);
	tix_entity_free(te);
	/* clearing a value: empty means none */
	TK_CHECK(call(&c, "POST", target, form(&c, "head", head, "start", "", "o.start", "2026-11-10", NULL)) == 303);
	te = ticket(&w, &t);
	TK_CHECK(te->u.ticket.plan.start == TIX_DATE_NONE);
	tix_entity_free(te);
	/* the quick status form on the ticket page */
	TK_CHECK(call(&c, "GET", target, NULL) == 200);
	HAS(&c, "name=\"o.status\" value=\"in-progress\"");
	TK_CHECK(call(&c, "POST", target, form(&c, "status", "blocked", "o.status", "in-progress", NULL)) == 303);
	te = ticket(&w, &t);
	TK_CHECK(te->u.ticket.status == TIX_TICKET_BLOCKED);
	tix_entity_free(te);
	TK_CHECK(call(&c, "POST", target, form(&c, "status", "wip", "o.status", "blocked", NULL)) == 422);
	/* a ticket can't sit under itself: core's rule, shown on the form */
	TK_CHECK(call(&c, "POST", target, form(&c, "parent", hex, "o.parent", "", NULL)) == 422);
	HAS(&c, "Not saved");

	/* comments */
	snprintf(target, sizeof(target), "/t/%s/comment", hex);
	TK_CHECK(call(&c, "POST", target, form(&c, "body", "First <b>!</b>", NULL)) == 303);
	TK_CHECK(call(&c, "POST", target, form(&c, "body", "  \n ", NULL)) == 422);
	te = ticket(&w, &t);
	TK_CHECK(te->u.ticket.ncomments == 1);
	char cid[TIX_OID_MAX_HEX + 16];
	tix_opid_str(&te->u.ticket.comments[0].id, cid);
	tix_entity_free(te);
	snprintf(target, sizeof(target), "/t/%s", hex);
	TK_CHECK(call(&c, "GET", target, NULL) == 200);
	snprintf(target, sizeof(target), "/comment/%s\">edit</a>", cid);
	HAS(&c, target);
	snprintf(target, sizeof(target), "/t/%s/comment/%s", hex, cid);
	TK_CHECK(call(&c, "GET", target, NULL) == 200);
	HAS(&c, "First &lt;b&gt;!&lt;/b&gt;</textarea>");
	TK_CHECK(call(&c, "POST", target, form(&c, "body", "First, edited", NULL)) == 303);
	te = ticket(&w, &t);
	TK_STR_EQ(te->u.ticket.comments[0].body, "First, edited");
	TK_CHECK(te->u.ticket.comments[0].edited);
	tix_entity_free(te);

	/* dependencies */
	tix_oid pred;
	TK_OK(tix_ticket_create(w.repo, "Survey", "", NULL, &pred, &e), &e);
	char phex[TIX_OID_MAX_HEX + 1];
	tix_oid_hex(&pred, phex);
	snprintf(target, sizeof(target), "/t/%s/dep", hex);
	TK_CHECK(call(&c, "POST", target, form(&c, "op", "add", "pred", phex, "kind", "SS", "lag", "-2", NULL)) == 303);
	te = ticket(&w, &t);
	TK_CHECK(te->u.ticket.plan.ndeps == 1 && te->u.ticket.plan.deps[0].kind == TIX_DEP_SS &&
		 te->u.ticket.plan.deps[0].lag == -2);
	tix_entity_free(te);
	TK_CHECK(call(&c, "POST", target, form(&c, "op", "add", "pred", phex, "kind", "XX", NULL)) == 422);
	HAS(&c, "dep: kind must be FS, SS, FF or SF");
	TK_CHECK(call(&c, "POST", target, form(&c, "op", "add", "pred", phex, "lag", "2d", NULL)) == 422);
	TK_CHECK(call(&c, "POST", target, form(&c, "op", "add", "pred", hex, NULL)) == 422); /* itself */
	TK_CHECK(call(&c, "POST", target, form(&c, "op", "remove", "pred", phex, NULL)) == 303);
	te = ticket(&w, &t);
	TK_CHECK(te->u.ticket.plan.ndeps == 0);
	tix_entity_free(te);

	/* resources: create, edit, assign */
	TK_CHECK(call(&c, "GET", "/res/new", NULL) == 200);
	TK_CHECK(call(&c, "POST", "/res/new", form(&c, "name", "Crew", "o.name", "", "kind", "role", "o.kind", "person",
						 "capacity", "300%", "o.capacity", "100", "rate", "95.5 eur", "o.rate", "",
						 NULL)) == 303);
	TK_CHECK(redirected_id(&c, "/res/", &crew));
	tix_entity *re = NULL;
	TK_OK(tix_entity_load(w.repo, TIX_ENTITY_RESOURCE, &crew, &re, &e), &e);
	TK_CHECK(re->u.resource.kind == TIX_RES_ROLE && re->u.resource.capacity == 300 && re->u.resource.rate == 9550);
	char rhead[TIX_OID_MAX_HEX + 1], rhex[TIX_OID_MAX_HEX + 1];
	tix_oid_hex(&re->head, rhead);
	tix_entity_free(re);
	tix_oid_hex(&crew, rhex);
	TK_CHECK(call(&c, "POST", "/res/new", form(&c, "name", "", "o.name", "", NULL)) == 422);
	TK_CHECK(call(&c, "POST", "/res/new", form(&c, "name", "X", "o.name", "", "capacity", "0", "o.capacity", "100",
						 NULL)) == 422);
	HAS(&c, "capacity: percent of one full-time unit");
	snprintf(target, sizeof(target), "/res/%s/edit", rhex);
	TK_CHECK(call(&c, "GET", target, NULL) == 200);
	HAS(&c, "name=\"o.rate\" value=\"95.50 EUR\"");
	snprintf(target, sizeof(target), "/res/%s", rhex);
	TK_CHECK(call(&c, "POST", target, form(&c, "head", rhead, "name", "Site crew", "o.name", "Crew", "active", "no",
					    "o.active", "yes", NULL)) == 303);
	TK_OK(tix_entity_load(w.repo, TIX_ENTITY_RESOURCE, &crew, &re, &e), &e);
	TK_STR_EQ(re->u.resource.name, "Site crew");
	TK_CHECK(!re->u.resource.active && re->u.resource.capacity == 300);
	tix_entity_free(re);
	snprintf(target, sizeof(target), "/t/%s/assign", hex);
	TK_CHECK(call(&c, "POST", target, form(&c, "op", "add", "res", rhex, "units", "50", "effort", "8", NULL)) == 303);
	te = ticket(&w, &t);
	TK_CHECK(te->u.ticket.plan.nassignments == 1 && te->u.ticket.plan.assignments[0].units == 50 &&
		 te->u.ticket.plan.assignments[0].effort == 8);
	tix_entity_free(te);
	TK_CHECK(call(&c, "POST", target, form(&c, "op", "add", "res", rhex, "units", "abc", NULL)) == 422);
	HAS(&c, "assign: units must look like 50%");
	TK_CHECK(call(&c, "POST", target, form(&c, "op", "remove", "res", rhex, NULL)) == 303);
	te = ticket(&w, &t);
	TK_CHECK(te->u.ticket.plan.nassignments == 0);
	tix_entity_free(te);

	/* calendars: create, exceptions */
	TK_CHECK(call(&c, "POST", "/cal/new", form(&c, "name", "Site", "o.name", "", "weekdays", "mon-sat",
						 "o.weekdays", "mon-fri", NULL)) == 303);
	TK_CHECK(redirected_id(&c, "/cal/", &cal));
	char chex[TIX_OID_MAX_HEX + 1];
	tix_oid_hex(&cal, chex);
	snprintf(target, sizeof(target), "/cal/%s/exception", chex);
	TK_CHECK(call(&c, "POST", target, form(&c, "op", "off", "from", "2026-12-24", "to", "2026-12-26", "note",
					    "Holidays", NULL)) == 303);
	tix_entity *ce = NULL;
	TK_OK(tix_entity_load(w.repo, TIX_ENTITY_CALENDAR, &cal, &ce, &e), &e);
	TK_CHECK(ce->u.calendar.weekdays == 0x3f && ce->u.calendar.nexceptions == 1);
	TK_STR_EQ(ce->u.calendar.exceptions[0].note, "Holidays");
	tix_entity_free(ce);
	snprintf(target, sizeof(target), "/cal/%s/edit", chex);
	TK_CHECK(call(&c, "GET", target, NULL) == 200);
	HAS(&c, "name=\"o.weekdays\" value=\"mon-sat\"");
	snprintf(target, sizeof(target), "/cal/%s", chex);
	TK_CHECK(call(&c, "GET", target, NULL) == 200);
	HAS(&c, "Holidays");
	snprintf(target, sizeof(target), "/cal/%s/exception", chex);
	TK_CHECK(call(&c, "POST", target, form(&c, "op", "off", "from", "soon", NULL)) == 422);
	TK_CHECK(call(&c, "POST", target, form(&c, "op", "clear", "from", "2026-12-24", "to", "2026-12-26", NULL)) == 303);
	TK_OK(tix_entity_load(w.repo, TIX_ENTITY_CALENDAR, &cal, &ce, &e), &e);
	TK_CHECK(ce->u.calendar.nexceptions == 0);
	tix_entity_free(ce);

	/* sync with no such remote: an error page, not a crash */
	TK_CHECK(call(&c, "GET", "/sync", NULL) == 200);
	HAS(&c, "Sync now");
	TK_CHECK(call(&c, "POST", "/sync", form(&c, NULL)) == 502);

	/* method mismatches */
	TK_CHECK(call(&c, "GET", "/sync", NULL) == 200);
	snprintf(target, sizeof(target), "/t/%s/dep", hex);
	TK_CHECK(call(&c, "GET", target, NULL) == 405);

	web_response_free(&c.res);
	world_close(&w);
}

static void test_read_only_pages(void)
{
	world w;
	world_open(&w, true);
	tix_oid t;
	TK_OK(tix_ticket_create(w.repo, "T", "", NULL, &t, &e), &e);
	client c = new_client(&w);
	login(&c);
	char hex[TIX_OID_MAX_HEX + 1], target[160];
	snprintf(target, sizeof(target), "/t/%s", tix_oid_hex(&t, hex));
	TK_CHECK(call(&c, "GET", target, NULL) == 200);
	HASNT(&c, "/edit\"");
	HASNT(&c, "Add a comment");
	HASNT(&c, "Add dependency");
	TK_CHECK(call(&c, "GET", "/tickets", NULL) == 200);
	HASNT(&c, "New ticket");
	TK_CHECK(call(&c, "GET", "/sync", NULL) == 200);
	HASNT(&c, "Sync now");
	TK_CHECK(call(&c, "POST", "/new", form(&c, "title", "x", "o.title", "", NULL)) == 405);
	web_response_free(&c.res);
	world_close(&w);
}

int main(void)
{
	test_checks();
	test_read_only();
	test_escaping();
	test_pages();
	test_forms();
	test_read_only_pages();
	return tk_finish();
}
