/* tix/base_buf.h -- growable byte buffer, always NUL-terminated. */
#ifndef TIX_BASE_BUF_H
#define TIX_BASE_BUF_H

#include "tix/base.h"

typedef struct tix_buf {
	char *data; /* NUL-terminated when cap > 0; NULL when empty-and-unallocated */
	size_t len;
	size_t cap;
} tix_buf;

#define TIX_BUF_INIT {NULL, 0, 0}

void tix_buf_free(tix_buf *b);
void tix_buf_reset(tix_buf *b);                          /* len = 0, keeps memory */
void tix_buf_add(tix_buf *b, const void *data, size_t n);
void tix_buf_addc(tix_buf *b, char c);
void tix_buf_adds(tix_buf *b, const char *s);
void tix_buf_addf(tix_buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* RETURNS "" for a never-written buffer, so callers can print it unconditionally. */
const char *tix_buf_cstr(const tix_buf *b);
/* OWNS: caller frees the result. Leaves b empty. Never NULL. */
char *tix_buf_detach(tix_buf *b);

#endif
