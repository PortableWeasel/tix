/* Request decoding: urlencoded forms, cookies, limits, hostile input. No sockets. */
#include "tix/base_json.h"
#include "tix/testkit.h"
#include "web_http.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct hdrs {
	const char *names[8], *values[8];
} hdrs;

static const char *hdr(void *ctx, const char *name, size_t *len)
{
	hdrs *h = ctx;
	for (int i = 0; i < 8 && h->names[i]; i++)
		if (!strcasecmp(h->names[i], name)) {
			*len = strlen(h->values[i]);
			return h->values[i];
		}
	return NULL;
}

static bool decode(const char *method, const char *uri, const char *query, const char *body, hdrs *h,
		   web_request *req, char *why)
{
	web_raw raw = {method, uri, query ? query : "", body ? body : "", strlen(method), strlen(uri),
		       query ? strlen(query) : 0, body ? strlen(body) : 0, hdr, h, NULL, NULL};
	return web_request_decode(&raw, req, why, 128);
}

static void test_forms(void)
{
	web_kvs kv = {0};
	char why[128];
	TK_CHECK(web_urldecode_form("a=1&b=x+y%20z&c=&d&&e=%E2%9C%93", 31, &kv, why, sizeof(why)));
	TK_CHECK(kv.n == 5);
	TK_STR_EQ(web_kvs_get(&kv, "a"), "1");
	TK_STR_EQ(web_kvs_get(&kv, "b"), "x y z");
	TK_STR_EQ(web_kvs_get(&kv, "c"), "");
	TK_STR_EQ(web_kvs_get(&kv, "d"), "");
	TK_STR_EQ(web_kvs_get(&kv, "e"), "\xe2\x9c\x93");
	TK_CHECK(web_kvs_get(&kv, "zz") == NULL);
	web_kvs_free(&kv);

	const char *rep = "l=bug&l=ui&x=1&l=p1";
	const char *vals[4];
	TK_CHECK(web_urldecode_form(rep, strlen(rep), &kv, why, sizeof(why)));
	TK_CHECK(web_kvs_all(&kv, "l", vals, 4) == 3 && !strcmp(vals[2], "p1"));
	TK_CHECK(web_kvs_all(&kv, "l", vals, 2) == 2);
	web_kvs_free(&kv);

	/* textarea line endings become LF */
	const char *crlf = "body=one%0D%0Atwo%0Dthree%0Afour";
	TK_CHECK(web_urldecode_form(crlf, strlen(crlf), &kv, why, sizeof(why)));
	TK_STR_EQ(web_kvs_get(&kv, "body"), "one\ntwo\nthree\nfour");
	web_kvs_free(&kv);

	/* rejected: bad escapes, NUL, invalid UTF-8, bad names */
	const char *bad[] = {"a=%", "a=%4", "a=%zz", "a=%00", "a=%C3%28", "a=%ED%A0%80", "%=1", "a b=1",
			     "<x>=1", "=1", "a%00b=1", "ab=%e2%9c"};
	for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
		if (web_urldecode_form(bad[i], strlen(bad[i]), &kv, why, sizeof(why)))
			tk_fail(__FILE__, __LINE__, "accepted '%s'", bad[i]);
		TK_CHECK(kv.n == 0 && kv.v == NULL);
	}
	/* a %-escape whose digits would lie past the end of the input */
	char tail[4] = {'a', '=', '%', '4'};
	TK_CHECK(!web_urldecode_form(tail, 3, &kv, why, sizeof(why)));
	TK_CHECK(!web_urldecode_form(tail, 4, &kv, why, sizeof(why)));

	/* field limit */
	tix_buf many = TIX_BUF_INIT;
	for (int i = 0; i <= WEB_MAX_FIELDS; i++)
		tix_buf_addf(&many, "%sf%d=1", i ? "&" : "", i);
	TK_CHECK(!web_urldecode_form(many.data, many.len, &kv, why, sizeof(why)) && strstr(why, "more than"));
	tix_buf_free(&many);
	char longname[WEB_MAX_NAME + 4];
	memset(longname, 'n', WEB_MAX_NAME + 1);
	strcpy(longname + WEB_MAX_NAME + 1, "=1");
	TK_CHECK(!web_urldecode_form(longname, strlen(longname), &kv, why, sizeof(why)));
}

