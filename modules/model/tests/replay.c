/* Replay semantics and DAG validation, on simulated clones and hand-built DAGs. */
#include "tix/model_replay.h"
#include "tix/sim.h"
#include "tix/testkit.h"

#include <stdlib.h>

static tix_error e = TIX_ERROR_INIT;

static tix_strs strs1(const char *a)
{
	tix_strs s = {0};
	if (a)
		tix_strs_add(&s, a);
	return s;
}

/* Clone with its own identity. */
static void new_clone(tix_sim_clone *c, const char *name, const char *person)
{
	tix_sim_clone_init(c, name, TIX_HASH_SHA1);
	char nonce[TIX_NONCE_HEX + 1];
	tix_op op = tix_op_identity_create(tix_sim_nonce(c, nonce), person, "x@example.org");
	TK_OK(tix_sim_create(c, TIX_ENTITY_IDENTITY, &op, 1, &c->me, &e), &e);
	tix_op_free(&op);
}

static void apply1(tix_sim_clone *c, const tix_oid *id, tix_op op)
{
	TK_OK(tix_sim_apply(c, TIX_ENTITY_TICKET, id, &op, 1, &e), &e);
	tix_op_free(&op);
}

static tix_entity *load(tix_sim_clone *c, tix_entity_type t, const tix_oid *id)
{
	char ref[128];
	tix_entity *ent = NULL;
	TK_OK(tix_sim_load(c, t, tix_sim_ref(t, id, ref), &ent, &e), &e);
	return ent;
}

static tix_oid new_ticket(tix_sim_clone *c, const char *title, const char *label)
{
	tix_strs l = strs1(label);
	char nonce[TIX_NONCE_HEX + 1];
	tix_op op = tix_op_ticket_create(tix_sim_nonce(c, nonce), title, "", &l);
	tix_oid id;
	TK_OK(tix_sim_create(c, TIX_ENTITY_TICKET, &op, 1, &id, &e), &e);
	tix_op_free(&op);
	tix_strs_free(&l);
	return id;
}

static void test_lifecycle(void)
{
	tix_sim_clone a;
	new_clone(&a, "a", "Ada");
	tix_oid t = new_ticket(&a, "First", "bug");
	apply1(&a, &t, tix_op_text(TIX_OP_SET_TITLE, "Renamed"));
	apply1(&a, &t, tix_op_set_status(TIX_TICKET_DONE));
	apply1(&a, &t, tix_op_text(TIX_OP_COMMENT, "hello"));
	tix_entity *ent = load(&a, TIX_ENTITY_TICKET, &t);
	TK_CHECK(ent && ent->u.ticket.ncomments == 1);
	if (!ent || ent->u.ticket.ncomments != 1)
		return;
	tix_opid cid = ent->u.ticket.comments[0].id;
	TK_CHECK(cid.line == 2);
	tix_entity_free(ent);
	apply1(&a, &t, tix_op_edit_comment(&cid, "hello, edited"));

	ent = load(&a, TIX_ENTITY_TICKET, &t);
	const tix_ticket *tk = &ent->u.ticket;
	TK_CHECK(tix_oid_eq(&ent->id, &t));
	TK_STR_EQ(tk->title, "Renamed");
	TK_CHECK(tk->status == TIX_TICKET_DONE);
	TK_CHECK(tk->labels.n == 1 && !strcmp(tk->labels.v[0], "bug"));
	TK_CHECK(tix_oid_eq(&tk->author, &a.me));
	TK_STR_EQ(tk->comments[0].body, "hello, edited");
	TK_CHECK(tk->comments[0].edited);
	TK_CHECK(ent->nops == 5 && ent->created < ent->updated);
	tix_entity_free(ent);

	ent = load(&a, TIX_ENTITY_IDENTITY, &a.me);
	TK_STR_EQ(ent->u.identity.name, "Ada");
	tix_entity_free(ent);
	tix_sim_clone_free(&a);
}

static void sync_both(tix_sim_clone *a, tix_sim_clone *b)
{
	TK_OK(tix_sim_sync(a, b, &e), &e);
	TK_OK(tix_sim_sync(b, a, &e), &e);
}

static void test_concurrent_lww(void)
{
	tix_sim_clone a, b;
	new_clone(&a, "a", "Ada");
	new_clone(&b, "b", "Bob");
	tix_oid t = new_ticket(&a, "T", NULL);
	sync_both(&a, &b);

	apply1(&a, &t, tix_op_text(TIX_OP_SET_TITLE, "from a"));
	apply1(&b, &t, tix_op_text(TIX_OP_SET_BODY, "b was here"));
	apply1(&b, &t, tix_op_text(TIX_OP_SET_TITLE, "from b")); /* higher lamport: wins */
	sync_both(&a, &b);
	TK_CHECK(a.merges + b.merges >= 1);

	tix_entity *ea = load(&a, TIX_ENTITY_TICKET, &t), *eb = load(&b, TIX_ENTITY_TICKET, &t);
	TK_STR_EQ(ea->u.ticket.title, "from b");
	TK_STR_EQ(eb->u.ticket.title, "from b");
	TK_STR_EQ(ea->u.ticket.body, "b was here");
	tix_entity_free(ea);
	tix_entity_free(eb);
	tix_sim_clone_free(&a);
	tix_sim_clone_free(&b);
}

static bool has_label(tix_sim_clone *c, const tix_oid *t, const char *l)
{
	tix_entity *ent = load(c, TIX_ENTITY_TICKET, t);
	bool r = ent && tix_strs_contains(&ent->u.ticket.labels, l);
	tix_entity_free(ent);
	return r;
}

