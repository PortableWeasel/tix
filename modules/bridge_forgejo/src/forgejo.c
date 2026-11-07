/* Forgejo issues over REST v1 (see tix/bridge_forgejo.h). The only file that includes curl.h. */
#include "tix/bridge_forgejo.h"
#include "tix/base_json.h"

#include <cjson/cJSON.h>
#include <curl/curl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_RESPONSE (32u << 20)
#define PAGE 50
#define MAX_PAGES 4000

typedef struct label {
	char *name;
	int64_t id;
} label;

typedef struct forgejo {
	tix_tracker base;
	char *key;      /* "forgejo:host/owner/repo" */
	char *api;      /* "https://host/api/v1/repos/owner/repo" */
	char *auth;     /* "Authorization: token ..." */
	bool plain_ok;  /* http allowed (loopback) */
	TIX_VEC(label) labels;
	bool have_labels;
	TIX_VEC(struct mark { int64_t number; char *marker; }) markers; /* from the last list */
} forgejo;

static forgejo *FJ(tix_tracker *t) { return (forgejo *)t; }

/* ---- URL -------------------------------------------------------------------------------- */

static bool name_ok(const char *s, size_t n)
{
	if (!n || n > 100)
		return false;
	for (size_t i = 0; i < n; i++)
		if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') ||
		      s[i] == '-' || s[i] == '_' || s[i] == '.'))
			return false;
	return !(n == 1 && s[0] == '.') && !(n == 2 && s[0] == '.' && s[1] == '.');
}

bool tix_bridge_forgejo_parse(const char *url, tix_buf *scheme, tix_buf *host, tix_buf *owner, tix_buf *repo)
{
	const char *p;
	bool https = !strncmp(url, "https://", 8), http = !strncmp(url, "http://", 7);
	if (!https && !http)
		return false;
	p = url + (https ? 8 : 7);
	size_t hl = strcspn(p, "/");
	if (!hl || hl > 255)
		return false;
	for (size_t i = 0; i < hl; i++) /* host[:port], letters digits . - : [ ] only */
		if (!((p[i] >= 'a' && p[i] <= 'z') || (p[i] >= 'A' && p[i] <= 'Z') || (p[i] >= '0' && p[i] <= '9') ||
		      p[i] == '.' || p[i] == '-' || p[i] == ':' || p[i] == '[' || p[i] == ']'))
			return false;
	tix_buf_reset(host);
	tix_buf_add(host, p, hl);
	p += hl;
	if (*p != '/')
		return false;
	p++;
	size_t ol = strcspn(p, "/");
	if (!name_ok(p, ol) || p[ol] != '/')
		return false;
	tix_buf_reset(owner);
	tix_buf_add(owner, p, ol);
	p += ol + 1;
	size_t rl = strcspn(p, "/");
	const char *rest = p + rl;
	if (*rest == '/' && rest[1])
		return false; /* nothing after owner/repo[/] */
	if (rl > 4 && !strncmp(p + rl - 4, ".git", 4))
		rl -= 4;
	if (!name_ok(p, rl))
		return false;
	tix_buf_reset(repo);
	tix_buf_add(repo, p, rl);
	tix_buf_reset(scheme);
	tix_buf_adds(scheme, https ? "https" : "http");
	if (http) { /* only loopback may go unencrypted */
		const char *h = host->data;
		size_t n = strcspn(h, ":");
		if (h[0] == '[')
			n = strcspn(h, "]") + 1;
		if (!((n == 9 && !strncmp(h, "127.0.0.1", 9)) || (n == 9 && !strncmp(h, "localhost", 9)) ||
		      (n == 5 && !strncmp(h, "[::1]", 5))))
			return false;
	}
	return true;
}

/* ---- HTTP ------------------------------------------------------------------------------- */

typedef struct sink {
	tix_buf b;
	bool too_big;
} sink;

static size_t on_data(char *ptr, size_t size, size_t n, void *ud)
{
	sink *s = ud;
	size_t len = size * n;
	if (s->b.len + len > MAX_RESPONSE) {
		s->too_big = true;
		return 0; /* aborts the transfer */
	}
	tix_buf_add(&s->b, ptr, len);
	return len;
}

