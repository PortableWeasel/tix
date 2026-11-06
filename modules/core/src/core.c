#include "core_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- clock --------------------------------------------------------------- */

static tix_status clock_load(tix_repo *r, tix_error *err)
{
	tix_buf b = TIX_BUF_INIT;
	tix_status st = tix_store_local_get(r->store, "clock", &b, err);
	uint64_t had = r->clock; /* never moves backwards (refresh) */
	if (st == TIX_ERR_NOT_FOUND) {
		tix_buf_free(&b);
		return TIX_OK;
	}
	if (!st) {
		uint64_t v = 0;
		size_t i = 0;
		for (; i < b.len && b.data[i] >= '0' && b.data[i] <= '9' && v <= TIX_LAMPORT_MAX; i++)
			v = v * 10 + (uint64_t)(b.data[i] - '0');
		if (i == 0 || v > TIX_LAMPORT_MAX || (i < b.len && !(i + 1 == b.len && b.data[i] == '\n')))
			st = tix_error_set(err, TIX_ERR_INVALID, "corrupt local clock '%s'", tix_buf_cstr(&b));
		else
			r->clock = v > had ? v : had;
	}
	tix_buf_free(&b);
	return st;
}

tix_status tix_repo_refresh(tix_repo *r, tix_error *err)
{
	TIX_REQUIRE(r);
	tix_store_refresh(r->store);
	return clock_load(r, err);
}

tix_status core_clock_save(tix_repo *r, tix_error *err)
{
	char s[32];
	int n = snprintf(s, sizeof(s), "%llu\n", (unsigned long long)r->clock);
	return tix_store_local_set(r->store, "clock", s, (size_t)n, err);
}

void core_clock_see(tix_repo *r, uint64_t lamport)
{
	if (lamport > r->clock)
		r->clock = lamport;
}

/* ---- DAG loading ----------------------------------------------------------- */

static size_t oid_hash(const tix_oid *o, size_t cap)
{
	uint64_t h;
	memcpy(&h, o->bytes, sizeof(h)); /* object ids are uniformly distributed */
	return (size_t)(h & (cap - 1));
}

bool core_oidset_add(core_oidset *s, const tix_oid *o)
{
	if ((s->n + 1) * 2 > s->cap) {
		core_oidset bigger = {0};
		bigger.cap = s->cap ? s->cap * 2 : 64;
		bigger.slots = tix_xcalloc(bigger.cap, sizeof(tix_oid));
		bigger.used = tix_xcalloc(bigger.cap, sizeof(bool));
		for (size_t i = 0; i < s->cap; i++)
			if (s->used[i])
				core_oidset_add(&bigger, &s->slots[i]);
		free(s->slots);
		free(s->used);
		*s = bigger;
	}
	for (size_t i = oid_hash(o, s->cap);; i = (i + 1) & (s->cap - 1)) {
		if (!s->used[i]) {
			s->used[i] = true;
			s->slots[i] = *o;
			s->n++;
			return true;
		}
		if (tix_oid_eq(&s->slots[i], o))
			return false;
	}
}

void core_oidset_free(core_oidset *s)
{
	free(s->slots);
	free(s->used);
	memset(s, 0, sizeof(*s));
}

bool core_oidset_has(const core_oidset *s, const tix_oid *o)
{
	if (!s->cap)
		return false;
	for (size_t i = oid_hash(o, s->cap); s->used[i]; i = (i + 1) & (s->cap - 1))
		if (tix_oid_eq(&s->slots[i], o))
			return true;
	return false;
}

void core_dag_free(core_dag *d)
{
	for (size_t i = 0; i < d->commits.n; i++)
		tix_commit_free(&d->commits.v[i]);
	free(d->commits.v);
}

