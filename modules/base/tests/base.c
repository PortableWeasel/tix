#include "tix/base.h"
#include "tix/base_buf.h"
#include "tix/base_json.h"
#include "tix/base_oid.h"
#include "tix/testkit.h"

#include <stdlib.h>

static void test_buf(void)
{
	tix_buf b = TIX_BUF_INIT;
	TK_STR_EQ(tix_buf_cstr(&b), "");
	tix_buf_adds(&b, "ab");
	tix_buf_addf(&b, "%d-%s", 42, "x");
	tix_buf_addc(&b, '!');
	TK_STR_EQ(tix_buf_cstr(&b), "ab42-x!");
	TK_CHECK(b.len == 7);
	for (int i = 0; i < 1000; i++)
		tix_buf_adds(&b, "0123456789");
	TK_CHECK(b.len == 10007);
	tix_buf_reset(&b);
	TK_STR_EQ(tix_buf_cstr(&b), "");
	char *d = tix_buf_detach(&b);
	TK_STR_EQ(d, "");
	free(d);
	tix_buf_free(&b);
}

static void test_error(void)
{
	tix_error e = TIX_ERROR_INIT;
	TK_CHECK(tix_error_set(&e, TIX_ERR_INVALID, "bad %s", "thing") == TIX_ERR_INVALID);
	TK_CHECK(tix_error_prefix(&e, "line %d", 3) == TIX_ERR_INVALID);
	TK_STR_EQ(e.msg, "line 3: bad thing");
	TK_CHECK(tix_error_set(NULL, TIX_ERR_IO, "ignored") == TIX_ERR_IO);
	TK_STR_EQ(tix_status_name(TIX_ERR_CONFLICT), "conflict");
}

static void test_oid(void)
{
	const char *h = "0123456789abcdef0123456789abcdef01234567";
	char out[TIX_OID_MAX_HEX + 1];
	tix_oid a, b;
	TK_CHECK(tix_oid_from_hex(&a, h, 40));
	TK_CHECK(a.algo == TIX_HASH_SHA1);
	TK_STR_EQ(tix_oid_hex(&a, out), h);
	TK_CHECK(!tix_oid_from_hex(&b, "0123456789ABCDEF0123456789abcdef01234567", 40));
	TK_CHECK(!tix_oid_from_hex(&b, h, 39));
	TK_CHECK(tix_oid_from_hex(&b, "1123456789abcdef0123456789abcdef01234567", 40));
	TK_CHECK(tix_oid_cmp(&a, &b) < 0 && tix_oid_cmp(&b, &a) > 0 && tix_oid_eq(&a, &a));
	char h64[65];
	for (int i = 0; i < 64; i++)
		h64[i] = "0123456789abcdef"[i % 16];
	h64[64] = 0;
	TK_CHECK(tix_oid_from_hex(&b, h64, 64) && b.algo == TIX_HASH_SHA256);
	TK_STR_EQ(tix_oid_hex(&b, out), h64);
}

static void test_json(void)
{
	tix_buf b = TIX_BUF_INIT;
	tix_jw w;
	tix_jw_init(&w, &b);
	tix_jw_obj_begin(&w);
	tix_jw_key(&w, "s");
	tix_jw_str(&w, "q\"b\\n\n\x01 é");
	tix_jw_key(&w, "i");
	tix_jw_int(&w, -12);
	tix_jw_key(&w, "a");
	tix_jw_arr_begin(&w);
	tix_jw_bool(&w, true);
	tix_jw_null(&w);
	tix_jw_arr_begin(&w);
	tix_jw_arr_end(&w);
	tix_jw_obj_begin(&w);
	tix_jw_obj_end(&w);
	tix_jw_arr_end(&w);
	tix_jw_obj_end(&w);
	TK_STR_EQ(tix_buf_cstr(&b), "{\"s\":\"q\\\"b\\\\n\\n\\u0001 é\",\"i\":-12,\"a\":[true,null,[],{}]}");
	tix_buf_free(&b);
}

static void test_utf8(void)
{
	TK_CHECK(tix_utf8_valid("plain", 5));
	TK_CHECK(tix_utf8_valid("é€😀", strlen("é€😀")));
	TK_CHECK(!tix_utf8_valid("\xc0\xaf", 2));         /* overlong */
	TK_CHECK(!tix_utf8_valid("\xed\xa0\x80", 3));     /* surrogate */
	TK_CHECK(!tix_utf8_valid("\xf4\x90\x80\x80", 4)); /* > U+10FFFF */
	TK_CHECK(!tix_utf8_valid("\xe2\x82", 2));         /* truncated */
	TK_CHECK(!tix_utf8_valid("a\0b", 3));             /* NUL */
	TK_CHECK(!tix_utf8_valid("\xff", 1));
}

static void test_strs(void)
{
	tix_strs s = {0}, c = {0};
	TK_CHECK(tix_strs_add(&s, "b"));
	TK_CHECK(tix_strs_add(&s, "a"));
	TK_CHECK(tix_strs_add(&s, "c"));
	TK_CHECK(!tix_strs_add(&s, "a"));
	TK_CHECK(s.n == 3);
	TK_STR_EQ(s.v[0], "a");
	TK_STR_EQ(s.v[2], "c");
	TK_CHECK(tix_strs_contains(&s, "b") && !tix_strs_contains(&s, "d"));
	tix_strs_copy(&c, &s);
	TK_STR_EQ(c.v[1], "b");
	tix_strs_free(&s);
	tix_strs_free(&c);
}

int main(void)
{
	test_buf();
	test_error();
	test_oid();
	test_json();
	test_utf8();
	test_strs();
	return tk_finish();
}
