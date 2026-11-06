/* tix res ... | tix cal ... (resources and working-time calendars) */
#include "cli.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static int need_value(int i, int argc, const char *opt)
{
	if (i + 1 >= argc) {
		cli_usage("%s needs a value", opt);
		return 0;
	}
	return 1;
}

typedef TIX_VEC(tix_op) op_vec;

static void free_ops(op_vec *ops)
{
	for (size_t i = 0; i < ops->n; i++)
		tix_op_free(&ops->v[i]);
	free(ops->v);
}

static int apply_or_create(cli *c, tix_entity_type t, const tix_oid *id, op_vec *ops, const char *what)
{
	tix_status st;
	tix_oid out;
	char s[16];
	if (id) {
		st = tix_entity_apply(c->repo, t, id, ops->v, ops->n, &c->err);
	} else {
		st = tix_entity_create(c->repo, t, ops->v, ops->n, &out, &c->err);
		if (!st)
			printf("Created %s %s\n", what, cli_short(&out, s));
	}
	return st ? cli_fail(c, st) : EXIT_OK;
}

static int list_named(cli *c, tix_entity_type t)
{
	tix_entity_list l;
	tix_status st = t == TIX_ENTITY_RESOURCE ? tix_resource_list(c->repo, &l, &c->err)
						 : tix_calendar_list(c->repo, &l, &c->err);
	if (st)
		return cli_fail(c, st);
	for (size_t i = 0; i < l.n; i++) {
		char s[16];
		const tix_entity *e = l.v[i];
		if (t == TIX_ENTITY_RESOURCE) {
			const tix_resource *r = &e->u.resource;
			printf("%s  %-9s %5d%%  %s%s\n", cli_short(&e->id, s), tix_field_get(TIX_F_RES_KIND)->enums[r->kind],
			       (int)r->capacity, r->name, r->active ? "" : "  (inactive)");
		} else {
			printf("%s  %s\n", cli_short(&e->id, s), e->u.calendar.name);
		}
	}
	if (l.nbroken)
		fprintf(stderr, "tix: skipped %zu invalid entr%s\n", l.nbroken, l.nbroken == 1 ? "y" : "ies");
	tix_entity_list_free(&l);
	return EXIT_OK;
}

/* ---- resources ---------------------------------------------------------------- */

/* "95.50" -> 9550 (minor units); at most two decimals. */
static bool parse_amount(const char *s, int64_t *out)
{
	int64_t whole = 0, frac = 0;
	int digits = 0;
	const char *p = s;
	if (!isdigit((unsigned char)*p))
		return false;
	for (; isdigit((unsigned char)*p); p++)
		if ((whole = whole * 10 + (*p - '0')) > 10000000)
			return false;
	if (*p == '.') {
		for (p++; isdigit((unsigned char)*p) && digits < 2; p++, digits++)
			frac = frac * 10 + (*p - '0');
		if (!digits)
			return false;
	}
	if (*p)
		return false;
	while (digits++ < 2)
		frac *= 10;
	*out = whole * 100 + frac;
	return true;
}

