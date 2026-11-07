/* tix stage ls | add | rm  (architecture §15.1) */
#include "cli.h"

#include <stdlib.h>
#include <string.h>

static int stage_ls(cli *c, int argc, char **argv)
{
	char s[16], state[TIX_STATE_STR];
	bool json = false;
	if (argc >= 2 && !strcmp(argv[argc - 1], "--json")) {
		json = true;
		argc--;
	}
	if (argc > 2 || (json && argc != 2))
		return cli_usage("usage: tix stage ls [TICKET] (--json needs a TICKET)");
	if (argc == 1) { /* every ticket that defines a list */
		tix_entity_list l;
		tix_ticket_query q = {.sort = TIX_SORT_WBS};
		tix_status st = tix_ticket_list(c->repo, &q, &l, &c->err);
		if (st)
			return cli_fail(c, st);
		size_t shown = 0;
		for (size_t i = 0; i < l.n; i++) {
			const tix_ticket *t = &l.v[i]->u.ticket;
			if (!t->nstages)
				continue;
			printf("%s  %s  (%zu stage%s)\n", cli_short(&l.v[i]->id, s), t->title, t->nstages,
			       t->nstages == 1 ? "" : "s");
			shown++;
		}
		if (!shown)
			printf("no stage lists (any stage is accepted; add one with `tix stage add ROOT NAME --phase P`)\n");
		tix_entity_list_free(&l);
		return EXIT_OK;
	}
	tix_oid id;
	int rc = cli_resolve(c, TIX_ENTITY_TICKET, argv[1], &id);
	if (rc)
		return rc;
	tix_entity *owner;
	tix_status st = tix_ticket_stages(c->repo, &id, NULL, &owner, &c->err);
	if (st)
		return cli_fail(c, st);
	if (json) {
		tix_buf b = TIX_BUF_INIT;
		tix_jw w;
		char hex[TIX_OID_MAX_HEX + 1];
		tix_jw_init(&w, &b);
		tix_jw_obj_begin(&w);
		tix_jw_key(&w, "owner");
		if (owner)
			tix_jw_str(&w, tix_oid_hex(&owner->id, hex));
		else
			tix_jw_null(&w);
		tix_jw_key(&w, "stages");
		tix_jw_arr_begin(&w);
		for (size_t i = 0; owner && i < owner->u.ticket.nstages; i++) {
			const tix_stage_def *d = &owner->u.ticket.stages[i];
			tix_jw_obj_begin(&w);
			tix_jw_key(&w, "name");
			tix_jw_str(&w, d->name);
			tix_jw_key(&w, "phase");
			tix_jw_str(&w, tix_ticket_status_name(d->phase));
			tix_jw_key(&w, "human");
			tix_jw_bool(&w, d->human);
			tix_jw_key(&w, "rank");
			tix_jw_int(&w, d->rank);
			tix_jw_obj_end(&w);
		}
		tix_jw_arr_end(&w);
		tix_jw_obj_end(&w);
		puts(tix_buf_cstr(&b));
		tix_buf_free(&b);
		tix_entity_free(owner);
		return EXIT_OK;
	}
	if (!owner) {
		printf("no stage list applies to %s (any stage is accepted)\n", cli_short(&id, s));
		return EXIT_OK;
	}
	const tix_ticket *t = &owner->u.ticket;
	printf("stages from %s %s:\n", cli_short(&owner->id, s), t->title);
	for (size_t i = 0; i < t->nstages; i++) {
		const tix_stage_def *d = &t->stages[i];
		printf("  %-32s  %-6s  %d\n", tix_state_str(d->phase, d->name, state), d->human ? "human" : "", (int)d->rank);
	}
	tix_entity_free(owner);
	return EXIT_OK;
}

static int stage_add(cli *c, int argc, char **argv)
{
	const char *ticket = NULL, *name = NULL;
	tix_ticket_status phase = TIX_TICKET_OPEN;
	bool have_phase = false, human = false;
	long rank = -1;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--phase") && i + 1 < argc) {
			if (!tix_ticket_status_parse(argv[++i], &phase))
				return cli_usage("stage add: unknown phase '%s'", argv[i]);
			have_phase = true;
		} else if (!strcmp(argv[i], "--human")) {
			human = true;
		} else if (!strcmp(argv[i], "--rank") && i + 1 < argc) {
			char *end;
			rank = strtol(argv[++i], &end, 10);
			if (*end || rank < 0 || rank > TIX_STAGE_RANK_MAX)
				return cli_usage("stage add: --rank is 0..%d", TIX_STAGE_RANK_MAX);
		} else if (!ticket && argv[i][0] != '-') {
			ticket = argv[i];
		} else if (!name && argv[i][0] != '-') {
			name = argv[i];
		} else {
			return cli_usage("stage add: unexpected '%s'", argv[i]);
		}
	}
	if (!ticket || !name || !have_phase)
		return cli_usage("usage: tix stage add TICKET NAME --phase PHASE [--human] [--rank N]");
	if (!tix_stage_valid(name))
		return cli_usage("stage names are [a-z][a-z0-9-], at most %d characters", TIX_STAGE_MAX);
	tix_oid id;
	int rc = cli_resolve(c, TIX_ENTITY_TICKET, ticket, &id);
	if (rc)
		return rc;
	if (rank < 0) { /* default: after the phase's last stage, in steps of 10 */
		tix_entity *e;
		tix_status st = tix_entity_load(c->repo, TIX_ENTITY_TICKET, &id, &e, &c->err);
		if (st)
			return cli_fail(c, st);
		rank = 10;
		for (size_t i = 0; i < e->u.ticket.nstages; i++)
			if (e->u.ticket.stages[i].phase == phase && e->u.ticket.stages[i].rank + 10 > rank &&
			    strcmp(e->u.ticket.stages[i].name, name))
				rank = e->u.ticket.stages[i].rank + 10;
		tix_entity_free(e);
		if (rank > TIX_STAGE_RANK_MAX)
			rank = TIX_STAGE_RANK_MAX;
	}
	tix_op op = tix_op_stage_add(name, phase, human, (int32_t)rank);
	tix_status st = tix_entity_apply(c->repo, TIX_ENTITY_TICKET, &id, &op, 1, &c->err);
	tix_op_free(&op);
	return st ? cli_fail(c, st) : EXIT_OK;
}

static int stage_rm(cli *c, int argc, char **argv)
{
	if (argc != 3)
		return cli_usage("usage: tix stage rm TICKET NAME");
	tix_oid id;
	int rc = cli_resolve(c, TIX_ENTITY_TICKET, argv[1], &id);
	if (rc)
		return rc;
	if (!tix_stage_valid(argv[2]))
		return cli_usage("stage rm: bad stage name '%s'", argv[2]);
	tix_op op = tix_op_stage_remove(argv[2]);
	tix_status st = tix_entity_apply(c->repo, TIX_ENTITY_TICKET, &id, &op, 1, &c->err);
	tix_op_free(&op);
	return st ? cli_fail(c, st) : EXIT_OK;
}

int cmd_stage(cli *c, int argc, char **argv)
{
	if (argc < 2)
		return cli_usage("usage: tix stage ls [TICKET] | add TICKET NAME --phase P [--human] [--rank N] | rm TICKET NAME");
	if (!strcmp(argv[1], "ls"))
		return stage_ls(c, argc - 1, argv + 1);
	if (!strcmp(argv[1], "add"))
		return stage_add(c, argc - 1, argv + 1);
	if (!strcmp(argv[1], "rm"))
		return stage_rm(c, argc - 1, argv + 1);
	return cli_usage("stage: unknown subcommand '%s'", argv[1]);
}
