/* tix assign | unassign | wbs | plan | warnings | export */
#include "cli.h"

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

/* ---- assign / unassign ------------------------------------------------------- */

int cmd_assign(cli *c, int argc, char **argv)
{
	const char *ticket = NULL, *res = NULL, *effort = NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--effort") && need_value(i, argc, argv[i])) {
			effort = argv[++i];
		} else if (argv[i][0] == '-') {
			return cli_usage("assign: unexpected '%s'", argv[i]);
		} else if (!ticket) {
			ticket = argv[i];
		} else if (!res) {
			res = argv[i];
		} else {
			return cli_usage("assign: unexpected '%s'", argv[i]);
		}
	}
	if (!ticket || !res)
		return cli_usage("usage: tix assign TICKET RESOURCE[:50%%] [--effort 8h]");
	tix_oid t;
	int rc = cli_resolve(c, TIX_ENTITY_TICKET, ticket, &t);
	if (rc)
		return rc;
	tix_input_ctx ctx = cli_input_ctx(c);
	tix_op op;
	bool syntax;
	tix_status st = tix_input_assignment(&ctx, res, effort, &op, &syntax, &c->err);
	if (st)
		return cli_input_fail(c, st, syntax, "");
	st = tix_entity_apply(c->repo, TIX_ENTITY_TICKET, &t, &op, 1, &c->err);
	tix_op_free(&op);
	return st ? cli_fail(c, st) : EXIT_OK;
}

int cmd_unassign(cli *c, int argc, char **argv)
{
	if (argc != 3)
		return cli_usage("usage: tix unassign TICKET RESOURCE");
	tix_oid t, r;
	int rc = cli_resolve(c, TIX_ENTITY_TICKET, argv[1], &t);
	if (!rc)
		rc = cli_resolve(c, TIX_ENTITY_RESOURCE, argv[2], &r);
	if (rc)
		return rc;
	tix_op op = tix_op_unassign(&r);
	tix_status st = tix_entity_apply(c->repo, TIX_ENTITY_TICKET, &t, &op, 1, &c->err);
	tix_op_free(&op);
	return st ? cli_fail(c, st) : EXIT_OK;
}

/* ---- shared: [ROOT] [--today D] ------------------------------------------------- */

typedef struct plan_args {
	const char *root;
	tix_date today;
	bool gantt, critical, load;
	const char *format;
	const char *baseline;
} plan_args;

static int parse_plan_args(int argc, char **argv, plan_args *a, bool allow_views, bool want_format)
{
	memset(a, 0, sizeof(*a));
	a->today = cli_today();
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--today") && need_value(i, argc, argv[i])) {
			if (!tix_input_date(argv[++i], cli_today(), &a->today))
				return cli_usage("--today: not a date");
		} else if (allow_views && !strcmp(argv[i], "--gantt")) {
			a->gantt = true;
		} else if (allow_views && !strcmp(argv[i], "--critical")) {
			a->critical = true;
		} else if (allow_views && !strcmp(argv[i], "--load")) {
			a->load = true;
		} else if (want_format && !strcmp(argv[i], "--format") && need_value(i, argc, argv[i])) {
			a->format = argv[++i];
		} else if (!strcmp(argv[i], "--baseline") && need_value(i, argc, argv[i])) {
			a->baseline = argv[++i];
		} else if (argv[i][0] != '-' && !a->root) {
			a->root = argv[i];
		} else {
			return cli_usage("%s: unexpected '%s'", argv[0], argv[i]);
		}
	}
	return EXIT_OK;
}

static int run_plan(cli *c, const plan_args *a, tix_plan_result *out)
{
	tix_oid root;
	if (a->root) {
		int rc = cli_resolve(c, TIX_ENTITY_TICKET, a->root, &root);
		if (rc)
			return rc;
	}
	tix_status st = tix_plan(c->repo, a->root ? &root : NULL, a->today, out, &c->err);
	return st ? cli_fail(c, st) : EXIT_OK;
}

/* --baseline B: the comparison, one entry per task (*v NULL without --baseline). */
typedef struct compare {
	tix_entity *baseline;
	tix_plan_variance *v;
	size_t removed;
} compare;