/* Copies a server message for an error, without control characters, at most 200 bytes. */
static void server_message(const tix_buf *body, char *out, size_t n)
{
	out[0] = '\0';
	cJSON *j = cJSON_ParseWithLength(body->data ? body->data : "", body->len);
	const cJSON *m = j ? cJSON_GetObjectItemCaseSensitive(j, "message") : NULL;
	const char *src = cJSON_IsString(m) ? m->valuestring : "";
	size_t k = 0;
	for (; *src && k + 1 < n && k < 200; src++)
		out[k++] = (unsigned char)*src < 0x20 ? ' ' : *src;
	out[k] = '\0';
	cJSON_Delete(j);
}

/* method path (relative to the repo API) [json body] -> parsed JSON (NULL for an empty
 * reply). RETURNS TIX_ERR_IO for transport problems, TIX_ERR_NOT_FOUND for 404, other
 * statuses TIX_ERR_IO with the server's message. */
static tix_status call(forgejo *f, const char *method, const char *path, const tix_buf *json, cJSON **out,
		       tix_error *err)
{
	*out = NULL;
	CURL *c = curl_easy_init();
	if (!c)
		return tix_error_set(err, TIX_ERR_IO, "forgejo: cannot start libcurl");
	char url[2048];
	snprintf(url, sizeof(url), "%s%s", f->api, path);
	struct curl_slist *h = NULL;
	h = curl_slist_append(h, f->auth);
	h = curl_slist_append(h, "Accept: application/json");
	if (json)
		h = curl_slist_append(h, "Content-Type: application/json");
	sink s = {TIX_BUF_INIT, false};
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
	curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
	curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, f->plain_ok ? "http,https" : "https");
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L); /* the token goes to this host only */
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_USERAGENT, "tix-bridge");
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &s);
	if (json) {
		curl_easy_setopt(c, CURLOPT_POSTFIELDS, json->data ? json->data : "");
		curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)json->len);
	}
	CURLcode rc = curl_easy_perform(c);
	long status = 0;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
	curl_slist_free_all(h);
	curl_easy_cleanup(c);
	tix_status st = TIX_OK;
	if (s.too_big) {
		st = tix_error_set(err, TIX_ERR_IO, "forgejo: %s %s: response over %u bytes", method, path, MAX_RESPONSE);
	} else if (rc != CURLE_OK) {
		st = tix_error_set(err, TIX_ERR_IO, "forgejo: %s %s: %s", method, path, curl_easy_strerror(rc));
	} else if (status < 200 || status > 299) {
		char msg[256];
		server_message(&s.b, msg, sizeof(msg));
		st = tix_error_set(err, status == 404 ? TIX_ERR_NOT_FOUND : TIX_ERR_IO, "forgejo: %s %s: HTTP %ld%s%s",
				   method, path, status, msg[0] ? ": " : "", msg);
	} else if (s.b.len) {
		*out = cJSON_ParseWithLength(s.b.data, s.b.len);
		if (!*out)
			st = tix_error_set(err, TIX_ERR_INVALID, "forgejo: %s %s: not JSON", method, path);
	}
	tix_buf_free(&s.b);
	return st;
}

/* ---- JSON fields ---------------------------------------------------------------------- */

static const char *jstr(const cJSON *o, const char *k)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
	return cJSON_IsString(v) ? v->valuestring : NULL;
}

static bool jint(const cJSON *o, const char *k, int64_t *out)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
	if (!cJSON_IsNumber(v) || v->valuedouble < 1 || v->valuedouble > 9.0e15 || v->valuedouble != (double)(int64_t)v->valuedouble)
		return false;
	*out = (int64_t)v->valuedouble;
	return true;
}

/* ---- labels ----------------------------------------------------------------------------- */

static tix_status load_labels(forgejo *f, tix_error *err)
{
	if (f->have_labels)
		return TIX_OK;
	for (int page = 1; page <= MAX_PAGES; page++) {
		char path[64];
		cJSON *j;
		snprintf(path, sizeof(path), "/labels?limit=%d&page=%d", PAGE, page);
		tix_status st = call(f, "GET", path, NULL, &j, err);
		if (st)
			return st;
		if (!cJSON_IsArray(j)) {
			cJSON_Delete(j);
			return tix_error_set(err, TIX_ERR_INVALID, "forgejo: labels: not a list");
		}
		int n = 0;
		const cJSON *it;
		cJSON_ArrayForEach(it, j)
		{
			n++;
			const char *name = jstr(it, "name");
			int64_t id;
			if (name && jint(it, "id", &id) && tix_utf8_valid(name, strlen(name))) {
				label l = {tix_xstrdup(name), id};
				TIX_VEC_PUSH(&f->labels, l);
			}
		}
		cJSON_Delete(j);
		if (n < PAGE)
			break;
	}
	f->have_labels = true;
	return TIX_OK;
}

