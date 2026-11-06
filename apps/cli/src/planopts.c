/* Parsing of planning values and the planning options shared by `new` and `edit`. */
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

bool cli_parse_date(const char *s, tix_date *out)
{
	if (!strcmp(s, "today")) {
		*out = cli_today();
		return true;
	}
	if (!strcmp(s, "tomorrow")) {
		*out = cli_today() + 1;
		return tix_date_valid(*out);
	}
	return tix_date_parse(s, out);
}

/* Non-negative integer with an optional unit suffix ("3", "3d"). */
bool cli_parse_count(const char *s, const char *unit, int64_t max, int64_t *out)
{
	char *end;
	if (*s < '0' || *s > '9')
		return false;
	long long v = strtoll(s, &end, 10);
	if (*end && strcmp(end, unit))
		return false;
	if (v < 0 || v > max)
		return false;
	*out = v;
	return true;
}

bool cli_is_none(const char *s)
{
	return !strcmp(s, "none") || !strcmp(s, "-");
}

static int id_or_none(cli *c, tix_entity_type t, const char *arg, tix_value *out)
{
	tix_oid id;
	if (cli_is_none(arg)) {
		*out = tix_value_null();
		return EXIT_OK;
	}
	int rc = cli_resolve(c, t, arg, &id);
	if (!rc)
		*out = tix_value_id(&id);
	return rc;
}

static int date_or_none(const char *opt, const char *arg, tix_value *out)
{
	tix_date d;
	if (cli_is_none(arg)) {
		*out = tix_value_null();
		return EXIT_OK;
	}
	if (!cli_parse_date(arg, &d))
		return cli_usage("%s: '%s' is not a date (YYYY-MM-DD, today, tomorrow or none)", opt, arg);
	*out = tix_value_int(TIX_V_DATE, d);
	return EXIT_OK;
}

static int count_or_none(const char *opt, const char *arg, const char *unit, int64_t max, tix_value *out)
{
	int64_t v;
	if (cli_is_none(arg)) {
		*out = tix_value_null();
		return EXIT_OK;
	}
	if (!cli_parse_count(arg, unit, max, &v))
		return cli_usage("%s: '%s' should look like 3%s (or none)", opt, arg, unit);
	*out = tix_value_int(TIX_V_INT, v);
	return EXIT_OK;
}

/* "+ID[:KIND[(+|-)Nd]]" or "-ID". */
static int parse_dep(cli *c, const char *arg, cli_plan *pl)
{
	char idbuf[128];
	bool add = arg[0] == '+';
	if ((arg[0] != '+' && arg[0] != '-') || !arg[1])
		return cli_usage("--dep: use +ID[:FS|SS|FF|SF[+Nd|-Nd]] to add or -ID to remove");
	const char *colon = strchr(arg + 1, ':');
	size_t idlen = colon ? (size_t)(colon - arg - 1) : strlen(arg + 1);
	if (idlen >= sizeof(idbuf))
		return cli_usage("--dep: id too long");
	memcpy(idbuf, arg + 1, idlen);
	idbuf[idlen] = '\0';
	tix_oid pred;
	int rc = cli_resolve(c, TIX_ENTITY_TICKET, idbuf, &pred);
	if (rc)
		return rc;
	if (!add) {
		if (colon)
			return cli_usage("--dep -ID takes no kind or lag");
		TIX_VEC_PUSH(&pl->ops, tix_op_dep_remove(&pred));
		return EXIT_OK;
	}
	tix_dep_kind kind = TIX_DEP_FS;
	long lag = 0;
	if (colon) {
		char k[3] = {colon[1], colon[1] ? colon[2] : 0, 0};
		if (!tix_dep_kind_parse(k, &kind))
			return cli_usage("--dep: kind must be FS, SS, FF or SF");
		const char *l = colon + 3;
		if (*l) {
			char *end;
			if (*l != '+' && *l != '-')
				return cli_usage("--dep: lag must look like +2d or -1d");
			lag = strtol(l, &end, 10);
			if (strcmp(end, "d") || lag < -TIX_LAG_MAX || lag > TIX_LAG_MAX)
				return cli_usage("--dep: lag must look like +2d or -1d (at most %d days)", TIX_LAG_MAX);
		}
	}
	TIX_VEC_PUSH(&pl->ops, tix_op_dep_add(&pred, kind, (int32_t)lag));
	return EXIT_OK;
}

static int parse_rank(cli *c, const char *arg, cli_plan *pl)
{
	pl->rank_set = true;
	if (!strcmp(arg, "first")) {
		pl->rank_pos = TIX_RANK_FIRST;
		return EXIT_OK;
	}
	if (!strcmp(arg, "last")) {
		pl->rank_pos = TIX_RANK_LAST;
		return EXIT_OK;
	}
	if (!strncmp(arg, "before:", 7) || !strncmp(arg, "after:", 6)) {
		pl->rank_pos = arg[0] == 'b' ? TIX_RANK_BEFORE : TIX_RANK_AFTER;
		return cli_resolve(c, TIX_ENTITY_TICKET, strchr(arg, ':') + 1, &pl->rank_anchor);
	}
	return cli_usage("--rank: use first, last, before:ID or after:ID");
}