static void test_or_set(void)
{
	tix_sim_clone a, b;
	new_clone(&a, "a", "Ada");
	new_clone(&b, "b", "Bob");
	tix_oid t = new_ticket(&a, "T", "x");
	sync_both(&a, &b);

	tix_strs x = strs1("x");
	apply1(&a, &t, tix_op_label(NULL, &x)); /* a removes x ...           */
	apply1(&b, &t, tix_op_label(&x, NULL)); /* ... while b re-adds it    */
	TK_CHECK(!has_label(&a, &t, "x") && has_label(&b, &t, "x"));
	sync_both(&a, &b);
	TK_CHECK(has_label(&a, &t, "x") && has_label(&b, &t, "x")); /* unseen add survives */

	apply1(&a, &t, tix_op_label(NULL, &x)); /* now a has seen both adds */
	sync_both(&a, &b);
	TK_CHECK(!has_label(&a, &t, "x") && !has_label(&b, &t, "x"));
	tix_strs_free(&x);
	tix_sim_clone_free(&a);
	tix_sim_clone_free(&b);
}

static void test_edit_needs_same_author(void)
{
	tix_sim_clone a, b;
	new_clone(&a, "a", "Ada");
	new_clone(&b, "b", "Bob");
	tix_oid t = new_ticket(&a, "T", NULL);
	apply1(&a, &t, tix_op_text(TIX_OP_COMMENT, "mine"));
	sync_both(&a, &b);
	tix_entity *ent = load(&b, TIX_ENTITY_TICKET, &t);
	tix_opid cid = ent->u.ticket.comments[0].id;
	tix_entity_free(ent);
	apply1(&b, &t, tix_op_edit_comment(&cid, "hijacked"));
	sync_both(&a, &b);
	ent = load(&a, TIX_ENTITY_TICKET, &t);
	TK_STR_EQ(ent->u.ticket.comments[0].body, "mine");
	TK_CHECK(!ent->u.ticket.comments[0].edited);
	tix_entity_free(ent);
	tix_sim_clone_free(&a);
	tix_sim_clone_free(&b);
}

static void test_identity_rules(void)
{
	tix_sim_clone a, b;
	new_clone(&a, "a", "Ada");
	new_clone(&b, "b", "Bob");
	sync_both(&a, &b);

	tix_op op = tix_op_key_add("ssh", "SHA256:k1", "ssh-ed25519 AAAA");
	TK_OK(tix_sim_apply(&a, TIX_ENTITY_IDENTITY, &a.me, &op, 1, &e), &e);
	tix_op_free(&op);
	sync_both(&a, &b);
	tix_entity *ent = load(&b, TIX_ENTITY_IDENTITY, &a.me);
	TK_CHECK(ent->u.identity.nkeys == 1);
	tix_entity_free(ent);

	/* kind: human by default, agent once set (§15.2), visible on other clones */
	TK_CHECK((ent = load(&b, TIX_ENTITY_IDENTITY, &a.me)) && ent->u.identity.kind == TIX_ID_HUMAN);
	tix_entity_free(ent);
	op = tix_op_set(TIX_F_ID_KIND, tix_value_int(TIX_V_ENUM, TIX_ID_AGENT));
	TK_OK(tix_sim_apply(&a, TIX_ENTITY_IDENTITY, &a.me, &op, 1, &e), &e);
	tix_op_free(&op);
	sync_both(&a, &b);
	TK_CHECK((ent = load(&b, TIX_ENTITY_IDENTITY, &a.me)) && ent->u.identity.kind == TIX_ID_AGENT);
	tix_entity_free(ent);

	/* Bob forges an edit of Ada's identity: Bob's own copy no longer validates,
	 * and Ada refuses it on sync. */
	op = tix_op_text(TIX_OP_SET_NAME, "Mallory");
	TK_OK(tix_sim_apply(&b, TIX_ENTITY_IDENTITY, &a.me, &op, 1, &e), &e);
	tix_op_free(&op);
	char ref[128];
	TK_STATUS(tix_sim_load(&b, TIX_ENTITY_IDENTITY, tix_sim_ref(TIX_ENTITY_IDENTITY, &a.me, ref), &ent, &e),
		  TIX_ERR_INVALID, &e);
	size_t before = a.rejected;
	TK_OK(tix_sim_sync(&a, &b, &e), &e);
	TK_CHECK(a.rejected == before + 1);
	ent = load(&a, TIX_ENTITY_IDENTITY, &a.me);
	TK_STR_EQ(ent->u.identity.name, "Ada");
	tix_entity_free(ent);
	tix_sim_clone_free(&a);
	tix_sim_clone_free(&b);
}

/* ---- hand-built DAGs ----------------------------------------------------- */

#define AUTH "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"

typedef struct hnode {
	tix_oid oid;
	tix_oid parents[2];
	size_t np;
	tix_buf pack;
} hnode;

static tix_oid fake(int n)
{
	tix_oid o;
	memset(&o, 0, sizeof(o));
	o.algo = TIX_HASH_SHA1;
	o.bytes[19] = (uint8_t)n;
	o.bytes[0] = 0x10;
	return o;
}

/* Node n with given parents, lamport, and one op (or none). */
static void hn(hnode *h, int n, int p1, int p2, uint64_t lamport, tix_op *op)
{
	tix_oid author;
	tix_oid_from_hex(&author, AUTH, 40);
	memset(h, 0, sizeof(*h));
	h->oid = fake(n);
	if (p1)
		h->parents[h->np++] = fake(p1);
	if (p2)
		h->parents[h->np++] = fake(p2);
	tix_pack p = {TIX_ENTITY_TICKET, author, lamport, 100, op, op ? 1 : 0};
	TIX_REQUIRE(tix_pack_encode(&p, &h->pack, NULL) == TIX_OK);
}

static tix_status run(hnode *h, size_t n, tix_entity **out)
{
	tix_dag_node nodes[8];
	for (size_t i = 0; i < n; i++)
		nodes[i] = (tix_dag_node){h[i].oid, h[i].parents, h[i].np, h[i].pack.data, h[i].pack.len};
	return tix_replay(TIX_ENTITY_TICKET, nodes, n, out, &e);
}

