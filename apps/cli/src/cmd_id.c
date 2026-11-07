/* tix id ... */
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

static void print_identity(const tix_entity *e, bool current)
{
	char hex[TIX_OID_MAX_HEX + 1];
	const tix_identity *id = &e->u.identity;
	printf("id:     %s%s\n", tix_oid_hex(&e->id, hex), current ? "  (you)" : "");
	printf("name:   %s\nemail:  %s\n", id->name, id->email);
	for (size_t i = 0; i < id->nkeys; i++)
		printf("key:    %s %s\n", id->keys[i].kind, id->keys[i].fingerprint);
}

static int use_identity(cli *c, const tix_oid *id)
{
	char hex[TIX_OID_MAX_HEX + 1];
	tix_status st = tix_repo_set_identity(c->repo, id, &c->err);
	if (!st)
		st = tix_store_libgit_config_set(c->store, "tix.identity", tix_oid_hex(id, hex), &c->err);
	return st ? cli_fail(c, st) : EXIT_OK;
}

static int id_new(cli *c, int argc, char **argv)
{
	tix_buf name = TIX_BUF_INIT, email = TIX_BUF_INIT;
	int rc = EXIT_OK;
	for (int i = 1; i < argc && rc == EXIT_OK; i++) {
		if (!strcmp(argv[i], "--name") && need_value(i, argc, argv[i]))
			tix_buf_adds(&name, argv[++i]);
		else if (!strcmp(argv[i], "--email") && need_value(i, argc, argv[i]))
			tix_buf_adds(&email, argv[++i]);
		else
			rc = cli_usage("id new: unexpected '%s'", argv[i]);
	}
	if (rc == EXIT_OK && !name.len && tix_store_libgit_config_get(c->store, "user.name", &name, NULL))
		rc = cli_usage("id new: no --name and no git config user.name");
	if (rc == EXIT_OK && !email.len && tix_store_libgit_config_get(c->store, "user.email", &email, NULL))
		rc = cli_usage("id new: no --email and no git config user.email");
	tix_oid id, cur;
	char s[16];
	if (rc == EXIT_OK) {
		tix_status st = tix_identity_create(c->repo, name.data, email.data, &id, &c->err);
		if (st)
			rc = cli_fail(c, st);
	}
	if (rc == EXIT_OK) {
		printf("Created identity %s (%s <%s>)\n", cli_short(&id, s), name.data, email.data);
		if (!tix_repo_identity(c->repo, &cur)) {
			rc = use_identity(c, &id);
			if (rc == EXIT_OK)
				printf("Now writing as %s\n", s);
		} else {
			printf("Still writing as %s; switch with `tix id use %s`\n", cli_short(&cur, s),
			       cli_short(&id, s));
		}
	}
	tix_buf_free(&name);
	tix_buf_free(&email);
	return rc;
}

static int id_ls(cli *c)
{
	tix_entity_list l;
	tix_oid cur;
	char s[16];
	bool have = tix_repo_identity(c->repo, &cur);
	tix_status st = tix_identity_list(c->repo, &l, &c->err);
	if (st)
		return cli_fail(c, st);
	for (size_t i = 0; i < l.n; i++) {
		const tix_entity *e = l.v[i];
		printf("%c %s  %s <%s>\n", have && tix_oid_eq(&cur, &e->id) ? '*' : ' ', cli_short(&e->id, s),
		       e->u.identity.name, e->u.identity.email);
	}
	if (l.nbroken)
		fprintf(stderr, "tix: skipped %zu invalid identit%s\n", l.nbroken, l.nbroken == 1 ? "y" : "ies");
	tix_entity_list_free(&l);
	return EXIT_OK;
}

static int id_show(cli *c, int argc, char **argv)
{
	tix_oid id, cur;
	bool have = tix_repo_identity(c->repo, &cur);
	if (argc > 2)
		return cli_usage("id show: too many arguments");
	if (argc == 2) {
		int rc = cli_resolve(c, TIX_ENTITY_IDENTITY, argv[1], &id);
		if (rc)
			return rc;
	} else if (have) {
		id = cur;
	} else {
		fprintf(stderr, "tix: no identity configured (create one with `tix id new`)\n");
		return EXIT_ERR;
	}
	tix_entity *e;
	tix_status st = tix_entity_load(c->repo, TIX_ENTITY_IDENTITY, &id, &e, &c->err);
	if (st)
		return cli_fail(c, st);
	print_identity(e, have && tix_oid_eq(&cur, &id));
	tix_entity_free(e);
	return EXIT_OK;
}