static int load_compare(cli *c, const plan_args *a, const tix_schedule *s, compare *cmp)
{
	memset(cmp, 0, sizeof(*cmp));
	if (!a->baseline)
		return EXIT_OK;
	tix_oid id;
	int rc = cli_resolve(c, TIX_ENTITY_BASELINE, a->baseline, &id);
	if (rc)
		return rc;
	tix_status st = tix_entity_load(c->repo, TIX_ENTITY_BASELINE, &id, &cmp->baseline, &c->err);
	if (st)
		return cli_fail(c, st);
	cmp->v = tix_xcalloc(s->ntasks ? s->ntasks : 1, sizeof(*cmp->v));
	cmp->removed = tix_plan_compare(s, &cmp->baseline->u.baseline, cmp->v);
	return EXIT_OK;
}

static void compare_free(compare *cmp)
{
	tix_entity_free(cmp->baseline);
	free(cmp->v);
}

/* "+2", "-1", "0"; "new" when not baselined; "-" without dates. */
static const char *slip_text(const tix_plan_variance *v, char *buf)
{
	if (!v->baselined)
		return "new";
	if (!v->dates)
		return "-";
	snprintf(buf, 16, v->finish_slip > 0 ? "+%d" : "%d", (int)v->finish_slip);
	return buf;
}

static void compare_summary(const tix_schedule *s, const compare *cmp)
{
	size_t later = 0, earlier = 0, added = 0;
	for (size_t i = 0; i < s->ntasks; i++) {
		const tix_plan_variance *v = &cmp->v[i];
		if (!v->baselined)
			added++;
		else if (v->dates && v->finish_slip > 0)
			later++;
		else if (v->dates && v->finish_slip < 0)
			earlier++;
	}
	char d[TIX_DATE_STR];
	const tix_baseline *b = &cmp->baseline->u.baseline;
	printf("baseline \"%s\" (%s): %zu finish later, %zu earlier, %zu new, %zu removed since\n", b->name,
	       tix_date_str(b->taken, d), later, earlier, added, cmp->removed);
}

/* One warning is printed as it is; more become a single line pointing at `tix warnings`,
 * so a ticket that depends on a big unestimated summary doesn't bury the output. */
static void print_diags(const tix_schedule *s, bool rooted)
{
	char id[16];
	if (s->ndiags == 1)
		fprintf(stderr, "tix: note: %s\n", s->diags[0].msg);
	else if (s->ndiags > 1 && rooted && s->ntasks)
		fprintf(stderr, "tix: note: %zu scheduling warnings; 'tix warnings --root %s' lists them\n", s->ndiags,
			cli_short(&s->tasks[0].ticket->id, id));
	else if (s->ndiags > 1)
		fprintf(stderr, "tix: note: %zu scheduling warnings; 'tix warnings' lists them\n", s->ndiags);
}

static const char *date_or_dash(tix_date d, char *buf)
{
	if (d == TIX_DATE_NONE)
		return "-";
	return tix_date_str(d, buf);
}

/* Title column: indented by depth, cut to width (UTF-8 safe). */
static void print_title(const tix_plan_task *t, int width, bool indent)
{
	char buf[512];
	int depth = indent ? (int)(t->depth > 12 ? 12 : t->depth) * 2 : 0;
	snprintf(buf, sizeof(buf), "%*s%s", depth, "", t->ticket->u.ticket.title);
	int cols = 0;
	size_t i = 0;
	for (; buf[i] && cols < width; i++) {
		if (((unsigned char)buf[i] & 0xc0) != 0x80)
			cols++;
		if (cols == width && buf[i + 1]) /* truncate with a marker */
			break;
	}
	while (buf[i] && ((unsigned char)buf[i] & 0xc0) == 0x80)
		i++;
	fwrite(buf, 1, i, stdout);
	if (buf[i]) {
		fputs("~", stdout);
		cols++;
	}
	for (; cols < width; cols++)
		putchar(' ');
}

/* ---- wbs ---------------------------------------------------------------------------- */

int cmd_wbs(cli *c, int argc, char **argv)
{
	plan_args a;
	tix_plan_result pr;
	int rc = parse_plan_args(argc, argv, &a, false, false);
	if (rc || (rc = run_plan(c, &a, &pr)))
		return rc;
	const tix_schedule *s = pr.schedule;
	compare cmp;
	if ((rc = load_compare(c, &a, s, &cmp))) {
		tix_plan_result_free(&pr);
		return rc;
	}
	char id[16], d1[TIX_DATE_STR], d2[TIX_DATE_STR], sl[16];
	for (size_t i = 0; i < s->ntasks; i++) {
		const tix_plan_task *t = &s->tasks[i];
		printf("%-10s %s  ", t->wbs, cli_short(&t->ticket->id, id));
		print_title(t, 36, true);
		printf("  %-11s", tix_ticket_status_name(t->ticket->u.ticket.status));
		if (t->scheduled)
			printf("  %s..%s", date_or_dash(t->start, d1), date_or_dash(t->finish, d2));
		else
			printf("  %-22s", "(unscheduled)");
		printf("  %3d%%", t->progress);
		if (t->effort >= 0)
			printf("  %lldh", (long long)t->effort);
		if (cmp.v)
			printf("  slip %s", slip_text(&cmp.v[i], sl));
		putchar('\n');
	}
	if (cmp.v)
		compare_summary(s, &cmp);
	print_diags(s, a.root != NULL);
	compare_free(&cmp);
	tix_plan_result_free(&pr);
	return EXIT_OK;
}

