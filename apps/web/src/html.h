/*
 * HTML output with escaping by construction (docs/architecture.md section 13.6).
 *
 * Markup only ever comes from string literals: HL() refuses anything else at compile time
 * (the literal is pasted between two "" tokens). Every other string goes through h_text,
 * which escapes & < > " ' and replaces Unicode bidirectional controls, so text is safe
 * both between tags and inside double-quoted attributes. URLs are built with h_url, which
 * percent-encodes everything but unreserved characters.
 */
#ifndef TIX_WEB_HTML_H
#define TIX_WEB_HTML_H

#include "tix/core.h"

typedef struct html {
	tix_buf *out; /* BORROWS */
} html;

void html_lit_(html *h, const char *s, size_t n);
#define HL(h, lit) html_lit_((h), "" lit "", sizeof("" lit "") - 1)

/* Text or attribute value (double-quoted). NULL writes nothing. */
void h_text(html *h, const char *s);
void h_textn(html *h, const char *s, size_t n);
void h_int(html *h, long long v);
/* One URL component (path segment or query value), percent-encoded. */
void h_url(html *h, const char *s);
/* Full hex id, and the short display id (TIX_ID_MIN_PREFIX characters). */
void h_id(html *h, const tix_oid *id);
void h_short(html *h, const tix_oid *id);
/* YYYY-MM-DD, or nothing for TIX_DATE_NONE. */
void h_date(html *h, tix_date d);

#endif