static int res_options(cli *c, int argc, char **argv, int start, op_vec *ops, bool creating)
{
	for (int i = start; i < argc; i++) {
		const char *a = argv[i];
		tix_oid id;
		int rc;
		if (!strcmp(a, "--name") && !creating && need_value(i, argc, a)) {
			TIX_VEC_PUSH(ops, tix_op_set(TIX_F_RES_NAME, tix_value_str(argv[++i])));
		} else if (!strcmp(a, "--kind") && need_value(i, argc, a)) {
			int k = tix_field_enum(TIX_F_RES_KIND, argv[++i]);
			if (k < 0)
				return cli_usage("--kind: person, role or equipment");
			TIX_VEC_PUSH(ops, tix_op_set(TIX_F_RES_KIND, tix_value_int(TIX_V_ENUM, k)));
		} else if (!strcmp(a, "--capacity") && need_value(i, argc, a)) {
			int64_t v;
			if (!cli_parse_count(argv[++i], "%", TIX_UNITS_MAX, &v) || v < 1)
				return cli_usage("--capacity: percent of one full-time unit, like 300%%");
			TIX_VEC_PUSH(ops, tix_op_set(TIX_F_CAPACITY, tix_value_int(TIX_V_INT, v)));
		} else if ((!strcmp(a, "--calendar") || !strcmp(a, "--identity")) && need_value(i, argc, a)) {
			bool cal = a[2] == 'c';
			tix_field f = cal ? TIX_F_RES_CALENDAR : TIX_F_RES_IDENTITY;
			const char *v = argv[++i];
			if (cli_is_none(v)) {
				TIX_VEC_PUSH(ops, tix_op_set(f, tix_value_null()));
			} else if (!cal && !strcmp(v, "me")) {
				if (!tix_repo_identity(c->repo, &id))
					return cli_usage("--identity me: no identity configured");
				TIX_VEC_PUSH(ops, tix_op_set(f, tix_value_id(&id)));
			} else {
				if ((rc = cli_resolve(c, cal ? TIX_ENTITY_CALENDAR : TIX_ENTITY_IDENTITY, v, &id)))
					return rc;
				TIX_VEC_PUSH(ops, tix_op_set(f, tix_value_id(&id)));
			}
		} else if (!strcmp(a, "--rate") && need_value(i, argc, a)) {
			const char *v = argv[++i];
			int64_t amount;
			if (cli_is_none(v)) {
				TIX_VEC_PUSH(ops, tix_op_set(TIX_F_RATE, tix_value_null()));
				TIX_VEC_PUSH(ops, tix_op_set(TIX_F_CURRENCY, tix_value_null()));
				continue;
			}
			char cur[4] = {0};
			const char *sp = strchr(v, ' ');
			char num[32];
			size_t nl = sp ? (size_t)(sp - v) : strlen(v);
			if (nl >= sizeof(num) || !sp || strlen(sp + 1) != 3)
				return cli_usage("--rate: an hourly amount and currency, like \"95.50 EUR\"");
			memcpy(num, v, nl);
			num[nl] = '\0';
			for (int k = 0; k < 3; k++)
				cur[k] = (char)toupper((unsigned char)sp[1 + k]);
			if (!parse_amount(num, &amount))
				return cli_usage("--rate: amount like 95 or 95.50");
			TIX_VEC_PUSH(ops, tix_op_set(TIX_F_RATE, tix_value_int(TIX_V_INT, amount)));
			TIX_VEC_PUSH(ops, tix_op_set(TIX_F_CURRENCY, tix_value_str(cur)));
		} else if (!strcmp(a, "--active") && !creating && need_value(i, argc, a)) {
			const char *v = argv[++i];
			if (strcmp(v, "yes") && strcmp(v, "no"))
				return cli_usage("--active: yes or no");
			TIX_VEC_PUSH(ops, tix_op_set(TIX_F_ACTIVE, tix_value_int(TIX_V_BOOL, !strcmp(v, "yes"))));
		} else {
			return cli_usage("res: unexpected '%s'", a);
		}
	}
	return EXIT_OK;
}

static int res_show(cli *c, const char *arg)
{
	tix_oid id;
	tix_entity *e;
	int rc = cli_resolve(c, TIX_ENTITY_RESOURCE, arg, &id);
	if (rc)
		return rc;
	tix_status st = tix_entity_load(c->repo, TIX_ENTITY_RESOURCE, &id, &e, &c->err);
	if (st)
		return cli_fail(c, st);
	const tix_resource *r = &e->u.resource;
	char hex[TIX_OID_MAX_HEX + 1], buf[300], s[16];
	printf("%s\nid:       %s\nkind:     %s\ncapacity: %d%%\n", r->name, tix_oid_hex(&e->id, hex),
	       tix_field_get(TIX_F_RES_KIND)->enums[r->kind], (int)r->capacity);
	if (!tix_oid_is_null(&r->calendar)) {
		tix_entity *cal;
		if (!tix_entity_load(c->repo, TIX_ENTITY_CALENDAR, &r->calendar, &cal, NULL)) {
			printf("calendar: %s %s\n", cli_short(&r->calendar, s), cal->u.calendar.name);
			tix_entity_free(cal);
		} else {
			printf("calendar: %s (not here)\n", cli_short(&r->calendar, s));
		}
	}
	if (!tix_oid_is_null(&r->identity))
		printf("identity: %s\n", cli_who(c, &r->identity, buf, sizeof(buf)));
	if (r->rate >= 0)
		printf("rate:     %lld.%02lld %s/h\n", (long long)(r->rate / 100), (long long)(r->rate % 100),
		       r->currency ? r->currency : "");
	printf("active:   %s\n", r->active ? "yes" : "no");
	tix_entity_free(e);
	return EXIT_OK;
}