static void expect_invalid(const char *what, hnode *h, size_t n)
{
	tix_entity *ent = NULL;
	tix_status st = run(h, n, &ent);
	tk_checks++;
	if (st != TIX_ERR_INVALID)
		tk_fail(__FILE__, __LINE__, "%s: got %s", what, tix_status_name(st));
	TK_CHECK(ent == NULL);
	tix_entity_free(ent);
	for (size_t i = 0; i < n; i++)
		tix_buf_free(&h[i].pack);
}

static void test_dag_validation(void)
{
	hnode h[4];
	tix_op create = tix_op_ticket_create("0123456789abcdef0123456789abcdef", "T", "", NULL), title = tix_op_text(TIX_OP_SET_TITLE, "x");
	tix_op unknown = {0};

	hn(&h[0], 1, 0, 0, 1, &create);
	hn(&h[1], 2, 9, 0, 2, &title);
	expect_invalid("missing parent", h, 2);

	hn(&h[0], 1, 0, 0, 5, &create);
	hn(&h[1], 2, 1, 0, 5, &title);
	expect_invalid("lamport not increasing", h, 2);

	hn(&h[0], 1, 0, 0, 1, &create);
	hn(&h[1], 2, 0, 0, 1, &create);
	expect_invalid("two roots", h, 2);

	hn(&h[0], 1, 0, 0, 1, &create);
	hn(&h[1], 2, 1, 0, 2, &title);
	hn(&h[2], 3, 1, 0, 2, &title);
	expect_invalid("two heads", h, 3);

	hn(&h[0], 1, 0, 0, 1, &create);
	hn(&h[1], 2, 1, 0, 2, &title);
	hn(&h[2], 3, 1, 0, 2, &title);
	hn(&h[3], 4, 2, 3, 3, &title);
	expect_invalid("merge with ops", h, 4);

	hn(&h[0], 1, 0, 0, 1, &create);
	hn(&h[1], 2, 1, 0, 2, &create);
	expect_invalid("create outside root", h, 2);

	hn(&h[0], 1, 0, 0, 1, &title);
	expect_invalid("root without create", h, 1);

	hn(&h[0], 1, 0, 0, 1, NULL);
	expect_invalid("empty root", h, 1);

	hn(&h[0], 1, 0, 0, 1, &create);
	hn(&h[1], 1, 0, 0, 1, &create);
	expect_invalid("duplicate commit", h, 2);

	hn(&h[0], 1, 0, 0, 1, &create);
	hn(&h[1], 2, 1, 3, 2, NULL);
	hn(&h[2], 3, 2, 0, 3, &title);
	hn(&h[3], 4, 2, 0, 3, &title);
	expect_invalid("cycle", h, 4);

	hn(&h[0], 1, 0, 0, 1, &create);
	tix_buf_free(&h[0].pack);
	tix_buf_adds(&h[0].pack, "{\"tix\":1,\"type\":\"identity\",\"author\":null,\"lamport\":1,\"time\":1}\n"
				 "{\"op\":\"create\",\"nonce\":\"0123456789abcdef0123456789abcdef\",\"name\":\"n\",\"email\":\"e\"}\n");
	expect_invalid("wrong entity type", h, 1);

	/* valid diamond with an unknown op; order of input must not matter */
	hn(&h[0], 1, 0, 0, 1, &create);
	hn(&h[1], 2, 1, 0, 2, &title);
	hn(&h[2], 3, 1, 0, 3, NULL);
	tix_buf_free(&h[2].pack);
	tix_buf_adds(&h[2].pack, "{\"tix\":1,\"type\":\"ticket\",\"author\":\"" AUTH "\",\"lamport\":3,\"time\":1}\n"
				 "{\"op\":\"from-the-future\",\"x\":1}\n");
	hn(&h[3], 4, 2, 3, 4, NULL);
	tix_entity *e1 = NULL, *e2 = NULL;
	TK_OK(run(h, 4, &e1), &e);
	hnode rev[4] = {h[3], h[1], h[2], h[0]};
	TK_OK(run(rev, 4, &e2), &e);
	if (e1 && e2) {
		tix_buf s1 = TIX_BUF_INIT, s2 = TIX_BUF_INIT;
		tix_entity_snapshot(e1, &s1);
		tix_entity_snapshot(e2, &s2);
		TK_STR_EQ(s1.data, s2.data);
		TK_CHECK(e1->nunknown == 1 && e1->nops == 2 && e1->max_lamport == 4);
		TK_CHECK(tix_oid_eq(&e1->head, &h[3].oid) && tix_oid_eq(&e1->id, &h[0].oid));
		TK_STR_EQ(e1->u.ticket.title, "x");
		tix_buf_free(&s1);
		tix_buf_free(&s2);
	}
	tix_entity_free(e1);
	tix_entity_free(e2);
	for (size_t i = 0; i < 4; i++)
		tix_buf_free(&h[i].pack);
	tix_op_free(&create);
	tix_op_free(&title);
	(void)unknown;
}

static tix_entity *loadt(tix_sim_clone *c, const tix_oid *t)
{
	return load(c, TIX_ENTITY_TICKET, t);
}