/* ---- plan ----------------------------------------------------------------------------- */

static void plan_table(const tix_schedule *s, bool critical_only, const compare *cmp)
{
	char id[16], d1[TIX_DATE_STR], d2[TIX_DATE_STR], d3[TIX_DATE_STR], sl[16];
	printf("%-10s %-7s  %-30s  %-10s  %-10s  %5s  %5s", "WBS", "ID", "TITLE", "START", "FINISH", "DAYS", "FLOAT");
	if (cmp->v)
		printf("  %-10s  %4s", "BASE FIN", "SLIP");
	putchar('\n');
	for (size_t i = 0; i < s->ntasks; i++) {
		const tix_plan_task *t = &s->tasks[i];
		if (critical_only && (!t->critical || t->summary))
			continue;
		printf("%-10s %s  ", t->wbs, cli_short(&t->ticket->id, id));
		print_title(t, 30, !critical_only);
		if (!t->scheduled) {
			printf("  (unscheduled)");
			if (cmp->v)
				printf("  %-10s  %4s", date_or_dash(cmp->v[i].finish, d3), slip_text(&cmp->v[i], sl));
			putchar('\n');
			continue;
		}
		printf("  %s  %s  %5lld  %5d", date_or_dash(t->start, d1), date_or_dash(t->finish, d2),
		       (long long)t->duration, (int)t->total_float);
		if (cmp->v)
			printf("  %-10s  %4s", date_or_dash(cmp->v[i].finish, d3), slip_text(&cmp->v[i], sl));
		printf("%s%s%s\n", t->critical ? "  critical" : "", t->milestone ? "  milestone" : "", t->late ? "  LATE" : "");
	}
	if (s->start != TIX_DATE_NONE)
		printf("\nplan: %s..%s\n", tix_date_str(s->start, d1), tix_date_str(s->finish, d2));
	if (cmp->v)
		compare_summary(s, cmp);
}

/* One column per day (or per week for long plans). */
static void gantt(const tix_schedule *s)
{
	if (s->start == TIX_DATE_NONE) {
		printf("nothing scheduled\n");
		return;
	}
	int64_t span = (int64_t)s->finish - s->start + 1;
	int step = span > 120 ? 7 : 1;
	int64_t cols = (span + step - 1) / step;
	if (cols > 160)
		cols = 160;
	char d1[TIX_DATE_STR];
	printf("%-10s %-30s|", "", step == 1 ? "(one column per day)" : "(one column per week)");
	for (int64_t k = 0; k < cols; k++) { /* mark Mondays / month starts */
		tix_date d = s->start + (tix_date)(k * step);
		int day;
		tix_date_ymd(d, NULL, NULL, &day);
		putchar(step == 1 ? (tix_date_weekday(d) == 0 ? '|' : ' ') : (day <= 7 ? '|' : ' '));
	}
	printf("\n%-10s %-30s|%s\n", "", "", tix_date_str(s->start, d1));
	for (size_t i = 0; i < s->ntasks; i++) {
		const tix_plan_task *t = &s->tasks[i];
		printf("%-10s ", t->wbs);
		print_title(t, 30, true);
		putchar('|');
		for (int64_t k = 0; k < cols && t->scheduled; k++) {
			tix_date lo = s->start + (tix_date)(k * step), hi = lo + step - 1;
			char ch = ' ';
			if (t->milestone && t->start >= lo && t->start <= hi)
				ch = '*';
			else if (!t->milestone && t->start <= hi && t->finish >= lo) {
				if (t->summary)
					ch = '-';
				else if (step == 1 && !tix_plan_is_working(t->calendar, lo))
					ch = '.';
				else
					ch = t->critical ? '#' : '=';
			}
			putchar(ch);
		}
		putchar('\n');
	}
	printf("\n# critical  = other  - summary  * milestone  . non-working day\n");
}

