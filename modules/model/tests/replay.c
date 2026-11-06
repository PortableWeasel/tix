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

int main(void)
{
	test_lifecycle();
	test_concurrent_lww();
	test_or_set();
	test_edit_needs_same_author();
	test_identity_rules();
	test_dag_validation();
	return tk_finish();
}