static void test_planning_fields(void)
{
	tix_sim_clone a, b;
	new_clone(&a, "a", "Ada");
	new_clone(&b, "b", "Bob");
	tix_oid t = new_ticket(&a, "T", NULL), p1 = new_ticket(&a, "P1", NULL), p2 = new_ticket(&a, "P2", NULL);
	tix_oid res = new_ticket(&a, "not really a resource", NULL);
	tix_date d;
	TIX_REQUIRE(tix_date_parse("2026-11-09", &d));

	/* defaults */
	tix_entity *x = loadt(&a, &t);
	TK_CHECK(x && tix_oid_is_null(&x->u.ticket.plan.parent) && x->u.ticket.plan.kind == TIX_KIND_TASK &&
		 x->u.ticket.plan.start == TIX_DATE_NONE && x->u.ticket.plan.duration == -1 &&
		 x->u.ticket.plan.progress == 0 && x->u.ticket.plan.started_at == -1 && !x->u.ticket.plan.ndeps);
	tix_entity_free(x);

	apply1(&a, &t, tix_op_set(TIX_F_PARENT, tix_value_id(&p1)));
	apply1(&a, &t, tix_op_set(TIX_F_START, tix_value_int(TIX_V_DATE, d)));
	apply1(&a, &t, tix_op_set(TIX_F_DURATION, tix_value_int(TIX_V_INT, 5)));
	apply1(&a, &t, tix_op_dep_add(&p1, TIX_DEP_FS, 0));
	apply1(&a, &t, tix_op_assign(&res, 50, -1));
	apply1(&a, &t, tix_op_set_status(TIX_TICKET_IN_PROGRESS));
	sync_both(&a, &b);

	/* concurrent: a re-parents to p2 and changes the dep's lag; b removes the dep
	 * (it saw only the first add) and nulls the duration */
	apply1(&a, &t, tix_op_set(TIX_F_PARENT, tix_value_id(&p2)));
	apply1(&a, &t, tix_op_dep_add(&p1, TIX_DEP_SS, 2));
	apply1(&b, &t, tix_op_dep_remove(&p1));
	apply1(&b, &t, tix_op_set(TIX_F_DURATION, tix_value_null()));
	apply1(&b, &t, tix_op_unassign(&res));
	apply1(&b, &t, tix_op_dep_add(&p2, TIX_DEP_FF, -1));
	apply1(&b, &t, tix_op_set_status(TIX_TICKET_DONE));
	sync_both(&a, &b);

	tix_entity *ea = loadt(&a, &t), *eb = loadt(&b, &t);
	tix_buf sa = TIX_BUF_INIT, sb = TIX_BUF_INIT;
	if (ea && eb) {
		tix_entity_snapshot(ea, &sa);
		tix_entity_snapshot(eb, &sb);
		TK_STR_EQ(tix_buf_cstr(&sa), tix_buf_cstr(&sb));
		const tix_ticket_plan *pl = &ea->u.ticket.plan;
		/* a's set-parent and b's set-duration are concurrent: both apply (different fields) */
		TK_CHECK(tix_oid_eq(&pl->parent, &p2) && pl->duration == -1 && pl->start == d);
		/* the dep on p1: a's re-add was unseen by b's remove, so it survives with a's value */
		TK_CHECK(pl->ndeps == 2);
		for (size_t i = 0; i < pl->ndeps; i++) {
			if (tix_oid_eq(&pl->deps[i].pred, &p1))
				TK_CHECK(pl->deps[i].kind == TIX_DEP_SS && pl->deps[i].lag == 2);
			else
				TK_CHECK(tix_oid_eq(&pl->deps[i].pred, &p2) && pl->deps[i].kind == TIX_DEP_FF &&
					 pl->deps[i].lag == -1);
		}
		TK_CHECK(pl->nassignments == 0); /* b saw the only assign */
		TK_CHECK(pl->started_at > 0 && pl->done_at >= pl->started_at);
		TK_CHECK(tix_order_cmp(&pl->parent_order, &pl->deps[0].order) != 0);
	}
	tix_entity_free(ea);
	tix_entity_free(eb);
	tix_buf_free(&sa);
	tix_buf_free(&sb);

	/* reopening clears done_at, started_at stays the first start */
	apply1(&a, &t, tix_op_set_status(TIX_TICKET_OPEN));
	x = loadt(&a, &t);
	TK_CHECK(x && x->u.ticket.plan.done_at == -1 && x->u.ticket.plan.started_at > 0);
	tix_entity_free(x);
	tix_sim_clone_free(&a);
	tix_sim_clone_free(&b);
}