static void test_requests(void)
{
	web_request req;
	char why[128];
	hdrs h = {{"Host", "Cookie", "Content-Type", "Origin"},
		  {"127.0.0.1:7480", "tix_session=abc123; other=x y; bad\x01=1;  theme=dark ", "application/x-www-form-urlencoded; charset=UTF-8",
		   "http://127.0.0.1:7480"}};
	TK_CHECK(decode("POST", "/t/abc", "q=1", "title=Hi&-x=", &h, &req, why));
	TK_STR_EQ(req.method, "POST");
	TK_STR_EQ(req.path, "/t/abc");
	TK_STR_EQ(req.host, "127.0.0.1:7480");
	TK_STR_EQ(req.origin, "http://127.0.0.1:7480");
	TK_CHECK(req.referer == NULL && req.form_body);
	TK_STR_EQ(web_kvs_get(&req.query, "q"), "1");
	TK_STR_EQ(web_kvs_get(&req.form, "title"), "Hi");
	TK_STR_EQ(web_kvs_get(&req.cookies, "tix_session"), "abc123");
	TK_STR_EQ(web_kvs_get(&req.cookies, "theme"), "dark");
	TK_CHECK(web_kvs_get(&req.cookies, "other") == NULL); /* a space is not a cookie value */
	TK_CHECK(req.cookies.n == 2);
	web_request_free(&req);

	/* a non-form body is refused (415 at the server); so are bad request lines */
	hdrs json = {{"Content-Type"}, {"application/json"}};
	TK_CHECK(!decode("POST", "/x", NULL, "{}", &json, &req, why) && strstr(why, "only form"));
	web_request_free(&req);
	hdrs none = {{0}, {0}};
	TK_CHECK(!decode("POST", "/x", NULL, "a=1", &none, &req, why));
	web_request_free(&req);
	hdrs tricky = {{"Content-Type"}, {"application/x-www-form-urlencodedX"}};
	TK_CHECK(!decode("POST", "/x", NULL, "a=1", &tricky, &req, why));
	web_request_free(&req);
	TK_CHECK(!decode("GET", "x", NULL, NULL, &none, &req, why));
	web_request_free(&req);
	TK_CHECK(!decode("GET", "/\xff", NULL, NULL, &none, &req, why));
	web_request_free(&req);
	TK_CHECK(!decode("GET", "/x", "a=%zz", NULL, &none, &req, why));
	web_request_free(&req);
	/* GET without a body: no form, no content type needed */
	TK_CHECK(decode("GET", "/", NULL, NULL, &none, &req, why) && !req.form_body && !req.form.n);
	web_request_free(&req);
	/* a header with invalid UTF-8 is present but empty, so it can never match */
	hdrs badhost = {{"Host"}, {"\xff\xfe"}};
	TK_CHECK(decode("GET", "/", NULL, NULL, &badhost, &req, why) && req.host && !*req.host);
	web_request_free(&req);
}

/* Random bytes through the decoder: never crashes, leaks or accepts invalid output. */
static void test_fuzz(void)
{
	uint64_t rng = 42;
	const char alphabet[] = "ab=&%+0F9eE\r\n\x00\xc3\xa9\xff-._ ";
	char buf[96];
	for (int round = 0; round < 20000; round++) {
		size_t n = tk_rand_below(&rng, sizeof(buf));
		for (size_t i = 0; i < n; i++)
			buf[i] = alphabet[tk_rand_below(&rng, sizeof(alphabet) - 1)];
		web_kvs kv = {0};
		char why[64];
		if (web_urldecode_form(buf, n, &kv, why, sizeof(why)))
			for (size_t i = 0; i < kv.n; i++) {
				TK_CHECK(tix_utf8_valid(kv.v[i].value, strlen(kv.v[i].value)));
				TK_CHECK(!strchr(kv.v[i].value, '\r'));
			}
		web_kvs_free(&kv);
	}
}

static void test_response(void)
{
	web_response res = WEB_RESPONSE_INIT;
	web_header(&res, "Location", "/t/abc");
	TK_STR_EQ(tix_buf_cstr(&res.headers), "Location: /t/abc\r\n");
	web_response_free(&res);
}

int main(void)
{
	test_forms();
	test_requests();
	test_fuzz();
	test_response();
	return tk_finish();
}
