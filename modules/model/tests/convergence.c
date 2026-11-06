/*
 * Convergence property: clones making random concurrent edits and syncing in
 * random orders must, once every clone has every op commit, produce
 * byte-identical snapshots for every entity, whatever their merge commits
 * look like. Replay must also be independent of the order the DAG is fed in.
 */
#include "tix/model_replay.h"
#include "tix/sim.h"
#include "tix/testkit.h"

#include <stdlib.h>

#define NCLONES 3
#define NTICKETS 2
#define STEPS 60
#define SEEDS 150

static tix_error e = TIX_ERROR_INIT;
static size_t total_merges, total_ops;

static const char *pick(uint64_t *rng, const char *const *pool, size_t n)
{
	return pool[tk_rand_below(rng, n)];
}

static tix_oid g_tickets[2]; /* this seed's tickets, for references between them */

/* Planning ops (M4): LWW fields, dependency and assignment OR-maps. */
static tix_op random_plan_op(uint64_t *rng)
{
	const tix_oid *other = &g_tickets[tk_rand_below(rng, 2)];
	switch (tk_rand_below(rng, 8)) {
	case 0:
		return tix_op_set(TIX_F_PARENT, tk_rand_below(rng, 2) ? tix_value_id(other) : tix_value_null());
	case 1: return tix_op_set(TIX_F_START, tix_value_int(TIX_V_DATE, 20400 + (int64_t)tk_rand_below(rng, 5)));
	case 2:
		return tix_op_set(TIX_F_DURATION,
				  tk_rand_below(rng, 3) ? tix_value_int(TIX_V_INT, (int64_t)tk_rand_below(rng, 4)) : tix_value_null());
	case 3: return tix_op_dep_add(other, (tix_dep_kind)tk_rand_below(rng, 4), (int32_t)tk_rand_below(rng, 5) - 2);
	case 4: return tix_op_dep_remove(other);
	case 5: return tix_op_assign(other, 25 * (int32_t)(1 + tk_rand_below(rng, 4)), (int64_t)tk_rand_below(rng, 3) - 1);
	case 6: return tix_op_unassign(other);
	default: return tix_op_set(TIX_F_RANK, tix_value_str(tk_rand_below(rng, 2) ? "i" : "r"));
	}
}

static tix_op random_ticket_op(uint64_t *rng, tix_sim_clone *c, const tix_oid *t, bool *ok)
{
	static const char *const titles[] = {"alpha", "beta", "gamma"};
	static const char *const labels[] = {"a", "b", "c"};
	tix_strs s = {0};
	tix_op op;
	*ok = true;
	if (tk_rand_below(rng, 2))
		return random_plan_op(rng);
	switch (tk_rand_below(rng, 7)) {
	case 0: return tix_op_text(TIX_OP_SET_TITLE, pick(rng, titles, 3));
	case 1: return tix_op_text(TIX_OP_SET_BODY, pick(rng, titles, 3));
	case 2: return tix_op_set_status((tix_ticket_status)tk_rand_below(rng, 5));
	case 3:
		tix_strs_add(&s, pick(rng, labels, 3));
		op = tix_op_label(&s, NULL);
		tix_strs_free(&s);
		return op;
	case 4:
		tix_strs_add(&s, pick(rng, labels, 3));
		op = tix_op_label(NULL, &s);
		tix_strs_free(&s);
		return op;
	case 5: return tix_op_text(TIX_OP_COMMENT, pick(rng, titles, 3));
	default: {
		char ref[128];
		tix_entity *ent;
		*ok = false;
		if (tix_sim_load(c, TIX_ENTITY_TICKET, tix_sim_ref(TIX_ENTITY_TICKET, t, ref), &ent, &e))
			return op = (tix_op){0};
		/* edit any comment: edits of others' comments must be ignored identically everywhere */
		if (ent->u.ticket.ncomments) {
			size_t k = tk_rand_below(rng, ent->u.ticket.ncomments);
			op = tix_op_edit_comment(&ent->u.ticket.comments[k].id, pick(rng, titles, 3));
			*ok = true;
		}
		tix_entity_free(ent);
		return op;
	}
	}
}

