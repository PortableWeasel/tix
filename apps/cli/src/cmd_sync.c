/* tix pull | push | sync [REMOTE] */
#include "cli.h"

#include <string.h>

/* [--verify P] [REMOTE]: REMOTE, else git config tix.remote, else "origin". */
static int pick_remote(cli *c, int argc, char **argv, const char *cmd, bool pulls, tix_buf *out)
{
	const char *name = NULL;
	for (int i = 1; i < argc; i++) {
		if (pulls && !strcmp(argv[i], "--verify") && i + 1 < argc) {
			bool ok;
			tix_verify_policy v = tix_verify_policy_parse(argv[++i], &ok);
			if (!ok)
				return cli_usage("--verify is off, warn or require");
			tix_repo_set_verify(c->repo, v);
		} else if (argv[i][0] == '-') {
			return cli_usage("unknown option '%s'", argv[i]);
		} else if (!name) {
			name = argv[i];
		} else {
			return cli_usage("usage: tix %s [REMOTE]", cmd);
		}
	}
	tix_buf_reset(out);
	if (name) {
		tix_buf_adds(out, name);
	} else if (tix_store_libgit_config_get(c->store, "tix.remote", out, NULL)) {
		tix_buf_reset(out);
		tix_buf_adds(out, "origin");
	}
	return EXIT_OK;
}

static int report(const char *remote, const tix_sync_report *rep, bool pulled, bool pushed)
{
	if (pulled) {
		if (rep->created || rep->forwarded || rep->merged)
			printf("%s: %zu new, %zu updated, %zu merged\n", remote, rep->created, rep->forwarded,
			       rep->merged);
		else
			printf("%s: nothing new\n", remote);
	}
	if (pushed)
		printf("%s: pushed %zu\n", remote, rep->pushed);
	if (rep->skipped)
		fprintf(stderr, "tix: %zu ref(s) of an unknown kind ignored (newer tix?)\n", rep->skipped);
	if (rep->unsigned_commits || rep->bad_signatures)
		fprintf(stderr, "tix: %zu unsigned and %zu badly signed incoming commit(s)\n", rep->unsigned_commits,
			rep->bad_signatures);
	for (size_t i = 0; i < rep->problems.n; i++)
		fprintf(stderr, "tix: warning: %s\n", rep->problems.v[i]);
	return rep->problems.n ? EXIT_ERR : EXIT_OK;
}

static int run(cli *c, int argc, char **argv, bool pull, bool push)
{
	tix_buf remote = TIX_BUF_INIT;
	tix_sync_report rep = {0};
	int rc = pick_remote(c, argc, argv, argv[0], pull, &remote);
	if (rc) {
		tix_buf_free(&remote);
		return rc;
	}
	tix_status st;
	if (pull && push)
		st = tix_sync(c->repo, remote.data, &rep, &c->err);
	else if (pull)
		st = tix_pull(c->repo, remote.data, &rep, &c->err);
	else
		st = tix_push(c->repo, remote.data, &rep, &c->err);
	if (st == TIX_ERR_CONFLICT && !pull)
		fprintf(stderr, "tix: %s has changes you don't have; run `tix sync` (or pull, then push)\n",
			remote.data);
	rc = st ? cli_fail(c, st) : EXIT_OK;
	int rrc = report(remote.data, &rep, pull && !st, push && !st);
	tix_sync_report_free(&rep);
	tix_buf_free(&remote);
	return rc ? rc : rrc;
}

int cmd_pull(cli *c, int argc, char **argv) { return run(c, argc, argv, true, false); }
int cmd_push(cli *c, int argc, char **argv) { return run(c, argc, argv, false, true); }
int cmd_sync(cli *c, int argc, char **argv) { return run(c, argc, argv, true, true); }