tix_status core_dag_load(tix_repo *r, const tix_oid *head, core_dag *d, tix_error *err)
{
	TIX_VEC(tix_oid) todo = {0};
	core_oidset seen = {0};
	tix_status st = TIX_OK;
	memset(d, 0, sizeof(*d));
	TIX_VEC_PUSH(&todo, *head);
	while (todo.n && !st) {
		tix_oid o = todo.v[--todo.n];
		if (!core_oidset_add(&seen, &o))
			continue;
		if (seen.n > TIX_ENTITY_MAX_COMMITS) {
			st = tix_error_set(err, TIX_ERR_INVALID, "entity exceeds %d commits", TIX_ENTITY_MAX_COMMITS);
			break;
		}
		tix_commit c;
		st = tix_store_commit_read(r->store, &o, &c, err);
		if (st == TIX_ERR_NOT_FOUND)
			st = tix_error_set(err, TIX_ERR_INVALID, "entity history incomplete (missing commit)");
		if (st)
			break;
		for (size_t k = 0; k < c.nparents; k++)
			TIX_VEC_PUSH(&todo, c.parents[k]);
		TIX_VEC_PUSH(&d->commits, c);
	}
	free(todo.v);
	core_oidset_free(&seen);
	if (st)
		core_dag_free(d);
	return st;
}

tix_status core_load_at(tix_repo *r, tix_entity_type t, const tix_oid *head, tix_entity **out, tix_error *err)
{
	core_dag d;
	tix_status st = core_dag_load(r, head, &d, err);
	if (st)
		return st;
	tix_dag_node *nodes = tix_xcalloc(d.commits.n, sizeof(*nodes));
	for (size_t i = 0; i < d.commits.n; i++) {
		const tix_commit *c = &d.commits.v[i];
		nodes[i] = (tix_dag_node){c->oid, c->parents, c->nparents, c->pack.data, c->pack.len};
	}
	st = tix_replay(t, nodes, d.commits.n, out, err);
	free(nodes);
	core_dag_free(&d);
	if (!st)
		core_clock_see(r, (*out)->max_lamport);
	return st;
}

void core_ref_of(tix_entity_type t, const tix_oid *id, char *out, size_t n)
{
	char hex[TIX_OID_MAX_HEX + 1];
	snprintf(out, n, "%s%s", tix_entity_ref_prefix(t), tix_oid_hex(id, hex));
}

/* ---- open / identity --------------------------------------------------------- */

tix_status tix_repo_open(tix_repo **out, const tix_repo_config *cfg, tix_error *err)
{
	TIX_REQUIRE(out && cfg && cfg->store && cfg->env.now && cfg->env.random);
	*out = NULL;
	tix_repo *r = tix_xcalloc(1, sizeof(*r));
	r->store = cfg->store;
	r->env = cfg->env;
	r->index = cfg->index;
	tix_status st = clock_load(r, err);
	if (!st && !tix_oid_is_null(&cfg->identity))
		st = tix_repo_set_identity(r, &cfg->identity, err);
	if (st) {
		free(r);
		return st;
	}
	*out = r;
	return TIX_OK;
}

void tix_repo_close(tix_repo *r)
{
	free(r);
}

bool tix_repo_identity(const tix_repo *r, tix_oid *out)
{
	if (tix_oid_is_null(&r->identity))
		return false;
	*out = r->identity;
	return true;
}

tix_status tix_repo_set_identity(tix_repo *r, const tix_oid *id, tix_error *err)
{
	tix_entity *e;
	if (id->algo != r->store->algo)
		return tix_error_set(err, TIX_ERR_INVALID, "identity id uses the wrong hash algorithm");
	tix_status st = tix_entity_load(r, TIX_ENTITY_IDENTITY, id, &e, err);
	if (st) {
		tix_error_prefix(err, "identity");
		return st;
	}
	tix_entity_free(e);
	r->identity = *id;
	return TIX_OK;
}

/* ---- generic operations --------------------------------------------------------- */

