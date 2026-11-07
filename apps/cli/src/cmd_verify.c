/* tix verify [ID]...: commit signatures against identities' keys (architecture §14.3). */
#include "cli.h"

#include <string.h>

/* RETURNS 0, or the failing status (TIX_ERR_IO: a verifier can't run, so stop). */
static tix_status one(cli *c, tix_entity_type t, const tix_oid *id, tix_verify_report *total, bool quiet_good)
{
	tix_verify_report r = {0};
	tix_status st = tix_entity_verify(c->repo, t, id, &r, &c->err);
	char s[16];
	if (st) {
		fprintf(stderr, "tix: %s %s: %s\n", tix_entity_type_name(t), cli_short(id, s), c->err.msg);
		return st;
	}
	if (!quiet_good || r.unsigned_commits || r.bad)
		printf("%-8s %s  %zu signed, %zu unsigned, %zu bad\n", tix_entity_type_name(t), cli_short(id, s), r.good,
		       r.unsigned_commits, r.bad);
	total->good += r.good;
	total->unsigned_commits += r.unsigned_commits;
	total->bad += r.bad;
	return TIX_OK;
}

int cmd_verify(cli *c, int argc, char **argv)
{
	tix_verify_report total = {0};
	bool failed = false;
	tix_status io = TIX_OK;
	if (argc > 1) {
		for (int i = 1; i < argc && !io; i++) {
			tix_oid id;
			tix_entity_type t = TIX_ENTITY_TICKET;
			bool found = false;
			for (size_t k = 0; k < TIX_ENTITY_NTYPES && !found; k++) {
				t = tix_entity_types[k];
				found = !tix_entity_resolve(c->repo, t, argv[i], &id, NULL);
			}
			if (!found)
				return cli_usage("verify: no entity matches '%s'", argv[i]);
			tix_status st = one(c, t, &id, &total, false);
			failed |= st != TIX_OK;
			if (st == TIX_ERR_IO)
				io = st;
		}
	} else {
		for (size_t k = 0; k < TIX_ENTITY_NTYPES && !io; k++) {
			tix_entity_type t = tix_entity_types[k];
			tix_refs refs;
			tix_status st = tix_store_ref_list(c->store, tix_entity_ref_prefix(t), &refs, &c->err);
			if (st)
				return cli_fail(c, st);
			size_t plen = strlen(tix_entity_ref_prefix(t));
			for (size_t i = 0; i < refs.n && !io; i++) {
				tix_oid id;
				const char *hex = refs.v[i].name + plen;
				if (!tix_oid_from_hex(&id, hex, strlen(hex)))
					continue;
				tix_status vst = one(c, t, &id, &total, true);
				failed |= vst != TIX_OK;
				if (vst == TIX_ERR_IO)
					io = vst;
			}
			tix_refs_free(&refs);
		}
	}
	printf("%zu commit%s: %zu signed, %zu unsigned, %zu bad\n", total.good + total.unsigned_commits + total.bad,
	       total.good + total.unsigned_commits + total.bad == 1 ? "" : "s", total.good, total.unsigned_commits,
	       total.bad);
	if (io)
		return cli_fail(c, io);
	if (failed || total.bad || (total.unsigned_commits && tix_repo_verify(c->repo) == TIX_VERIFY_REQUIRE))
		return EXIT_ERR;
	return EXIT_OK;
}
