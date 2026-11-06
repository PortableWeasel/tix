/* tix new | ls | show | comment | edit */
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

static void trim(tix_buf *b)
{
	size_t start = 0, end = b->len;
	while (start < end && (b->data[start] == '\n' || b->data[start] == ' ' || b->data[start] == '\t'))
		start++;
	while (end > start && (b->data[end - 1] == '\n' || b->data[end - 1] == ' ' || b->data[end - 1] == '\t'))
		end--;
	memmove(b->data, b->data + start, end - start);
	b->len = end - start;
	if (b->data)
		b->data[b->len] = '\0';
}

/* Editor text: first non-empty line is the title, the rest is the body. */
static void split_title(tix_buf *text, tix_buf *title, tix_buf *body)
{
	trim(text);
	const char *nl = memchr(text->data ? text->data : "", '\n', text->len);
	size_t tl = nl ? (size_t)(nl - text->data) : text->len;
	tix_buf_add(title, text->data ? text->data : "", tl);
	if (nl)
		tix_buf_adds(body, nl + 1);
	trim(title);
	trim(body);
}

int cmd_new(cli *c, int argc, char **argv)
{
	tix_buf title = TIX_BUF_INIT, body = TIX_BUF_INIT, text = TIX_BUF_INIT;
	tix_strs labels = {0};
	bool have_title = false, have_body = false;
	int rc = EXIT_OK;
	for (int i = 1; i < argc && rc == EXIT_OK; i++) {
		if (!strcmp(argv[i], "-t") && need_value(i, argc, argv[i])) {
			tix_buf_adds(&title, argv[++i]);
			have_title = true;
		} else if (!strcmp(argv[i], "-m") && need_value(i, argc, argv[i])) {
			tix_buf_adds(&body, argv[++i]);
			have_body = true;
		} else if (!strcmp(argv[i], "-l") && need_value(i, argc, argv[i])) {
			tix_strs_add(&labels, argv[++i]);
		} else {
			rc = cli_usage("new: unexpected '%s'", argv[i]);
		}
	}
	if (rc == EXIT_OK && !have_title) {
		static const char tmpl[] = "\n\n# Title on the first line, then a blank line, then the description.\n"
					   "# Lines starting with '#' are ignored. An empty title aborts.\n";
		rc = cli_edit_text(c, tmpl, &text);
		if (rc == EXIT_OK) {
			tix_buf ebody = TIX_BUF_INIT;
			split_title(&text, &title, &ebody);
			if (!have_body) {
				tix_buf_free(&body);
				body = ebody;
			} else {
				tix_buf_free(&ebody);
			}
			if (!title.len) {
				fprintf(stderr, "tix: empty title; aborting\n");
				rc = EXIT_ERR;
			}
		}
	}
	tix_oid id;
	char s[16];
	if (rc == EXIT_OK) {
		tix_status st = tix_ticket_create(c->repo, tix_buf_cstr(&title), tix_buf_cstr(&body), &labels, &id, &c->err);
		if (st)
			rc = cli_fail(c, st);
		else
			printf("Created ticket %s\n", cli_short(&id, s));
	}
	tix_buf_free(&title);
	tix_buf_free(&body);
	tix_buf_free(&text);
	tix_strs_free(&labels);
	return rc;
}

static void print_labels(const tix_strs *l)
{
	if (!l->n)
		return;
	fputs("  [", stdout);
	for (size_t i = 0; i < l->n; i++)
		printf("%s%s", i ? ", " : "", l->v[i]);
	fputs("]", stdout);
}

