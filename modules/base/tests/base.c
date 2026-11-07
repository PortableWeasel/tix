#include "tix/base.h"
#include "tix/base_buf.h"
#include "tix/base_json.h"
#include "tix/base_oid.h"
#include "tix/base_date.h"
#include "tix/base_sha256.h"
#include <stdio.h>
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

static void test_date(void)
{
	tix_date d;
	char s[TIX_DATE_STR];
	TK_CHECK(tix_date_parse("1970-01-01", &d) && d == 0);
	TK_CHECK(tix_date_weekday(0) == 3); /* Thursday */
	TK_CHECK(tix_date_parse("2026-11-06", &d) && tix_date_weekday(d) == 4); /* Friday */
	TK_STR_EQ(tix_date_str(d, s), "2026-11-06");
	TK_CHECK(tix_date_parse("2024-02-29", &d));
	TK_STR_EQ(tix_date_str(d + 1, s), "2024-03-01");
	TK_CHECK(tix_date_parse("9999-12-31", &d) && d == TIX_DATE_MAX);
	TK_CHECK(tix_date_parse("2000-02-29", &d)); /* 400-year leap */
	const char *bad[] = {"2023-02-29", "1900-02-29", "1969-12-31", "2026-13-01", "2026-00-10", "2026-04-31",
			     "2026-1-01", "2026-01-1", "20260101", "2026-01-01x", " 2026-01-01", "+026-01-01",
			     "", "2026/01/01", "2026-01-00"};
	for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++)
		TK_CHECK(!tix_date_parse(bad[i], &d));
	/* round trip over every day of a few centuries */
	bool ok = true;
	for (tix_date x = 0; x <= TIX_DATE_MAX && ok; x += 13) {
		tix_date y;
		ok = tix_date_parse(tix_date_str(x, s), &y) && y == x;
	}
	TK_CHECK(ok);
	TK_CHECK(tix_date_from_unix(86399) == 0 && tix_date_from_unix(86400) == 1);
	TK_CHECK(tix_date_from_unix(-5) == 0 && tix_date_from_unix(INT64_MAX) == TIX_DATE_MAX);
	TK_CHECK(!tix_date_valid(TIX_DATE_NONE));
}

static void hex32(const uint8_t *d, char *out)
{
	for (int i = 0; i < 32; i++)
		snprintf(out + 2 * i, 3, "%02x", d[i]);
}

static void test_sha256(void)
{
	uint8_t d[32];
	char h[65];
	tix_sha256("", 0, d);
	hex32(d, h);
	TK_STR_EQ(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	tix_sha256("abc", 3, d);
	hex32(d, h);
	TK_STR_EQ(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"; /* 448 bits: padding spills */
	tix_sha256(two, strlen(two), d);
	hex32(d, h);
	TK_STR_EQ(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
	/* RFC 4231 test cases 1, 2 and 6 (a key longer than a block) */
	uint8_t k1[20];
	memset(k1, 0x0b, sizeof(k1));
	tix_hmac_sha256(k1, sizeof(k1), "Hi There", 8, d);
	hex32(d, h);
	TK_STR_EQ(h, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
	tix_hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, d);
	hex32(d, h);
	TK_STR_EQ(h, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
	uint8_t k6[131];
	memset(k6, 0xaa, sizeof(k6));
	const char *m6 = "Test Using Larger Than Block-Size Key - Hash Key First";
	tix_hmac_sha256(k6, sizeof(k6), m6, strlen(m6), d);
	hex32(d, h);
	TK_STR_EQ(h, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
	uint8_t a[4] = {1, 2, 3, 4}, b[4] = {1, 2, 3, 5};
	TK_CHECK(tix_bytes_eq_ct(a, a, 4) && !tix_bytes_eq_ct(a, b, 4));
}

int main(void)
{
	test_sha256();
	test_buf();
	test_error();
	test_oid();
	test_json();
	test_utf8();
	test_strs();
	test_date();
	return tk_finish();
}
