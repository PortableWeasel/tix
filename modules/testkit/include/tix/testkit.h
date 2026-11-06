/*
 * tix/testkit.h -- minimal test helpers. Linked only into tests.
 *
 *   int main(void) { TK_CHECK(1 + 1 == 2); return tk_finish(); }
 */
#ifndef TIX_TESTKIT_H
#define TIX_TESTKIT_H

#include "tix/base.h"

#include <stdio.h>
#include <string.h>

extern int tk_failures;
extern int tk_checks;

void tk_fail(const char *file, int line, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

#define TK_CHECK(cond)                                                       \
	do {                                                                 \
		tk_checks++;                                                 \
		if (!(cond))                                                 \
			tk_fail(__FILE__, __LINE__, "CHECK(%s)", #cond);     \
	} while (0)

#define TK_STR_EQ(a, b)                                                                     \
	do {                                                                                \
		const char *tk_a_ = (a), *tk_b_ = (b);                                      \
		tk_checks++;                                                                \
		if (!tk_a_ || !tk_b_ || strcmp(tk_a_, tk_b_))                               \
			tk_fail(__FILE__, __LINE__, "%s == %s\n    got:  \"%s\"\n    want: \"%s\"", \
				#a, #b, tk_a_ ? tk_a_ : "(null)", tk_b_ ? tk_b_ : "(null)");        \
	} while (0)

/* Checks a status expression; `err` (tix_error *) supplies the message. */
#define TK_STATUS(expr, want, err)                                                        \
	do {                                                                              \
		tix_status tk_s_ = (expr);                                                \
		tk_checks++;                                                              \
		if (tk_s_ != (want))                                                      \
			tk_fail(__FILE__, __LINE__, "%s -> %s (want %s): %s", #expr,      \
				tix_status_name(tk_s_), tix_status_name(want),            \
				tk_errmsg(err));                                 \
	} while (0)
#define TK_OK(expr, err) TK_STATUS(expr, TIX_OK, err)

/* RETURNS e->msg, or "" for NULL. */
const char *tk_errmsg(const tix_error *e);

/* Prints a summary. RETURNS the process exit code (0 = all passed). */
int tk_finish(void);

/* Deterministic PRNG (splitmix64). */
uint64_t tk_rand(uint64_t *state);
/* RETURNS uniform value in [0, n). REQUIRES n > 0. */
uint64_t tk_rand_below(uint64_t *state, uint64_t n);

#endif