int cmd_ls(cli *c, int argc, char **argv)
{
	tix_ticket_query q = {.status_mask = TIX_STATUS_MASK_ACTIVE};
	tix_oid author;
	bool all = false, explicit_status = false;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--all")) {
			all = true;
		} else if (!strcmp(argv[i], "--status") && need_value(i, argc, argv[i])) {
			tix_ticket_status s;
			if (!tix_ticket_status_parse(argv[++i], &s))
				return cli_usage("unknown status '%s'", argv[i]);
			q.status_mask = (explicit_status ? q.status_mask : 0) | (1u << s);
			explicit_status = true;
		} else if (!strcmp(argv[i], "--label") && need_value(i, argc, argv[i])) {
			q.label = argv[++i];
		} else if (!strcmp(argv[i], "--author") && need_value(i, argc, argv[i])) {
			int rc = cli_resolve(c, TIX_ENTITY_IDENTITY, argv[++i], &author);
			if (rc)
				return rc;
			q.author = &author;
		} else {
			return cli_usage("ls: unexpected '%s'", argv[i]);
		}
	}
	if (all && !explicit_status)
		q.status_mask = 0;
	tix_entity_list l;
	tix_status st = tix_ticket_list(c->repo, &q, &l, &c->err);
	if (st)
		return cli_fail(c, st);
	for (size_t i = 0; i < l.n; i++) {
		const tix_ticket *t = &l.v[i]->u.ticket;
		char s[16];
		printf("%s  %-11s  %s", cli_short(&l.v[i]->id, s), tix_ticket_status_name(t->status), t->title);
		print_labels(&t->labels);
		putchar('\n');
	}
	if (l.nbroken)
		fprintf(stderr, "tix: skipped %zu invalid ticket%s\n", l.nbroken, l.nbroken == 1 ? "" : "s");
	tix_entity_list_free(&l);
	return EXIT_OK;
}

static void short_opid(const tix_opid *id, char *out)
{
	char s[16];
	snprintf(out, 32, "%s:%u", cli_short(&id->commit, s), (unsigned)id->line);
}

static void indent(const char *text)
{
	fputs("    ", stdout);
	for (const char *p = text; *p; p++) {
		putchar(*p);
		if (*p == '\n' && p[1])
			fputs("    ", stdout);
	}
	putchar('\n');
}

static int load_ticket_arg(cli *c, const char *arg, tix_entity **out)
{
	tix_oid id;
	int rc = cli_resolve(c, TIX_ENTITY_TICKET, arg, &id);
	if (rc)
		return rc;
	tix_status st = tix_entity_load(c->repo, TIX_ENTITY_TICKET, &id, out, &c->err);
	return st ? cli_fail(c, st) : EXIT_OK;
}

int cmd_show(cli *c, int argc, char **argv)
{
	if (argc != 2)
		return cli_usage("show: expected one ticket ID");
	tix_entity *e;
	int rc = load_ticket_arg(c, argv[1], &e);
	if (rc)
		return rc;
	const tix_ticket *t = &e->u.ticket;
	char hex[TIX_OID_MAX_HEX + 1], who[300], when[32], cid[32];
	printf("%s\n", t->title);
	printf("id:       %s\n", tix_oid_hex(&e->id, hex));
	printf("status:   %s\n", tix_ticket_status_name(t->status));
	printf("author:   %s\n", cli_who(c, &t->author, who, sizeof(who)));
	printf("created:  %s\n", cli_time(e->created, when));
	printf("updated:  %s\n", cli_time(e->updated, when));
	if (t->labels.n) {
		fputs("labels:   ", stdout);
		for (size_t i = 0; i < t->labels.n; i++)
			printf("%s%s", i ? ", " : "", t->labels.v[i]);
		putchar('\n');
	}
	if (t->body[0]) {
		putchar('\n');
		indent(t->body);
	}
	for (size_t i = 0; i < t->ncomments; i++) {
		const tix_comment *cm = &t->comments[i];
		short_opid(&cm->id, cid);
		printf("\n--- %s  %s  %s%s\n", cid, cli_who(c, &cm->author, who, sizeof(who)),
		       cli_time(cm->time, when), cm->edited ? "  (edited)" : "");
		indent(cm->body);
	}
	tix_entity_free(e);
	return EXIT_OK;
}

/* "<hex prefix>:<line>" against the ticket's comments (prefix >= 7 chars). */
static int resolve_comment(const tix_entity *e, const char *arg, tix_opid *out)
{
	const char *colon = strchr(arg, ':');
	size_t plen = colon ? (size_t)(colon - arg) : 0;
	char *end;
	unsigned long line = colon ? strtoul(colon + 1, &end, 10) : 0;
	if (!colon || plen < TIX_ID_MIN_PREFIX || *end || !line)
		return cli_usage("comment id must look like abc1234:2 (as shown by `tix show`)");
	size_t found = 0;
	for (size_t i = 0; i < e->u.ticket.ncomments; i++) {
		const tix_comment *cm = &e->u.ticket.comments[i];
		char hex[TIX_OID_MAX_HEX + 1];
		tix_oid_hex(&cm->id.commit, hex);
		if (cm->id.line == line && !strncmp(hex, arg, plen)) {
			*out = cm->id;
			found++;
		}
	}
	if (found != 1) {
		fprintf(stderr, "tix: %s comment '%s'\n", found ? "ambiguous" : "no such", arg);
		return EXIT_ERR;
	}
	return EXIT_OK;
}

