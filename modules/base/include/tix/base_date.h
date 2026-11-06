/*
 * tix/base_date.h -- civil dates as day numbers (pure, no time zones, no locale).
 *
 * A tix_date counts days since 1970-01-01 (proleptic Gregorian). Planning works in
 * whole days (architecture §5), so there is no time of day. The wire form is
 * "YYYY-MM-DD", years 1970..9999 only (TIX_DATE_MIN..TIX_DATE_MAX).
 */
#ifndef TIX_BASE_DATE_H
#define TIX_BASE_DATE_H

#include "tix/base.h"

typedef int32_t tix_date;

#define TIX_DATE_MIN 0            /* 1970-01-01 */
#define TIX_DATE_MAX 2932896      /* 9999-12-31 */
#define TIX_DATE_NONE INT32_MIN   /* "unset" in read models */
#define TIX_DATE_STR 11           /* "YYYY-MM-DD" + NUL */

/* RETURNS false unless s is exactly "YYYY-MM-DD", a real calendar day in range. */
bool tix_date_parse(const char *s, tix_date *out);
/* REQUIRES d in range. Writes "YYYY-MM-DD" into out[TIX_DATE_STR]. RETURNS out. */
char *tix_date_str(tix_date d, char *out);
/* REQUIRES d in range. Fills y/m/d (any may be NULL). */
void tix_date_ymd(tix_date d, int *year, int *month, int *day);
/* 0 = Monday ... 6 = Sunday. REQUIRES d in range. */
int tix_date_weekday(tix_date d);
/* The UTC day containing unix time t (clamped to the range). */
tix_date tix_date_from_unix(int64_t t);
/* RETURNS true if d is a valid in-range date (not TIX_DATE_NONE). */
static inline bool tix_date_valid(tix_date d)
{
	return d >= TIX_DATE_MIN && d <= TIX_DATE_MAX;
}

#endif
