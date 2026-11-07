/* tix bridge pull | push | sync: tickets <-> Forgejo issues (architecture §14.4). */
#include "cli.h"
#include "tix/bridge_forgejo.h"

#include <stdlib.h>
#include <string.h>

static const char usage[] =
	"usage: tix bridge pull|push|sync [--url URL] [--under ID | --label L | --all] [--no-comments] [--dry-run]";

/* The token: TIX_FORGEJO_TOKEN, else git's credential helpers for the URL's host. */
static int token_for(cli *c, const char *url, tix_buf *tok)
{
	const char *env = getenv("TIX_FORGEJO_TOKEN");
	if (env && *env) {
		tix_buf_adds(tok, env);
		return EXIT_OK;
	}
	tix_buf scheme = TIX_BUF_INIT, host = TIX_BUF_INIT, owner = TIX_BUF_INIT, repo = TIX_BUF_INIT, path = TIX_BUF_INIT;
	int rc = EXIT_OK;
	if (!tix_bridge_forgejo_parse(url, &scheme, &host, &owner, &repo)) {
		rc = cli_usage("bridge: %s is not https://host/owner/repo", url);
	} else {
		tix_buf_addf(&path, "%s/%s", owner.data, repo.data);
		if (tix_store_libgit_credential(c->store, scheme.data, host.data, path.data, tok, &c->err)) {
			fprintf(stderr, "tix: no token for %s: set TIX_FORGEJO_TOKEN, or store one with a git credential "
					"helper (username anything, password the token)\n", host.data);
			rc = EXIT_ERR;
		}
	}
	tix_buf_free(&scheme);
	tix_buf_free(&host);
	tix_buf_free(&owner);
	tix_buf_free(&repo);
	tix_buf_free(&path);
	return rc;
}

int cmd_bridge(cli *c, int argc, char **argv)
{
	if (argc < 2)
		return cli_usage("%s", usage);
	tix_bridge_opts o = {.comments = true};
	if (!strcmp(argv[1], "pull"))
		o.mode = TIX_BRIDGE_PULL;
	else if (!strcmp(argv[1], "push"))
		o.mode = TIX_BRIDGE_PUSH;
	else if (!strcmp(argv[1], "sync"))
		o.mode = TIX_BRIDGE_SYNC;
	else
		return cli_usage("%s", usage);
	const char *url_arg = NULL, *under_arg = NULL;
	tix_oid under;
	for (int i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "--url") && i + 1 < argc)
			url_arg = argv[++i];
		else if (!strcmp(argv[i], "--under") && i + 1 < argc)
			under_arg = argv[++i];
		else if (!strcmp(argv[i], "--label") && i + 1 < argc)
			o.label = argv[++i];
		else if (!strcmp(argv[i], "--all"))
			o.all = true;
		else if (!strcmp(argv[i], "--no-comments"))
			o.comments = false;
		else if (!strcmp(argv[i], "--dry-run"))
			o.dry_run = true;
		else
			return cli_usage("bridge: unexpected '%s'\n%s", argv[i], usage);
	}
	if (under_arg) {
		int rc = cli_resolve(c, TIX_ENTITY_TICKET, under_arg, &under);
		if (rc)
			return rc;
		o.under = &under;
	}
	tix_buf url = TIX_BUF_INIT, tok = TIX_BUF_INIT;
	if (url_arg)
		tix_buf_adds(&url, url_arg);
	else if (tix_store_libgit_config_get(c->store, "tix.bridge.url", &url, NULL)) {
		tix_buf_free(&url);
		return cli_usage("bridge: give --url https://host/owner/repo (or git config tix.bridge.url)");
	}
	int rc = token_for(c, url.data, &tok);
	tix_tracker *t = NULL;
	if (!rc) {
		tix_status st = tix_bridge_forgejo_open(url.data, tok.data, &t, &c->err);
		memset(tok.data, 0, tok.len);
		if (st)
			rc = cli_fail(c, st);
	}
	tix_buf_free(&tok);
	tix_bridge_report rep = {0};
	if (!rc) {
		tix_status st = tix_bridge_run(c->repo, c->store, t, &o, &rep, &c->err);
		for (size_t i = 0; i < rep.actions.n; i++)
			printf("%s\n", rep.actions.v[i]);
		for (size_t i = 0; i < rep.conflicts.n; i++)
			fprintf(stderr, "tix: conflict: %s\n", rep.conflicts.v[i]);
		if (!o.dry_run)
			printf("%s: %zu ticket%s and %zu issue%s created, %zu and %zu updated, %zu comment%s in, %zu out\n",
			       t->key, rep.tickets_created, rep.tickets_created == 1 ? "" : "s", rep.issues_created,
			       rep.issues_created == 1 ? "" : "s", rep.tickets_updated, rep.issues_updated, rep.comments_in,
			       rep.comments_in == 1 ? "" : "s", rep.comments_out);
		if (st)
			rc = cli_fail(c, st);
		else if (rep.conflicts.n)
			rc = EXIT_ERR;
	}
	tix_bridge_report_free(&rep);
	tix_tracker_destroy(t);
	tix_buf_free(&url);
	return rc;
}