tix_status tix_entity_resolve(tix_repo *r, tix_entity_type t, const char *prefix, tix_oid *out, tix_error *err)
{
	size_t n = strlen(prefix), hexlen = tix_oid_rawlen(r->store->algo) * 2;
	if (n < TIX_ID_MIN_PREFIX || n > hexlen)
		return tix_error_set(err, TIX_ERR_INVALID, "id '%s' must be %d to %zu hex characters", prefix,
				     TIX_ID_MIN_PREFIX, hexlen);
	for (const char *p = prefix; *p; p++)
		if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')))
			return tix_error_set(err, TIX_ERR_INVALID, "id '%s' is not lowercase hex", prefix);
	char ref[128];
	snprintf(ref, sizeof(ref), "%s%s", tix_entity_ref_prefix(t), prefix);
	tix_refs refs;
	tix_status st = tix_store_ref_list(r->store, ref, &refs, err);
	if (st)
		return st;
	size_t plen = strlen(tix_entity_ref_prefix(t)), matches = 0;
	tix_oid found;
	for (size_t i = 0; i < refs.n; i++) {
		const char *hex = refs.v[i].name + plen;
		tix_oid id;
		if (strlen(hex) != hexlen || !tix_oid_from_hex(&id, hex, hexlen))
			continue; /* not an entity ref */
		found = id;
		matches++;
	}
	tix_refs_free(&refs);
	if (matches == 0)
		return tix_error_set(err, TIX_ERR_NOT_FOUND, "no %s matches '%s'", tix_entity_type_name(t), prefix);
	if (matches > 1)
		return tix_error_set(err, TIX_ERR_AMBIGUOUS, "'%s' matches %zu %ss", prefix, matches,
				     tix_entity_type_name(t));
	*out = found;
	return TIX_OK;
}

/* RETURNS true and fills *out on a usable cache hit; any failure is a miss. */
static bool cache_get(tix_repo *r, tix_entity_type t, const tix_oid *id, const tix_oid *head, tix_entity **out)
{
	tix_buf b = TIX_BUF_INIT;
	tix_entity *e = NULL;
	bool ok = !tix_index_get(r->index, t, id, head, &b, NULL) &&
		  !tix_entity_decode(b.data ? b.data : "", b.len, r->store->algo, &e, NULL) && e->type == t &&
		  tix_oid_eq(&e->id, id) && tix_oid_eq(&e->head, head);
	tix_buf_free(&b);
	if (!ok) {
		tix_entity_free(e);
		return false;
	}
	core_clock_see(r, e->max_lamport);
	*out = e;
	return true;
}

tix_status tix_entity_load(tix_repo *r, tix_entity_type t, const tix_oid *id, tix_entity **out, tix_error *err)
{
	char ref[128];
	tix_oid head;
	*out = NULL;
	core_ref_of(t, id, ref, sizeof(ref));
	tix_status st = tix_store_ref_read(r->store, ref, &head, err);
	if (st)
		return st;
	if (r->index && cache_get(r, t, id, &head, out))
		return TIX_OK;
	if ((st = core_load_at(r, t, &head, out, err)))
		return st;
	if (!tix_oid_eq(&(*out)->id, id)) { /* ref name must match its root */
		tix_entity_free(*out);
		*out = NULL;
		return tix_error_set(err, TIX_ERR_INVALID, "%s does not point at the history of that id", ref);
	}
	if (r->index) { /* only validated entities are cached */
		tix_buf enc = TIX_BUF_INIT;
		tix_entity_encode(*out, &enc);
		tix_index_put(r->index, t, id, &head, enc.data, enc.len, NULL);
		tix_buf_free(&enc);
	}
	return TIX_OK;
}

static void summary(tix_entity_type t, const tix_op *ops, size_t n, char *out, size_t cap)
{
	size_t used = (size_t)snprintf(out, cap, "tix %s:", tix_entity_type_name(t));
	for (size_t i = 0; i < n && used < cap; i++)
		used += (size_t)snprintf(out + used, cap - used, " %s", tix_op_wire_name(&ops[i]));
}

