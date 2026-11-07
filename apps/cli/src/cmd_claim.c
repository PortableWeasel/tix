/* tix claim ID [--for D] | --renew | --release ; tix claim --next ...  (architecture §15.3) */
#include "cli.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* "30m", "2h", "90s", "1d"; a bare number is minutes. RETURNS seconds, or -1. */
static long long parse_duration(const char *s)
{
	char *end;
	long long v = strtoll(s, &end, 10);
	if (end == s || v <= 0)
		return -1;
	if (!*end || !strcmp(end, "m"))
		return v * 60;
	if (!strcmp(end, "s"))
		return v;
	if (!strcmp(end, "h"))
		return v * 3600;
	if (!strcmp(end, "d"))
		return v * 86400;
	return -1;
}

/* The remote to sync claims through: REMOTE, else tix.remote, else origin; NULL when
 * that remote isn't configured (claims then stay in this repository). */
static const char *claim_remote(cli *c, const char *name, tix_buf *out)
{
	tix_buf key = TIX_BUF_INIT, url = TIX_BUF_INIT;
	tix_buf_reset(out);
	if (name)
		tix_buf_adds(out, name);
	else if (tix_store_libgit_config_get(c->store, "tix.remote", out, NULL)) {
		tix_buf_reset(out);
		tix_buf_adds(out, "origin");
	}
	tix_buf_addf(&key, "remote.%s.url", out->data);
	bool ok = !tix_store_libgit_config_get(c->store, key.data, &url, NULL);
	tix_buf_free(&key);
	tix_buf_free(&url);
	return ok ? out->data : NULL;
}

static int claim_next(cli *c, int argc, char **argv)
{
	TIX_VEC(tix_ticket_state) states = {0};
	char (*stages)[TIX_STAGE_MAX + 1] = calloc((size_t)argc + 1, sizeof(*stages));
	tix_oid under;
	tix_claim_query q = {.lease = 1800};
	const char *remote_name = NULL;
	bool local = false;
	int rc = EXIT_OK;
	for (int i = 1; i < argc && rc == EXIT_OK; i++) {
		if (!strcmp(argv[i], "--next")) {
			continue;
		} else if (!strcmp(argv[i], "--status") && i + 1 < argc) {
			tix_ticket_state st;
			i++;
			if (!tix_state_parse(argv[i], &st.status, stages[i])) {
				rc = cli_usage("unknown status '%s'", argv[i]);
				break;
			}
			st.stage = stages[i][0] ? stages[i] : NULL;
			TIX_VEC_PUSH(&states, st);
		} else if (!strcmp(argv[i], "--under") && i + 1 < argc) {
			if (!(rc = cli_resolve(c, TIX_ENTITY_TICKET, argv[++i], &under)))
				q.under = &under;
		} else if (!strcmp(argv[i], "--for") && i + 1 < argc) {
			long long d = parse_duration(argv[++i]);
			if (d < 1 || d > TIX_CLAIM_MAX_SECS)
				rc = cli_usage("--for is a duration up to 24h, like 30m or 2h");
			q.lease = d;
		} else if (!strcmp(argv[i], "--remote") && i + 1 < argc) {
			remote_name = argv[++i];
		} else if (!strcmp(argv[i], "--local")) {
			local = true;
		} else {
			rc = cli_usage("claim --next: unexpected '%s'", argv[i]);
		}
	}
	tix_buf rb = TIX_BUF_INIT;
	if (rc == EXIT_OK) {
		q.states = states.v;
		q.nstates = states.n;
		q.remote = local ? NULL : claim_remote(c, remote_name, &rb);
		if (remote_name && !q.remote && !local)
			rc = cli_usage("remote '%s' isn't configured", remote_name);
	}
	if (rc == EXIT_OK) {
		tix_oid id, me;
		tix_status st = tix_claim_next(c->repo, &q, &id, &c->err);
		if (st == TIX_ERR_NOT_FOUND && tix_repo_identity(c->repo, &me)) { /* no work: not an error */
			fprintf(stderr, "tix: %s\n", c->err.msg);
			rc = EXIT_NO_WORK;
		} else if (st) {
			rc = cli_fail(c, st);
		} else {
			tix_entity *e;
			char s[16];
			if (!tix_entity_load(c->repo, TIX_ENTITY_TICKET, &id, &e, NULL)) {
				printf("Claimed %s %s (for %lld min)\n", cli_short(&id, s), e->u.ticket.title,
				       (long long)(q.lease / 60));
				tix_entity_free(e);
			}
		}
	}
	tix_buf_free(&rb);
	free(states.v);
	free(stages);
	return rc;
}

int cmd_claim(cli *c, int argc, char **argv)
{
	for (int i = 1; i < argc; i++)
		if (!strcmp(argv[i], "--next"))
			return claim_next(c, argc, argv);
	const char *ticket = NULL;
	bool release = false;
	long long lease = 1800;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--release")) {
			release = true;
		} else if (!strcmp(argv[i], "--renew")) {
			/* a renewal is a claim by the holder; kept as a flag for readability */
		} else if (!strcmp(argv[i], "--for") && i + 1 < argc) {
			lease = parse_duration(argv[++i]);
			if (lease < 1 || lease > TIX_CLAIM_MAX_SECS)
				return cli_usage("--for is a duration up to 24h, like 30m or 2h");
		} else if (!ticket && argv[i][0] != '-') {
			ticket = argv[i];
		} else {
			return cli_usage("claim: unexpected '%s'", argv[i]);
		}
	}
	if (!ticket)
		return cli_usage("usage: tix claim ID [--for 30m] | ID --renew | ID --release | --next [...]");
	tix_oid id;
	int rc = cli_resolve(c, TIX_ENTITY_TICKET, ticket, &id);
	if (rc)
		return rc;
	tix_op op = release ? tix_op_release() : tix_op_claim((int64_t)time(NULL) + lease);
	tix_status st = tix_entity_apply(c->repo, TIX_ENTITY_TICKET, &id, &op, 1, &c->err);
	tix_op_free(&op);
	if (st)
		return cli_fail(c, st);
	char s[16];
	if (release)
		printf("Released %s\n", cli_short(&id, s));
	else
		printf("Claimed %s for %lld min\n", cli_short(&id, s), lease / 60);
	return EXIT_OK;
}
