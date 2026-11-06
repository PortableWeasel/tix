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
static tix_status f_rf(tix_store *s, const char *r, tix_error *er) { return tix_store_remote_fetch(inner(s), r, er); }
static tix_status f_rp(tix_store *s, const char *r, const char *const *refs, size_t n, tix_error *er) { return tix_store_remote_push(inner(s), r, refs, n, er); }
static void f_destroy(tix_store *s) { (void)s; }
static const tix_store_vt flaky_vt = {f_cw, f_cr, f_rr, f_rl, f_cas, f_lg, f_ls, f_rf, f_rp, f_destroy};

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

/* ---- sync --------------------------------------------------------------- */

typedef struct clone {
	fake_env env;
	tix_store *s;
	tix_repo *r;
} clone;

static void clone_open(clone *c, tix_store *server, uint64_t seed)
{
	c->env = (fake_env){1700000000, seed};
	c->s = tix_store_mem_new(server->algo);
	tix_store_mem_add_remote(c->s, "origin", server);
	c->r = open_repo(c->s, &c->env, NULL);
}

static void clone_close(clone *c)
{
	tix_repo_close(c->r);
	tix_store_destroy(c->s);
}

static void snapshot(clone *c, const tix_oid *t, tix_buf *out)
{
	tix_entity *ent = load_ticket(c->r, t);
	tix_buf_reset(out);
	if (ent)
		tix_entity_snapshot(ent, out);
	tix_entity_free(ent);
}

static void apply1(clone *c, const tix_oid *t, tix_op op)
{
	TK_OK(tix_entity_apply(c->r, TIX_ENTITY_TICKET, t, &op, 1, &e), &e);
	tix_op_free(&op);
}

static void test_sync_basic(void)
{
	tix_store *server = tix_store_mem_new(TIX_HASH_SHA1);
	clone a, b;
	tix_oid ada, bob, t;
	tix_sync_report rep = {0};
	tix_buf sa = TIX_BUF_INIT, sb = TIX_BUF_INIT;
	clone_open(&a, server, 11);
	clone_open(&b, server, 12);

	TK_OK(tix_identity_create(a.r, "Ada", "a@x", &ada, &e), &e);
	TK_OK(tix_repo_set_identity(a.r, &ada, &e), &e);
	TK_OK(tix_ticket_create(a.r, "Shared", "", NULL, &t, &e), &e);
	TK_OK(tix_push(a.r, "origin", &rep, &e), &e);
	TK_CHECK(rep.pushed == 2);
	TK_OK(tix_push(a.r, "origin", &rep, &e), &e);
	TK_CHECK(rep.pushed == 0); /* nothing new since */
	tix_sync_report_free(&rep);

	TK_OK(tix_pull(b.r, "origin", &rep, &e), &e);
	TK_CHECK(rep.created == 2 && rep.rejected == 0 && rep.merged == 0);
	tix_sync_report_free(&rep);
	TK_OK(tix_identity_create(b.r, "Bob", "b@x", &bob, &e), &e);
	TK_OK(tix_repo_set_identity(b.r, &bob, &e), &e);

	/* concurrent edits on both clones */
	apply1(&a, &t, tix_op_text(TIX_OP_SET_TITLE, "Shared, renamed"));
	tix_strs l = {0};
	tix_strs_add(&l, "bug");
	apply1(&b, &t, tix_op_label(&l, NULL));
	tix_strs_free(&l);
	TK_OK(tix_sync(a.r, "origin", &rep, &e), &e);
	tix_sync_report_free(&rep);

	/* b's push is refused (the server moved), sync pulls, merges, pushes */
	TK_STATUS(tix_push(b.r, "origin", &rep, &e), TIX_ERR_CONFLICT, &e);
	tix_sync_report_free(&rep);
	TK_OK(tix_sync(b.r, "origin", &rep, &e), &e);
	TK_CHECK(rep.merged == 1 && rep.pushed >= 1 && rep.problems.n == 0);
	tix_sync_report_free(&rep);
	TK_OK(tix_pull(a.r, "origin", &rep, &e), &e);
	TK_CHECK(rep.forwarded >= 1 && rep.merged == 0);
	tix_sync_report_free(&rep);

	snapshot(&a, &t, &sa);
	snapshot(&b, &t, &sb);
	TK_STR_EQ(tix_buf_cstr(&sa), tix_buf_cstr(&sb));
	tix_entity *ent = load_ticket(a.r, &t);
	TK_CHECK(ent && !strcmp(ent->u.ticket.title, "Shared, renamed") &&
		 tix_strs_contains(&ent->u.ticket.labels, "bug"));
	/* the clock moved past everything adopted: a new edit sorts last */
	uint64_t before = ent ? ent->max_lamport : 0;
	tix_entity_free(ent);
	apply1(&b, &t, tix_op_set_status(TIX_TICKET_DONE));
	ent = load_ticket(b.r, &t);
	TK_CHECK(ent && ent->max_lamport > before && ent->u.ticket.status == TIX_TICKET_DONE);
	tix_entity_free(ent);

	tix_buf_free(&sa);
	tix_buf_free(&sb);
	clone_close(&a);
	clone_close(&b);
	tix_store_destroy(server);
}