typedef struct date_opt {
	const char *opt;
	tix_field field;
} date_opt;

static const date_opt date_opts[] = {
	{"--start", TIX_F_START},
	{"--finish", TIX_F_FINISH},
	{"--deadline", TIX_F_DEADLINE},
	{"--actual-start", TIX_F_ACTUAL_START},
	{"--actual-finish", TIX_F_ACTUAL_FINISH},
};

int cli_plan_opt(cli *c, int argc, char **argv, int *i, cli_plan *pl)
{
	const char *opt = argv[*i];
	bool known = false;
	for (size_t k = 0; k < sizeof(date_opts) / sizeof(*date_opts); k++)
		known |= !strcmp(opt, date_opts[k].opt);
	static const char *const others[] = {"--parent", "--rank",   "--kind",     "--calendar", "--mode",
					     "--duration", "--effort", "--progress", "--dep"};
	for (size_t k = 0; k < sizeof(others) / sizeof(*others); k++)
		known |= !strcmp(opt, others[k]);
	if (!known)
		return CLI_NOT_MINE;
	if (*i + 1 >= argc)
		return cli_usage("%s needs a value", opt);
	const char *arg = argv[++*i];
	tix_value v;
	int rc = EXIT_OK;

	for (size_t k = 0; k < sizeof(date_opts) / sizeof(*date_opts); k++)
		if (!strcmp(opt, date_opts[k].opt)) {
			if (!(rc = date_or_none(opt, arg, &v)))
				TIX_VEC_PUSH(&pl->ops, tix_op_set(date_opts[k].field, v));
			return rc;
		}
	if (!strcmp(opt, "--parent")) {
		if (!(rc = id_or_none(c, TIX_ENTITY_TICKET, arg, &v))) {
			pl->parent_set = true;
			memset(&pl->parent, 0, sizeof(pl->parent));
			if (v.kind == TIX_V_ID)
				pl->parent = v.id;
			TIX_VEC_PUSH(&pl->ops, tix_op_set(TIX_F_PARENT, v));
		}
	} else if (!strcmp(opt, "--calendar")) {
		if (!(rc = id_or_none(c, TIX_ENTITY_CALENDAR, arg, &v)))
			TIX_VEC_PUSH(&pl->ops, tix_op_set(TIX_F_CALENDAR, v));
	} else if (!strcmp(opt, "--rank")) {
		rc = parse_rank(c, arg, pl);
	} else if (!strcmp(opt, "--kind") || !strcmp(opt, "--mode")) {
		tix_field f = opt[2] == 'k' ? TIX_F_KIND : TIX_F_MODE;
		int k = tix_field_enum(f, arg);
		if (k < 0)
			return cli_usage("%s: use %s", opt, f == TIX_F_KIND ? "task or milestone" : "auto or manual");
		TIX_VEC_PUSH(&pl->ops, tix_op_set(f, tix_value_int(TIX_V_ENUM, k)));
	} else if (!strcmp(opt, "--duration")) {
		if (!(rc = count_or_none(opt, arg, "d", TIX_DURATION_MAX, &v)))
			TIX_VEC_PUSH(&pl->ops, tix_op_set(TIX_F_DURATION, v));
	} else if (!strcmp(opt, "--effort")) {
		if (!(rc = count_or_none(opt, arg, "h", TIX_EFFORT_MAX, &v)))
			TIX_VEC_PUSH(&pl->ops, tix_op_set(TIX_F_EFFORT, v));
	} else if (!strcmp(opt, "--progress")) {
		int64_t p;
		if (!cli_parse_count(arg, "%", 100, &p))
			return cli_usage("--progress: a percentage 0-100");
		TIX_VEC_PUSH(&pl->ops, tix_op_set(TIX_F_PROGRESS, tix_value_int(TIX_V_INT, p)));
	} else { /* --dep */
		rc = parse_dep(c, arg, pl);
	}
	return rc;
}

int cli_plan_finish(cli *c, cli_plan *pl, const tix_oid *self, const tix_oid *current_parent)
{
	if (!pl->rank_set)
		return EXIT_OK;
	const tix_oid *parent = pl->parent_set ? (tix_oid_is_null(&pl->parent) ? NULL : &pl->parent)
					       : (current_parent && !tix_oid_is_null(current_parent) ? current_parent : NULL);
	tix_oid none;
	memset(&none, 0, sizeof(none));
	tix_buf rank = TIX_BUF_INIT;
	tix_status st = tix_ticket_rank(c->repo, parent, self ? self : &none, pl->rank_pos, &pl->rank_anchor, &rank,
					&c->err);
	if (st) {
		tix_buf_free(&rank);
		return cli_fail(c, st);
	}
	TIX_VEC_PUSH(&pl->ops, tix_op_set(TIX_F_RANK, tix_value_str(rank.data)));
	tix_buf_free(&rank);
	return EXIT_OK;
}

void cli_plan_free(cli_plan *pl)
{
	for (size_t i = 0; i < pl->ops.n; i++)
		tix_op_free(&pl->ops.v[i]);
	free(pl->ops.v);
	memset(pl, 0, sizeof(*pl));
}
