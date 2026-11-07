/*
 * tix/base.h -- status codes, errors, contracts, allocation, small vectors.
 *
 * Every other module depends on base. Nothing here does I/O except the
 * contract-failure reporter (stderr, then abort), which only fires on
 * programmer errors.
 */
#ifndef TIX_BASE_H
#define TIX_BASE_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Conditions a caller can act on. Programmer errors use TIX_REQUIRE instead. */
typedef enum tix_status {
	TIX_OK = 0,
	TIX_ERR_INVALID,     /* malformed or rule-violating data (incl. remote input) */
	TIX_ERR_NOT_FOUND,   /* object, ref or entity absent */
	TIX_ERR_EXISTS,      /* create of something that already exists */
	TIX_ERR_CONFLICT,    /* compare-and-swap lost: value changed underneath */
	TIX_ERR_AMBIGUOUS,   /* id prefix matches more than one entity */
	TIX_ERR_IO,          /* storage / process failure */
	TIX_ERR_UNSUPPORTED, /* valid but not handled by this version */
	TIX_ERR_REFUSED,     /* well-formed, but a policy forbids it (the autonomy guard, §15.5) */
} tix_status;

/* RETURNS a static, human-readable name ("ok", "invalid", ...). */
const char *tix_status_name(tix_status s);

/* Structured error. Functions taking `tix_error *err` accept NULL (= don't care). */
typedef struct tix_error {
	tix_status code;
	char msg[256];
} tix_error;

#define TIX_ERROR_INIT {TIX_OK, {0}}

/* Records code + formatted message in err (if non-NULL). RETURNS code, so
 * callers can write `return tix_error_set(err, TIX_ERR_INVALID, "...")`. */
tix_status tix_error_set(tix_error *err, tix_status code, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

/* Prepends "<prefix>: " to an existing message, keeping the code. RETURNS err->code
 * (or TIX_OK when err is NULL). */
tix_status tix_error_prefix(tix_error *err, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

/* Contract checks: always on. Report and abort. Never use for untrusted input. */
_Noreturn void tix_require_fail(const char *file, int line, const char *expr);
#define TIX_REQUIRE(cond) \
	((cond) ? (void)0 : tix_require_fail(__FILE__, __LINE__, #cond))

/* Allocators: abort on OOM (not a status). Sizes are overflow-checked. */
void *tix_xmalloc(size_t n);
void *tix_xcalloc(size_t count, size_t size);
void *tix_xrealloc(void *p, size_t n);
void *tix_xreallocarray(void *p, size_t count, size_t size);
char *tix_xstrdup(const char *s);
char *tix_xstrndup(const char *s, size_t n);

/*
 * Growable vector of T: `TIX_VEC(T) v = {0};`.
 * TIX_VEC_PUSH(&v, x) appends a copy of x. Free with free(v.v).
 */
#define TIX_VEC(T) struct { T *v; size_t n, cap; }
#define TIX_VEC_GROW(vec)                                                        \
	do {                                                                     \
		if ((vec)->n == (vec)->cap) {                                    \
			(vec)->cap = (vec)->cap ? (vec)->cap * 2 : 8;            \
			(vec)->v = tix_xreallocarray((vec)->v, (vec)->cap,       \
						     sizeof(*(vec)->v));         \
		}                                                                \
	} while (0)
#define TIX_VEC_PUSH(vec, x)                    \
	do {                                    \
		TIX_VEC_GROW(vec);              \
		(vec)->v[(vec)->n++] = (x);     \
	} while (0)

/* Sorted array of owned strings (sets of labels, etc.). */
typedef struct tix_strs {
	char **v;
	size_t n;
} tix_strs;

void tix_strs_free(tix_strs *s);
/* Inserts a copy of str keeping v sorted (strcmp) and unique. RETURNS true if added. */
bool tix_strs_add(tix_strs *s, const char *str);
bool tix_strs_contains(const tix_strs *s, const char *str);
void tix_strs_copy(tix_strs *dst, const tix_strs *src);

#endif
