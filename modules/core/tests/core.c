/* core on store_mem: identity flow, writes, resolve, retries, validation, queries. */
#include "tix/core.h"
#include "tix/index_mem.h"
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
static void f_refresh(tix_store *s) { tix_store_refresh(inner(s)); }
static void f_destroy(tix_store *s) { (void)s; }
static const tix_store_vt flaky_vt = {f_cw, f_cr, f_rr, f_rl, f_cas, f_lg, f_ls, f_rf, f_rp, f_refresh, f_destroy};

static tix_repo *open_repo(tix_store *s, fake_env *env, const tix_oid *identity)
{
	tix_repo_config cfg = {.store = s, .env = {fake_now, fake_random, env}};
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

	tix_ticket_query q = {.status_mask = TIX_STATUS_MASK_ACTIVE};
	TK_OK(tix_ticket_list(r, &q, &l, &e), &e);
	TK_CHECK(l.n == 2);
	tix_entity_list_free(&l);
	q = (tix_ticket_query){.label = "ui", .author = &me};
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
	tix_repo_config cfg = {.store = s, .env = {fake_now, fake_random, &env}};
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

static void set1(clone *c, const tix_oid *t, tix_field f, tix_value v, tix_status want)
{
	tix_op op = tix_op_set(f, v);
	TK_STATUS(tix_entity_apply(c->r, TIX_ENTITY_TICKET, t, &op, 1, &e), want, &e);
	tix_op_free(&op);
}

static void op1(clone *c, tix_entity_type type, const tix_oid *id, tix_op op, tix_status want)
{
	TK_STATUS(tix_entity_apply(c->r, type, id, &op, 1, &e), want, &e);
	tix_op_free(&op);
}

static void test_planning(void)
{
	tix_store *server = tix_store_mem_new(TIX_HASH_SHA1);
	clone a, b;
	tix_oid me, proj, t1, t2, t3, dev, cal, other_id;
	tix_sync_report rep = {0};
	clone_open(&a, server, 31);
	clone_open(&b, server, 32);
	TK_OK(tix_identity_create(a.r, "Ada", "a@x", &me, &e), &e);
	TK_OK(tix_repo_set_identity(a.r, &me, &e), &e);

	/* create with planning ops in the same pack */
	TK_OK(tix_ticket_create(a.r, "Project", "", NULL, &proj, &e), &e);
	tix_op ops[] = {tix_op_ticket_create(NULL, "Dig", "", NULL), tix_op_set(TIX_F_PARENT, tix_value_id(&proj)),
			tix_op_set(TIX_F_DURATION, tix_value_int(TIX_V_INT, 3))};
	TK_OK(tix_entity_create(a.r, TIX_ENTITY_TICKET, ops, 3, &t1, &e), &e);
	for (int i = 0; i < 3; i++)
		tix_op_free(&ops[i]);
	TK_OK(tix_ticket_create(a.r, "Pour", "", NULL, &t2, &e), &e);
	TK_OK(tix_ticket_create(a.r, "Unrelated", "", NULL, &t3, &e), &e);
	TK_OK(tix_resource_create(a.r, "Digger", TIX_RES_EQUIPMENT, &dev, &e), &e);
	TK_OK(tix_calendar_create(a.r, "Site", &cal, &e), &e);
	tix_oid ghost = t3;
	ghost.bytes[0] ^= 0x5a;

	/* references must exist and have the right type */
	set1(&a, &t2, TIX_F_PARENT, tix_value_id(&ghost), TIX_ERR_NOT_FOUND);
	set1(&a, &t2, TIX_F_PARENT, tix_value_id(&dev), TIX_ERR_NOT_FOUND); /* a resource, not a ticket */
	set1(&a, &t2, TIX_F_PARENT, tix_value_id(&t2), TIX_ERR_INVALID);
	set1(&a, &t2, TIX_F_PARENT, tix_value_id(&proj), TIX_OK);
	set1(&a, &proj, TIX_F_PARENT, tix_value_id(&t2), TIX_ERR_INVALID); /* cycle */
	set1(&a, &t2, TIX_F_CALENDAR, tix_value_id(&t1), TIX_ERR_NOT_FOUND);
	set1(&a, &proj, TIX_F_CALENDAR, tix_value_id(&cal), TIX_OK);
	set1(&a, &t2, TIX_F_PARENT, tix_value_null(), TIX_OK);
	set1(&a, &t2, TIX_F_PARENT, tix_value_id(&proj), TIX_OK);
	op1(&a, TIX_ENTITY_TICKET, &t2, tix_op_dep_add(&t1, TIX_DEP_FS, 0), TIX_OK);
	op1(&a, TIX_ENTITY_TICKET, &t1, tix_op_dep_add(&t2, TIX_DEP_FS, 0), TIX_ERR_INVALID); /* cycle */
	op1(&a, TIX_ENTITY_TICKET, &t1, tix_op_dep_add(&t1, TIX_DEP_FS, 0), TIX_ERR_INVALID);
	op1(&a, TIX_ENTITY_TICKET, &t1, tix_op_dep_remove(&t2), TIX_ERR_NOT_FOUND);
	op1(&a, TIX_ENTITY_TICKET, &t2, tix_op_assign(&cal, 100, -1), TIX_ERR_NOT_FOUND);
	op1(&a, TIX_ENTITY_TICKET, &t2, tix_op_assign(&dev, 100, -1), TIX_OK);
	op1(&a, TIX_ENTITY_TICKET, &t2, tix_op_set(TIX_F_DURATION, tix_value_int(TIX_V_INT, 2)), TIX_OK);
	op1(&a, TIX_ENTITY_TICKET, &t1, tix_op_unassign(&dev), TIX_ERR_NOT_FOUND);
	op1(&a, TIX_ENTITY_RESOURCE, &dev, tix_op_set(TIX_F_RES_CALENDAR, tix_value_id(&t1)), TIX_ERR_NOT_FOUND);
	op1(&a, TIX_ENTITY_RESOURCE, &dev, tix_op_set(TIX_F_RES_IDENTITY, tix_value_id(&me)), TIX_OK);
	op1(&a, TIX_ENTITY_CALENDAR, &cal, tix_op_exception_remove(20400, 20400), TIX_ERR_NOT_FOUND);
	op1(&a, TIX_ENTITY_CALENDAR, &cal, tix_op_exception_add(20400, 20400, false, "x"), TIX_OK);
	op1(&a, TIX_ENTITY_CALENDAR, &cal, tix_op_exception_remove(20400, 20400), TIX_OK);

	/* queries */
	tix_entity_list l;
	tix_ticket_query q = {.under = &proj};
	TK_OK(tix_ticket_list(a.r, &q, &l, &e), &e);
	TK_CHECK(l.n == 2);
	tix_entity_list_free(&l);
	q = (tix_ticket_query){.assignee = &dev};
	TK_OK(tix_ticket_list(a.r, &q, &l, &e), &e);
	TK_CHECK(l.n == 1 && tix_oid_eq(&l.v[0]->id, &t2));
	tix_entity_list_free(&l);
	TK_OK(tix_resource_list(a.r, &l, &e), &e);
	TK_CHECK(l.n == 1 && !strcmp(l.v[0]->u.resource.name, "Digger") &&
		 l.v[0]->u.resource.kind == TIX_RES_EQUIPMENT && tix_oid_eq(&l.v[0]->u.resource.identity, &me));
	tix_entity_list_free(&l);

	/* ranks among Project's children */
	tix_buf rank = TIX_BUF_INIT;
	TK_STATUS(tix_ticket_rank(a.r, &proj, &t2, TIX_RANK_AFTER, &t1, &rank, &e), TIX_ERR_INVALID, &e); /* t1 unranked */
	TK_STATUS(tix_ticket_rank(a.r, &proj, &t2, TIX_RANK_AFTER, &t3, &rank, &e), TIX_ERR_INVALID, &e); /* not a sibling */
	TK_OK(tix_ticket_rank(a.r, &proj, &t1, TIX_RANK_LAST, NULL, &rank, &e), &e);
	set1(&a, &t1, TIX_F_RANK, tix_value_str(rank.data), TIX_OK);
	tix_buf_reset(&rank);
	TK_OK(tix_ticket_rank(a.r, &proj, &t2, TIX_RANK_BEFORE, &t1, &rank, &e), &e);
	set1(&a, &t2, TIX_F_RANK, tix_value_str(rank.data), TIX_OK);
	tix_buf_free(&rank);

	/* the plan: Pour (2d) first by rank, but depends on Dig (3d) */
	tix_plan_result pr;
	tix_date today;
	TIX_REQUIRE(tix_date_parse("2026-11-09", &today));
	TK_OK(tix_plan(a.r, &proj, today, &pr, &e), &e);
	if (pr.schedule) {
		const tix_schedule *s = pr.schedule;
		TK_CHECK(s->ntasks == 3 && !strcmp(s->tasks[1].wbs, "1.1") && tix_oid_eq(&s->tasks[1].ticket->id, &t2));
		TK_CHECK(s->tasks[2].start == today && s->tasks[1].start == today + 3 && s->finish == today + 4);
		TK_CHECK(s->tasks[0].calendar && tix_oid_eq(&s->tasks[0].calendar->id, &cal) && s->nloads == 1);
	}
	tix_plan_result_free(&pr);
	TK_STATUS(tix_plan(a.r, &ghost, today, &pr, &e), TIX_ERR_NOT_FOUND, &e);

	/* resources and calendars sync like everything else */
	TK_OK(tix_sync(a.r, "origin", &rep, &e), &e);
	TK_CHECK(rep.pushed == 7);
	tix_sync_report_free(&rep);
	TK_OK(tix_pull(b.r, "origin", &rep, &e), &e);
	TK_CHECK(rep.created == 7 && rep.skipped == 0);
	tix_sync_report_free(&rep);
	TK_OK(tix_calendar_list(b.r, &l, &e), &e);
	TK_CHECK(l.n == 1 && !strcmp(l.v[0]->u.calendar.name, "Site"));
	tix_entity_list_free(&l);
	TK_OK(tix_identity_create(b.r, "Bob", "b@x", &other_id, &e), &e);
	TK_OK(tix_repo_set_identity(b.r, &other_id, &e), &e);
	op1(&b, TIX_ENTITY_RESOURCE, &dev, tix_op_set(TIX_F_CAPACITY, tix_value_int(TIX_V_INT, 200)), TIX_OK); /* anyone may edit */

	clone_close(&a);
	clone_close(&b);
	tix_store_destroy(server);
}

static void snapshots(tix_repo *r, tix_buf *out)
{
	tix_entity_list l;
	tix_buf_reset(out);
	TK_OK(tix_ticket_list(r, NULL, &l, &e), &e);
	for (size_t i = 0; i < l.n; i++)
		tix_entity_snapshot(l.v[i], out);
	tix_entity_list_free(&l);
}

static void test_index(void)
{
	fake_env env = {1700000000, 41};
	tix_store *s = tix_store_mem_new(TIX_HASH_SHA1);
	tix_index *x = tix_index_mem_new();
	tix_repo_config cfg = {.store = s, .env = {fake_now, fake_random, &env}, .index = x};
	tix_repo *r = NULL, *plain = NULL;
	tix_oid me, t[3];
	TK_OK(tix_repo_open(&r, &cfg, &e), &e);
	TK_OK(tix_identity_create(r, "Ada", "a@x", &me, &e), &e);
	TK_OK(tix_repo_set_identity(r, &me, &e), &e);
	for (int i = 0; i < 3; i++)
		TK_OK(tix_ticket_create(r, "T", "", NULL, &t[i], &e), &e);
	tix_op op = tix_op_text(TIX_OP_COMMENT, "hello");
	TK_OK(tix_entity_apply(r, TIX_ENTITY_TICKET, &t[1], &op, 1, &e), &e);
	tix_op_free(&op);

	tix_buf with = TIX_BUF_INIT, without = TIX_BUF_INIT;
	size_t h0 = tix_index_mem_hits(x);
	snapshots(r, &with); /* fills the cache */
	size_t h1 = tix_index_mem_hits(x), m1 = tix_index_mem_misses(x);
	snapshots(r, &with);
	TK_CHECK(tix_index_mem_hits(x) - h1 == 3 && tix_index_mem_misses(x) == m1 && h1 >= h0);
	TK_OK(tix_repo_open(&plain, &(tix_repo_config){.store = s, .env = {fake_now, fake_random, &env}, .identity = me}, &e), &e);
	snapshots(plain, &without);
	TK_STR_EQ(tix_buf_cstr(&with), tix_buf_cstr(&without));

	/* a moved ref misses once, then hits again */
	op = tix_op_set_status(TIX_TICKET_DONE);
	TK_OK(tix_entity_apply(plain, TIX_ENTITY_TICKET, &t[0], &op, 1, &e), &e); /* a writer without the cache */
	tix_op_free(&op);
	size_t m2 = tix_index_mem_misses(x);
	tix_entity *ent = load_ticket(r, &t[0]);
	TK_CHECK(ent && ent->u.ticket.status == TIX_TICKET_DONE && tix_index_mem_misses(x) == m2 + 1);
	tix_entity_free(ent);
	ent = load_ticket(r, &t[0]);
	TK_CHECK(ent && tix_index_mem_misses(x) == m2 + 1);
	tix_entity_free(ent);

	/* garbage, or another entity's (valid) bytes, are ignored and replaced */
	tix_index_mem_corrupt(x, TIX_ENTITY_TICKET, &t[1], "{\"v\":1,");
	ent = load_ticket(r, &t[1]);
	TK_CHECK(ent && ent->u.ticket.ncomments == 1);
	tix_entity_free(ent);
	tix_buf other = TIX_BUF_INIT, head2 = TIX_BUF_INIT;
	ent = load_ticket(r, &t[2]);
	if (ent)
		tix_entity_encode(ent, &other);
	tix_entity_free(ent);
	tix_index_mem_corrupt(x, TIX_ENTITY_TICKET, &t[1], tix_buf_cstr(&other));
	ent = load_ticket(r, &t[1]);
	TK_CHECK(ent && tix_oid_eq(&ent->id, &t[1]) && ent->u.ticket.ncomments == 1);
	tix_entity_free(ent);
	snapshots(r, &with);
	snapshots(plain, &without);
	TK_STR_EQ(tix_buf_cstr(&with), tix_buf_cstr(&without));

	/* lists decode only what they return; the rest is filtered on cached summaries */
	tix_entity_list l0;
	TK_OK(tix_ticket_list(r, NULL, &l0, &e), &e); /* warm: every entry has a summary */
	tix_entity_list_free(&l0);
	size_t hs = tix_index_mem_hits(x), ms = tix_index_mem_misses(x), sc = tix_index_mem_scans(x);
	tix_ticket_query done = {.status_mask = 1u << TIX_TICKET_DONE};
	TK_OK(tix_ticket_list(r, &done, &l0, &e), &e);
	TK_CHECK(l0.n == 1 && tix_oid_eq(&l0.v[0]->id, &t[0]));
	TK_CHECK(tix_index_mem_scans(x) == sc + 1 && tix_index_mem_hits(x) == hs + 1 && tix_index_mem_misses(x) == ms);
	tix_entity_list_free(&l0);
	/* a summary that doesn't decode is a miss for that ticket only: it is loaded instead */
	tix_index_mem_corrupt_summary(x, TIX_ENTITY_TICKET, &t[0], "tixsum 1 1\n1:9");
	TK_OK(tix_ticket_list(r, &done, &l0, &e), &e);
	TK_CHECK(l0.n == 1 && tix_oid_eq(&l0.v[0]->id, &t[0]) && l0.v[0]->u.ticket.status == TIX_TICKET_DONE);
	tix_entity_list_free(&l0);
	/* ... and repaired: next time only the returned ticket is decoded */
	hs = tix_index_mem_hits(x);
	TK_OK(tix_ticket_list(r, &done, &l0, &e), &e);
	TK_CHECK(l0.n == 1 && tix_index_mem_hits(x) == hs + 1);
	tix_entity_list_free(&l0);
	tix_index_mem_corrupt_summary(x, TIX_ENTITY_TICKET, &t[1], "");
	tix_ticket_query open = {.status_mask = 1u << TIX_TICKET_OPEN, .sort = TIX_SORT_WBS};
	TK_OK(tix_ticket_list(r, &open, &l0, &e), &e);
	TK_CHECK(l0.n == 2);
	tix_entity_list_free(&l0);
	/* a summary for an old head is ignored (the ref moved under a writer without the cache) */
	op = tix_op_set_status(TIX_TICKET_OPEN);
	TK_OK(tix_entity_apply(plain, TIX_ENTITY_TICKET, &t[0], &op, 1, &e), &e);
	tix_op_free(&op);
	TK_OK(tix_ticket_list(r, &done, &l0, &e), &e);
	TK_CHECK(l0.n == 0);
	tix_entity_list_free(&l0);

	/* cache hits still advance the clock: a fresh repo whose clock was lost writes after them */
	tix_repo_close(r);
	TK_OK(tix_store_local_set(s, "clock", "1\n", 2, &e), &e);
	cfg.identity = me;
	TK_OK(tix_repo_open(&r, &cfg, &e), &e);
	size_t h3 = tix_index_mem_hits(x);
	tix_entity_list l;
	TK_OK(tix_ticket_list(r, NULL, &l, &e), &e);
	uint64_t maxl = 0;
	for (size_t i = 0; i < l.n; i++)
		maxl = l.v[i]->max_lamport > maxl ? l.v[i]->max_lamport : maxl;
	tix_entity_list_free(&l);
	TK_CHECK(tix_index_mem_hits(x) > h3);
	op = tix_op_text(TIX_OP_COMMENT, "after a lost clock");
	TK_OK(tix_entity_apply(r, TIX_ENTITY_TICKET, &t[2], &op, 1, &e), &e);
	tix_op_free(&op);
	ent = load_ticket(r, &t[2]);
	TK_CHECK(ent && ent->max_lamport > maxl);
	tix_entity_free(ent);

	tix_buf_free(&with);
	tix_buf_free(&without);
	tix_buf_free(&other);
	tix_buf_free(&head2);
	tix_repo_close(r);
	tix_repo_close(plain);
	tix_index_destroy(x);
	tix_store_destroy(s);
}

/* tix_repo_refresh: another process (a second repo on the same store) advanced the clock. */
static void test_refresh(void)
{
	fake_env env = {1700000000, 11};
	tix_store *s = tix_store_mem_new(TIX_HASH_SHA1);
	tix_repo *a = open_repo(s, &env, NULL);
	tix_oid me, t, u;
	TK_OK(tix_identity_create(a, "Ada", "ada@example.org", &me, &e), &e);
	TK_OK(tix_repo_set_identity(a, &me, &e), &e);
	tix_repo *b = open_repo(s, &env, &me);
	for (int i = 0; i < 5; i++)
		TK_OK(tix_ticket_create(b, "busy", "", NULL, &t, &e), &e);
	tix_entity *bt = load_ticket(b, &t);
	uint64_t seen = bt->max_lamport;
	tix_entity_free(bt);
	TK_OK(tix_repo_refresh(a, &e), &e);
	TK_OK(tix_ticket_create(a, "after refresh", "", NULL, &u, &e), &e);
	tix_entity *at = load_ticket(a, &u);
	TK_CHECK(at->max_lamport > seen); /* a's next write is ordered after b's */
	tix_entity_free(at);
	/* the clock never moves backwards, and a corrupt clock is reported, state unchanged */
	TK_OK(tix_store_local_set(s, "clock", "1\n", 2, &e), &e);
	TK_OK(tix_repo_refresh(a, &e), &e);
	TK_OK(tix_ticket_create(a, "still ordered", "", NULL, &u, &e), &e);
	at = load_ticket(a, &u);
	TK_CHECK(at->max_lamport > seen + 1);
	tix_entity_free(at);
	TK_OK(tix_store_local_set(s, "clock", "x\n", 2, &e), &e);
	TK_STATUS(tix_repo_refresh(a, &e), TIX_ERR_INVALID, &e);
	tix_repo_close(b);
	tix_repo_close(a);
	tix_store_destroy(s);
}

/* tix_ticket_query.text: id prefix or title substring, ASCII case-insensitive. */
static void test_text_query(void)
{
	fake_env env = {1700000000, 5};
	tix_store *s = tix_store_mem_new(TIX_HASH_SHA1);
	tix_repo *r = open_repo(s, &env, NULL);
	tix_oid me, a, t;
	TK_OK(tix_identity_create(r, "Ada", "a@x", &me, &e), &e);
	TK_OK(tix_repo_set_identity(r, &me, &e), &e);
	TK_OK(tix_ticket_create(r, "Crash on Save \xe2\x9c\x93", "", NULL, &a, &e), &e);
	TK_OK(tix_ticket_create(r, "Other", "", NULL, &t, &e), &e);
	TK_OK(tix_ticket_create(r, "Saved search", "", NULL, &t, &e), &e);
	struct {
		const char *text;
		size_t n;
	} hits[] = {{"SAVE", 2}, {"crash on s", 1}, {"\xe2\x9c\x93", 1}, {"zzz", 0}, {"", 3}, {"save x", 0}};
	tix_entity_list l;
	for (size_t i = 0; i < sizeof(hits) / sizeof(*hits); i++) {
		tix_ticket_query q = {.text = hits[i].text};
		TK_OK(tix_ticket_list(r, &q, &l, &e), &e);
		TK_CHECK(l.n == hits[i].n);
		tix_entity_list_free(&l);
	}
	char ahex[TIX_OID_MAX_HEX + 1];
	tix_oid_hex(&a, ahex);
	ahex[8] = '\0';
	tix_ticket_query q = {.text = ahex};
	TK_OK(tix_ticket_list(r, &q, &l, &e), &e);
	TK_CHECK(l.n == 1 && tix_oid_eq(&l.v[0]->id, &a));
	tix_entity_list_free(&l);
	tix_repo_close(r);
	tix_store_destroy(s);
}

/* Put t under parent (NULL = top level) at pos, as the CLI's --parent/--rank do. */
static void place(tix_repo *r, const tix_oid *t, const tix_oid *parent, tix_rank_pos pos, const tix_oid *anchor)
{
	tix_buf rank = TIX_BUF_INIT;
	TK_OK(tix_ticket_rank(r, parent, t, pos, anchor, &rank, &e), &e);
	tix_op ops[2] = {tix_op_set(TIX_F_PARENT, parent ? tix_value_id(parent) : tix_value_null()),
			 tix_op_set(TIX_F_RANK, tix_value_str(tix_buf_cstr(&rank)))};
	TK_OK(tix_entity_apply(r, TIX_ENTITY_TICKET, t, ops, 2, &e), &e);
	tix_op_free(&ops[0]);
	tix_op_free(&ops[1]);
	tix_buf_free(&rank);
}

static void expect_order(tix_repo *r, tix_ticket_query q, const tix_oid *want, size_t n)
{
	tix_entity_list l;
	TK_OK(tix_ticket_list(r, &q, &l, &e), &e);
	TK_CHECK(l.n == n);
	for (size_t i = 0; i < n && i < l.n; i++)
		if (!tix_oid_eq(&l.v[i]->id, &want[i]))
			tk_fail(__FILE__, __LINE__, "sort %d: position %zu is '%s'", (int)q.sort, i, l.v[i]->u.ticket.title);
	tix_entity_list_free(&l);
}

/* Baselines: more tasks than one pack holds become a chain under one ref; rename; list. */
static void test_baseline(void)
{
	fake_env env = {1700000000, 13};
	tix_store *s = tix_store_mem_new(TIX_HASH_SHA1);
	tix_repo *r = open_repo(s, &env, NULL);
	tix_oid me, first, b1, b2, bad;
	tix_date today;
	TIX_REQUIRE(tix_date_parse("2026-11-09", &today));
	TK_STATUS(tix_baseline_take(r, "nobody", NULL, today, &b1, NULL, &e), TIX_ERR_NOT_FOUND, &e);
	TK_OK(tix_identity_create(r, "Ada", "a@x", &me, &e), &e);
	TK_OK(tix_repo_set_identity(r, &me, &e), &e);
	size_t n = TIX_PACK_MAX_OPS + 100;
	for (size_t i = 0; i < n; i++) {
		tix_oid t;
		TK_OK(tix_ticket_create(r, "T", "", NULL, &t, &e), &e);
		if (!i)
			first = t;
		tix_op d = tix_op_set(TIX_F_DURATION, tix_value_int(TIX_V_INT, 2));
		TK_OK(tix_entity_apply(r, TIX_ENTITY_TICKET, &t, &d, 1, &e), &e);
		tix_op_free(&d);
	}
	size_t got = 0;
	TK_OK(tix_baseline_take(r, "Plan of record", NULL, today, &b1, &got, &e), &e);
	TK_CHECK(got == n);
	tix_entity *bl = NULL;
	TK_OK(tix_entity_load(r, TIX_ENTITY_BASELINE, &b1, &bl, &e), &e);
	TK_CHECK(bl && bl->u.baseline.ntasks == n && !strcmp(bl->u.baseline.name, "Plan of record") &&
		 bl->u.baseline.taken == today && !tix_oid_eq(&bl->head, &b1)); /* head is the chain's end */
	const tix_baseline_task *bt = bl ? tix_baseline_find(&bl->u.baseline, &first) : NULL;
	TK_CHECK(bt && bt->duration == 2 && bt->start == today);
	tix_entity_free(bl);
	tix_op ren = tix_op_text(TIX_OP_BASELINE_SET_NAME, "Approved");
	TK_OK(tix_entity_apply(r, TIX_ENTITY_BASELINE, &b1, &ren, 1, &e), &e);
	tix_op_free(&ren);
	TK_OK(tix_baseline_take(r, "Later, one root", &first, today + 7, &b2, &got, &e), &e);
	TK_CHECK(got == 1);
	memset(&bad, 0, sizeof(bad));
	bad.algo = TIX_HASH_SHA1;
	bad.bytes[0] = 0xee;
	TK_STATUS(tix_baseline_take(r, "x", &bad, today, &b1, NULL, &e), TIX_ERR_NOT_FOUND, &e);
	tix_entity_list l;
	TK_OK(tix_baseline_list(r, &l, &e), &e);
	TK_CHECK(l.n == 2 && tix_oid_eq(&l.v[0]->id, &b2) && !strcmp(l.v[1]->u.baseline.name, "Approved"));
	TK_CHECK(l.n == 2 && tix_oid_eq(&l.v[0]->u.baseline.root, &first));
	tix_entity_list_free(&l);
	tix_repo_close(r);
	tix_store_destroy(s);
}

static void test_stamp(void)
{
	fake_env env = {1700000000, 11};
	tix_store *s = tix_store_mem_new(TIX_HASH_SHA1);
	tix_repo *r = open_repo(s, &env, NULL);
	char a[TIX_STAMP_LEN], b[TIX_STAMP_LEN], c[TIX_STAMP_LEN];
	tix_oid me, t;
	TK_OK(tix_repo_stamp(r, a, &e), &e);
	TK_CHECK(strlen(a) == 16);
	TK_OK(tix_identity_create(r, "Ada", "a@x", &me, &e), &e);
	TK_OK(tix_repo_set_identity(r, &me, &e), &e);
	TK_OK(tix_ticket_create(r, "One", "", NULL, &t, &e), &e);
	TK_OK(tix_repo_stamp(r, b, &e), &e);
	TK_CHECK(strcmp(a, b) != 0);
	TK_OK(tix_repo_stamp(r, c, &e), &e);
	TK_STR_EQ(b, c); /* stable while nothing changes */
	tix_op op = tix_op_set_status(TIX_TICKET_DONE);
	TK_OK(tix_entity_apply(r, TIX_ENTITY_TICKET, &t, &op, 1, &e), &e);
	tix_op_free(&op);
	TK_OK(tix_repo_stamp(r, c, &e), &e);
	TK_CHECK(strcmp(b, c) != 0); /* an edit moves a head */
	tix_repo_close(r);
	tix_store_destroy(s);
}

static void test_sort(void)
{
	fake_env env = {1700000000, 9};
	tix_store *s = tix_store_mem_new(TIX_HASH_SHA1);
	tix_repo *r = open_repo(s, &env, NULL);
	tix_oid me, root, c1, c2, top2;
	TK_OK(tix_identity_create(r, "Ada", "a@x", &me, &e), &e);
	TK_OK(tix_repo_set_identity(r, &me, &e), &e);
	TK_OK(tix_ticket_create(r, "Root", "", NULL, &root, &e), &e);
	TK_OK(tix_ticket_create(r, "First child", "", NULL, &c1, &e), &e);
	TK_OK(tix_ticket_create(r, "Second child", "", NULL, &c2, &e), &e);
	TK_OK(tix_ticket_create(r, "Top two", "", NULL, &top2, &e), &e);
	place(r, &root, NULL, TIX_RANK_LAST, NULL);
	place(r, &top2, NULL, TIX_RANK_LAST, NULL);
	place(r, &c1, &root, TIX_RANK_LAST, NULL);
	place(r, &c2, &root, TIX_RANK_BEFORE, &c1); /* c2 sorts first among the children */
	tix_op op = tix_op_set_status(TIX_TICKET_IN_PROGRESS);
	TK_OK(tix_entity_apply(r, TIX_ENTITY_TICKET, &c1, &op, 1, &e), &e);
	tix_op_free(&op);

	expect_order(r, (tix_ticket_query){.sort = TIX_SORT_WBS}, (tix_oid[]){root, c2, c1, top2}, 4);
	expect_order(r, (tix_ticket_query){.sort = TIX_SORT_STATUS}, (tix_oid[]){c1, root, c2, top2}, 4);
	expect_order(r, (tix_ticket_query){.sort = TIX_SORT_NEWEST}, (tix_oid[]){top2, c2, c1, root}, 4);
	expect_order(r, (tix_ticket_query){.sort = TIX_SORT_UPDATED}, (tix_oid[]){c1, c2, top2, root}, 4);
	/* the parent is filtered out, but its rank still orders what's left */
	expect_order(r, (tix_ticket_query){.sort = TIX_SORT_WBS, .under = &root}, (tix_oid[]){c2, c1}, 2);
	expect_order(r, (tix_ticket_query){.sort = TIX_SORT_WBS, .status_mask = 1u << TIX_TICKET_OPEN},
		     (tix_oid[]){root, c2, top2}, 3);
	tix_repo_close(r);
	tix_store_destroy(s);
}

int main(void)
{
	test_sort();
	test_stamp();
	test_baseline();
	test_flow();
	test_resolve();
	test_retry();
	test_list_and_validation();
	test_sync_basic();
	test_sync_hostile();
	test_sync_convergence();
	test_planning();
	test_index();
	test_refresh();
	test_text_query();
	return tk_finish();
}
