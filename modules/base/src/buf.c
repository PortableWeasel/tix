#include "tix/base_buf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void tix_buf_free(tix_buf *b)
{
	free(b->data);
	b->data = NULL;
	b->len = b->cap = 0;
}

void tix_buf_reset(tix_buf *b)
{
	b->len = 0;
	if (b->data)
		b->data[0] = '\0';
}

static void reserve(tix_buf *b, size_t extra)
{
	TIX_REQUIRE(extra < SIZE_MAX - b->len - 1);
	size_t need = b->len + extra + 1;
	if (need <= b->cap)
		return;
	size_t cap = b->cap ? b->cap : 64;
	while (cap < need)
		cap = cap > SIZE_MAX / 2 ? need : cap * 2;
	b->data = tix_xrealloc(b->data, cap);
	b->cap = cap;
}

void tix_buf_add(tix_buf *b, const void *data, size_t n)
{
	reserve(b, n);
	if (n)
		memcpy(b->data + b->len, data, n);
	b->len += n;
	b->data[b->len] = '\0';
}

void tix_buf_addc(tix_buf *b, char c)
{
	tix_buf_add(b, &c, 1);
}

void tix_buf_adds(tix_buf *b, const char *s)
{
	tix_buf_add(b, s, strlen(s));
}

void tix_buf_addf(tix_buf *b, const char *fmt, ...)
{
	va_list ap, ap2;
	va_start(ap, fmt);
	va_copy(ap2, ap);
	int n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	TIX_REQUIRE(n >= 0);
	reserve(b, (size_t)n);
	vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
	va_end(ap2);
	b->len += (size_t)n;
}

const char *tix_buf_cstr(const tix_buf *b)
{
	return b->data ? b->data : "";
}

char *tix_buf_detach(tix_buf *b)
{
	char *p = b->data ? b->data : tix_xstrdup("");
	b->data = NULL;
	b->len = b->cap = 0;
	return p;
}