tix_status core_write_pack(tix_repo *r, tix_entity_type t, const tix_oid *author, const tix_oid *parents,
			   size_t nparents, const tix_op *ops, size_t n, tix_oid *out, uint64_t *lamport,
			   tix_error *err)
{
	tix_pack p = {t, {0}, r->clock + 1, r->env.now(r->env.ctx), (tix_op *)ops, n};
	char msg[256];
	tix_buf b = TIX_BUF_INIT;
	if (author)
		p.author = *author;
	if (p.time < 0)
		p.time = 0;
	if (n)
		summary(t, ops, n, msg, sizeof(msg));
	else
		snprintf(msg, sizeof(msg), "tix %s: merge", tix_entity_type_name(t));
	tix_status st = tix_pack_encode(&p, &b, err);
	if (!st)
		st = tix_store_commit_write(r->store, parents, nparents, b.data, b.len, msg, out, err);
	tix_buf_free(&b);
	*lamport = p.lamport;
	return st;
}

static void fill_nonce(tix_repo *r, char **nonce)
{
	static const char digits[] = "0123456789abcdef";
	uint8_t raw[TIX_NONCE_HEX / 2];
	char hex[TIX_NONCE_HEX + 1];
	if (*nonce)
		return;
	r->env.random(r->env.ctx, raw, sizeof(raw));
	for (size_t i = 0; i < sizeof(raw); i++) {
		hex[2 * i] = digits[raw[i] >> 4];
		hex[2 * i + 1] = digits[raw[i] & 15];
	}
	hex[TIX_NONCE_HEX] = '\0';
	*nonce = tix_xstrdup(hex);
}

static char **create_nonce(tix_op *op)
{
	switch (op->kind) {
	case TIX_OP_TICKET_CREATE: return &op->u.ticket_create.nonce;
	case TIX_OP_IDENTITY_CREATE: return &op->u.identity_create.nonce;
	case TIX_OP_RESOURCE_CREATE: return &op->u.resource_create.nonce;
	case TIX_OP_CALENDAR_CREATE: return &op->u.calendar_create.nonce;
	default: TIX_REQUIRE(!"not a create op"); return NULL;
	}
}

static tix_status precheck(tix_repo *r, tix_entity_type t, const tix_entity *e, const tix_op *ops, size_t n,
			   tix_error *err);

tix_status tix_entity_create(tix_repo *r, tix_entity_type t, tix_op *ops, size_t n, tix_oid *out, tix_error *err)
{
	TIX_REQUIRE(ops && n >= 1 && out);
	tix_op_kind want = tix_create_kind(t);
	if (ops[0].kind != want)
		return tix_error_set(err, TIX_ERR_INVALID, "first op must be %s's create", tix_entity_type_name(t));
	for (size_t i = 1; i < n; i++)
		if (tix_op_is_create(ops[i].kind))
			return tix_error_set(err, TIX_ERR_INVALID, "only the first op may be a create");
	const tix_oid *author = NULL;
	if (t != TIX_ENTITY_IDENTITY) { /* every other type needs an author */
		if (tix_oid_is_null(&r->identity))
			return tix_error_set(err, TIX_ERR_NOT_FOUND, "no identity configured (create one with `tix id new`)");
		author = &r->identity;
	}
	tix_status pst = precheck(r, t, NULL, ops + 1, n - 1, err);
	if (pst)
		return pst;
	fill_nonce(r, create_nonce(&ops[0]));

	uint64_t lamport;
	char ref[128];
	tix_status st = core_write_pack(r, t, author, NULL, 0, ops, n, out, &lamport, err);
	if (st)
		return st;
	core_ref_of(t, out, ref, sizeof(ref));
	if ((st = tix_store_ref_cas(r->store, ref, NULL, out, err)))
		return st;
	core_clock_see(r, lamport);
	return core_clock_save(r, err);
}

