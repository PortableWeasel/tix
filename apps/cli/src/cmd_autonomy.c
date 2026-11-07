/* tix autonomy ID on|off|halt|show  (architecture §15.5) */
#include "cli.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* "08:00" (the next time the local clock shows it) or "YYYY-MM-DD HH:MM" (local).
 * RETURNS unix seconds, or -1. */
static int64_t parse_until(const char *s, int64_t now)
{
	struct tm tm;
	time_t t = (time_t)now;
	int y, mo, d, h, mi;
	char tail;
	if (!localtime_r(&t, &tm))
		return -1;
	if (sscanf(s, "%d-%d-%d %d:%d%c", &y, &mo, &d, &h, &mi, &tail) == 5) {
		tm.tm_year = y - 1900;
		tm.tm_mon = mo - 1;
		tm.tm_mday = d;
	} else if (sscanf(s, "%d:%d%c", &h, &mi, &tail) != 2) {
		return -1;
	}
	if (h < 0 || h > 23 || mi < 0 || mi > 59)
		return -1;
	tm.tm_hour = h;
	tm.tm_min = mi;
	tm.tm_sec = 0;
	tm.tm_isdst = -1;
	time_t at = mktime(&tm);
	if (at == (time_t)-1)
		return -1;
	if (!strchr(s, '-') && at <= t) { /* "08:00" already passed today: tomorrow */
		tm.tm_mday++;
		tm.tm_isdst = -1;
		at = mktime(&tm);
	}
	return (int64_t)at;
}

static long long parse_for(const char *s)
{
	char *end;
	long long v = strtoll(s, &end, 10);
	if (end == s || v <= 0)
		return -1;
	if (!strcmp(end, "h"))
		return v * 3600;
	if (!strcmp(end, "m") || !*end)
		return v * 60;
	if (!strcmp(end, "d"))
		return v * 86400;
	return -1;
}

static int show(cli *c, const tix_oid *id, bool json)
{
	tix_autonomy au;
	int64_t now = (int64_t)time(NULL);
	tix_status st = tix_ticket_autonomy(c->repo, id, now, &au, &c->err);
	if (st)
		return cli_fail(c, st);
	if (json) {
		tix_buf b = TIX_BUF_INIT;
		tix_jw w;
		char hex[TIX_OID_MAX_HEX + 1];
		tix_jw_init(&w, &b);
		tix_jw_obj_begin(&w);
		tix_jw_key(&w, "mode");
		tix_jw_str(&w, tix_autonomy_mode_name(au.mode));
		tix_jw_key(&w, "until");
		if (au.set && au.until >= 0)
			tix_jw_int(&w, au.until);
		else
			tix_jw_null(&w);
		tix_jw_key(&w, "owner");
		if (au.set)
			tix_jw_str(&w, tix_oid_hex(&au.owner, hex));
		else
			tix_jw_null(&w);
		tix_jw_obj_end(&w);
		puts(tix_buf_cstr(&b));
		tix_buf_free(&b);
		return EXIT_OK;
	}
	char s[16], when[32];
	if (!au.set) {
		printf("supervised (nothing up the tree sets autonomy)\n");
		return EXIT_OK;
	}
	printf("%s", tix_autonomy_mode_name(au.mode));
	if (au.mode == TIX_AUTO_AUTONOMOUS && au.until >= 0)
		printf(" until %s", cli_time(au.until, when));
	else if (au.mode == TIX_AUTO_SUPERVISED && au.until >= 0 && au.until <= now)
		printf(" (autonomy expired %s)", cli_time(au.until, when));
	printf("  (set on %s)\n", cli_short(&au.owner, s));
	return EXIT_OK;
}

int cmd_autonomy(cli *c, int argc, char **argv)
{
	if (argc < 3)
		return cli_usage("usage: tix autonomy ID on --until 08:00|--for 10h|--no-expiry | off | halt | show");
	tix_oid id;
	int rc = cli_resolve(c, TIX_ENTITY_TICKET, argv[1], &id);
	if (rc)
		return rc;
	const char *what = argv[2];
	if (!strcmp(what, "show")) {
		bool json = argc == 4 && !strcmp(argv[3], "--json");
		if (argc != 3 && !json)
			return cli_usage("autonomy show takes only --json");
		return show(c, &id, json);
	}
	tix_autonomy_mode mode;
	int64_t until = -1, now = (int64_t)time(NULL);
	if (!strcmp(what, "on")) {
		bool no_expiry = false, have = false;
		mode = TIX_AUTO_AUTONOMOUS;
		for (int i = 3; i < argc; i++) {
			if (!strcmp(argv[i], "--until") && i + 1 < argc) {
				if ((until = parse_until(argv[++i], now)) < 0)
					return cli_usage("--until is HH:MM or \"YYYY-MM-DD HH:MM\" (local time)");
				have = true;
			} else if (!strcmp(argv[i], "--for") && i + 1 < argc) {
				long long d = parse_for(argv[++i]);
				if (d < 0)
					return cli_usage("--for is a duration like 10h or 90m");
				until = now + d;
				have = true;
			} else if (!strcmp(argv[i], "--no-expiry")) {
				no_expiry = true;
			} else {
				return cli_usage("autonomy on: unexpected '%s'", argv[i]);
			}
		}
		if (have == no_expiry)
			return cli_usage("autonomy on needs an end: --until 08:00, --for 10h, or (deliberately) --no-expiry");
		if (have && until <= now)
			return cli_usage("autonomy on: that end is in the past");
	} else if (!strcmp(what, "off") && argc == 3) {
		mode = TIX_AUTO_SUPERVISED;
	} else if (!strcmp(what, "halt") && argc == 3) {
		mode = TIX_AUTO_HALTED;
	} else {
		return cli_usage("autonomy: on, off, halt or show");
	}
	tix_op op = tix_op_set_autonomy(mode, until);
	tix_status st = tix_entity_apply(c->repo, TIX_ENTITY_TICKET, &id, &op, 1, &c->err);
	tix_op_free(&op);
	if (st)
		return cli_fail(c, st);
	return show(c, &id, false);
}