static tix_op random_identity_op(uint64_t *rng)
{
	static const char *const names[] = {"Ada", "Ada L.", "A. Lovelace"};
	static const char *const fprs[] = {"SHA256:k1", "SHA256:k2"};
	switch (tk_rand_below(rng, 3)) {
	case 0: return tix_op_text(TIX_OP_SET_NAME, pick(rng, names, 3));
	case 1: return tix_op_key_add("ssh", pick(rng, fprs, 2), pick(rng, names, 3));
	default: return tix_op_key_remove(pick(rng, fprs, 2));
	}
}

/* Ops on the shared resource / calendar (any author may edit them). */
static tix_op random_shared_op(uint64_t *rng, tix_entity_type t)
{
	if (t == TIX_ENTITY_RESOURCE) {
		switch (tk_rand_below(rng, 3)) {
		case 0: return tix_op_set(TIX_F_CAPACITY, tix_value_int(TIX_V_INT, 50 * (int64_t)(1 + tk_rand_below(rng, 3))));
		case 1: return tix_op_set(TIX_F_RES_NAME, tix_value_str(tk_rand_below(rng, 2) ? "dev" : "devs"));
		default: return tix_op_set(TIX_F_ACTIVE, tix_value_int(TIX_V_BOOL, (int64_t)tk_rand_below(rng, 2)));
		}
	}
	tix_date d = 20400 + (tix_date)tk_rand_below(rng, 3);
	switch (tk_rand_below(rng, 3)) {
	case 0: return tix_op_exception_add(d, d + 1, tk_rand_below(rng, 2), tk_rand_below(rng, 2) ? "x" : "y");
	case 1: return tix_op_exception_remove(d, d + 1);
	default: return tix_op_set(TIX_F_HOURS, tix_value_int(TIX_V_INT, 6 + (int64_t)tk_rand_below(rng, 3)));
	}
}

static void snapshot_at(tix_sim_clone *c, const char *ref, tix_entity_type t, tix_buf *out)
{
	tix_entity *ent = NULL;
	tix_buf_reset(out);
	TK_OK(tix_sim_load(c, t, ref, &ent, &e), &e);
	if (ent)
		tix_entity_snapshot(ent, out);
	tix_entity_free(ent);
}

/* The cache codec must reproduce the entity exactly. */
static void check_codec(const tix_entity *ent, tix_hash_algo algo)
{
	tix_buf enc = TIX_BUF_INIT, enc2 = TIX_BUF_INIT, s1 = TIX_BUF_INIT, s2 = TIX_BUF_INIT;
	tix_entity *back = NULL;
	tix_entity_encode(ent, &enc);
	TK_OK(tix_entity_decode(enc.data, enc.len, algo, &back, &e), &e);
	if (back) {
		tix_entity_encode(back, &enc2);
		TK_STR_EQ(tix_buf_cstr(&enc2), tix_buf_cstr(&enc));
		tix_entity_snapshot(ent, &s1);
		tix_entity_snapshot(back, &s2);
		TK_STR_EQ(tix_buf_cstr(&s2), tix_buf_cstr(&s1));
		TK_CHECK(back->max_lamport == ent->max_lamport && tix_oid_eq(&back->head, &ent->head));
	}
	/* every truncation is rejected, never crashes */
	for (size_t cut = 0; cut < enc.len; cut += 1 + enc.len / 7) {
		tix_entity *x = NULL;
		tix_status st = tix_entity_decode(enc.data, cut, algo, &x, NULL);
		tk_checks++;
		if (st == TIX_OK)
			tk_fail(__FILE__, __LINE__, "truncated cache entry (%zu of %zu bytes) decoded", cut, enc.len);
		tix_entity_free(x);
	}
	/* flipped bytes: any status is fine, as long as nothing crashes or leaks */
	for (size_t at = 0; at < enc.len; at += 1 + enc.len / 11) {
		static const char subst[] = {'"', '0', '-', '9', '{', ']', 'x', '\\', ','};
		char saved = enc.data[at];
		enc.data[at] = subst[at % sizeof(subst)];
		tix_entity *x = NULL;
		if (tix_entity_decode(enc.data, enc.len, algo, &x, NULL) == TIX_OK) {
			tix_buf again = TIX_BUF_INIT;
			tix_entity_encode(x, &again); /* whatever decoded must be usable */
			tix_buf_free(&again);
		}
		tix_entity_free(x);
		enc.data[at] = saved;
	}
	tix_entity_free(back);
	tix_buf_free(&enc);
	tix_buf_free(&enc2);
	tix_buf_free(&s1);
	tix_buf_free(&s2);
}