tix_status tix_entity_apply(tix_repo *r, tix_entity_type t, const tix_oid *id, const tix_op *ops, size_t n,
			    tix_error *err)
{
	TIX_REQUIRE(id && ops && n >= 1);
	for (size_t i = 0; i < n; i++)
		if (tix_op_is_create(ops[i].kind))
			return tix_error_set(err, TIX_ERR_INVALID, "create ops cannot be applied to an existing %s",
					     tix_entity_type_name(t));
	if (tix_oid_is_null(&r->identity))
		return tix_error_set(err, TIX_ERR_NOT_FOUND, "no identity configured (create one with `tix id new`)");
	if (t == TIX_ENTITY_IDENTITY && !tix_oid_eq(id, &r->identity))
		return tix_error_set(err, TIX_ERR_INVALID, "you can only edit your own identity");

	char ref[128];
	core_ref_of(t, id, ref, sizeof(ref));
	tix_status st = TIX_ERR_CONFLICT;
	for (int attempt = 0; attempt < CORE_CAS_ATTEMPTS && st == TIX_ERR_CONFLICT; attempt++) {
		tix_oid head, commit;
		tix_entity *e;
		uint64_t lamport;
		if ((st = tix_store_ref_read(r->store, ref, &head, err)))
			return st;
		if ((st = core_load_at(r, t, &head, &e, err)))
			return st; /* never build on history that doesn't validate */
		st = precheck(r, t, e, ops, n, err);
		tix_entity_free(e);
		if (st)
			return st;
		if ((st = core_write_pack(r, t, &r->identity, &head, 1, ops, n, &commit, &lamport, err)))
			return st;
		st = tix_store_ref_cas(r->store, ref, &head, &commit, err);
		if (!st) {
			core_clock_see(r, lamport);
			return core_clock_save(r, err);
		}
	}
	return st;
}

/* ---- up-front checks ---------------------------------------------------------- */

static const char *short_hex(const tix_oid *id, char *out)
{
	char hex[TIX_OID_MAX_HEX + 1];
	snprintf(out, TIX_ID_MIN_PREFIX + 1, "%s", tix_oid_hex(id, hex));
	return out;
}

static tix_status must_exist(tix_repo *r, tix_entity_type t, const tix_oid *id, tix_error *err)
{
	tix_entity *e;
	char s[16];
	if (id->algo != r->store->algo || tix_entity_load(r, t, id, &e, NULL))
		return tix_error_set(err, TIX_ERR_NOT_FOUND, "no %s %s", tix_entity_type_name(t), short_hex(id, s));
	tix_entity_free(e);
	return TIX_OK;
}

static const tix_entity *find_ticket(const tix_entity_list *all, const tix_oid *id)
{
	for (size_t i = 0; i < all->n; i++)
		if (tix_oid_eq(&all->v[i]->id, id))
			return all->v[i];
	return NULL;
}

/* RETURNS true if `target` is reachable from `from` by following parent links. */
static bool parent_reaches(const tix_entity_list *all, const tix_oid *from, const tix_oid *target)
{
	const tix_oid *cur = from;
	for (size_t steps = 0; cur && steps <= all->n; steps++) {
		if (tix_oid_eq(cur, target))
			return true;
		const tix_entity *t = find_ticket(all, cur);
		cur = t && !tix_oid_is_null(&t->u.ticket.plan.parent) ? &t->u.ticket.plan.parent : NULL;
	}
	return false;
}

/* RETURNS true if `target` is reachable from `from` along dependencies (pred links). */
static bool dep_reaches(const tix_entity_list *all, const tix_oid *from, const tix_oid *target)
{
	TIX_VEC(tix_oid) todo = {0};
	core_oidset seen = {0};
	bool found = false;
	TIX_VEC_PUSH(&todo, *from);
	while (todo.n && !found) {
		tix_oid o = todo.v[--todo.n];
		if (!core_oidset_add(&seen, &o))
			continue;
		if (tix_oid_eq(&o, target)) {
			found = true;
			break;
		}
		const tix_entity *t = find_ticket(all, &o);
		for (size_t k = 0; t && k < t->u.ticket.plan.ndeps; k++)
			TIX_VEC_PUSH(&todo, t->u.ticket.plan.deps[k].pred);
	}
	free(todo.v);
	core_oidset_free(&seen);
	return found;
}