int cmd_res(cli *c, int argc, char **argv)
{
	if (argc < 2)
		return cli_usage("usage: tix res new NAME | ls | show RES | edit RES ...");
	const char *sub = argv[1];
	op_vec ops = {0};
	int rc;
	if (!strcmp(sub, "ls"))
		return argc == 2 ? list_named(c, TIX_ENTITY_RESOURCE) : cli_usage("res ls takes no arguments");
	if (!strcmp(sub, "show"))
		return argc == 3 ? res_show(c, argv[2]) : cli_usage("usage: tix res show RES");
	if (!strcmp(sub, "new")) {
		if (argc < 3 || argv[2][0] == '-')
			return cli_usage("usage: tix res new NAME [--kind K] [--capacity 100%%] [--calendar C] "
					 "[--identity ID|me] [--rate \"95 EUR\"]");
		TIX_VEC_PUSH(&ops, tix_op_resource_create(NULL, argv[2], TIX_RES_PERSON));
		if (!(rc = res_options(c, argc, argv, 3, &ops, true)))
			rc = apply_or_create(c, TIX_ENTITY_RESOURCE, NULL, &ops, "resource");
		free_ops(&ops);
		return rc;
	}
	if (!strcmp(sub, "edit")) {
		tix_oid id;
		if (argc < 4)
			return cli_usage("usage: tix res edit RES [--name N] [--kind K] [--capacity P%%] [--calendar C] "
					 "[--identity ID] [--rate R] [--active yes|no]");
		if ((rc = cli_resolve(c, TIX_ENTITY_RESOURCE, argv[2], &id)))
			return rc;
		if (!(rc = res_options(c, argc, argv, 3, &ops, false)))
			rc = apply_or_create(c, TIX_ENTITY_RESOURCE, &id, &ops, "resource");
		free_ops(&ops);
		return rc;
	}
	return cli_usage("res: unknown subcommand '%s'", sub);
}

/* ---- calendars ------------------------------------------------------------------- */

static const char *const day_names[] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};

static int day_index(const char *s, size_t n)
{
	for (int i = 0; i < 7; i++)
		if (n == 3 && !strncmp(s, day_names[i], 3))
			return i;
	return -1;
}

/* "mon-fri", "mon,wed,fri", "mon-thu,sat" */
static bool parse_weekdays(const char *s, int64_t *out)
{
	unsigned mask = 0;
	while (*s) {
		const char *end = strchr(s, ',');
		size_t n = end ? (size_t)(end - s) : strlen(s);
		const char *dash = memchr(s, '-', n);
		int a = day_index(s, dash ? (size_t)(dash - s) : n);
		int b = dash ? day_index(dash + 1, n - (size_t)(dash - s) - 1) : a;
		if (a < 0 || b < 0 || b < a)
			return false;
		for (int d = a; d <= b; d++)
			mask |= 1u << d;
		s += n + (end ? 1 : 0);
		if (end && !*s)
			return false;
	}
	if (!mask)
		return false;
	*out = mask;
	return true;
}

/* "FROM[..TO][=NOTE]" */
static bool parse_range(const char *s, tix_date *from, tix_date *to, const char **note)
{
	char a[TIX_DATE_STR + 16], b[TIX_DATE_STR + 16];
	const char *eq = strchr(s, '=');
	size_t n = eq ? (size_t)(eq - s) : strlen(s);
	const char *dots = strstr(s, "..");
	if (dots && dots > s + n)
		dots = NULL;
	size_t an = dots ? (size_t)(dots - s) : n;
	if (an >= sizeof(a))
		return false;
	memcpy(a, s, an);
	a[an] = '\0';
	if (!cli_parse_date(a, from))
		return false;
	*to = *from;
	if (dots) {
		size_t bn = n - an - 2;
		if (bn >= sizeof(b))
			return false;
		memcpy(b, dots + 2, bn);
		b[bn] = '\0';
		if (!cli_parse_date(b, to))
			return false;
	}
	if (note)
		*note = eq ? eq + 1 : "";
	return true;
}

