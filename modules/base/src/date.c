/* Civil dates (H. Hinnant's days_from_civil / civil_from_days). */
#include "tix/base_date.h"

#include <stdio.h>
#include <string.h>

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
	y -= m <= 2;
	int64_t era = (y >= 0 ? y : y - 399) / 400;
	unsigned yoe = (unsigned)(y - era * 400);
	unsigned doy = (153 * (m + (m > 2 ? (unsigned)-3 : 9)) + 2) / 5 + d - 1;
	unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (int64_t)doe - 719468;
}

void tix_date_ymd(tix_date dn, int *year, int *month, int *day)
{
	TIX_REQUIRE(tix_date_valid(dn));
	int64_t z = (int64_t)dn + 719468;
	int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	unsigned doe = (unsigned)(z - era * 146097);
	unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int64_t y = (int64_t)yoe + era * 400;
	unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	unsigned mp = (5 * doy + 2) / 153;
	unsigned d = doy - (153 * mp + 2) / 5 + 1;
	unsigned m = mp < 10 ? mp + 3 : mp - 9;
	if (year)
		*year = (int)(y + (m <= 2));
	if (month)
		*month = (int)m;
	if (day)
		*day = (int)d;
}

static bool digits(const char *s, int n, int *out)
{
	int v = 0;
	for (int i = 0; i < n; i++) {
		if (s[i] < '0' || s[i] > '9')
			return false;
		v = v * 10 + (s[i] - '0');
	}
	*out = v;
	return true;
}

bool tix_date_parse(const char *s, tix_date *out)
{
	int y, m, d;
	if (strlen(s) != 10 || s[4] != '-' || s[7] != '-' || !digits(s, 4, &y) || !digits(s + 5, 2, &m) ||
	    !digits(s + 8, 2, &d))
		return false;
	if (y < 1970 || m < 1 || m > 12 || d < 1)
		return false;
	static const int mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
	if (d > mdays[m - 1] + (m == 2 && leap))
		return false;
	*out = (tix_date)days_from_civil(y, (unsigned)m, (unsigned)d);
	return true;
}

char *tix_date_str(tix_date dn, char *out)
{
	int y, m, d;
	tix_date_ymd(dn, &y, &m, &d);
	snprintf(out, TIX_DATE_STR, "%04d-%02d-%02d", y, m, d);
	return out;
}

int tix_date_weekday(tix_date d)
{
	TIX_REQUIRE(tix_date_valid(d));
	return (int)((d + 3) % 7); /* 1970-01-01 was a Thursday */
}

tix_date tix_date_from_unix(int64_t t)
{
	int64_t d = t >= 0 ? t / 86400 : -((-t + 86399) / 86400);
	if (d < TIX_DATE_MIN)
		return TIX_DATE_MIN;
	if (d > TIX_DATE_MAX)
		return TIX_DATE_MAX;
	return (tix_date)d;
}