static void test_stages(void)
{
	tix_sim_clone a, b;
	new_clone(&a, "a", "Ada");
	new_clone(&b, "b", "Bob");
	tix_oid t = new_ticket(&a, "T", NULL);

	/* set-state sets both; a following set-status clears the stage; the pair a
	 * writer emits (set-status then set-state) reads as one write */
	apply1(&a, &t, tix_op_set_state(TIX_TICKET_IN_PROGRESS, "review"));
	tix_entity *x = loadt(&a, &t);
	TK_CHECK(x && x->u.ticket.status == TIX_TICKET_IN_PROGRESS && x->u.ticket.stage &&
		 !strcmp(x->u.ticket.stage, "review") && x->u.ticket.plan.started_at > 0);
	tix_entity_free(x);
	apply1(&a, &t, tix_op_set_status(TIX_TICKET_OPEN));
	x = loadt(&a, &t);
	TK_CHECK(x && x->u.ticket.status == TIX_TICKET_OPEN && !x->u.ticket.stage);
	tix_entity_free(x);
	tix_op pair[] = {tix_op_set_status(TIX_TICKET_DONE), tix_op_set_state(TIX_TICKET_DONE, "unaccepted")};
	TK_OK(tix_sim_apply(&a, TIX_ENTITY_TICKET, &t, pair, 2, &e), &e);
	tix_op_free(&pair[0]);
	tix_op_free(&pair[1]);
	x = loadt(&a, &t);
	TK_CHECK(x && x->u.ticket.status == TIX_TICKET_DONE && x->u.ticket.stage &&
		 !strcmp(x->u.ticket.stage, "unaccepted") && x->u.ticket.plan.done_at > 0);
	tix_entity_free(x);

	/* stage list: OR-map by name, sorted by (phase, rank, name) */
	apply1(&a, &t, tix_op_stage_add("ready", TIX_TICKET_OPEN, false, 40));
	apply1(&a, &t, tix_op_stage_add("draft", TIX_TICKET_OPEN, true, 10));
	apply1(&a, &t, tix_op_stage_add("review", TIX_TICKET_IN_PROGRESS, false, 20));
	sync_both(&a, &b);
	apply1(&a, &t, tix_op_stage_remove("ready", TIX_TICKET_OPEN));
	apply1(&b, &t, tix_op_stage_add("ready", TIX_TICKET_OPEN, false, 50)); /* concurrent re-add survives */
	apply1(&b, &t, tix_op_stage_remove("draft", TIX_TICKET_OPEN));
	apply1(&a, &t, tix_op_stage_add("draft", TIX_TICKET_OPEN, false, 5)); /* concurrent re-add survives */
	/* one name may serve several phases; removing one leaves the other */
	apply1(&a, &t, tix_op_stage_add("needs-input", TIX_TICKET_OPEN, true, 30));
	apply1(&a, &t, tix_op_stage_add("needs-input", TIX_TICKET_IN_PROGRESS, true, 30));
	apply1(&a, &t, tix_op_stage_remove("needs-input", TIX_TICKET_OPEN));
	apply1(&a, &t, tix_op_stage_remove("needs-input", TIX_TICKET_IN_PROGRESS));
	sync_both(&a, &b);
	tix_entity *ea = loadt(&a, &t), *eb = loadt(&b, &t);
	tix_buf sa = TIX_BUF_INIT, sb = TIX_BUF_INIT;
	if (ea && eb) {
		tix_entity_snapshot(ea, &sa);
		tix_entity_snapshot(eb, &sb);
		TK_STR_EQ(tix_buf_cstr(&sa), tix_buf_cstr(&sb));
		const tix_ticket *tk = &ea->u.ticket;
		TK_CHECK(tk->nstages == 3);
		if (tk->nstages == 3) {
			TK_STR_EQ(tk->stages[0].name, "draft");
			TK_CHECK(!tk->stages[0].human && tk->stages[0].rank == 5);
			TK_STR_EQ(tk->stages[1].name, "ready");
			TK_CHECK(tk->stages[1].rank == 50);
			TK_STR_EQ(tk->stages[2].name, "review");
			TK_CHECK(tk->stages[2].phase == TIX_TICKET_IN_PROGRESS);
		}
		/* the entity cache codec round-trips the new fields */
		tix_buf enc = TIX_BUF_INIT, again = TIX_BUF_INIT;
		tix_entity *dec = NULL;
		tix_entity_encode(ea, &enc);
		TK_OK(tix_entity_decode(enc.data, enc.len, TIX_HASH_SHA1, &dec, &e), &e);
		if (dec) {
			tix_entity_snapshot(dec, &again);
			TK_STR_EQ(tix_buf_cstr(&again), tix_buf_cstr(&sa));
		}
		tix_entity_free(dec);
		tix_buf_free(&enc);
		tix_buf_free(&again);
	}
	tix_entity_free(ea);
	tix_entity_free(eb);
	tix_buf_free(&sa);
	tix_buf_free(&sb);
	tix_sim_clone_free(&a);
	tix_sim_clone_free(&b);
}

/* §15.3: the first live claim wins, identically everywhere; renewals; expiry; release. */
static void test_claims(void)
{
	tix_sim_clone a, b;
	new_clone(&a, "a", "Ada");
	new_clone(&b, "b", "Bob");
	tix_oid t = new_ticket(&a, "T", NULL);
	sync_both(&a, &b);
	const int64_t far = (int64_t)TIX_LAMPORT_MAX - 1;

	/* concurrent claims: both clones end up with the same, earlier claimant */
	apply1(&a, &t, tix_op_claim(far));
	apply1(&b, &t, tix_op_claim(far));
	apply1(&b, &t, tix_op_claim(far)); /* Bob renews his own (losing) claim */
	sync_both(&a, &b);
	tix_entity *ea = loadt(&a, &t), *eb = loadt(&b, &t);
	TK_CHECK(ea && eb && tix_oid_eq(&ea->u.ticket.claim.identity, &eb->u.ticket.claim.identity));
	tix_oid winner = ea->u.ticket.claim.identity;
	TK_CHECK(tix_oid_eq(&winner, &a.me) || tix_oid_eq(&winner, &b.me));
	TK_CHECK(tix_claim_live(&ea->u.ticket.claim, 0));
	int64_t since = ea->u.ticket.claim.since;
	tix_entity_free(ea);
	tix_entity_free(eb);

	tix_sim_clone *w = tix_oid_eq(&winner, &a.me) ? &a : &b, *l = w == &a ? &b : &a;
	/* the loser can't take a live claim over; the winner's renewal keeps `since` */
	apply1(l, &t, tix_op_claim(far));
	apply1(w, &t, tix_op_claim(far - 5));
	sync_both(&a, &b);
	ea = loadt(&a, &t);
	TK_CHECK(ea && tix_oid_eq(&ea->u.ticket.claim.identity, &winner) && ea->u.ticket.claim.since == since &&
		 (ea->u.ticket.claim.until == far || ea->u.ticket.claim.until == far - 5));
	tix_entity_free(ea);

	/* an expired claim is anyone's: winner lets it lapse (until in the past) */
	apply1(w, &t, tix_op_claim(1));
	apply1(l, &t, tix_op_claim(far)); /* not seen the lapse yet: still the winner's */
	ea = loadt(l, &t);
	TK_CHECK(ea && tix_oid_eq(&ea->u.ticket.claim.identity, &winner));
	tix_entity_free(ea);
	sync_both(&a, &b);
	apply1(l, &t, tix_op_claim(far));
	ea = loadt(l, &t);
	TK_CHECK(ea && tix_oid_eq(&ea->u.ticket.claim.identity, &l->me));
	TK_CHECK(!tix_claim_live(&ea->u.ticket.claim, far));
	tix_entity_free(ea);

	/* a release by someone who doesn't hold a live claim frees nothing */
	apply1(w, &t, tix_op_release());
	sync_both(&a, &b);
	ea = loadt(&a, &t);
	TK_CHECK(ea && tix_oid_eq(&ea->u.ticket.claim.identity, &l->me));
	tix_entity_free(ea);

	/* the loser-yields protocol: whichever of two concurrent claims replays first holds,
	 * the other waits; when the holder releases, the waiting one takes over */
	tix_oid t2 = new_ticket(&a, "T2", NULL);
	sync_both(&a, &b);
	apply1(&a, &t2, tix_op_claim(far));
	apply1(&b, &t2, tix_op_claim(far));
	sync_both(&a, &b);
	ea = loadt(&a, &t2);
	TK_CHECK(ea && ea->u.ticket.nwaiting == 1);
	tix_oid holder = ea->u.ticket.claim.identity, waiter = ea->u.ticket.waiting[0].identity;
	TK_CHECK(!tix_oid_eq(&holder, &waiter));
	tix_entity_free(ea);
	tix_sim_clone *hc = tix_oid_eq(&holder, &a.me) ? &a : &b;
	apply1(hc, &t2, tix_op_release());
	sync_both(&a, &b);
	ea = loadt(&a, &t2);
	eb = loadt(&b, &t2);
	TK_CHECK(ea && eb && tix_oid_eq(&ea->u.ticket.claim.identity, &waiter) && ea->u.ticket.nwaiting == 0 &&
		 tix_oid_eq(&eb->u.ticket.claim.identity, &waiter));
	tix_entity_free(ea);
	tix_entity_free(eb);
	/* the waiter, had it yielded instead, would leave the holder alone */
	tix_oid t3 = new_ticket(&a, "T3", NULL);
	sync_both(&a, &b);
	apply1(&a, &t3, tix_op_claim(far));
	apply1(&b, &t3, tix_op_claim(far));
	sync_both(&a, &b);
	ea = loadt(&a, &t3);
	holder = ea->u.ticket.claim.identity;
	waiter = ea->u.ticket.waiting[0].identity;
	tix_entity_free(ea);
	apply1(tix_oid_eq(&waiter, &a.me) ? &a : &b, &t3, tix_op_release());
	sync_both(&a, &b);
	ea = loadt(&b, &t3);
	TK_CHECK(ea && tix_oid_eq(&ea->u.ticket.claim.identity, &holder) && ea->u.ticket.nwaiting == 0);
	tix_entity_free(ea);
	sync_both(&a, &b);
	ea = loadt(&a, &t);
	eb = loadt(&b, &t);
	tix_buf sa = TIX_BUF_INIT, sb = TIX_BUF_INIT;
	if (ea && eb) {
		tix_entity_snapshot(ea, &sa);
		tix_entity_snapshot(eb, &sb);
		TK_STR_EQ(tix_buf_cstr(&sa), tix_buf_cstr(&sb));
	}
	tix_entity_free(ea);
	tix_entity_free(eb);
	tix_buf_free(&sa);
	tix_buf_free(&sb);
	tix_sim_clone_free(&a);
	tix_sim_clone_free(&b);
}