static tix_status fj_labels(tix_tracker *t, tix_strs *out, tix_error *err)
{
	forgejo *f = FJ(t);
	tix_status st = load_labels(f, err);
	for (size_t i = 0; !st && i < f->labels.n; i++)
		tix_strs_add(out, f->labels.v[i].name);
	return st;
}

/* ---- issues ----------------------------------------------------------------------------- */

static void remember_marker(forgejo *f, int64_t number, const char *marker)
{
	for (size_t i = 0; i < f->markers.n; i++)
		if (f->markers.v[i].number == number) {
			free(f->markers.v[i].marker);
			f->markers.v[i].marker = marker ? tix_xstrdup(marker) : NULL;
			return;
		}
	struct mark m = {number, marker ? tix_xstrdup(marker) : NULL};
	TIX_VEC_PUSH(&f->markers, m);
}

static const char *marker_of(const forgejo *f, int64_t number)
{
	for (size_t i = 0; i < f->markers.n; i++)
		if (f->markers.v[i].number == number)
			return f->markers.v[i].marker;
	return NULL;
}

static tix_status fj_list(tix_tracker *t, tix_issues *out, tix_error *err)
{
	forgejo *f = FJ(t);
	TIX_VEC(tix_issue) v = {0};
	tix_status st = TIX_OK;
	for (int page = 1; page <= MAX_PAGES && !st; page++) {
		char path[96];
		cJSON *j;
		snprintf(path, sizeof(path), "/issues?state=all&type=issues&limit=%d&page=%d", PAGE, page);
		if ((st = call(f, "GET", path, NULL, &j, err)))
			break;
		if (!cJSON_IsArray(j)) {
			cJSON_Delete(j);
			st = tix_error_set(err, TIX_ERR_INVALID, "forgejo: issues: not a list");
			break;
		}
		int n = 0;
		const cJSON *it;
		cJSON_ArrayForEach(it, j)
		{
			n++;
			int64_t number;
			const char *title = jstr(it, "title"), *state = jstr(it, "state"), *body = jstr(it, "body");
			const cJSON *pr = cJSON_GetObjectItemCaseSensitive(it, "pull_request");
			if (!jint(it, "number", &number) || !title || !state || (pr && !cJSON_IsNull(pr)))
				continue; /* not an issue we can use */
			tix_issue iss = {.number = number, .title = tix_xstrdup(title), .closed = !strcmp(state, "closed")};
			tix_bridge_split_marker(body ? body : "", &iss.body, &iss.marker);
			const cJSON *labels = cJSON_GetObjectItemCaseSensitive(it, "labels"), *l;
			if (cJSON_IsArray(labels))
				cJSON_ArrayForEach(l, labels)
				{
					const char *name = jstr(l, "name");
					if (name && tix_utf8_valid(name, strlen(name)))
						tix_strs_add(&iss.labels, name);
				}
			remember_marker(f, number, iss.marker);
			TIX_VEC_PUSH(&v, iss);
		}
		cJSON_Delete(j);
		if (n < PAGE)
			break;
	}
	out->v = v.v;
	out->n = v.n;
	if (st)
		tix_issues_free(out);
	return st;
}

/* Label ids for names the repository has. */
static void label_ids(forgejo *f, tix_jw *w, const tix_strs *names)
{
	tix_jw_key(w, "labels");
	tix_jw_arr_begin(w);
	for (size_t i = 0; i < names->n; i++)
		for (size_t k = 0; k < f->labels.n; k++)
			if (!strcmp(f->labels.v[k].name, names->v[i]))
				tix_jw_int(w, f->labels.v[k].id);
	tix_jw_arr_end(w);
}

static tix_status set_labels(forgejo *f, int64_t number, const tix_strs *names, tix_error *err)
{
	tix_status st = load_labels(f, err);
	if (st)
		return st;
	tix_buf b = TIX_BUF_INIT;
	tix_jw w;
	tix_jw_init(&w, &b);
	tix_jw_obj_begin(&w);
	label_ids(f, &w, names);
	tix_jw_obj_end(&w);
	char path[64];
	cJSON *j;
	snprintf(path, sizeof(path), "/issues/%" PRId64 "/labels", number);
	st = call(f, "PUT", path, &b, &j, err);
	cJSON_Delete(j);
	tix_buf_free(&b);
	return st;
}