int cmd_comment(cli *c, int argc, char **argv)
{
	const char *ticket = NULL, *msg = NULL, *edit = NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-m") && need_value(i, argc, argv[i]))
			msg = argv[++i];
		else if (!strcmp(argv[i], "--edit") && need_value(i, argc, argv[i]))
			edit = argv[++i];
		else if (!ticket && argv[i][0] != '-')
			ticket = argv[i];
		else
			return cli_usage("comment: unexpected '%s'", argv[i]);
	}
	if (!ticket)
		return cli_usage("comment: expected a ticket ID");
	tix_entity *e;
	int rc = load_ticket_arg(c, ticket, &e);
	if (rc)
		return rc;
	tix_opid target;
	if (edit && (rc = resolve_comment(e, edit, &target))) {
		tix_entity_free(e);
		return rc;
	}
	tix_buf text = TIX_BUF_INIT;
	if (msg) {
		tix_buf_adds(&text, msg);
	} else {
		rc = cli_edit_text(c, "\n# Write your comment. Lines starting with '#' are ignored.\n", &text);
		trim(&text);
		if (!rc && !text.len) {
			fprintf(stderr, "tix: empty comment; aborting\n");
			rc = EXIT_ERR;
		}
	}
	if (!rc) {
		tix_op op = edit ? tix_op_edit_comment(&target, text.data) : tix_op_text(TIX_OP_COMMENT, text.data);
		tix_status st = tix_entity_apply(c->repo, TIX_ENTITY_TICKET, &e->id, &op, 1, &c->err);
		tix_op_free(&op);
		if (st)
			rc = cli_fail(c, st);
	}
	tix_buf_free(&text);
	tix_entity_free(e);
	return rc;
}

int cmd_edit(cli *c, int argc, char **argv)
{
	TIX_VEC(tix_op) ops = {0};
	tix_strs add = {0}, rm = {0};
	const char *ticket = NULL;
	int rc = EXIT_OK;
	for (int i = 1; i < argc && rc == EXIT_OK; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "--title") && need_value(i, argc, a)) {
			TIX_VEC_PUSH(&ops, tix_op_text(TIX_OP_SET_TITLE, argv[++i]));
		} else if (!strcmp(a, "--body") && need_value(i, argc, a)) {
			TIX_VEC_PUSH(&ops, tix_op_text(TIX_OP_SET_BODY, argv[++i]));
		} else if (!strcmp(a, "--status") && need_value(i, argc, a)) {
			tix_ticket_status s;
			if (!tix_ticket_status_parse(argv[++i], &s))
				rc = cli_usage("unknown status '%s'", argv[i]);
			else
				TIX_VEC_PUSH(&ops, tix_op_set_status(s));
		} else if (a[0] == '+' && a[1]) {
			tix_strs_add(&add, a + 1);
		} else if (a[0] == '-' && a[1] && a[1] != '-') {
			tix_strs_add(&rm, a + 1);
		} else if (!ticket && a[0] != '-' && a[0] != '+') {
			ticket = a;
		} else {
			rc = cli_usage("edit: unexpected '%s'", a);
		}
	}
	if (rc == EXIT_OK && (add.n || rm.n))
		TIX_VEC_PUSH(&ops, tix_op_label(&add, &rm));
	if (rc == EXIT_OK && !ticket)
		rc = cli_usage("edit: expected a ticket ID");
	if (rc == EXIT_OK && !ops.n)
		rc = cli_usage("edit: nothing to change");
	tix_oid id;
	if (rc == EXIT_OK && !(rc = cli_resolve(c, TIX_ENTITY_TICKET, ticket, &id))) {
		tix_status st = tix_entity_apply(c->repo, TIX_ENTITY_TICKET, &id, ops.v, ops.n, &c->err);
		if (st)
			rc = cli_fail(c, st);
	}
	for (size_t i = 0; i < ops.n; i++)
		tix_op_free(&ops.v[i]);
	free(ops.v);
	tix_strs_free(&add);
	tix_strs_free(&rm);
	return rc;
}