/* §15.4: typed comments join the comment list; item states fold from later updates. */
static void test_typed_comments(void)
{
	tix_sim_clone a, b;
	new_clone(&a, "a", "Ada");
	new_clone(&b, "b", "Bob");
	tix_oid t = new_ticket(&a, "T", NULL);
	apply1(&a, &t, tix_op_text(TIX_OP_COMMENT, "plain"));
	apply1(&a, &t, tix_op_typed_comment("review", "Findings",
					    "{\"items\":[{\"id\":\"f1\",\"text\":\"a\",\"state\":\"open\"},"
					    "{\"id\":\"f2\",\"text\":\"b\",\"state\":\"open\"}]}", NULL));
	tix_entity *x = loadt(&a, &t);
	TK_CHECK(x && x->u.ticket.ncomments == 2);
	tix_opid rid = x->u.ticket.comments[1].id;
	TK_CHECK(!x->u.ticket.comments[0].kind && x->u.ticket.comments[1].kind &&
		 !strcmp(x->u.ticket.comments[1].kind, "review"));
	tix_entity_free(x);
	char ref[200], opid[TIX_OID_MAX_HEX + 12], data[400];
	snprintf(ref, sizeof(ref), "%s#f1", tix_opid_str(&rid, opid));
	snprintf(data, sizeof(data), "{\"updates\":[{\"ref\":\"%s\",\"state\":\"addressed\"}]}", ref);
	sync_both(&a, &b);
	apply1(&b, &t, tix_op_typed_comment("fix", "fixed in abc", data, &rid));
	sync_both(&a, &b);
	x = loadt(&a, &t);
	if (x && x->u.ticket.ncomments == 3) {
		const tix_ticket *tk = &x->u.ticket;
		char *s1 = tix_note_item_state(tk, &tk->comments[1], "f1");
		char *s2 = tix_note_item_state(tk, &tk->comments[1], "f2");
		char *s3 = tix_note_item_state(tk, &tk->comments[1], "nope");
		TK_CHECK(s1 && !strcmp(s1, "addressed") && s2 && !strcmp(s2, "open") && !s3);
		free(s1);
		free(s2);
		TK_CHECK(!tix_opid_cmp(&tk->comments[2].reply_to, &rid));
		/* the codec round-trips typed comments */
		tix_buf enc = TIX_BUF_INIT, s_a = TIX_BUF_INIT, s_b = TIX_BUF_INIT;
		tix_entity *dec = NULL;
		tix_entity_encode(x, &enc);
		TK_OK(tix_entity_decode(enc.data, enc.len, TIX_HASH_SHA1, &dec, &e), &e);
		tix_entity_snapshot(x, &s_a);
		if (dec)
			tix_entity_snapshot(dec, &s_b);
		TK_STR_EQ(tix_buf_cstr(&s_a), tix_buf_cstr(&s_b));
		tix_entity_free(dec);
		tix_buf_free(&enc);
		tix_buf_free(&s_a);
		tix_buf_free(&s_b);
	} else {
		TK_CHECK(!"three comments");
	}
	tix_entity_free(x);
	/* an edit of a typed comment changes its body only, and only by its author */
	apply1(&b, &t, tix_op_edit_comment(&rid, "Bob's rewrite"));
	apply1(&a, &t, tix_op_edit_comment(&rid, "Findings, edited"));
	sync_both(&a, &b);
	x = loadt(&b, &t);
	TK_CHECK(x && !strcmp(x->u.ticket.comments[1].body, "Findings, edited") && x->u.ticket.comments[1].data);
	tix_entity_free(x);
	tix_sim_clone_free(&a);
	tix_sim_clone_free(&b);
}