/* What a hostile or broken remote can send, and what pull does with it. */
static void test_sync_hostile(void)
{
	tix_store *server = tix_store_mem_new(TIX_HASH_SHA1);
	clone a, b;
	tix_oid ada, t, junk, got;
	tix_sync_report rep = {0};
	char ref[160], hex[TIX_OID_MAX_HEX + 1];
	clone_open(&a, server, 21);
	clone_open(&b, server, 22);
	TK_OK(tix_identity_create(a.r, "Ada", "a@x", &ada, &e), &e);
	TK_OK(tix_repo_set_identity(a.r, &ada, &e), &e);
	TK_OK(tix_ticket_create(a.r, "Fine", "", NULL, &t, &e), &e);
	TK_OK(tix_sync(a.r, "origin", &rep, &e), &e);
	tix_sync_report_free(&rep);
	TK_OK(tix_pull(b.r, "origin", &rep, &e), &e);
	tix_sync_report_free(&rep);

	/* 1: a malformed pack under a fresh ticket name */
	const char *bad = "{\"tix\":1,\"type\":\"ticket\",\"author\":null,\"lamport\":1,\"time\":0}\n{\"op\":\"create\"}\n";
	TK_OK(tix_store_commit_write(server, NULL, 0, bad, strlen(bad), "junk", &junk, &e), &e);
	snprintf(ref, sizeof(ref), "refs/tix/tickets/%s", tix_oid_hex(&junk, hex));
	TK_OK(tix_store_ref_cas(server, ref, NULL, &junk, &e), &e);
	/* 2: valid history filed under someone else's id */
	tix_oid fake = t;
	fake.bytes[0] ^= 0x55;
	snprintf(ref, sizeof(ref), "refs/tix/tickets/%s", tix_oid_hex(&fake, hex));
	TK_OK(tix_store_ref_cas(server, ref, NULL, &t, &e), &e);
	/* 3: an identity's history under the ticket namespace */
	snprintf(ref, sizeof(ref), "refs/tix/tickets/%s", tix_oid_hex(&ada, hex));
	TK_OK(tix_store_ref_cas(server, ref, NULL, &ada, &e), &e);
	/* 4: a namespace this version doesn't know, and a non-id name */
	TK_OK(tix_store_ref_cas(server, "refs/tix/widgets/x", NULL, &t, &e), &e);
	TK_OK(tix_store_ref_cas(server, "refs/tix/tickets/not-an-id", NULL, &t, &e), &e);
	/* 5: an existing ticket rewound to junk (a forced push by someone else) */
	snprintf(ref, sizeof(ref), "refs/tix/tickets/%s", tix_oid_hex(&t, hex));
	tix_oid head;
	TK_OK(tix_store_ref_read(server, ref, &head, &e), &e);
	TK_OK(tix_store_ref_cas(server, ref, &head, &junk, &e), &e);

	TK_OK(tix_pull(b.r, "origin", &rep, &e), &e);
	TK_CHECK(rep.rejected == 4 && rep.skipped == 2 && rep.created == 0 && rep.problems.n == 4);
	tix_sync_report_free(&rep);
	tix_entity_list l;
	TK_OK(tix_ticket_list(b.r, NULL, &l, &e), &e);
	TK_CHECK(l.n == 1 && l.nbroken == 0); /* nothing hostile was adopted */
	tix_entity_list_free(&l);
	TK_OK(tix_store_ref_read(b.s, ref, &got, &e), &e);
	TK_CHECK(tix_oid_eq(&got, &head)); /* the good local history is untouched */

	/* a's next sync completes: the rewound ref is reported, not retried forever */
	apply1(&a, &t, tix_op_text(TIX_OP_COMMENT, "still here"));
	TK_OK(tix_sync(a.r, "origin", &rep, &e), &e);
	TK_CHECK(rep.rejected == 4 && rep.unpushable == 1 && rep.problems.n == 5);
	tix_sync_report_free(&rep);
	TK_OK(tix_store_ref_read(server, ref, &got, &e), &e);
	TK_CHECK(tix_oid_eq(&got, &junk)); /* never forced over */

	TK_STATUS(tix_pull(b.r, "nowhere", &rep, &e), TIX_ERR_NOT_FOUND, &e);
	TK_STATUS(tix_push(b.r, "--all", &rep, &e), TIX_ERR_INVALID, &e);
	tix_sync_report_free(&rep);
	clone_close(&a);
	clone_close(&b);
	tix_store_destroy(server);
}