static tix_status list_all(tix_repo *r, tix_entity_type t, tix_entity_list *out, tix_error *err);

/* Ticket ops: references must resolve; no parent or dependency cycles. */
static tix_status precheck_ticket(tix_repo *r, const tix_entity *e, const tix_op *op, tix_entity_list *all,
				  tix_error *err)
{
	char a[16];
	const tix_oid *self = e ? &e->id : NULL;
	tix_status st;
	switch (op->kind) {
	case TIX_OP_EDIT_COMMENT: {
		const tix_comment *found = NULL;
		for (size_t k = 0; e && k < e->u.ticket.ncomments; k++)
			if (!tix_opid_cmp(&e->u.ticket.comments[k].id, &op->u.edit_comment.target))
				found = &e->u.ticket.comments[k];
		if (!found)
			return tix_error_set(err, TIX_ERR_NOT_FOUND, "no such comment on this ticket");
		if (!tix_oid_eq(&found->author, &r->identity))
			return tix_error_set(err, TIX_ERR_INVALID, "you can only edit your own comments");
		return TIX_OK;
	}
	case TIX_OP_SET_FIELD:
		if (op->u.set.v.kind != TIX_V_ID)
			return TIX_OK;
		if (op->u.set.field == TIX_F_CALENDAR)
			return must_exist(r, TIX_ENTITY_CALENDAR, &op->u.set.v.id, err);
		/* TIX_F_PARENT */
		if ((st = must_exist(r, TIX_ENTITY_TICKET, &op->u.set.v.id, err)))
			return st;
		if (self && tix_oid_eq(self, &op->u.set.v.id))
			return tix_error_set(err, TIX_ERR_INVALID, "a ticket can't be its own parent");
		if (self && !all->v && (st = list_all(r, TIX_ENTITY_TICKET, all, err)))
			return st;
		if (self && parent_reaches(all, &op->u.set.v.id, self))
			return tix_error_set(err, TIX_ERR_INVALID, "%s is below this ticket; that would make a cycle",
					     short_hex(&op->u.set.v.id, a));
		return TIX_OK;
	case TIX_OP_DEP_ADD:
		if ((st = must_exist(r, TIX_ENTITY_TICKET, &op->u.dep_add.pred, err)))
			return st;
		if (self && tix_oid_eq(self, &op->u.dep_add.pred))
			return tix_error_set(err, TIX_ERR_INVALID, "a ticket can't depend on itself");
		if (self && !all->v && (st = list_all(r, TIX_ENTITY_TICKET, all, err)))
			return st;
		if (self && dep_reaches(all, &op->u.dep_add.pred, self))
			return tix_error_set(err, TIX_ERR_INVALID, "%s already depends on this ticket; that would make a cycle",
					     short_hex(&op->u.dep_add.pred, a));
		return TIX_OK;
	case TIX_OP_DEP_REMOVE:
		for (size_t k = 0; e && k < e->u.ticket.plan.ndeps; k++)
			if (tix_oid_eq(&e->u.ticket.plan.deps[k].pred, &op->u.unref.target))
				return TIX_OK;
		return tix_error_set(err, TIX_ERR_NOT_FOUND, "no dependency on %s", short_hex(&op->u.unref.target, a));
	case TIX_OP_ASSIGN:
		return must_exist(r, TIX_ENTITY_RESOURCE, &op->u.assign.resource, err);
	case TIX_OP_UNASSIGN:
		for (size_t k = 0; e && k < e->u.ticket.plan.nassignments; k++)
			if (tix_oid_eq(&e->u.ticket.plan.assignments[k].resource, &op->u.unref.target))
				return TIX_OK;
		return tix_error_set(err, TIX_ERR_NOT_FOUND, "%s is not assigned", short_hex(&op->u.unref.target, a));
	default:
		return TIX_OK;
	}
}

/* Rules that model would silently ignore get a clear error up front instead.
 * e is the current state (NULL while creating). */
