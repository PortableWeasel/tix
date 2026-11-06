/* core on store_mem: identity flow, writes, resolve, retries, validation, queries. */
#include "tix/core.h"
#include "tix/store_mem.h"
#include "tix/testkit.h"

#include <stdlib.h>

static tix_error e = TIX_ERROR_INIT;

typedef struct fake_env {
	int64_t now;
	uint64_t rng;
} fake_env;

static int64_t fake_now(void *ctx)
{
	return ((fake_env *)ctx)->now += 60;
}

static void fake_random(void *ctx, uint8_t *out, size_t n)
{
	fake_env *f = ctx;
	for (size_t i = 0; i < n; i++)
		out[i] = (uint8_t)tk_rand(&f->rng);
}

/* A store wrapper whose ref_cas reports CONFLICT for the next `fail` calls. */
typedef struct flaky {
	tix_store base;
	tix_store *inner;
	int fail, cas_calls;
} flaky;

static tix_store *inner(tix_store *s) { return ((flaky *)s)->inner; }
static tix_status f_cw(tix_store *s, const tix_oid *p, size_t np, const char *pk, size_t len, const char *m,
		       tix_oid *o, tix_error *er) { return tix_store_commit_write(inner(s), p, np, pk, len, m, o, er); }
static tix_status f_cr(tix_store *s, const tix_oid *o, tix_commit *c, tix_error *er) { return tix_store_commit_read(inner(s), o, c, er); }
static tix_status f_rr(tix_store *s, const char *n, tix_oid *o, tix_error *er) { return tix_store_ref_read(inner(s), n, o, er); }
static tix_status f_rl(tix_store *s, const char *p, tix_refs *o, tix_error *er) { return tix_store_ref_list(inner(s), p, o, er); }
static tix_status f_lg(tix_store *s, const char *n, tix_buf *o, tix_error *er) { return tix_store_local_get(inner(s), n, o, er); }
static tix_status f_ls(tix_store *s, const char *n, const char *d, size_t l, tix_error *er) { return tix_store_local_set(inner(s), n, d, l, er); }
static tix_status f_cas(tix_store *s, const char *n, const tix_oid *old, const tix_oid *new_oid, tix_error *er)
{
	flaky *f = (flaky *)s;
	f->cas_calls++;
	if (f->fail > 0) {
		f->fail--;
		return tix_error_set(er, TIX_ERR_CONFLICT, "simulated concurrent writer");
	}
	return tix_store_ref_cas(f->inner, n, old, new_oid, er);
}
static void f_destroy(tix_store *s) { (void)s; }
static const tix_store_vt flaky_vt = {f_cw, f_cr, f_rr, f_rl, f_cas, f_lg, f_ls, f_destroy};

static tix_repo *open_repo(tix_store *s, fake_env *env, const tix_oid *identity)
{
	tix_repo_config cfg = {s, {fake_now, fake_random, env}, {0}};
	tix_repo *r = NULL;
	if (identity)
		cfg.identity = *identity;
	TK_OK(tix_repo_open(&r, &cfg, &e), &e);
	return r;
}

static tix_entity *load_ticket(tix_repo *r, const tix_oid *id)
{
	tix_entity *t = NULL;
	TK_OK(tix_entity_load(r, TIX_ENTITY_TICKET, id, &t, &e), &e);
	return t;
}