/* Random edits on several clones, syncing through one server in random order:
 * once everyone has synced twice, every clone has byte-identical tickets. */
static void test_sync_convergence(void)
{
	enum { CLONES = 3, STEPS = 120, SEEDS = 12 };
	size_t merges = 0;
	for (uint64_t seed = 1; seed <= SEEDS; seed++) {
		tix_store *server = tix_store_mem_new(seed % 3 ? TIX_HASH_SHA1 : TIX_HASH_SHA256);
		clone c[CLONES];
		tix_oid me[CLONES];
		TIX_VEC(tix_oid) tickets = {0};
		uint64_t rng = seed * 7919;
		tix_sync_report rep = {0};
		char name[16];
		for (int i = 0; i < CLONES; i++) {
			clone_open(&c[i], server, seed * 100 + (uint64_t)i);
			snprintf(name, sizeof(name), "user%d", i);
			TK_OK(tix_identity_create(c[i].r, name, "u@x", &me[i], &e), &e);
			TK_OK(tix_repo_set_identity(c[i].r, &me[i], &e), &e);
		}
		for (int step = 0; step < STEPS; step++) {
			clone *k = &c[tk_rand_below(&rng, CLONES)];
			uint64_t what = tk_rand_below(&rng, 10);
			tix_entity_list l;
			TK_OK(tix_ticket_list(k->r, NULL, &l, &e), &e);
			if (what == 0 || l.n == 0) {
				tix_oid t;
				TK_OK(tix_ticket_create(k->r, "new", "", NULL, &t, &e), &e);
			} else if (what <= 6) {
				const tix_oid *t = &l.v[tk_rand_below(&rng, l.n)]->id;
				tix_strs lab = {0};
				snprintf(name, sizeof(name), "l%d", (int)tk_rand_below(&rng, 4));
				tix_strs_add(&lab, name);
				switch (tk_rand_below(&rng, 5)) {
				case 0: apply1(k, t, tix_op_text(TIX_OP_SET_TITLE, name)); break;
				case 1: apply1(k, t, tix_op_set_status((tix_ticket_status)tk_rand_below(&rng, 5))); break;
				case 2: apply1(k, t, tix_op_label(&lab, NULL)); break;
				case 3: apply1(k, t, tix_op_label(NULL, &lab)); break;
				default: apply1(k, t, tix_op_text(TIX_OP_COMMENT, name)); break;
				}
				tix_strs_free(&lab);
			} else if (what <= 7) {
				TK_OK(tix_pull(k->r, "origin", &rep, &e), &e);
			} else {
				TK_OK(tix_sync(k->r, "origin", &rep, &e), &e);
			}
			tix_entity_list_free(&l);
		}
		for (int round = 0; round < 2; round++)
			for (int i = 0; i < CLONES; i++)
				TK_OK(tix_sync(c[i].r, "origin", &rep, &e), &e);
		TK_CHECK(rep.rejected == 0 && rep.problems.n == 0);
		merges += rep.merged;
		tix_sync_report_free(&rep);

		tix_entity_list ref;
		TK_OK(tix_ticket_list(c[0].r, NULL, &ref, &e), &e);
		for (int i = 1; i < CLONES; i++) {
			tix_entity_list l;
			TK_OK(tix_ticket_list(c[i].r, NULL, &l, &e), &e);
			TK_CHECK(l.n == ref.n);
			for (size_t j = 0; j < l.n && j < ref.n; j++) {
				tix_buf x = TIX_BUF_INIT, y = TIX_BUF_INIT;
				tix_entity_snapshot(ref.v[j], &x);
				tix_entity_snapshot(l.v[j], &y);
				TK_STR_EQ(tix_buf_cstr(&x), tix_buf_cstr(&y));
				tix_buf_free(&x);
				tix_buf_free(&y);
			}
			tix_entity_list_free(&l);
		}
		tix_entity_list_free(&ref);
		free(tickets.v);
		for (int i = 0; i < CLONES; i++)
			clone_close(&c[i]);
		tix_store_destroy(server);
	}
	TK_CHECK(merges > 0); /* the scenario really exercised merging */
}

int main(void)
{
	test_flow();
	test_resolve();
	test_retry();
	test_list_and_validation();
	test_sync_basic();
	test_sync_hostile();
	test_sync_convergence();
	return tk_finish();
}
