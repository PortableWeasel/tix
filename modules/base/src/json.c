#include "tix/base_json.h"

#include <inttypes.h>
#include <string.h>

void tix_jw_init(tix_jw *w, tix_buf *out)
{
	memset(w, 0, sizeof(*w));
	w->out = out;
}

static void value_prefix(tix_jw *w)
{
	if (w->after_key) {
		w->after_key = false;
		return;
	}
	if (w->depth > 0) {
		if (w->need_comma[w->depth - 1])
			tix_buf_addc(w->out, ',');
		w->need_comma[w->depth - 1] = true;
	}
}

static void open(tix_jw *w, char c)
{
	value_prefix(w);
	TIX_REQUIRE(w->depth < TIX_JSON_MAX_DEPTH);
	tix_buf_addc(w->out, c);
	w->need_comma[w->depth++] = false;
}

static void close_(tix_jw *w, char c)
{
	TIX_REQUIRE(w->depth > 0 && !w->after_key);
	w->depth--;
	tix_buf_addc(w->out, c);
}

void tix_jw_obj_begin(tix_jw *w) { open(w, '{'); }
void tix_jw_obj_end(tix_jw *w) { close_(w, '}'); }
void tix_jw_arr_begin(tix_jw *w) { open(w, '['); }
void tix_jw_arr_end(tix_jw *w) { close_(w, ']'); }

void tix_jw_key(tix_jw *w, const char *key)
{
	TIX_REQUIRE(w->depth > 0 && !w->after_key);
	TIX_REQUIRE(!strpbrk(key, "\"\\"));
	value_prefix(w);
	tix_buf_addc(w->out, '"');
	tix_buf_adds(w->out, key);
	tix_buf_adds(w->out, "\":");
	w->after_key = true;
}

void tix_jw_str(tix_jw *w, const char *s)
{
	static const char hex[] = "0123456789abcdef";
	value_prefix(w);
	tix_buf_addc(w->out, '"');
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		switch (*p) {
		case '"': tix_buf_adds(w->out, "\\\""); break;
		case '\\': tix_buf_adds(w->out, "\\\\"); break;
		case '\n': tix_buf_adds(w->out, "\\n"); break;
		case '\r': tix_buf_adds(w->out, "\\r"); break;
		case '\t': tix_buf_adds(w->out, "\\t"); break;
		case '\b': tix_buf_adds(w->out, "\\b"); break;
		case '\f': tix_buf_adds(w->out, "\\f"); break;
		default:
			if (*p < 0x20) {
				char esc[7] = {'\\', 'u', '0', '0', hex[*p >> 4], hex[*p & 15], 0};
				tix_buf_adds(w->out, esc);
			} else {
				tix_buf_addc(w->out, (char)*p);
			}
		}
	}
	tix_buf_addc(w->out, '"');
}

void tix_jw_int(tix_jw *w, int64_t v)
{
	value_prefix(w);
	tix_buf_addf(w->out, "%" PRId64, v);
}

void tix_jw_bool(tix_jw *w, bool v)
{
	value_prefix(w);
	tix_buf_adds(w->out, v ? "true" : "false");
}

void tix_jw_null(tix_jw *w)
{
	value_prefix(w);
	tix_buf_adds(w->out, "null");
}

bool tix_utf8_valid(const char *s, size_t len)
{
	const unsigned char *p = (const unsigned char *)s, *end = p + len;
	while (p < end) {
		unsigned c = *p;
		size_t n;
		uint32_t cp, min;
		if (c == 0)
			return false;
		if (c < 0x80) {
			p++;
			continue;
		} else if ((c & 0xe0) == 0xc0) {
			n = 2, cp = c & 0x1f, min = 0x80;
		} else if ((c & 0xf0) == 0xe0) {
			n = 3, cp = c & 0x0f, min = 0x800;
		} else if ((c & 0xf8) == 0xf0) {
			n = 4, cp = c & 0x07, min = 0x10000;
		} else {
			return false;
		}
		if ((size_t)(end - p) < n)
			return false;
		for (size_t i = 1; i < n; i++) {
			if ((p[i] & 0xc0) != 0x80)
				return false;
			cp = cp << 6 | (p[i] & 0x3f);
		}
		if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
			return false;
		p += n;
	}
	return true;
}