static void test_flow(void)
{
	fake_env env = {1700000000, 1};
	tix_store *s = tix_store_mem_new(TIX_HASH_SHA1);
	tix_repo *r = open_repo(s, &env, NULL);
	tix_oid me, t1, t2, got;
	tix_strs labels = {0};
	tix_strs_add(&labels, "bug");

	TK_CHECK(!tix_repo_identity(r, &got));
	TK_STATUS(tix_ticket_create(r, "x", "", NULL, &t1, &e), TIX_ERR_NOT_FOUND, &e);
	TK_OK(tix_identity_create(r, "Ada", "ada@example.org", &me, &e), &e);
	TK_OK(tix_repo_set_identity(r, &me, &e), &e);
	TK_CHECK(tix_repo_identity(r, &got) && tix_oid_eq(&got, &me));

	TK_OK(tix_ticket_create(r, "Same", "", &labels, &t1, &e), &e);
	TK_OK(tix_ticket_create(r, "Same", "", &labels, &t2, &e), &e);
	TK_CHECK(!tix_oid_eq(&t1, &t2)); /* nonces keep identical creates apart */

	tix_op ops[2] = {tix_op_text(TIX_OP_SET_TITLE, "Renamed"), tix_op_text(TIX_OP_COMMENT, "hi")};
	TK_OK(tix_entity_apply(r, TIX_ENTITY_TICKET, &t1, ops, 2, &e), &e);
	tix_op_free(&ops[0]);
	tix_op_free(&ops[1]);
	tix_entity *t = load_ticket(r, &t1);
	if (t) {
		TK_STR_EQ(t->u.ticket.title, "Renamed");
		TK_CHECK(t->u.ticket.ncomments == 1 && tix_oid_eq(&t->u.ticket.comments[0].author, &me));
		TK_CHECK(t->max_lamport == 4); /* identity 1, tickets 2 and 3, edit 4 */
	}
	tix_entity_free(t);

	/* comment edits: own comment fine, unknown comment refused */
	t = load_ticket(r, &t1);
	tix_opid cid = t->u.ticket.comments[0].id, bogus = cid;
	tix_entity_free(t);
	bogus.line = 99;
	tix_op ed = tix_op_edit_comment(&cid, "hi!"), bad = tix_op_edit_comment(&bogus, "x");
	TK_OK(tix_entity_apply(r, TIX_ENTITY_TICKET, &t1, &ed, 1, &e), &e);
	TK_STATUS(tix_entity_apply(r, TIX_ENTITY_TICKET, &t1, &bad, 1, &e), TIX_ERR_NOT_FOUND, &e);
	tix_op_free(&bad);

	/* clock persisted in local state and reloaded */
	tix_buf b = TIX_BUF_INIT;
	TK_OK(tix_store_local_get(s, "clock", &b, &e), &e);
	TK_STR_EQ(tix_buf_cstr(&b), "5\n");
	tix_repo_close(r);
	r = open_repo(s, &env, &me);
	TK_OK(tix_ticket_create(r, "Later", "", NULL, &t2, &e), &e);
	t = load_ticket(r, &t2);
	TK_CHECK(t && t->max_lamport == 6);
	tix_entity_free(t);

	/* rule violations */
	tix_op create = tix_op_ticket_create(NULL, "x", "", NULL), title = tix_op_text(TIX_OP_SET_TITLE, "y");
	TK_STATUS(tix_entity_apply(r, TIX_ENTITY_TICKET, &t1, &create, 1, &e), TIX_ERR_INVALID, &e);
	TK_STATUS(tix_entity_create(r, TIX_ENTITY_TICKET, &title, 1, &got, &e), TIX_ERR_INVALID, &e);
	tix_op_free(&create);
	tix_op_free(&title);
	tix_oid other;
	TK_OK(tix_identity_create(r, "Bob", "bob@example.org", &other, &e), &e);
	tix_op rename = tix_op_text(TIX_OP_SET_NAME, "Mallory");
	TK_STATUS(tix_entity_apply(r, TIX_ENTITY_IDENTITY, &other, &rename, 1, &e), TIX_ERR_INVALID, &e);
	TK_OK(tix_entity_apply(r, TIX_ENTITY_IDENTITY, &me, &rename, 1, &e), &e); /* own: fine */
	tix_op_free(&rename);

	/* another identity may not edit Ada's comment */
	TK_OK(tix_repo_set_identity(r, &other, &e), &e);
	TK_STATUS(tix_entity_apply(r, TIX_ENTITY_TICKET, &t1, &ed, 1, &e), TIX_ERR_INVALID, &e);
	tix_op_free(&ed);
	tix_op st = tix_op_set_status(TIX_TICKET_DONE);
	TK_STATUS(tix_entity_apply(r, TIX_ENTITY_TICKET, &other, &st, 1, &e), TIX_ERR_NOT_FOUND, &e); /* no such ticket */
	tix_op_free(&st);

	tix_buf_free(&b);
	tix_strs_free(&labels);
	tix_repo_close(r);
	tix_store_destroy(s);
}