static tix_status precheck(tix_repo *r, tix_entity_type t, const tix_entity *e, const tix_op *ops, size_t n,
			   tix_error *err)
{
	tix_entity_list all = {0};
	tix_status st = TIX_OK;
	for (size_t i = 0; i < n && !st; i++) {
		const tix_op *op = &ops[i];
		if (t == TIX_ENTITY_TICKET) {
			st = precheck_ticket(r, e, op, &all, err);
		} else if (t == TIX_ENTITY_RESOURCE && op->kind == TIX_OP_SET_FIELD && op->u.set.v.kind == TIX_V_ID) {
			st = must_exist(r, op->u.set.field == TIX_F_RES_IDENTITY ? TIX_ENTITY_IDENTITY : TIX_ENTITY_CALENDAR,
					&op->u.set.v.id, err);
		} else if (t == TIX_ENTITY_CALENDAR && op->kind == TIX_OP_EXCEPTION_REMOVE) {
			bool found = false;
			for (size_t k = 0; e && k < e->u.calendar.nexceptions && !found; k++)
				found = e->u.calendar.exceptions[k].from == op->u.exception_remove.from &&
					e->u.calendar.exceptions[k].to == op->u.exception_remove.to;
			if (!found)
				st = tix_error_set(err, TIX_ERR_NOT_FOUND, "no such exception on this calendar");
		}
	}
	tix_entity_list_free(&all);
	return st;
}

/* ---- listing --------------------------------------------------------------- */

void tix_entity_list_free(tix_entity_list *l)
{
	for (size_t i = 0; i < l->n; i++)
		tix_entity_free(l->v[i]);
	free(l->v);
	memset(l, 0, sizeof(*l));
}

static tix_status list_all(tix_repo *r, tix_entity_type t, tix_entity_list *out, tix_error *err)
{
	tix_refs refs;
	TIX_VEC(tix_entity *) v = {0};
	memset(out, 0, sizeof(*out));
	tix_status st = tix_store_ref_list(r->store, tix_entity_ref_prefix(t), &refs, err);
	if (st)
		return st;
	size_t plen = strlen(tix_entity_ref_prefix(t)), hexlen = tix_oid_rawlen(r->store->algo) * 2;
	for (size_t i = 0; i < refs.n; i++) {
		tix_oid id;
		tix_entity *e;
		const char *hex = refs.v[i].name + plen;
		if (strlen(hex) != hexlen || !tix_oid_from_hex(&id, hex, hexlen) ||
		    tix_entity_load(r, t, &id, &e, NULL)) {
			out->nbroken++;
			continue;
		}
		TIX_VEC_PUSH(&v, e);
	}
	tix_refs_free(&refs);
	out->v = v.v;
	out->n = v.n;
	return TIX_OK;
}

static int cmp_ticket(const void *pa, const void *pb)
{
	const tix_entity *a = *(tix_entity *const *)pa, *b = *(tix_entity *const *)pb;
	if (a->created != b->created)
		return a->created > b->created ? -1 : 1;
	return tix_oid_cmp(&a->id, &b->id);
}

static const char *entity_name(const tix_entity *e)
{
	switch (e->type) {
	case TIX_ENTITY_IDENTITY: return e->u.identity.name;
	case TIX_ENTITY_RESOURCE: return e->u.resource.name;
	case TIX_ENTITY_CALENDAR: return e->u.calendar.name;
	default: return e->u.ticket.title;
	}
}

static int cmp_by_name(const void *pa, const void *pb)
{
	const tix_entity *a = *(tix_entity *const *)pa, *b = *(tix_entity *const *)pb;
	int c = strcmp(entity_name(a), entity_name(b));
	return c ? c : tix_oid_cmp(&a->id, &b->id);
}

