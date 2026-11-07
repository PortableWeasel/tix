/* Handing out work: tix_claim_next (architecture §15.3). */
#include "core_internal.h"

#include <stdlib.h>
#include <string.h>

bool tix_ticket_deps_done(tix_repo *r, const tix_entity *t)
{
	for (size_t i = 0; i < t->u.ticket.plan.ndeps; i++) {
		tix_entity *p;
		if (tix_entity_load(r, TIX_ENTITY_TICKET, &t->u.ticket.plan.deps[i].pred, &p, NULL))
			return false;
		tix_ticket_status s = p->u.ticket.status;
		tix_entity_free(p);
		if (s != TIX_TICKET_DONE && s != TIX_TICKET_CANCELLED)
			return false;
	}
	return true;
}

static bool state_matches(const tix_claim_query *q, const tix_ticket *t)
{
	if (!q->nstates)
		return t->status == TIX_TICKET_OPEN || t->status == TIX_TICKET_IN_PROGRESS;
	for (size_t i = 0; i < q->nstates; i++) {
		const tix_ticket_state *s = &q->states[i];
		if (s->status != t->status)
			continue;
		if (!s->stage || (!*s->stage && !t->stage) || (t->stage && !strcmp(s->stage, t->stage)))
			return true;
	}
	return false;
}

/* Is t's stage tagged human on the list that applies to it? */
static bool in_human_stage(tix_repo *r, const tix_entity *t)
{
	if (!t->u.ticket.stage)
		return false;
	tix_entity *owner = NULL;
	if (tix_ticket_stages(r, &t->id, NULL, &owner, NULL))
		return false;
	const tix_stage_def *d = tix_stage_find(owner, t->u.ticket.stage);
	bool human = d && d->human;
	tix_entity_free(owner);
	return human;
}

/* Does someone other than me hold or wait for a live claim on t? */
static bool other_claim(tix_repo *r, const tix_ticket *t, int64_t now)
{
	if (tix_claim_live(&t->claim, now) && !tix_oid_eq(&t->claim.identity, &r->identity))
		return true;
	for (size_t i = 0; i < t->nwaiting; i++)
		if (t->waiting[i].until > now && !tix_oid_eq(&t->waiting[i].identity, &r->identity))
			return true;
	return false;
}

/* Do I hold or wait for a claim on t? */
static bool mine(tix_repo *r, const tix_ticket *t)
{
	if (tix_oid_eq(&t->claim.identity, &r->identity))
		return true;
	for (size_t i = 0; i < t->nwaiting; i++)
		if (tix_oid_eq(&t->waiting[i].identity, &r->identity))
			return true;
	return false;
}

/*
 * Whoever sees a contested claim yields (§15.3): two claims met in a merge, and the one
 * that reached the remote first was pushed without seeing the other, so its owner never
 * sees the contest; the other side does, and releases in the same push that would show it.
 * RETURNS the first write error; *n counts the claims released.
 */
static tix_status yield_contested(tix_repo *r, int64_t now, size_t *n, tix_error *err)
{
	tix_entity_list all;
	*n = 0;
	tix_status st = tix_ticket_list(r, NULL, &all, err);
	for (size_t i = 0; !st && i < all.n; i++) {
		const tix_ticket *t = &all.v[i]->u.ticket;
		if (!mine(r, t) || !other_claim(r, t, now))
			continue;
		tix_op rel = tix_op_release();
		st = tix_entity_apply(r, TIX_ENTITY_TICKET, &all.v[i]->id, &rel, 1, err);
		tix_op_free(&rel);
		(*n)++;
	}
	tix_entity_list_free(&all);
	return st;
}

/* Candidates in WBS order; mine (already claimed by me, live) first. */
static tix_status candidates(tix_repo *r, const tix_claim_query *q, int64_t now, tix_entity_list *out, tix_error *err)
{
	tix_ticket_query tq = {.under = q->under, .sort = TIX_SORT_WBS};
	tix_entity_list all;
	tix_status st = tix_ticket_list(r, &tq, &all, err);
	if (st)
		return st;
	TIX_VEC(tix_entity *) mine = {0}, rest = {0};
	for (size_t i = 0; i < all.n; i++) {
		tix_entity *e = all.v[i];
		const tix_ticket *t = &e->u.ticket;
		bool live = tix_claim_live(&t->claim, now), own = live && tix_oid_eq(&t->claim.identity, &r->identity) &&
								!other_claim(r, t, now);
		if (state_matches(q, t) && (!live || own) && !in_human_stage(r, e) && tix_ticket_deps_done(r, e)) {
			if (own)
				TIX_VEC_PUSH(&mine, e);
			else
				TIX_VEC_PUSH(&rest, e);
			all.v[i] = NULL;
		}
	}
	tix_entity_list_free(&all);
	memset(out, 0, sizeof(*out));
	out->v = tix_xcalloc(mine.n + rest.n + 1, sizeof(*out->v));
	for (size_t i = 0; i < mine.n; i++)
		out->v[out->n++] = mine.v[i];
	for (size_t i = 0; i < rest.n; i++)
		out->v[out->n++] = rest.v[i];
	free(mine.v);
	free(rest.v);
	return TIX_OK;
}