static int cal_options(int argc, char **argv, int start, op_vec *ops, bool creating)
{
	for (int i = start; i < argc; i++) {
		const char *a = argv[i];
		int64_t v;
		if (!strcmp(a, "--name") && !creating && need_value(i, argc, a)) {
			TIX_VEC_PUSH(ops, tix_op_set(TIX_F_CAL_NAME, tix_value_str(argv[++i])));
		} else if (!strcmp(a, "--weekdays") && need_value(i, argc, a)) {
			if (!parse_weekdays(argv[++i], &v))
				return cli_usage("--weekdays: like mon-fri or mon,tue,thu");
			TIX_VEC_PUSH(ops, tix_op_set(TIX_F_WEEKDAYS, tix_value_int(TIX_V_INT, v)));
		} else if (!strcmp(a, "--hours") && need_value(i, argc, a)) {
			if (!cli_parse_count(argv[++i], "h", 24, &v) || v < 1)
				return cli_usage("--hours: working hours per day, 1-24");
			TIX_VEC_PUSH(ops, tix_op_set(TIX_F_HOURS, tix_value_int(TIX_V_INT, v)));
		} else if ((!strcmp(a, "--off") || !strcmp(a, "--on")) && need_value(i, argc, a)) {
			tix_date from, to;
			const char *note;
			if (!parse_range(argv[++i], &from, &to, &note))
				return cli_usage("%s: FROM[..TO][=NOTE], dates as YYYY-MM-DD", a);
			TIX_VEC_PUSH(ops, tix_op_exception_add(from, to, !strcmp(a, "--on"), note));
		} else if (!strcmp(a, "--clear") && !creating && need_value(i, argc, a)) {
			tix_date from, to;
			if (!parse_range(argv[++i], &from, &to, NULL))
				return cli_usage("--clear: FROM[..TO] of an existing exception");
			TIX_VEC_PUSH(ops, tix_op_exception_remove(from, to));
		} else {
			return cli_usage("cal: unexpected '%s'", a);
		}
	}
	return EXIT_OK;
}

static int cal_show(cli *c, const char *arg)
{
	tix_oid id;
	tix_entity *e;
	int rc = cli_resolve(c, TIX_ENTITY_CALENDAR, arg, &id);
	if (rc)
		return rc;
	tix_status st = tix_entity_load(c->repo, TIX_ENTITY_CALENDAR, &id, &e, &c->err);
	if (st)
		return cli_fail(c, st);
	const tix_calendar *cal = &e->u.calendar;
	char hex[TIX_OID_MAX_HEX + 1], d1[TIX_DATE_STR], d2[TIX_DATE_STR];
	printf("%s\nid:       %s\nweekdays:", cal->name, tix_oid_hex(&e->id, hex));
	for (int d = 0; d < 7; d++)
		if (cal->weekdays & (1u << d))
			printf(" %s", day_names[d]);
	printf("\nhours:    %d\n", cal->hours);
	for (size_t i = 0; i < cal->nexceptions; i++) {
		const tix_cal_exception *x = &cal->exceptions[i];
		printf("%-10s%s", x->working ? "on:" : "off:", tix_date_str(x->from, d1));
		if (x->to != x->from)
			printf("..%s", tix_date_str(x->to, d2));
		if (x->note[0])
			printf("  %s", x->note);
		putchar('\n');
	}
	tix_entity_free(e);
	return EXIT_OK;
}

int cmd_cal(cli *c, int argc, char **argv)
{
	if (argc < 2)
		return cli_usage("usage: tix cal new NAME | ls | show CAL | edit CAL ...");
	const char *sub = argv[1];
	op_vec ops = {0};
	int rc;
	if (!strcmp(sub, "ls"))
		return argc == 2 ? list_named(c, TIX_ENTITY_CALENDAR) : cli_usage("cal ls takes no arguments");
	if (!strcmp(sub, "show"))
		return argc == 3 ? cal_show(c, argv[2]) : cli_usage("usage: tix cal show CAL");
	if (!strcmp(sub, "new")) {
		if (argc < 3 || argv[2][0] == '-')
			return cli_usage("usage: tix cal new NAME [--weekdays mon-fri] [--hours 8] [--off D[..D][=NOTE]]");
		TIX_VEC_PUSH(&ops, tix_op_calendar_create(NULL, argv[2]));
		if (!(rc = cal_options(argc, argv, 3, &ops, true)))
			rc = apply_or_create(c, TIX_ENTITY_CALENDAR, NULL, &ops, "calendar");
		free_ops(&ops);
		return rc;
	}
	if (!strcmp(sub, "edit")) {
		tix_oid id;
		if (argc < 4)
			return cli_usage("usage: tix cal edit CAL [--name N] [--weekdays W] [--hours H] "
					 "[--off|--on FROM[..TO][=NOTE]] [--clear FROM[..TO]]");
		if ((rc = cli_resolve(c, TIX_ENTITY_CALENDAR, argv[2], &id)))
			return rc;
		if (!(rc = cal_options(argc, argv, 3, &ops, false)))
			rc = apply_or_create(c, TIX_ENTITY_CALENDAR, &id, &ops, "calendar");
		free_ops(&ops);
		return rc;
	}
	return cli_usage("cal: unknown subcommand '%s'", sub);
}
