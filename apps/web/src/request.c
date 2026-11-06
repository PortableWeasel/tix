/* Request decoding and response helpers (web_http.h). Pure: no sockets, no Mongoose. */
#include "web_http.h"

#include "tix/base_json.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static bool fail(char *why, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static bool fail(char *why, size_t n, const char *fmt, ...)
{
	if (why && n) {
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(why, n, fmt, ap);
		va_end(ap);
	}
	return false;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static bool name_ok(const char *s, size_t n)
{
	if (!n || n > WEB_MAX_NAME)
		return false;
	for (size_t i = 0; i < n; i++) {
		char c = s[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
		      c == '_' || c == '-'))
			return false;
	}
	return true;
}

/* %-decodes s[0..n) ('+' is a space) into a fresh string; CR LF and lone CR become LF.
 * RETURNS NULL on a bad escape, a NUL byte or invalid UTF-8. */
static char *decode(const char *s, size_t n)
{
	char *out = tix_xmalloc(n + 1);
	size_t o = 0;
	for (size_t i = 0; i < n; i++) {
		char c = s[i];
		if (c == '+') {
			c = ' ';
		} else if (c == '%') {
			int hi = i + 2 < n ? hexval(s[i + 1]) : -1; /* needs both digits inside s */
			int lo = i + 2 < n ? hexval(s[i + 2]) : -1;
			if (hi < 0 || lo < 0) {
				free(out);
				return NULL;
			}
			c = (char)(hi << 4 | lo);
			i += 2;
		}
		if (c == '\0') {
			free(out);
			return NULL;
		}
		out[o++] = c;
	}
	out[o] = '\0';
	/* line endings: CRLF and CR -> LF */
	size_t w = 0;
	for (size_t r = 0; r < o; r++) {
		if (out[r] == '\r') {
			out[w++] = '\n';
			if (r + 1 < o && out[r + 1] == '\n')
				r++;
		} else {
			out[w++] = out[r];
		}
	}
	out[w] = '\0';
	if (!tix_utf8_valid(out, w)) {
		free(out);
		return NULL;
	}
	return out;
}

void web_kvs_free(web_kvs *kv)
{
	for (size_t i = 0; i < kv->n; i++) {
		free(kv->v[i].name);
		free(kv->v[i].value);
	}
	free(kv->v);
	kv->v = NULL;
	kv->n = 0;
}

static void kvs_push(web_kvs *kv, char *name, char *value)
{
	kv->v = tix_xreallocarray(kv->v, kv->n + 1, sizeof(*kv->v));
	kv->v[kv->n].name = name;
	kv->v[kv->n].value = value;
	kv->n++;
}

bool web_urldecode_form(const char *s, size_t len, web_kvs *out, char *why, size_t why_len)
{
	size_t i = 0;
	while (i < len) {
		const char *amp = memchr(s + i, '&', len - i);
		size_t end = amp ? (size_t)(amp - s) : len;
		if (end > i) { /* empty pieces ("a=1&&b=2") are skipped, as browsers do */
			const char *eq = memchr(s + i, '=', end - i);
			size_t nend = eq ? (size_t)(eq - s) : end;
			if (out->n >= WEB_MAX_FIELDS) {
				web_kvs_free(out);
				return fail(why, why_len, "more than %d fields", WEB_MAX_FIELDS);
			}
			char *name = decode(s + i, nend - i);
			if (!name || !name_ok(name, strlen(name))) {
				free(name);
				web_kvs_free(out);
				return fail(why, why_len, "bad field name");
			}
			char *value = eq ? decode(eq + 1, end - nend - 1) : tix_xstrdup("");
			if (!value) {
				fail(why, why_len, "field '%s' is not valid text", name);
				free(name);
				web_kvs_free(out);
				return false;
			}
			kvs_push(out, name, value);
		}
		i = end + 1;
	}
	return true;
}