static tix_status fj_create(tix_tracker *t, const tix_issue_update *in, int64_t *number, tix_error *err)
{
	forgejo *f = FJ(t);
	tix_status st = load_labels(f, err);
	if (st)
		return st;
	tix_buf b = TIX_BUF_INIT, body = TIX_BUF_INIT;
	if (in->marker)
		tix_bridge_add_marker(in->body, in->marker, &body);
	else
		tix_buf_adds(&body, in->body ? in->body : "");
	tix_jw w;
	tix_jw_init(&w, &b);
	tix_jw_obj_begin(&w);
	tix_jw_key(&w, "title");
	tix_jw_str(&w, in->title ? in->title : "(untitled)");
	tix_jw_key(&w, "body");
	tix_jw_str(&w, tix_buf_cstr(&body));
	if (in->set_labels && in->labels)
		label_ids(f, &w, in->labels);
	tix_jw_obj_end(&w);
	cJSON *j;
	st = call(f, "POST", "/issues", &b, &j, err);
	if (!st && !jint(j, "number", number))
		st = tix_error_set(err, TIX_ERR_INVALID, "forgejo: created issue without a number");
	if (!st)
		remember_marker(f, *number, in->marker);
	cJSON_Delete(j);
	tix_buf_free(&b);
	tix_buf_free(&body);
	return st;
}

static tix_status fj_update(tix_tracker *t, int64_t number, const tix_issue_update *u, tix_error *err)
{
	forgejo *f = FJ(t);
	tix_status st = TIX_OK;
	if (u->title || u->body || u->set_closed || u->marker) {
		tix_buf b = TIX_BUF_INIT, body = TIX_BUF_INIT;
		tix_jw w;
		tix_jw_init(&w, &b);
		tix_jw_obj_begin(&w);
		if (u->title) {
			tix_jw_key(&w, "title");
			tix_jw_str(&w, u->title);
		}
		if (u->body || u->marker) {
			/* keep (or set) the marker: the body we were given has none */
			const char *mk = u->marker ? u->marker : marker_of(f, number);
			if (mk)
				tix_bridge_add_marker(u->body ? u->body : "", mk, &body);
			else
				tix_buf_adds(&body, u->body ? u->body : "");
			tix_jw_key(&w, "body");
			tix_jw_str(&w, tix_buf_cstr(&body));
		}
		if (u->set_closed) {
			tix_jw_key(&w, "state");
			tix_jw_str(&w, u->closed ? "closed" : "open");
		}
		tix_jw_obj_end(&w);
		char path[48];
		cJSON *j;
		snprintf(path, sizeof(path), "/issues/%" PRId64, number);
		st = call(f, "PATCH", path, &b, &j, err);
		cJSON_Delete(j);
		tix_buf_free(&b);
		tix_buf_free(&body);
	}
	if (!st && u->set_labels && u->labels)
		st = set_labels(f, number, u->labels, err);
	return st;
}

static tix_status fj_comments(tix_tracker *t, int64_t number, tix_issue_comments *out, tix_error *err)
{
	forgejo *f = FJ(t);
	TIX_VEC(tix_issue_comment) v = {0};
	tix_status st = TIX_OK;
	for (int page = 1; page <= MAX_PAGES && !st; page++) {
		char path[96];
		cJSON *j;
		snprintf(path, sizeof(path), "/issues/%" PRId64 "/comments?limit=%d&page=%d", number, PAGE, page);
		if ((st = call(f, "GET", path, NULL, &j, err)))
			break;
		if (!cJSON_IsArray(j)) {
			cJSON_Delete(j);
			st = tix_error_set(err, TIX_ERR_INVALID, "forgejo: comments: not a list");
			break;
		}
		int n = 0;
		const cJSON *it;
		cJSON_ArrayForEach(it, j)
		{
			n++;
			int64_t id;
			const char *body = jstr(it, "body");
			const cJSON *user = cJSON_GetObjectItemCaseSensitive(it, "user");
			const char *login = cJSON_IsObject(user) ? jstr(user, "login") : NULL;
			if (!jint(it, "id", &id) || !body)
				continue;
			tix_issue_comment c = {.id = id, .author = tix_xstrdup(login ? login : "someone")};
			tix_bridge_split_marker(body, &c.body, &c.marker);
			TIX_VEC_PUSH(&v, c);
		}
		cJSON_Delete(j);
		if (n < PAGE)
			break;
	}
	out->v = v.v;
	out->n = v.n;
	if (st)
		tix_issue_comments_free(out);
	return st;
}

