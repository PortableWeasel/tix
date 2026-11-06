/* Escaping HTML writer (html.h). */
#include "html.h"

#include <stdio.h>
#include <string.h>

void html_lit_(html *h, const char *s, size_t n)
{
	tix_buf_add(h->out, s, n);
}

/* U+202A..U+202E and U+2066..U+2069 (embeddings, overrides, isolates): E2 80 AA-AE, E2 81 A6-A9. */
static bool bidi_control(const unsigned char *p, size_t left)
{
	return left >= 3 && p[0] == 0xe2 &&
	       ((p[1] == 0x80 && p[2] >= 0xaa && p[2] <= 0xae) || (p[1] == 0x81 && p[2] >= 0xa6 && p[2] <= 0xa9));
}

void h_textn(html *h, const char *s, size_t n)
{
	size_t run = 0; /* bytes copied as they are, flushed in one go */
	for (size_t i = 0; i < n; i++) {
		const char *rep = NULL;
		size_t skip = 0;
		switch (s[i]) {
		case '&': rep = "&amp;"; break;
		case '<': rep = "&lt;"; break;
		case '>': rep = "&gt;"; break;
		case '"': rep = "&quot;"; break;
		case '\'': rep = "&#39;"; break;
		case '\0': rep = "\xef\xbf\xbd"; break; /* never emitted raw */
		default:
			if (bidi_control((const unsigned char *)s + i, n - i)) {
				rep = "\xef\xbf\xbd";
				skip = 2;
			}
		}
		if (!rep)
			continue;
		tix_buf_add(h->out, s + run, i - run);
		tix_buf_adds(h->out, rep);
		i += skip;
		run = i + 1;
	}
	tix_buf_add(h->out, s + run, n - run);
}

void h_text(html *h, const char *s)
{
	if (s)
		h_textn(h, s, strlen(s));
}

void h_int(html *h, long long v)
{
	tix_buf_addf(h->out, "%lld", v);
}

void h_url(html *h, const char *s)
{
	static const char hex[] = "0123456789ABCDEF";
	for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
		unsigned char c = *p;
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
		    c == '_' || c == '~') {
			tix_buf_addc(h->out, (char)c);
		} else {
			tix_buf_addc(h->out, '%');
			tix_buf_addc(h->out, hex[c >> 4]);
			tix_buf_addc(h->out, hex[c & 15]);
		}
	}
}

void h_id(html *h, const tix_oid *id)
{
	char hex[TIX_OID_MAX_HEX + 1];
	tix_buf_adds(h->out, tix_oid_hex(id, hex));
}

void h_short(html *h, const tix_oid *id)
{
	char hex[TIX_OID_MAX_HEX + 1];
	tix_oid_hex(id, hex);
	tix_buf_add(h->out, hex, TIX_ID_MIN_PREFIX);
}

void h_date(html *h, tix_date d)
{
	char buf[TIX_DATE_STR];
	if (d != TIX_DATE_NONE)
		tix_buf_adds(h->out, tix_date_str(d, buf));
}