static void load_view(const tix_schedule *s)
{
	char d1[TIX_DATE_STR], id[16];
	if (!s->nloads) {
		printf("no assigned resources in this plan\n");
		return;
	}
	size_t cols = s->ndays > 160 ? 160 : s->ndays;
	printf("resource load from %s, one column per day (%% of capacity):\n", tix_date_str(s->start, d1));
	for (size_t i = 0; i < s->nloads; i++) {
		const tix_plan_load *l = &s->loads[i];
		printf("%s  %-20.20s |", cli_short(&l->resource->id, id), l->resource->u.resource.name);
		for (size_t d = 0; d < cols; d++) {
			int32_t u = l->units[d];
			char ch = u == 0 ? (l->working[d] ? ' ' : '.') : u > (l->working[d] ? 100 : 0) ? '!' : u >= 100 ? '#'
												   : u >= 50 ? '+'
													     : ':';
			putchar(ch);
		}
		printf("|  peak %d%%%s\n", (int)l->peak, l->over_days ? "  OVER-ALLOCATED" : "");
	}
	printf("\n! over  # full  + half or more  : less  . non-working\n");
}

int cmd_plan(cli *c, int argc, char **argv)
{
	plan_args a;
	tix_plan_result pr;
	int rc = parse_plan_args(argc, argv, &a, true, false);
	if (rc || (rc = run_plan(c, &a, &pr)))
		return rc;
	const tix_schedule *s = pr.schedule;
	compare cmp;
	if ((rc = load_compare(c, &a, s, &cmp))) {
		tix_plan_result_free(&pr);
		return rc;
	}
	if (a.gantt)
		gantt(s);
	if (a.load) {
		if (a.gantt)
			putchar('\n');
		load_view(s);
	}
	if (!a.gantt && !a.load)
		plan_table(s, a.critical, &cmp);
	else if (a.critical) {
		putchar('\n');
		plan_table(s, true, &cmp);
	}
	print_diags(s, a.root != NULL);
	compare_free(&cmp);
	tix_plan_result_free(&pr);
	return EXIT_OK;
}

/* ---- warnings ------------------------------------------------------------------------ */

int cmd_warnings(cli *c, int argc, char **argv)
{
	const char *ticket = NULL;
	plan_args a;
	memset(&a, 0, sizeof(a));
	a.today = cli_today();
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--today") && need_value(i, argc, argv[i])) {
			if (!tix_input_date(argv[++i], cli_today(), &a.today))
				return cli_usage("--today: not a date");
		} else if (!strcmp(argv[i], "--root") && need_value(i, argc, argv[i])) {
			a.root = argv[++i];
		} else if (argv[i][0] != '-' && !ticket) {
			ticket = argv[i];
		} else {
			return cli_usage("warnings: unexpected '%s'", argv[i]);
		}
	}
	tix_oid tid;
	int rc;
	if (ticket && (rc = cli_resolve(c, TIX_ENTITY_TICKET, ticket, &tid)))
		return rc;
	tix_plan_result pr;
	if ((rc = run_plan(c, &a, &pr)))
		return rc;
	const tix_schedule *s = pr.schedule;
	for (size_t i = 0; i < s->ndiags; i++)
		if (!ticket || tix_oid_eq(&s->diags[i].id, &tid))
			printf("%s\n", s->diags[i].msg);
	tix_plan_result_free(&pr);
	return EXIT_OK;
}

/* ---- export ---------------------------------------------------------------------------- */

int cmd_export(cli *c, int argc, char **argv)
{
	plan_args a;
	tix_plan_result pr;
	tix_export_format f;
	int rc = parse_plan_args(argc, argv, &a, false, true);
	if (rc)
		return rc;
	if (!a.format || !tix_export_format_parse(a.format, &f))
		return cli_usage("export: --format mermaid|csv|json|mspdi is required");
	if ((rc = run_plan(c, &a, &pr)))
		return rc;
	compare cmp;
	if ((rc = load_compare(c, &a, pr.schedule, &cmp))) {
		tix_plan_result_free(&pr);
		return rc;
	}
	tix_buf out = TIX_BUF_INIT;
	const char *title = NULL;
	if (a.root && pr.schedule->ntasks)
		title = pr.schedule->tasks[0].ticket->u.ticket.title;
	tix_export_compare(pr.schedule, f, title, cmp.v, &out);
	fwrite(out.data ? out.data : "", 1, out.len, stdout);
	print_diags(pr.schedule, a.root != NULL);
	compare_free(&cmp);
	tix_buf_free(&out);
	tix_plan_result_free(&pr);
	return EXIT_OK;
}