static tix_status fj_comment(tix_tracker *t, int64_t number, const char *body, const char *marker, int64_t *id,
			     tix_error *err)
{
	forgejo *f = FJ(t);
	tix_buf b = TIX_BUF_INIT, text = TIX_BUF_INIT;
	tix_bridge_add_marker(body, marker, &text);
	tix_jw w;
	tix_jw_init(&w, &b);
	tix_jw_obj_begin(&w);
	tix_jw_key(&w, "body");
	tix_jw_str(&w, tix_buf_cstr(&text));
	tix_jw_obj_end(&w);
	char path[64];
	cJSON *j;
	snprintf(path, sizeof(path), "/issues/%" PRId64 "/comments", number);
	tix_status st = call(f, "POST", path, &b, &j, err);
	if (!st && !jint(j, "id", id))
		*id = 0;
	cJSON_Delete(j);
	tix_buf_free(&b);
	tix_buf_free(&text);
	return st;
}

static void fj_destroy(tix_tracker *t)
{
	forgejo *f = FJ(t);
	for (size_t i = 0; i < f->labels.n; i++)
		free(f->labels.v[i].name);
	free(f->labels.v);
	for (size_t i = 0; i < f->markers.n; i++)
		free(f->markers.v[i].marker);
	free(f->markers.v);
	if (f->auth) /* the token: don't leave it in freed memory */
		memset(f->auth, 0, strlen(f->auth));
	free(f->auth);
	free(f->api);
	free(f->key);
	free(f);
}

static const tix_tracker_vt fj_vt = {fj_list, fj_labels, fj_create, fj_update, fj_comments, fj_comment, fj_destroy};

tix_status tix_bridge_forgejo_open(const char *url, const char *token, tix_tracker **out, tix_error *err)
{
	TIX_REQUIRE(url && token && out);
	*out = NULL;
	tix_buf scheme = TIX_BUF_INIT, host = TIX_BUF_INIT, owner = TIX_BUF_INIT, repo = TIX_BUF_INIT;
	if (!tix_bridge_forgejo_parse(url, &scheme, &host, &owner, &repo)) {
		tix_buf_free(&scheme);
		tix_buf_free(&host);
		tix_buf_free(&owner);
		tix_buf_free(&repo);
		return tix_error_set(err, TIX_ERR_INVALID,
				     "not a repository URL tix will use: https://host/owner/repo (http only for localhost)");
	}
	for (const char *p = token; *p; p++)
		if ((unsigned char)*p <= ' ' || (unsigned char)*p >= 0x7f) {
			tix_buf_free(&scheme);
			tix_buf_free(&host);
			tix_buf_free(&owner);
			tix_buf_free(&repo);
			return tix_error_set(err, TIX_ERR_INVALID, "the access token has characters a token can't have");
		}
	static bool inited;
	if (!inited) {
		curl_global_init(CURL_GLOBAL_DEFAULT);
		inited = true;
	}
	forgejo *f = tix_xcalloc(1, sizeof(*f));
	f->base.vt = &fj_vt;
	tix_buf b = TIX_BUF_INIT;
	tix_buf_addf(&b, "forgejo:%s/%s/%s", host.data, owner.data, repo.data);
	f->key = tix_xstrdup(b.data);
	tix_buf_reset(&b);
	tix_buf_addf(&b, "%s://%s/api/v1/repos/%s/%s", scheme.data, host.data, owner.data, repo.data);
	f->api = tix_xstrdup(b.data);
	tix_buf_reset(&b);
	tix_buf_addf(&b, "Authorization: token %s", token);
	f->auth = tix_xstrdup(b.data);
	memset(b.data, 0, b.len);
	tix_buf_free(&b);
	f->plain_ok = !strcmp(scheme.data, "http");
	f->base.key = f->key;
	tix_buf_free(&scheme);
	tix_buf_free(&host);
	tix_buf_free(&owner);
	tix_buf_free(&repo);
	*out = &f->base;
	return TIX_OK;
}
