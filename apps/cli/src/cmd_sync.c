/* tix pull | push | sync [REMOTE] */
#include "cli.h"

#include <string.h>

/* REMOTE argument, else git config tix.remote, else "origin". */
static int pick_remote(cli *c, int argc, char **argv, const char *cmd, tix_buf *out)
{
	if (argc > 2)
		return cli_usage("usage: tix %s [REMOTE]", cmd);
	tix_buf_reset(out);
	if (argc == 2) {
		if (argv[1][0] == '-')
			return cli_usage("unknown option '%s'", argv[1]);
		tix_buf_adds(out, argv[1]);
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
	for (size_t i = 0; i < rep->problems.n; i++)
		fprintf(stderr, "tix: warning: %s\n", rep->problems.v[i]);
	return rep->problems.n ? EXIT_ERR : EXIT_OK;
}

static int run(cli *c, int argc, char **argv, bool pull, bool push)
{
	tix_buf remote = TIX_BUF_INIT;
	tix_sync_report rep = {0};
	int rc = pick_remote(c, argc, argv, argv[0], &remote);
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
