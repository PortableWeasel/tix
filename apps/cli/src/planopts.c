/* CLI glue for field values: options map to tix/input.h fields ("--start" -> "start"). */
#include "cli.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

tix_date cli_today(void)
{
	const char *fixed = getenv("TIX_TODAY"); /* for scripts and tests */
	tix_date d;
	if (fixed && tix_date_parse(fixed, &d))
		return d;
	time_t now = time(NULL);
	struct tm tm;
	char buf[16];
	if (!localtime_r(&now, &tm) || !strftime(buf, sizeof(buf), "%Y-%m-%d", &tm) || !tix_date_parse(buf, &d))
		return tix_date_from_unix((int64_t)now);
	return d;
}

tix_input_ctx cli_input_ctx(cli *c)
{
	return (tix_input_ctx){c->repo, cli_today()};
}

int cli_input_fail(cli *c, tix_status st, bool syntax, const char *prefix)
{
	if (!syntax)
		return cli_fail(c, st);
	return cli_usage("%s%s", prefix, c->err.msg);
}

int cli_input_opt(cli *c, tix_entity_type t, bool creating, int argc, char **argv, int *i, tix_input *in)
{
	const char *opt = argv[*i];
	if (strncmp(opt, "--", 2) || !tix_input_has(t, opt + 2))
		return CLI_NOT_MINE;
	const char *field = opt + 2;
	if (creating && (!strcmp(field, "name") || !strcmp(field, "active") || !strcmp(field, "clear")))
		return CLI_NOT_MINE;
	if (*i + 1 >= argc)
		return cli_usage("%s needs a value", opt);
	tix_input_ctx ctx = cli_input_ctx(c);
	tix_status st = tix_input_field(&ctx, t, field, argv[++*i], in, &c->err);
	return st ? cli_input_fail(c, st, in->syntax, "--") : EXIT_OK;
}