/* "a=b; c=d". Values are taken as they are (no %-decoding); malformed pieces are skipped. */
static void parse_cookies(const char *s, size_t len, web_kvs *out)
{
	size_t i = 0;
	while (i < len && out->n < WEB_MAX_FIELDS) {
		while (i < len && (s[i] == ' ' || s[i] == '\t'))
			i++;
		const char *semi = memchr(s + i, ';', len - i);
		size_t end = semi ? (size_t)(semi - s) : len;
		const char *eq = memchr(s + i, '=', end - i);
		if (eq && name_ok(s + i, (size_t)(eq - s) - i)) {
			size_t vlen = end - (size_t)(eq - s) - 1;
			const char *v = eq + 1;
			while (vlen && (v[vlen - 1] == ' ' || v[vlen - 1] == '\t'))
				vlen--;
			bool clean = true;
			for (size_t k = 0; k < vlen; k++)
				if ((unsigned char)v[k] < 0x21 || (unsigned char)v[k] > 0x7e || v[k] == '"' || v[k] == ',' ||
				    v[k] == '\\')
					clean = false;
			if (clean)
				kvs_push(out, tix_xstrndup(s + i, (size_t)(eq - s) - i), tix_xstrndup(v, vlen));
		}
		i = end + 1;
	}
}

static char *header_dup(const web_raw *raw, const char *name)
{
	size_t n = 0;
	const char *v = raw->header ? raw->header(raw->ctx, name, &n) : NULL;
	if (!v)
		return NULL;
	if (memchr(v, '\0', n) || !tix_utf8_valid(v, n))
		return tix_xstrdup(""); /* present but unusable: never matches anything */
	return tix_xstrndup(v, n);
}

bool web_request_decode(const web_raw *raw, web_request *req, char *why, size_t why_len)
{
	memset(req, 0, sizeof(*req));
	if (!raw->method_len || !raw->uri_len || raw->uri[0] != '/' || memchr(raw->uri, '\0', raw->uri_len) ||
	    !tix_utf8_valid(raw->uri, raw->uri_len))
		return fail(why, why_len, "bad request line");
	req->method = tix_xstrndup(raw->method, raw->method_len);
	req->path = tix_xstrndup(raw->uri, raw->uri_len);
	req->host = header_dup(raw, "Host");
	req->origin = header_dup(raw, "Origin");
	req->referer = header_dup(raw, "Referer");
	req->content_type = header_dup(raw, "Content-Type");
	size_t clen = 0;
	const char *cookie = raw->header ? raw->header(raw->ctx, "Cookie", &clen) : NULL;
	if (cookie)
		parse_cookies(cookie, clen, &req->cookies);
	if (raw->query_len && !web_urldecode_form(raw->query, raw->query_len, &req->query, why, why_len))
		return false;
	if (raw->body_len) {
		if (raw->body_len > WEB_MAX_BODY)
			return fail(why, why_len, "body too large");
		const char *ct = req->content_type ? req->content_type : "";
		size_t base = strcspn(ct, ";");
		while (base && ct[base - 1] == ' ')
			base--;
		if (base != 33 || strncasecmp(ct, "application/x-www-form-urlencoded", 33))
			return fail(why, why_len, "only form bodies are accepted");
		if (!web_urldecode_form(raw->body, raw->body_len, &req->form, why, why_len))
			return false;
		req->form_body = true;
	}
	return true;
}

void web_request_free(web_request *req)
{
	free(req->method);
	free(req->path);
	free(req->host);
	free(req->origin);
	free(req->referer);
	free(req->content_type);
	web_kvs_free(&req->query);
	web_kvs_free(&req->form);
	web_kvs_free(&req->cookies);
	memset(req, 0, sizeof(*req));
}

const char *web_kvs_get(const web_kvs *kv, const char *name)
{
	for (size_t i = 0; i < kv->n; i++)
		if (!strcmp(kv->v[i].name, name))
			return kv->v[i].value;
	return NULL;
}

size_t web_kvs_all(const web_kvs *kv, const char *name, const char **out, size_t max)
{
	size_t n = 0;
	for (size_t i = 0; i < kv->n && n < max; i++)
		if (!strcmp(kv->v[i].name, name))
			out[n++] = kv->v[i].value;
	return n;
}

void web_header(web_response *res, const char *name, const char *value)
{
	TIX_REQUIRE(name && value && !strpbrk(name, "\r\n:") && !strpbrk(value, "\r\n"));
	tix_buf_addf(&res->headers, "%s: %s\r\n", name, value);
}

void web_response_free(web_response *res)
{
	tix_buf_free(&res->headers);
	tix_buf_free(&res->body);
}