static void test_resource_and_calendar(void)
{
	tix_sim_clone a, b;
	char nonce[TIX_NONCE_HEX + 1];
	tix_oid r, c;
	tix_date d;
	TIX_REQUIRE(tix_date_parse("2026-12-24", &d));
	new_clone(&a, "a", "Ada");
	new_clone(&b, "b", "Bob");
	tix_op op = tix_op_resource_create(tix_sim_nonce(&a, nonce), "Ada", TIX_RES_PERSON);
	TK_OK(tix_sim_create(&a, TIX_ENTITY_RESOURCE, &op, 1, &r, &e), &e);
	tix_op_free(&op);
	op = tix_op_calendar_create(tix_sim_nonce(&a, nonce), "Office");
	TK_OK(tix_sim_create(&a, TIX_ENTITY_CALENDAR, &op, 1, &c, &e), &e);
	tix_op_free(&op);

	tix_entity *x = load(&a, TIX_ENTITY_RESOURCE, &r);
	TK_CHECK(x && !strcmp(x->u.resource.name, "Ada") && x->u.resource.capacity == 100 && x->u.resource.active &&
		 x->u.resource.rate == -1 && !x->u.resource.currency);
	tix_entity_free(x);
	x = load(&a, TIX_ENTITY_CALENDAR, &c);
	TK_CHECK(x && x->u.calendar.weekdays == TIX_WEEKDAYS_MON_FRI && x->u.calendar.hours == 8 &&
		 !x->u.calendar.nexceptions);
	tix_entity_free(x);

	tix_op rops[] = {tix_op_set(TIX_F_CAPACITY, tix_value_int(TIX_V_INT, 50)),
			 tix_op_set(TIX_F_RES_CALENDAR, tix_value_id(&c)),
			 tix_op_set(TIX_F_RES_NAME, tix_value_str("Ada L."))};
	TK_OK(tix_sim_apply(&a, TIX_ENTITY_RESOURCE, &r, rops, 3, &e), &e);
	for (int i = 0; i < 3; i++)
		tix_op_free(&rops[i]);
	op = tix_op_exception_add(d, d + 2, false, "Holidays");
	TK_OK(tix_sim_apply(&a, TIX_ENTITY_CALENDAR, &c, &op, 1, &e), &e);
	tix_op_free(&op);
	sync_both(&a, &b);
	/* concurrent: a removes the holiday, b re-adds it as working days */
	op = tix_op_exception_remove(d, d + 2);
	TK_OK(tix_sim_apply(&a, TIX_ENTITY_CALENDAR, &c, &op, 1, &e), &e);
	tix_op_free(&op);
	op = tix_op_exception_add(d, d + 2, true, "On call");
	TK_OK(tix_sim_apply(&b, TIX_ENTITY_CALENDAR, &c, &op, 1, &e), &e);
	tix_op_free(&op);
	op = tix_op_exception_add(d + 7, d + 7, false, "");
	TK_OK(tix_sim_apply(&b, TIX_ENTITY_CALENDAR, &c, &op, 1, &e), &e);
	tix_op_free(&op);
	sync_both(&a, &b);

	x = load(&a, TIX_ENTITY_RESOURCE, &r);
	TK_CHECK(x && !strcmp(x->u.resource.name, "Ada L.") && x->u.resource.capacity == 50 &&
		 tix_oid_eq(&x->u.resource.calendar, &c));
	tix_entity_free(x);
	x = load(&b, TIX_ENTITY_CALENDAR, &c);
	TK_CHECK(x && x->u.calendar.nexceptions == 2);
	if (x && x->u.calendar.nexceptions == 2) {
		TK_CHECK(x->u.calendar.exceptions[0].from == d && x->u.calendar.exceptions[0].working &&
			 !strcmp(x->u.calendar.exceptions[0].note, "On call"));
		TK_CHECK(x->u.calendar.exceptions[1].from == d + 7 && !x->u.calendar.exceptions[1].working);
	}
	tix_entity_free(x);

	/* resource packs need an author, like tickets */
	tix_sim_clone_free(&a);
	tix_sim_clone_free(&b);
}

