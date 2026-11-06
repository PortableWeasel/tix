/* Planning use cases: sibling ranks and running the planner. */
#include "core_internal.h"

#include <stdlib.h>
#include <string.h>

static bool same_parent(const tix_entity *t, const tix_oid *parent)
{
	const tix_oid *p = &t->u.ticket.plan.parent;
	return parent ? tix_oid_eq(p, parent) : tix_oid_is_null(p);
}

static int cmp_rank(const void *pa, const void *pb)
{
	return strcmp((*(tix_entity *const *)pa)->u.ticket.plan.rank, (*(tix_entity *const *)pb)->u.ticket.plan.rank);
}

tix_status tix_ticket_rank(tix_repo *r, const tix_oid *parent, const tix_oid *self, tix_rank_pos pos,
			   const tix_oid *anchor, tix_buf *out, tix_error *err)
{
	TIX_REQUIRE(r && self && out && ((pos != TIX_RANK_BEFORE && pos != TIX_RANK_AFTER) || anchor));
	tix_entity_list all;
	tix_status st = tix_ticket_list(r, NULL, &all, err);
	if (st)
		return st;
	/* ranked siblings, in rank order (ties are impossible to split; we skip past them) */
	TIX_VEC(const tix_entity *) sib = {0};
	bool anchor_sibling = false;
	const tix_entity *anchor_e = NULL;
	for (size_t i = 0; i < all.n; i++) {
		const tix_entity *t = all.v[i];
		if (tix_oid_eq(&t->id, self) || !same_parent(t, parent))
			continue;
		if (anchor && tix_oid_eq(&t->id, anchor)) {
			anchor_sibling = true;
			anchor_e = t;
		}
		if (t->u.ticket.plan.rank)
			TIX_VEC_PUSH(&sib, t);
	}
	if (sib.n)
		qsort(sib.v, sib.n, sizeof(*sib.v), cmp_rank);
	const char *lo = NULL, *hi = NULL;
	if ((pos == TIX_RANK_BEFORE || pos == TIX_RANK_AFTER) && !anchor_sibling) {
		st = tix_error_set(err, TIX_ERR_INVALID, "the anchor is not a sibling of this ticket");
	} else if ((pos == TIX_RANK_BEFORE || pos == TIX_RANK_AFTER) && !anchor_e->u.ticket.plan.rank) {
		st = tix_error_set(err, TIX_ERR_INVALID, "the anchor has no rank yet (give it one with first/last)");
	} else {
		const char *ar = anchor_e ? anchor_e->u.ticket.plan.rank : NULL;
		switch (pos) {
		case TIX_RANK_FIRST: hi = sib.n ? sib.v[0]->u.ticket.plan.rank : NULL; break;
		case TIX_RANK_LAST: lo = sib.n ? sib.v[sib.n - 1]->u.ticket.plan.rank : NULL; break;
		case TIX_RANK_AFTER:
			lo = ar;
			for (size_t i = 0; i < sib.n && !hi; i++)
				if (strcmp(sib.v[i]->u.ticket.plan.rank, ar) > 0)
					hi = sib.v[i]->u.ticket.plan.rank;
			break;
		case TIX_RANK_BEFORE:
			hi = ar;
			for (size_t i = 0; i < sib.n; i++)
				if (strcmp(sib.v[i]->u.ticket.plan.rank, ar) < 0)
					lo = sib.v[i]->u.ticket.plan.rank;
			break;
		}
		st = tix_rank_between(lo, hi, out, err);
	}
	free(sib.v);
	tix_entity_list_free(&all);
	return st;
}

void tix_plan_result_free(tix_plan_result *res)
{
	tix_schedule_free(res->schedule);
	tix_entity_list_free(&res->tickets);
	tix_entity_list_free(&res->resources);
	tix_entity_list_free(&res->calendars);
	memset(res, 0, sizeof(*res));
}

tix_status tix_plan(tix_repo *r, const tix_oid *root, tix_date today, tix_plan_result *out, tix_error *err)
{
	TIX_REQUIRE(r && out && tix_date_valid(today));
	memset(out, 0, sizeof(*out));
	tix_status st = tix_ticket_list(r, NULL, &out->tickets, err);
	if (!st)
		st = tix_resource_list(r, &out->resources, err);
	if (!st)
		st = tix_calendar_list(r, &out->calendars, err);
	if (!st) {
		tix_plan_input in = {(const tix_entity *const *)out->tickets.v, out->tickets.n,
				     (const tix_entity *const *)out->resources.v, out->resources.n,
				     (const tix_entity *const *)out->calendars.v, out->calendars.n, root, today};
		st = tix_plan_compute(&in, &out->schedule, err);
	}
	if (st)
		tix_plan_result_free(out);
	return st;
}