static bool matches(const tix_ticket_query *q, const tix_entity *e, const tix_entity_list *all)
{
	const tix_ticket *t = &e->u.ticket;
	if (!q)
		return true;
	if (q->assignee) {
		bool hit = false;
		for (size_t k = 0; k < t->plan.nassignments && !hit; k++)
			hit = tix_oid_eq(&t->plan.assignments[k].resource, q->assignee);
		if (!hit)
			return false;
	}
	if (q->under && (tix_oid_eq(&e->id, q->under) || tix_oid_is_null(&t->plan.parent) ||
			 !parent_reaches(all, &t->plan.parent, q->under)))
		return false;
	if (q->status_mask && !(q->status_mask & (1u << t->status)))
		return false;
	if (q->label && !tix_strs_contains(&t->labels, q->label))
		return false;
	if (q->author && !tix_oid_eq(q->author, &t->author))
		return false;
	return true;
}

tix_status tix_ticket_list(tix_repo *r, const tix_ticket_query *q, tix_entity_list *out, tix_error *err)
{
	tix_status st = list_all(r, TIX_ENTITY_TICKET, out, err);
	if (st)
		return st;
	/* decide first (the WBS walk needs every ticket), then drop */
	bool *keep_it = tix_xcalloc(out->n ? out->n : 1, sizeof(bool));
	for (size_t i = 0; i < out->n; i++)
		keep_it[i] = matches(q, out->v[i], out);
	size_t keep = 0;
	for (size_t i = 0; i < out->n; i++) {
		if (keep_it[i])
			out->v[keep++] = out->v[i];
		else
			tix_entity_free(out->v[i]);
	}
	free(keep_it);
	out->n = keep;
	if (out->n)
		qsort(out->v, out->n, sizeof(*out->v), cmp_ticket);
	return TIX_OK;
}

static tix_status list_by_name(tix_repo *r, tix_entity_type t, tix_entity_list *out, tix_error *err)
{
	tix_status st = list_all(r, t, out, err);
	if (!st && out->n)
		qsort(out->v, out->n, sizeof(*out->v), cmp_by_name);
	return st;
}

tix_status tix_identity_list(tix_repo *r, tix_entity_list *out, tix_error *err)
{
	return list_by_name(r, TIX_ENTITY_IDENTITY, out, err);
}

tix_status tix_resource_list(tix_repo *r, tix_entity_list *out, tix_error *err)
{
	return list_by_name(r, TIX_ENTITY_RESOURCE, out, err);
}

tix_status tix_calendar_list(tix_repo *r, tix_entity_list *out, tix_error *err)
{
	return list_by_name(r, TIX_ENTITY_CALENDAR, out, err);
}

/* ---- conveniences ------------------------------------------------------------- */

tix_status tix_identity_create(tix_repo *r, const char *name, const char *email, tix_oid *out, tix_error *err)
{
	tix_op op = tix_op_identity_create(NULL, name, email);
	tix_status st = tix_entity_create(r, TIX_ENTITY_IDENTITY, &op, 1, out, err);
	tix_op_free(&op);
	return st;
}

tix_status tix_ticket_create(tix_repo *r, const char *title, const char *body, const tix_strs *labels,
			     tix_oid *out, tix_error *err)
{
	tix_op op = tix_op_ticket_create(NULL, title, body, labels);
	tix_status st = tix_entity_create(r, TIX_ENTITY_TICKET, &op, 1, out, err);
	tix_op_free(&op);
	return st;
}

tix_status tix_resource_create(tix_repo *r, const char *name, tix_resource_kind kind, tix_oid *out, tix_error *err)
{
	tix_op op = tix_op_resource_create(NULL, name, kind);
	tix_status st = tix_entity_create(r, TIX_ENTITY_RESOURCE, &op, 1, out, err);
	tix_op_free(&op);
	return st;
}

tix_status tix_calendar_create(tix_repo *r, const char *name, tix_oid *out, tix_error *err)
{
	tix_op op = tix_op_calendar_create(NULL, name);
	tix_status st = tix_entity_create(r, TIX_ENTITY_CALENDAR, &op, 1, out, err);
	tix_op_free(&op);
	return st;
}
