/*
 * tix/base_json.h -- canonical JSON writer + UTF-8 validation.
 *
 * The writer produces one byte-stable encoding: no whitespace, keys in the order
 * the caller emits them, integers only, strings escaped minimally (", \, and
 * control characters; everything else is raw UTF-8). Stored bytes depend on it,
 * so changing its output is a format change.
 */
#ifndef TIX_BASE_JSON_H
#define TIX_BASE_JSON_H

#include "tix/base_buf.h"

#define TIX_JSON_MAX_DEPTH 16

typedef struct tix_jw {
	tix_buf *out;            /* BORROWS */
	int depth;
	bool need_comma[TIX_JSON_MAX_DEPTH];
	bool after_key;
} tix_jw;

void tix_jw_init(tix_jw *w, tix_buf *out);
void tix_jw_obj_begin(tix_jw *w);
void tix_jw_obj_end(tix_jw *w);
void tix_jw_arr_begin(tix_jw *w);
void tix_jw_arr_end(tix_jw *w);
/* REQUIRES: inside an object, key is plain ASCII without quotes/backslashes. */
void tix_jw_key(tix_jw *w, const char *key);
/* REQUIRES: s is valid UTF-8 without NUL (validate untrusted text first). */
void tix_jw_str(tix_jw *w, const char *s);
void tix_jw_int(tix_jw *w, int64_t v);
void tix_jw_bool(tix_jw *w, bool v);
void tix_jw_null(tix_jw *w);

/* Strict UTF-8: no overlongs, no surrogates, <= U+10FFFF, no NUL. */
bool tix_utf8_valid(const char *s, size_t len);

#endif
