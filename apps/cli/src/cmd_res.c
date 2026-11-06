/* tix res ... | tix cal ... (resources and working-time calendars) */
#include "cli.h"

#include <stdlib.h>
#include <string.h>

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

/* --FIELD VALUE options of `res`/`cal` (tix/input.h), appended to ops. */
static int field_options(cli *c, tix_entity_type t, const char *what, int argc, char **argv, int start, op_vec *ops,
			 bool creating)
{
	tix_input in = {0};
	int rc = EXIT_OK;
	for (int i = start; i < argc && rc == EXIT_OK; i++) {
		int prc = cli_input_opt(c, t, creating, argc, argv, &i, &in);
		rc = prc == CLI_NOT_MINE ? cli_usage("%s: unexpected '%s'", what, argv[i]) : prc;
	}
	for (size_t k = 0; rc == EXIT_OK && k < in.ops.n; k++) {
		TIX_VEC_PUSH(ops, in.ops.v[k]); /* moved */
		memset(&in.ops.v[k], 0, sizeof(tix_op));
	}
	tix_input_free(&in);
	return rc;
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
		if (!(rc = field_options(c, TIX_ENTITY_RESOURCE, "res", argc, argv, 3, &ops, true)))
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
		if (!(rc = field_options(c, TIX_ENTITY_RESOURCE, "res", argc, argv, 3, &ops, false)))
			rc = apply_or_create(c, TIX_ENTITY_RESOURCE, &id, &ops, "resource");
		free_ops(&ops);
		return rc;
	}
	return cli_usage("res: unknown subcommand '%s'", sub);
}

/* ---- calendars ------------------------------------------------------------------- */

static const char *const day_names[] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};

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
		if (!(rc = field_options(c, TIX_ENTITY_CALENDAR, "cal", argc, argv, 3, &ops, true)))
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
		if (!(rc = field_options(c, TIX_ENTITY_CALENDAR, "cal", argc, argv, 3, &ops, false)))
			rc = apply_or_create(c, TIX_ENTITY_CALENDAR, &id, &ops, "calendar");
		free_ops(&ops);
		return rc;
	}
	return cli_usage("cal: unknown subcommand '%s'", sub);
}