static void check_order_independence(uint64_t *rng, tix_sim_clone *c, const tix_ref *ref,
				     tix_entity_type t, const tix_buf *want)
{
	tix_sim_dag d;
	tix_entity *ent = NULL;
	TK_OK(tix_sim_dag_load(c->store, &ref->oid, &d, &e), &e);
	for (size_t i = d.n; i > 1; i--) {
		size_t j = tk_rand_below(rng, i);
		tix_dag_node tmp = d.nodes[i - 1];
		d.nodes[i - 1] = d.nodes[j];
		d.nodes[j] = tmp;
	}
	TK_OK(tix_replay(t, d.nodes, d.n, &ent, &e), &e);
	if (ent)
		check_codec(ent, (tix_hash_algo)ref->oid.algo);
	if (ent) {
		tix_buf s = TIX_BUF_INIT;
		tix_entity_snapshot(ent, &s);
		TK_STR_EQ(tix_buf_cstr(&s), tix_buf_cstr(want));
		tix_buf_free(&s);
	}
	tix_entity_free(ent);
	tix_sim_dag_free(&d);
}

static void run_seed(uint64_t seed, tix_hash_algo algo)
{
	uint64_t rng = seed;
	tix_sim_clone c[NCLONES];
	tix_oid tickets[NTICKETS];
	char name[8];

	for (int i = 0; i < NCLONES; i++) {
		snprintf(name, sizeof(name), "c%d", i);
		tix_sim_clone_init(&c[i], name, algo);
		char nonce[TIX_NONCE_HEX + 1];
		tix_op op = tix_op_identity_create(tix_sim_nonce(&c[i], nonce), "Clone", "c@example.org");
		TK_OK(tix_sim_create(&c[i], TIX_ENTITY_IDENTITY, &op, 1, &c[i].me, &e), &e);
		tix_op_free(&op);
	}
	tix_strs a = {0};
	tix_strs_add(&a, "a");
	for (int k = 0; k < NTICKETS; k++) {
		char nonce[TIX_NONCE_HEX + 1];
		tix_op op = tix_op_ticket_create(tix_sim_nonce(&c[0], nonce), "ticket", "", &a);
		TK_OK(tix_sim_create(&c[0], TIX_ENTITY_TICKET, &op, 1, &tickets[k], &e), &e);
		tix_op_free(&op);
	}
	tix_strs_free(&a);
	g_tickets[0] = tickets[0];
	g_tickets[1] = tickets[1];
	tix_oid shared[2];
	{
		char nonce[TIX_NONCE_HEX + 1];
		tix_op op = tix_op_resource_create(tix_sim_nonce(&c[0], nonce), "dev", TIX_RES_ROLE);
		TK_OK(tix_sim_create(&c[0], TIX_ENTITY_RESOURCE, &op, 1, &shared[0], &e), &e);
		tix_op_free(&op);
		op = tix_op_calendar_create(tix_sim_nonce(&c[0], nonce), "cal");
		TK_OK(tix_sim_create(&c[0], TIX_ENTITY_CALENDAR, &op, 1, &shared[1], &e), &e);
		tix_op_free(&op);
	}
	for (int i = 1; i < NCLONES; i++)
		TK_OK(tix_sim_sync(&c[i], &c[0], &e), &e);

	for (int step = 0; step < STEPS; step++) {
		tix_sim_clone *me = &c[tk_rand_below(&rng, NCLONES)];
		uint64_t r = tk_rand_below(&rng, 10);
		if (r < 3) {
			tix_sim_clone *other = &c[tk_rand_below(&rng, NCLONES)];
			if (other != me)
				TK_OK(tix_sim_sync(me, other, &e), &e);
		} else if (r < 9) {
			const tix_oid *t = &tickets[tk_rand_below(&rng, NTICKETS)];
			tix_op ops[2];
			size_t n = 0, want = 1 + tk_rand_below(&rng, 2);
			for (size_t k = 0; k < want; k++) {
				bool ok;
				tix_op op = random_ticket_op(&rng, me, t, &ok);
				if (ok)
					ops[n++] = op;
			}
			if (n)
				TK_OK(tix_sim_apply(me, TIX_ENTITY_TICKET, t, ops, n, &e), &e);
			total_ops += n;
			for (size_t k = 0; k < n; k++)
				tix_op_free(&ops[k]);
		} else if (tk_rand_below(&rng, 2)) {
			int k = (int)tk_rand_below(&rng, 2);
			tix_entity_type st = k ? TIX_ENTITY_CALENDAR : TIX_ENTITY_RESOURCE;
			tix_op op = random_shared_op(&rng, st);
			TK_OK(tix_sim_apply(me, st, &shared[k], &op, 1, &e), &e);
			tix_op_free(&op);
			total_ops++;
		} else {
			tix_op op = random_identity_op(&rng);
			TK_OK(tix_sim_apply(me, TIX_ENTITY_IDENTITY, &me->me, &op, 1, &e), &e);
			tix_op_free(&op);
			total_ops++;
		}
	}

	for (int round = 0; round < 2; round++)
		for (int i = 0; i < NCLONES; i++)
			for (int j = 0; j < NCLONES; j++)
				if (i != j)
					TK_OK(tix_sim_sync(&c[i], &c[j], &e), &e);

	tix_refs refs;
	TK_OK(tix_store_ref_list(c[0].store, "refs/tix/", &refs, &e), &e);
	TK_CHECK(refs.n == NCLONES + NTICKETS + 2);
	tix_buf want = TIX_BUF_INIT, got = TIX_BUF_INIT;
	for (size_t r = 0; r < refs.n; r++) {
		tix_entity_type t = TIX_ENTITY_IDENTITY;
		for (size_t k = 0; k < TIX_ENTITY_NTYPES; k++) {
			const char *pre = tix_entity_ref_prefix(tix_entity_types[k]);
			if (!strncmp(refs.v[r].name, pre, strlen(pre)))
				t = tix_entity_types[k];
		}
		snapshot_at(&c[0], refs.v[r].name, t, &want);
		for (int i = 1; i < NCLONES; i++) {
			snapshot_at(&c[i], refs.v[r].name, t, &got);
			tk_checks++;
			if (strcmp(tix_buf_cstr(&want), tix_buf_cstr(&got)))
				tk_fail(__FILE__, __LINE__, "seed %llu: %s diverged on c%d\n  c0: %s\n  c%d: %s",
					(unsigned long long)seed, refs.v[r].name, i, tix_buf_cstr(&want), i,
					tix_buf_cstr(&got));
		}
		check_order_independence(&rng, &c[NCLONES - 1], &refs.v[r], t, &want);
	}
	tix_buf_free(&want);
	tix_buf_free(&got);
	tix_refs_free(&refs);
	for (int i = 0; i < NCLONES; i++) {
		TK_CHECK(c[i].rejected == 0);
		total_merges += c[i].merges;
		tix_sim_clone_free(&c[i]);
	}
}

int main(void)
{
	for (uint64_t seed = 1; seed <= SEEDS; seed++)
		run_seed(seed, seed % 5 ? TIX_HASH_SHA1 : TIX_HASH_SHA256);
	fprintf(stderr, "%d seeds, %zu ops, %zu merge commits\n", SEEDS, total_ops, total_merges);
	TK_CHECK(total_merges > SEEDS); /* the test only means something with real concurrency */
	return tk_finish();
}