static void test_resolve(void)
{
	fake_env env = {1700000000, 2};
	tix_store *s = tix_store_mem_new(TIX_HASH_SHA1);
	tix_repo *r = open_repo(s, &env, NULL);
	tix_oid me, t, got;
	char hex[TIX_OID_MAX_HEX + 1];
	TK_OK(tix_identity_create(r, "Ada", "a@x", &me, &e), &e);
	TK_OK(tix_repo_set_identity(r, &me, &e), &e);
	TK_OK(tix_ticket_create(r, "T", "", NULL, &t, &e), &e);
	tix_oid_hex(&t, hex);

	TK_OK(tix_entity_resolve(r, TIX_ENTITY_TICKET, hex, &got, &e), &e);
	TK_CHECK(tix_oid_eq(&got, &t));
	hex[7] = 0;
	TK_OK(tix_entity_resolve(r, TIX_ENTITY_TICKET, hex, &got, &e), &e);
	TK_CHECK(tix_oid_eq(&got, &t));
	hex[6] = 0;
	TK_STATUS(tix_entity_resolve(r, TIX_ENTITY_TICKET, hex, &got, &e), TIX_ERR_INVALID, &e);
	TK_STATUS(tix_entity_resolve(r, TIX_ENTITY_TICKET, "ABCDEF12", &got, &e), TIX_ERR_INVALID, &e);
	TK_STATUS(tix_entity_resolve(r, TIX_ENTITY_IDENTITY, tix_oid_hex(&t, hex), &got, &e), TIX_ERR_NOT_FOUND, &e);

	/* two ids sharing a prefix (refs made by hand: only names matter to resolve) */
	TK_OK(tix_store_ref_cas(s, "refs/tix/tickets/abcdef1000000000000000000000000000000000", NULL, &t, &e), &e);
	TK_OK(tix_store_ref_cas(s, "refs/tix/tickets/abcdef1999999999999999999999999999999999", NULL, &t, &e), &e);
	TK_STATUS(tix_entity_resolve(r, TIX_ENTITY_TICKET, "abcdef1", &got, &e), TIX_ERR_AMBIGUOUS, &e);
	TK_OK(tix_entity_resolve(r, TIX_ENTITY_TICKET, "abcdef10", &got, &e), &e);

	/* ...and those refs point at a history whose root is another id */
	TK_STATUS(tix_entity_load(r, TIX_ENTITY_TICKET, &got, &(tix_entity *){0}, &e), TIX_ERR_INVALID, &e);
	tix_entity_list l;
	TK_OK(tix_ticket_list(r, NULL, &l, &e), &e);
	TK_CHECK(l.n == 1 && l.nbroken == 2);
	tix_entity_list_free(&l);

	tix_repo_close(r);
	tix_store_destroy(s);
}

static void test_retry(void)
{
	fake_env env = {1700000000, 3};
	flaky f = {{&flaky_vt, TIX_HASH_SHA1}, tix_store_mem_new(TIX_HASH_SHA1), 0, 0};
	tix_repo *r = open_repo(&f.base, &env, NULL);
	tix_oid me, t;
	TK_OK(tix_identity_create(r, "Ada", "a@x", &me, &e), &e);
	TK_OK(tix_repo_set_identity(r, &me, &e), &e);
	TK_OK(tix_ticket_create(r, "T", "", NULL, &t, &e), &e);

	tix_op op = tix_op_set_status(TIX_TICKET_DONE);
	f.fail = 2;
	f.cas_calls = 0;
	TK_OK(tix_entity_apply(r, TIX_ENTITY_TICKET, &t, &op, 1, &e), &e);
	TK_CHECK(f.cas_calls == 3);
	tix_entity *ent = load_ticket(r, &t);
	TK_CHECK(ent && ent->u.ticket.status == TIX_TICKET_DONE);
	tix_entity_free(ent);

	f.fail = 100;
	TK_STATUS(tix_entity_apply(r, TIX_ENTITY_TICKET, &t, &op, 1, &e), TIX_ERR_CONFLICT, &e);
	tix_op_free(&op);
	tix_repo_close(r);
	tix_store_destroy(f.inner);
}

