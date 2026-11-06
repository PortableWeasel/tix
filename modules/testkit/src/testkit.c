#include "tix/testkit.h"

#include <stdarg.h>

int tk_failures;
int tk_checks;

void tk_fail(const char *file, int line, const char *fmt, ...)
{
	va_list ap;
	tk_failures++;
	fprintf(stderr, "FAIL %s:%d: ", file, line);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

int tk_finish(void)
{
	fprintf(stderr, "%d checks, %d failures\n", tk_checks, tk_failures);
	return tk_failures ? 1 : 0;
}

uint64_t tk_rand(uint64_t *state)
{
	uint64_t z = (*state += 0x9e3779b97f4a7c15ull);
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
	return z ^ (z >> 31);
}

uint64_t tk_rand_below(uint64_t *state, uint64_t n)
{
	TIX_REQUIRE(n > 0);
	return tk_rand(state) % n;
}

const char *tk_errmsg(const tix_error *e)
{
	return e ? e->msg : "";
}