static int id_edit(cli *c, int argc, char **argv)
{
	TIX_VEC(tix_op) ops = {0};
	tix_oid me;
	int rc = EXIT_OK;
	for (int i = 1; i < argc && rc == EXIT_OK; i++) {
		if (!strcmp(argv[i], "--name") && need_value(i, argc, argv[i])) {
			TIX_VEC_PUSH(&ops, tix_op_text(TIX_OP_SET_NAME, argv[++i]));
		} else if (!strcmp(argv[i], "--email") && need_value(i, argc, argv[i])) {
			TIX_VEC_PUSH(&ops, tix_op_text(TIX_OP_SET_EMAIL, argv[++i]));
		} else if (!strcmp(argv[i], "--add-key")) {
			if (i + 3 >= argc)
				rc = cli_usage("--add-key needs KIND FINGERPRINT PUBKEY");
			else {
				TIX_VEC_PUSH(&ops, tix_op_key_add(argv[i + 1], argv[i + 2], argv[i + 3]));
				i += 3;
			}
		} else if (!strcmp(argv[i], "--add-signing-key")) {
			tix_buf kind = TIX_BUF_INIT, fpr = TIX_BUF_INIT, pub = TIX_BUF_INIT;
			tix_status st = tix_store_libgit_signing_key(c->store, &kind, &fpr, &pub, &c->err);
			if (st) {
				rc = cli_fail(c, st);
			} else {
				TIX_VEC_PUSH(&ops, tix_op_key_add(kind.data, fpr.data, pub.data));
				printf("Adding %s key %s\n", kind.data, fpr.data);
			}
			tix_buf_free(&kind);
			tix_buf_free(&fpr);
			tix_buf_free(&pub);
		} else if (!strcmp(argv[i], "--remove-key") && need_value(i, argc, argv[i])) {
			TIX_VEC_PUSH(&ops, tix_op_key_remove(argv[++i]));
		} else {
			rc = cli_usage("id edit: unexpected '%s'", argv[i]);
		}
	}
	if (rc == EXIT_OK && !ops.n)
		rc = cli_usage("id edit: nothing to change");
	if (rc == EXIT_OK && !tix_repo_identity(c->repo, &me)) {
		fprintf(stderr, "tix: no identity configured (create one with `tix id new`)\n");
		rc = EXIT_ERR;
	}
	if (rc == EXIT_OK) {
		tix_status st = tix_entity_apply(c->repo, TIX_ENTITY_IDENTITY, &me, ops.v, ops.n, &c->err);
		if (st)
			rc = cli_fail(c, st);
	}
	for (size_t i = 0; i < ops.n; i++)
		tix_op_free(&ops.v[i]);
	free(ops.v);
	return rc;
}

int cmd_id(cli *c, int argc, char **argv)
{
	if (argc < 2)
		return cli_usage("id: expected new, ls, show, use or edit");
	const char *sub = argv[1];
	argc--;
	argv++;
	if (!strcmp(sub, "new"))
		return id_new(c, argc, argv);
	if (!strcmp(sub, "ls"))
		return argc == 1 ? id_ls(c) : cli_usage("id ls takes no arguments");
	if (!strcmp(sub, "show"))
		return id_show(c, argc, argv);
	if (!strcmp(sub, "edit"))
		return id_edit(c, argc, argv);
	if (!strcmp(sub, "use")) {
		tix_oid id;
		char s[16];
		if (argc != 2)
			return cli_usage("id use: expected one ID");
		int rc = cli_resolve(c, TIX_ENTITY_IDENTITY, argv[1], &id);
		if (!rc && !(rc = use_identity(c, &id)))
			printf("Now writing as %s\n", cli_short(&id, s));
		return rc;
	}
	return cli_usage("id: unknown subcommand '%s'", sub);
}