static void test_list_and_validation(void)
{
	fake_env env = {1700000000, 4};
	tix_store *s = tix_store_mem_new(TIX_HASH_SHA256);
	tix_repo *r = open_repo(s, &env, NULL);
	tix_oid me, bob, a, b, c;
	tix_strs ui = {0};
	tix_strs_add(&ui, "ui");
	TK_OK(tix_identity_create(r, "Ada", "a@x", &me, &e), &e);
	TK_OK(tix_identity_create(r, "Bob", "b@x", &bob, &e), &e);
	TK_OK(tix_repo_set_identity(r, &me, &e), &e);
	TK_OK(tix_ticket_create(r, "A", "", &ui, &a, &e), &e);
	TK_OK(tix_ticket_create(r, "B", "", NULL, &b, &e), &e);
	TK_OK(tix_repo_set_identity(r, &bob, &e), &e);
	TK_OK(tix_ticket_create(r, "C", "", &ui, &c, &e), &e);
	tix_op done = tix_op_set_status(TIX_TICKET_DONE);
	TK_OK(tix_entity_apply(r, TIX_ENTITY_TICKET, &b, &done, 1, &e), &e);
	tix_op_free(&done);

	tix_entity_list l;
	TK_OK(tix_ticket_list(r, NULL, &l, &e), &e);
	TK_CHECK(l.n == 3 && l.nbroken == 0);
	if (l.n == 3) /* newest first */
		TK_CHECK(tix_oid_eq(&l.v[0]->id, &c) && tix_oid_eq(&l.v[2]->id, &a));
	tix_entity_list_free(&l);

	tix_ticket_query q = {TIX_STATUS_MASK_ACTIVE, NULL, NULL};
	TK_OK(tix_ticket_list(r, &q, &l, &e), &e);
	TK_CHECK(l.n == 2);
	tix_entity_list_free(&l);
	q = (tix_ticket_query){0, "ui", &me};
	TK_OK(tix_ticket_list(r, &q, &l, &e), &e);
	TK_CHECK(l.n == 1 && tix_oid_eq(&l.v[0]->id, &a));
	tix_entity_list_free(&l);
	TK_OK(tix_identity_list(r, &l, &e), &e);
	TK_CHECK(l.n == 2 && !strcmp(l.v[0]->u.identity.name, "Ada"));
	tix_entity_list_free(&l);

	/* a ticket ref pointing at an identity's history is skipped and reported */
	char ref[160], hex[TIX_OID_MAX_HEX + 1];
	snprintf(ref, sizeof(ref), "refs/tix/tickets/%s", tix_oid_hex(&me, hex));
	TK_OK(tix_store_ref_cas(s, ref, NULL, &me, &e), &e);
	TK_OK(tix_ticket_list(r, NULL, &l, &e), &e);
	TK_CHECK(l.n == 3 && l.nbroken == 1);
	tix_entity_list_free(&l);
	tix_repo_close(r);

	/* corrupt clock refuses to open; unknown identity refuses to open */
	TK_OK(tix_store_local_set(s, "clock", "12x\n", 4, &e), &e);
	tix_repo_config cfg = {s, {fake_now, fake_random, &env}, {0}};
	TK_STATUS(tix_repo_open(&r, &cfg, &e), TIX_ERR_INVALID, &e);
	TK_OK(tix_store_local_set(s, "clock", "12\n", 3, &e), &e);
	cfg.identity = a; /* a ticket id, not an identity */
	TK_STATUS(tix_repo_open(&r, &cfg, &e), TIX_ERR_NOT_FOUND, &e);
	tix_strs_free(&ui);
	tix_store_destroy(s);
}

int main(void)
{
	test_flow();
	test_resolve();
	test_retry();
	test_list_and_validation();
	return tk_finish();
}
