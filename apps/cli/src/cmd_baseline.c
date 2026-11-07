/* tix baseline take | ls | show | rename (architecture §14.2) */
#include "cli.h"

#include <stdlib.h>
#include <string.h>

static void print_money(int64_t cost, const char *currency)
{
	if (cost < 0)
		printf("%10s", "-");
	else
		printf("%7lld.%02lld %s", (long long)(cost / 100), (long long)(cost % 100), currency);
}

static int take(cli *c, int argc, char **argv)
{
	const char *name = NULL, *root = NULL;
	tix_date today = cli_today();
	for (int i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "--today") && i + 1 < argc) {
			if (!tix_input_date(argv[++i], cli_today(), &today))
				return cli_usage("--today: not a date");
		} else if (argv[i][0] == '-') {
			return cli_usage("baseline take: unexpected '%s'", argv[i]);
		} else if (!name) {
			name = argv[i];
		} else if (!root) {
			root = argv[i];
		} else {
			return cli_usage("baseline take: unexpected '%s'", argv[i]);
		}
	}
	if (!name)
		return cli_usage("usage: tix baseline take NAME [ROOT] [--today D]");
	tix_oid r, id;
	if (root) {
		int rc = cli_resolve(c, TIX_ENTITY_TICKET, root, &r);
		if (rc)
			return rc;
	}
	size_t n = 0;
	tix_status st = tix_baseline_take(c->repo, name, root ? &r : NULL, today, &id, &n, &c->err);
	if (st)
		return cli_fail(c, st);
	char s[16];
	printf("Took baseline %s \"%s\": %zu task%s\n", cli_short(&id, s), name, n, n == 1 ? "" : "s");
	return EXIT_OK;
}

static int list(cli *c)
{
	tix_entity_list l;
	tix_status st = tix_baseline_list(c->repo, &l, &c->err);
	if (st)
		return cli_fail(c, st);
	for (size_t i = 0; i < l.n; i++) {
		const tix_baseline *b = &l.v[i]->u.baseline;
		char s[16], r[16], d[TIX_DATE_STR];
		printf("%s  %s  %s  (%zu task%s", cli_short(&l.v[i]->id, s), tix_date_str(b->taken, d), b->name, b->ntasks,
		       b->ntasks == 1 ? "" : "s");
		if (!tix_oid_is_null(&b->root))
			printf(", from %s", cli_short(&b->root, r));
		printf(")\n");
	}
	if (l.nbroken)
		fprintf(stderr, "tix: skipped %zu invalid baseline%s\n", l.nbroken, l.nbroken == 1 ? "" : "s");
	tix_entity_list_free(&l);
	return EXIT_OK;
}

static int show(cli *c, const char *arg)
{
	tix_oid id;
	tix_entity *e;
	int rc = cli_resolve(c, TIX_ENTITY_BASELINE, arg, &id);
	if (rc)
		return rc;
	tix_status st = tix_entity_load(c->repo, TIX_ENTITY_BASELINE, &id, &e, &c->err);
	if (st)
		return cli_fail(c, st);
	const tix_baseline *b = &e->u.baseline;
	char hex[TIX_OID_MAX_HEX + 1], d1[TIX_DATE_STR], d2[TIX_DATE_STR], s[16];
	printf("%s\nid:       %s\ntaken:    %s\n", b->name, tix_oid_hex(&e->id, hex), tix_date_str(b->taken, d1));
	if (!tix_oid_is_null(&b->root))
		printf("root:     %s\n", cli_short(&b->root, s));
	printf("tasks:    %zu\n\n%-7s  %-22s  %5s  %7s  %14s\n", b->ntasks, "TICKET", "DATES", "DAYS", "EFFORT", "COST");
	for (size_t i = 0; i < b->ntasks; i++) {
		const tix_baseline_task *t = &b->tasks[i];
		printf("%s  ", cli_short(&t->ticket, s));
		if (t->start != TIX_DATE_NONE)
			printf("%s..%s", tix_date_str(t->start, d1), tix_date_str(t->finish, d2));
		else
			printf("%-22s", "(unscheduled)");
		if (t->duration >= 0)
			printf("  %5lld", (long long)t->duration);
		else
			printf("  %5s", "-");
		if (t->effort >= 0)
			printf("  %6lldh", (long long)t->effort);
		else
			printf("  %7s", "-");
		printf("  ");
		print_money(t->cost, t->currency);
		putchar('\n');
	}
	tix_entity_free(e);
	return EXIT_OK;
}

static int rename_baseline(cli *c, const char *arg, const char *name)
{
	tix_oid id;
	int rc = cli_resolve(c, TIX_ENTITY_BASELINE, arg, &id);
	if (rc)
		return rc;
	tix_op op = tix_op_text(TIX_OP_BASELINE_SET_NAME, name);
	tix_status st = tix_entity_apply(c->repo, TIX_ENTITY_BASELINE, &id, &op, 1, &c->err);
	tix_op_free(&op);
	return st ? cli_fail(c, st) : EXIT_OK;
}

int cmd_baseline(cli *c, int argc, char **argv)
{
	const char *sub = argc > 1 ? argv[1] : "";
	if (!strcmp(sub, "take"))
		return take(c, argc, argv);
	if (!strcmp(sub, "ls") && argc == 2)
		return list(c);
	if (!strcmp(sub, "show") && argc == 3)
		return show(c, argv[2]);
	if (!strcmp(sub, "rename") && argc == 4)
		return rename_baseline(c, argv[2], argv[3]);
	return cli_usage("usage: tix baseline take NAME [ROOT] | ls | show B | rename B NAME");
}