static void test_rank(void)
{
	tix_buf b = TIX_BUF_INIT;
	TK_OK(tix_rank_between(NULL, NULL, &b, &e), &e);
	TK_STR_EQ(tix_buf_cstr(&b), "i");
	/* repeated insertion at the front, back and middle stays ordered and valid */
	char lo[TIX_RANK_MAX + 1] = "i", hi[TIX_RANK_MAX + 1] = "i";
	for (int i = 0; i < 40; i++) {
		tix_buf_reset(&b);
		TK_OK(tix_rank_between(NULL, lo, &b, &e), &e);
		TK_CHECK(strcmp(b.data, lo) < 0 && tix_rank_valid(b.data));
		snprintf(lo, sizeof(lo), "%s", b.data);
		tix_buf_reset(&b);
		TK_OK(tix_rank_between(hi, NULL, &b, &e), &e);
		TK_CHECK(strcmp(b.data, hi) > 0 && tix_rank_valid(b.data));
		snprintf(hi, sizeof(hi), "%s", b.data);
	}
	char a[TIX_RANK_MAX + 1] = "a", z[TIX_RANK_MAX + 1] = "b";
	tix_status st = TIX_OK;
	int steps = 0;
	while (!st && steps < 1000) { /* halve the gap until the length limit says stop */
		tix_buf_reset(&b);
		st = tix_rank_between(a, z, &b, &e);
		if (!st) {
			TK_CHECK(strcmp(a, b.data) < 0 && strcmp(b.data, z) < 0 && tix_rank_valid(b.data));
			snprintf(steps % 2 ? a : z, TIX_RANK_MAX + 1, "%s", b.data);
		}
		steps++;
	}
	TK_CHECK(st == TIX_ERR_CONFLICT && steps > 60);
	const char *pairs[][2] = {{"a", "a1"}, {"0i", "1"}, {"az", "b"}, {"y", "z"}, {"zz", NULL}, {NULL, "01"}};
	for (size_t i = 0; i < sizeof(pairs) / sizeof(*pairs); i++) {
		tix_buf_reset(&b);
		TK_OK(tix_rank_between(pairs[i][0], pairs[i][1], &b, &e), &e);
		TK_CHECK((!pairs[i][0] || strcmp(pairs[i][0], b.data) < 0) &&
			 (!pairs[i][1] || strcmp(b.data, pairs[i][1]) < 0));
	}
	TK_STATUS(tix_rank_between("b", "a", &b, &e), TIX_ERR_INVALID, &e);
	TK_STATUS(tix_rank_between("b", "b", &b, &e), TIX_ERR_INVALID, &e);
	TK_STATUS(tix_rank_between("a0", NULL, &b, &e), TIX_ERR_INVALID, &e);
	tix_buf_free(&b);
}

/* Baselines: snapshots that sync; for one ticket the last task op in total order wins,
 * and every clone ends up with the same snapshot after merging. */
static void test_baseline(void)
{
	tix_sim_clone a, b;
	new_clone(&a, "a", "Ada");
	new_clone(&b, "b", "Bob");
	tix_oid t1 = new_ticket(&a, "One", NULL), t2 = new_ticket(&a, "Two", NULL), bl;
	tix_date d1, d2;
	TIX_REQUIRE(tix_date_parse("2026-11-09", &d1) && tix_date_parse("2026-11-12", &d2));
	char nonce[TIX_NONCE_HEX + 1];
	tix_op ops[] = {
		tix_op_baseline_create(tix_sim_nonce(&a, nonce), "v1", d1, NULL),
		tix_op_baseline_task(&t2, d1, d2, 4, 32, 1000, "EUR"),
		tix_op_baseline_task(&t1, d1, d1, 1, -1, -1, NULL),
	};
	TK_OK(tix_sim_create(&a, TIX_ENTITY_BASELINE, ops, 3, &bl, &e), &e);
	for (int i = 0; i < 3; i++)
		tix_op_free(&ops[i]);
	sync_both(&a, &b);
	/* concurrently: a corrects t1, b renames; then a later correction from b wins for t1 */
	tix_op fix = tix_op_baseline_task(&t1, d1, d2, 4, -1, -1, NULL);
	TK_OK(tix_sim_apply(&a, TIX_ENTITY_BASELINE, &bl, &fix, 1, &e), &e);
	tix_op_free(&fix);
	tix_op ren = tix_op_text(TIX_OP_BASELINE_SET_NAME, "v1 (approved)");
	TK_OK(tix_sim_apply(&b, TIX_ENTITY_BASELINE, &bl, &ren, 1, &e), &e);
	tix_op_free(&ren);
	tix_op later = tix_op_baseline_task(&t1, d2, d2, 1, -1, -1, NULL);
	TK_OK(tix_sim_apply(&b, TIX_ENTITY_BASELINE, &bl, &later, 1, &e), &e);
	tix_op_free(&later);
	sync_both(&a, &b);
	tix_entity *ea = load(&a, TIX_ENTITY_BASELINE, &bl), *eb = load(&b, TIX_ENTITY_BASELINE, &bl);
	TK_CHECK(ea && eb);
	if (ea && eb) {
		const tix_baseline *x = &ea->u.baseline;
		TK_STR_EQ(x->name, "v1 (approved)");
		TK_CHECK(x->taken == d1 && tix_oid_is_null(&x->root) && x->ntasks == 2);
		const tix_baseline_task *k1 = tix_baseline_find(x, &t1), *k2 = tix_baseline_find(x, &t2);
		TK_CHECK(k1 && k1->start == d2 && k1->duration == 1); /* b's later op */
		TK_CHECK(k2 && k2->cost == 1000 && !strcmp(k2->currency, "EUR") && k2->effort == 32);
		tix_buf sa = TIX_BUF_INIT, sb = TIX_BUF_INIT;
		tix_entity_snapshot(ea, &sa);
		tix_entity_snapshot(eb, &sb);
		TK_STR_EQ(tix_buf_cstr(&sa), tix_buf_cstr(&sb));
		/* the cache codec round-trips it */
		tix_buf enc = TIX_BUF_INIT, sc = TIX_BUF_INIT;
		tix_entity *dec = NULL;
		tix_entity_encode(ea, &enc);
		TK_OK(tix_entity_decode(enc.data, enc.len, TIX_HASH_SHA1, &dec, &e), &e);
		if (dec)
			tix_entity_snapshot(dec, &sc);
		TK_STR_EQ(tix_buf_cstr(&sc), tix_buf_cstr(&sa));
		tix_entity_free(dec);
		tix_buf_free(&enc);
		tix_buf_free(&sc);
		tix_buf_free(&sa);
		tix_buf_free(&sb);
	}
	tix_entity_free(ea);
	tix_entity_free(eb);
	tix_sim_clone_free(&a);
	tix_sim_clone_free(&b);
}

int main(void)
{
	test_typed_comments();
	test_claims();
	test_stages();
	test_baseline();
	test_lifecycle();
	test_concurrent_lww();
	test_or_set();
	test_edit_needs_same_author();
	test_identity_rules();
	test_dag_validation();
	test_planning_fields();
	test_resource_and_calendar();
	test_rank();
	return tk_finish();
}