/*
 * Pushes a fresh claim on id. A refused push means the remote moved: pull, and if the
 * merge shows someone else's live claim on id, theirs reached the remote first, so mine
 * yields (release, then sync) and *lost is set. Otherwise push again.
 */
static tix_status publish_claim(tix_repo *r, const char *remote, const tix_oid *id, bool *lost, tix_error *err)
{
	*lost = false;
	for (int round = 0; round < 3; round++) {
		tix_sync_report rep = {0};
		tix_status st = tix_push(r, remote, &rep, err);
		tix_sync_report_free(&rep);
		if (st != TIX_ERR_CONFLICT)
			return st;
		if ((st = tix_pull(r, remote, &rep, err))) {
			tix_sync_report_free(&rep);
			return st;
		}
		tix_sync_report_free(&rep);
		tix_entity *e;
		if ((st = tix_entity_load(r, TIX_ENTITY_TICKET, id, &e, err)))
			return st;
		bool theirs = other_claim(r, &e->u.ticket, r->env.now(r->env.ctx));
		tix_entity_free(e);
		if (theirs) {
			*lost = true;
			tix_op rel = tix_op_release();
			st = tix_entity_apply(r, TIX_ENTITY_TICKET, id, &rel, 1, err);
			tix_op_free(&rel);
			if (!st) {
				tix_sync_report srep = {0};
				st = tix_sync(r, remote, &srep, err);
				tix_sync_report_free(&srep);
			}
			return st;
		}
	}
	return tix_error_set(err, TIX_ERR_CONFLICT, "the remote kept moving; try again");
}

tix_status tix_claim_next(tix_repo *r, const tix_claim_query *q, tix_oid *out, tix_error *err)
{
	TIX_REQUIRE(q && out);
	if (tix_oid_is_null(&r->identity))
		return tix_error_set(err, TIX_ERR_NOT_FOUND, "no identity configured (create one with `tix id new`)");
	if (q->lease < 1 || q->lease > TIX_CLAIM_MAX_SECS)
		return tix_error_set(err, TIX_ERR_INVALID, "a lease is 1 second to %d hours", TIX_CLAIM_MAX_SECS / 3600);
	tix_status st;
	if (q->remote) {
		tix_sync_report rep = {0};
		st = tix_pull(r, q->remote, &rep, err);
		tix_sync_report_free(&rep);
		if (st)
			return st;
	}
	size_t yielded;
	if ((st = yield_contested(r, r->env.now(r->env.ctx), &yielded, err)))
		return st;
	if (yielded && q->remote) { /* publish the hand-over before anything else */
		tix_sync_report rep = {0};
		st = tix_sync(r, q->remote, &rep, err);
		tix_sync_report_free(&rep);
		if (st)
			return st;
	}
	tix_strs tried = {0};
	for (int attempt = 0; attempt < 5; attempt++) {
		int64_t now = r->env.now(r->env.ctx);
		tix_entity_list c;
		if ((st = candidates(r, q, now, &c, err)))
			break;
		tix_entity *pick = NULL;
		char hex[TIX_OID_MAX_HEX + 1];
		for (size_t i = 0; i < c.n && !pick; i++)
			if (!tix_strs_contains(&tried, tix_oid_hex(&c.v[i]->id, hex)))
				pick = c.v[i];
		if (!pick) {
			tix_entity_list_free(&c);
			st = tix_error_set(err, TIX_ERR_NOT_FOUND, "no work: nothing matches that isn't claimed, waiting "
								 "for a person, or waiting on another ticket");
			break;
		}
		tix_oid id = pick->id;
		tix_strs_add(&tried, tix_oid_hex(&id, hex));
		tix_entity_list_free(&c);
		tix_op op = tix_op_claim(now + q->lease);
		st = tix_entity_apply(r, TIX_ENTITY_TICKET, &id, &op, 1, err);
		tix_op_free(&op);
		if (st == TIX_ERR_CONFLICT) /* someone's claim landed meanwhile: next one */
			continue;
		if (st)
			break;
		bool lost = false;
		if (q->remote && (st = publish_claim(r, q->remote, &id, &lost, err)))
			break;
		tix_entity *e;
		if (!lost && (st = tix_entity_load(r, TIX_ENTITY_TICKET, &id, &e, err)))
			break;
		if (!lost) {
			lost = !tix_oid_eq(&e->u.ticket.claim.identity, &r->identity);
			tix_entity_free(e);
		}
		if (!lost) {
			*out = id;
			tix_strs_free(&tried);
			return TIX_OK;
		}
		st = tix_error_set(err, TIX_ERR_NOT_FOUND, "every candidate was taken by someone else meanwhile");
	}
	tix_strs_free(&tried);
	return st;
}
