#include "tix/base.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *tix_status_name(tix_status s)
{
	switch (s) {
	case TIX_OK: return "ok";
	case TIX_ERR_INVALID: return "invalid";
	case TIX_ERR_NOT_FOUND: return "not-found";
	case TIX_ERR_EXISTS: return "exists";
	case TIX_ERR_CONFLICT: return "conflict";
	case TIX_ERR_AMBIGUOUS: return "ambiguous";
	case TIX_ERR_IO: return "io";
	case TIX_ERR_UNSUPPORTED: return "unsupported";
	case TIX_ERR_REFUSED: return "refused";
	}
	return "unknown";
}

tix_status tix_error_set(tix_error *err, tix_status code, const char *fmt, ...)
{
	if (err) {
		va_list ap;
		err->code = code;
		va_start(ap, fmt);
		vsnprintf(err->msg, sizeof(err->msg), fmt, ap);
		va_end(ap);
	}
	return code;
}

tix_status tix_error_prefix(tix_error *err, const char *fmt, ...)
{
	char pre[128], old[sizeof(err->msg)];
	va_list ap;

	if (!err)
		return TIX_OK;
	va_start(ap, fmt);
	vsnprintf(pre, sizeof(pre), fmt, ap);
	va_end(ap);
	char joined[sizeof(pre) + sizeof(old) + 2];
	memcpy(old, err->msg, sizeof(old));
	snprintf(joined, sizeof(joined), "%s: %s", pre, old);
	/* messages may be truncated */
	memcpy(err->msg, joined, sizeof(err->msg) - 1);
	err->msg[sizeof(err->msg) - 1] = '\0';
	return err->code;
}

_Noreturn void tix_require_fail(const char *file, int line, const char *expr)
{
	fprintf(stderr, "tix: contract violated at %s:%d: REQUIRE(%s)\n", file, line, expr);
	abort();
}

static _Noreturn void oom(void)
{
	fputs("tix: out of memory\n", stderr);
	abort();
}

void *tix_xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);
	if (!p)
		oom();
	return p;
}

void *tix_xcalloc(size_t count, size_t size)
{
	void *p = calloc(count ? count : 1, size ? size : 1);
	if (!p)
		oom();
	return p;
}

void *tix_xrealloc(void *p, size_t n)
{
	void *q = realloc(p, n ? n : 1);
	if (!q)
		oom();
	return q;
}

void *tix_xreallocarray(void *p, size_t count, size_t size)
{
	if (size && count > SIZE_MAX / size)
		oom();
	return tix_xrealloc(p, count * size);
}

char *tix_xstrdup(const char *s)
{
	return tix_xstrndup(s, strlen(s));
}

char *tix_xstrndup(const char *s, size_t n)
{
	char *p = tix_xmalloc(n + 1);
	memcpy(p, s, n);
	p[n] = '\0';
	return p;
}

void tix_strs_free(tix_strs *s)
{
	for (size_t i = 0; i < s->n; i++)
		free(s->v[i]);
	free(s->v);
	s->v = NULL;
	s->n = 0;
}

/* RETURNS index of str, or the insertion point with *found = false. */
static size_t strs_find(const tix_strs *s, const char *str, bool *found)
{
	size_t lo = 0, hi = s->n;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		int c = strcmp(s->v[mid], str);
		if (!c) {
			*found = true;
			return mid;
		}
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	*found = false;
	return lo;
}

bool tix_strs_add(tix_strs *s, const char *str)
{
	bool found;
	size_t at = strs_find(s, str, &found);
	if (found)
		return false;
	s->v = tix_xreallocarray(s->v, s->n + 1, sizeof(*s->v));
	memmove(s->v + at + 1, s->v + at, (s->n - at) * sizeof(*s->v));
	s->v[at] = tix_xstrdup(str);
	s->n++;
	return true;
}

bool tix_strs_contains(const tix_strs *s, const char *str)
{
	bool found;
	strs_find(s, str, &found);
	return found;
}

void tix_strs_copy(tix_strs *dst, const tix_strs *src)
{
	dst->n = src->n;
	dst->v = src->n ? tix_xcalloc(src->n, sizeof(*dst->v)) : NULL;
	for (size_t i = 0; i < src->n; i++)
		dst->v[i] = tix_xstrdup(src->v[i]);
}
