/* DAG validation, total ordering and state folding (see tix/model_replay.h). */
#include "tix/base_json.h"
#include "tix/model_replay.h"

#include <stdlib.h>
#include <string.h>

typedef struct ctx {
	const tix_dag_node *nodes;
	size_t n;
	tix_hash_algo algo;
	size_t *by_oid;  /* node indices sorted by oid */
	size_t **pidx;   /* parent indices per node */
	tix_pack *packs; /* decoded pack per node */
	size_t *topo;    /* topological order (parents first) */
	uint64_t *anc;   /* ancestor bitsets, n * words, or NULL */
	size_t words;
} ctx;

typedef struct op_ref {
	size_t node;
	uint32_t line;
	const tix_op *op;
} op_ref;

static const ctx *sort_ctx; /* qsort has no user pointer in C17 */

#include <stdio.h>

static int cmp_by_oid(const void *a, const void *b)
{
	return tix_oid_cmp(&sort_ctx->nodes[*(const size_t *)a].oid, &sort_ctx->nodes[*(const size_t *)b].oid);
}

static int cmp_op_ref(const void *pa, const void *pb)
{
	const op_ref *a = pa, *b = pb;
	uint64_t la = sort_ctx->packs[a->node].lamport, lb = sort_ctx->packs[b->node].lamport;
	if (la != lb)
		return la < lb ? -1 : 1;
	int c = tix_oid_cmp(&sort_ctx->nodes[a->node].oid, &sort_ctx->nodes[b->node].oid);
	if (c)
		return c;
	return a->line < b->line ? -1 : a->line > b->line;
}

static bool find_node(const ctx *c, const tix_oid *oid, size_t *out)
{
	size_t lo = 0, hi = c->n;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		int r = tix_oid_cmp(&c->nodes[c->by_oid[mid]].oid, oid);
		if (!r) {
			*out = c->by_oid[mid];
			return true;
		}
		if (r < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return false;
}

static void ctx_free(ctx *c)
{
	if (c->pidx)
		for (size_t i = 0; i < c->n; i++)
			free(c->pidx[i]);
	if (c->packs)
		for (size_t i = 0; i < c->n; i++)
			tix_pack_free(&c->packs[i]);
	free(c->pidx);
	free(c->packs);
	free(c->by_oid);
	free(c->topo);
	free(c->anc);
}

static tix_status node_err(tix_error *err, const ctx *c, size_t i, const char *what)
{
	char hex[TIX_OID_MAX_HEX + 1];
	return tix_error_set(err, TIX_ERR_INVALID, "commit %s: %s", tix_oid_hex(&c->nodes[i].oid, hex), what);
}

/* Index, parents, single root/head, acyclic. ENSURES: c->topo filled. */
static tix_status check_graph(ctx *c, size_t *root, size_t *head, tix_error *err)
{
	size_t n = c->n, nroots = 0, nheads = 0;
	c->by_oid = tix_xcalloc(n, sizeof(size_t));
	for (size_t i = 0; i < n; i++) {
		if (c->nodes[i].oid.algo != c->algo)
			return node_err(err, c, i, "mixed hash algorithms");
		c->by_oid[i] = i;
	}
	sort_ctx = c;
	qsort(c->by_oid, n, sizeof(size_t), cmp_by_oid);
	for (size_t i = 1; i < n; i++)
		if (tix_oid_eq(&c->nodes[c->by_oid[i - 1]].oid, &c->nodes[c->by_oid[i]].oid))
			return node_err(err, c, c->by_oid[i], "duplicate commit");

	size_t *nchildren = tix_xcalloc(n, sizeof(size_t));
	tix_status st = TIX_OK;
	c->pidx = tix_xcalloc(n, sizeof(size_t *));
	for (size_t i = 0; i < n && !st; i++) {
		const tix_dag_node *d = &c->nodes[i];
		c->pidx[i] = d->nparents ? tix_xcalloc(d->nparents, sizeof(size_t)) : NULL;
		for (size_t k = 0; k < d->nparents && !st; k++) {
			if (!find_node(c, &d->parents[k], &c->pidx[i][k]))
				st = node_err(err, c, i, "parent missing from DAG");
			for (size_t m = 0; m < k && !st; m++)
				if (c->pidx[i][m] == c->pidx[i][k])
					st = node_err(err, c, i, "duplicate parent");
			if (!st)
				nchildren[c->pidx[i][k]]++;
		}
		if (!d->nparents) {
			nroots++;
			*root = i;
		}
	}
	for (size_t i = 0; i < n && !st; i++)
		if (!nchildren[i]) {
			nheads++;
			*head = i;
		}
	if (!st && nroots != 1)
		st = tix_error_set(err, TIX_ERR_INVALID, "DAG has %zu roots (need 1)", nroots);
	if (!st && nheads != 1)
		st = tix_error_set(err, TIX_ERR_INVALID, "DAG has %zu heads (need 1)", nheads);
	free(nchildren);
	if (st)
		return st;

	/* Kahn's algorithm over child lists (CSR layout). */
	size_t *start = tix_xcalloc(n + 1, sizeof(size_t)), *fill = tix_xcalloc(n, sizeof(size_t));
	size_t *pending = tix_xcalloc(n, sizeof(size_t));
	for (size_t i = 0; i < n; i++)
		for (size_t k = 0; k < c->nodes[i].nparents; k++)
			start[c->pidx[i][k] + 1]++;
	for (size_t i = 0; i < n; i++)
		start[i + 1] += start[i];
	size_t *child = tix_xcalloc(start[n] ? start[n] : 1, sizeof(size_t));
	for (size_t i = 0; i < n; i++) {
		pending[i] = c->nodes[i].nparents;
		for (size_t k = 0; k < c->nodes[i].nparents; k++) {
			size_t p = c->pidx[i][k];
			child[start[p] + fill[p]++] = i;
		}
	}
	c->topo = tix_xcalloc(n, sizeof(size_t));
	size_t qh = 0, qt = 0;
	c->topo[qt++] = *root;
	while (qh < qt) {
		size_t v = c->topo[qh++];
		for (size_t e = start[v]; e < start[v + 1]; e++)
			if (--pending[child[e]] == 0)
				c->topo[qt++] = child[e];
	}
	free(start);
	free(fill);
	free(pending);
	free(child);
	if (qt != n)
		return tix_error_set(err, TIX_ERR_INVALID, "DAG has a cycle or unreachable commits");
	return TIX_OK;
}

static tix_status check_packs(ctx *c, tix_entity_type type, size_t root, tix_error *err)
{
	char hex[TIX_OID_MAX_HEX + 1];
	c->packs = tix_xcalloc(c->n, sizeof(tix_pack));
	for (size_t i = 0; i < c->n; i++) {
		const tix_dag_node *d = &c->nodes[i];
		tix_pack *p = &c->packs[i];
		tix_status st = tix_pack_decode(d->pack, d->pack_len, c->algo, p, err);
		if (st) {
			tix_error_prefix(err, "commit %s", tix_oid_hex(&d->oid, hex));
			return st;
		}
		if (p->type != type)
			return node_err(err, c, i, "pack has the wrong entity type");
	}
	const tix_oid *id = &c->nodes[root].oid;
	for (size_t i = 0; i < c->n; i++) {
		const tix_pack *p = &c->packs[i];
		size_t np = c->nodes[i].nparents;
		for (size_t k = 0; k < np; k++)
			if (p->lamport <= c->packs[c->pidx[i][k]].lamport)
				return node_err(err, c, i, "lamport not greater than parent's");
		if (np >= 2 && p->nops)
			return node_err(err, c, i, "merge commit carries ops");
		for (size_t k = 0; k < p->nops; k++) {
			bool want_create = i == root && k == 0;
			if (tix_op_is_create(p->ops[k].kind) != want_create)
				return node_err(err, c, i, want_create ? "root must start with create"
								       : "create outside the root");
		}
		if (i == root && !p->nops)
			return node_err(err, c, i, "root pack is empty");
		if (type == TIX_ENTITY_IDENTITY && p->nops) {
			if (i == root && !tix_oid_is_null(&p->author))
				return node_err(err, c, i, "identity root must have no author");
			if (i != root && !tix_oid_eq(&p->author, id))
				return node_err(err, c, i, "identity edited by another identity");
		}
	}
	return TIX_OK;
}

static void build_ancestors(ctx *c)
{
	c->words = (c->n + 63) / 64;
	c->anc = tix_xcalloc(c->n * c->words, sizeof(uint64_t));
	for (size_t t = 0; t < c->n; t++) {
		size_t i = c->topo[t];
		uint64_t *me = c->anc + i * c->words;
		for (size_t k = 0; k < c->nodes[i].nparents; k++) {
			size_t p = c->pidx[i][k];
			const uint64_t *pa = c->anc + p * c->words;
			for (size_t w = 0; w < c->words; w++)
				me[w] |= pa[w];
			me[p / 64] |= 1ull << (p % 64);
		}
	}
}

/* Observed-remove: does the remove at (rn, rl) see the add at (an, al)? */
static bool observes(const ctx *c, size_t rn, uint32_t rl, size_t an, uint32_t al)
{
	if (rn == an)
		return al < rl;
	return (c->anc[rn * c->words + an / 64] >> (an % 64)) & 1;
}

typedef struct or_ent {
	char key[TIX_FPR_MAX + 1]; /* label, fingerprint, id hex or date range */
	size_t node;
	uint32_t line;
	const tix_op *op; /* the op carrying the value (keys) */
} or_ent;

typedef TIX_VEC(or_ent) or_vec;

/* RETURNS index into adds of the latest surviving add for key, or SIZE_MAX. */
static size_t or_survivor(const ctx *c, const or_vec *adds, const or_vec *rems, const char *key)
{
	size_t best = SIZE_MAX;
	for (size_t a = 0; a < adds->n; a++) {
		if (strcmp(adds->v[a].key, key))
			continue;
		bool removed = false;
		for (size_t r = 0; r < rems->n && !removed; r++)
			removed = !strcmp(rems->v[r].key, key) &&
				  observes(c, rems->v[r].node, rems->v[r].line, adds->v[a].node, adds->v[a].line);
		if (!removed)
			best = a; /* adds are in total order: last survivor wins */
	}
	return best;
}

static void push_or(or_vec *v, const char *key, const op_ref *r)
{
	or_ent e;
	TIX_REQUIRE(strlen(key) < sizeof(e.key)); /* validated by the codec */
	strcpy(e.key, key);
	e.node = r->node;
	e.line = r->line;
	e.op = r->op;
	TIX_VEC_PUSH(v, e);
}

static void push_or_id(or_vec *v, const tix_oid *id, const op_ref *r)
{
	char hex[TIX_OID_MAX_HEX + 1];
	push_or(v, tix_oid_hex(id, hex), r);
}

static void push_or_range(or_vec *v, tix_date from, tix_date to, const op_ref *r)
{
	char a[TIX_DATE_STR], b[TIX_DATE_STR], key[2 * TIX_DATE_STR];
	snprintf(key, sizeof(key), "%s:%s", tix_date_str(from, a), tix_date_str(to, b));
	push_or(v, key, r);
}

int tix_order_cmp(const tix_order *a, const tix_order *b)
{
	if (a->lamport != b->lamport)
		return a->lamport < b->lamport ? -1 : 1;
	return tix_opid_cmp(&a->id, &b->id);
}

static tix_order order_of(const ctx *c, size_t node, uint32_t line)
{
	tix_order o = {c->packs[node].lamport, {c->nodes[node].oid, line}};
	return o;
}

/* Latest set-field per field, in total order (LWW). */
typedef struct lww {
	const tix_value *v; /* NULL = never set */
	tix_order order;
} lww;

static void fold_field(const ctx *c, const op_ref *r, lww *slots)
{
	lww *l = &slots[r->op->u.set.field];
	l->v = &r->op->u.set.v;
	l->order = order_of(c, r->node, r->line);
}

static bool lww_null(const lww *l)
{
	return !l->v || l->v->kind == TIX_V_NULL;
}

static int64_t lww_int(const lww *l, int64_t unset)
{
	return lww_null(l) ? unset : l->v->i;
}

static tix_date lww_date(const lww *l)
{
	return lww_null(l) ? TIX_DATE_NONE : (tix_date)l->v->i;
}

static tix_oid lww_id(const lww *l)
{
	tix_oid none;
	memset(&none, 0, sizeof(none));
	return lww_null(l) ? none : l->v->id;
}

static char *lww_str(const lww *l)
{
	return lww_null(l) ? NULL : tix_xstrdup(l->v->s);
}

static int cmp_dep(const void *a, const void *b)
{
	return tix_oid_cmp(&((const tix_dep *)a)->pred, &((const tix_dep *)b)->pred);
}

static int cmp_assignment(const void *a, const void *b)
{
	return tix_oid_cmp(&((const tix_assignment *)a)->resource, &((const tix_assignment *)b)->resource);
}

static void fold_plan(const ctx *c, const lww *f, const or_vec *dadd, const or_vec *drem, const or_vec *aadd,
		      const or_vec *arem, tix_ticket_plan *pl)
{
	pl->parent = lww_id(&f[TIX_F_PARENT]);
	pl->parent_order = f[TIX_F_PARENT].order;
	pl->rank = lww_str(&f[TIX_F_RANK]);
	pl->kind = (tix_ticket_kind)lww_int(&f[TIX_F_KIND], TIX_KIND_TASK);
	pl->calendar = lww_id(&f[TIX_F_CALENDAR]);
	pl->mode = (tix_sched_mode)lww_int(&f[TIX_F_MODE], TIX_MODE_AUTO);
	pl->start = lww_date(&f[TIX_F_START]);
	pl->finish = lww_date(&f[TIX_F_FINISH]);
	pl->deadline = lww_date(&f[TIX_F_DEADLINE]);
	pl->actual_start = lww_date(&f[TIX_F_ACTUAL_START]);
	pl->actual_finish = lww_date(&f[TIX_F_ACTUAL_FINISH]);
	pl->duration = lww_int(&f[TIX_F_DURATION], -1);
	pl->effort = lww_int(&f[TIX_F_EFFORT], -1);
	pl->progress = (int)lww_int(&f[TIX_F_PROGRESS], 0);
	pl->external = lww_str(&f[TIX_F_EXTERNAL]);

	tix_strs seen = {0};
	TIX_VEC(tix_dep) deps = {0};
	for (size_t a = 0; a < dadd->n; a++) {
		if (!tix_strs_add(&seen, dadd->v[a].key))
			continue;
		size_t s = or_survivor(c, dadd, drem, dadd->v[a].key);
		if (s == SIZE_MAX)
			continue;
		const tix_op *op = dadd->v[s].op;
		tix_dep d = {op->u.dep_add.pred, op->u.dep_add.kind, op->u.dep_add.lag,
			     order_of(c, dadd->v[s].node, dadd->v[s].line)};
		TIX_VEC_PUSH(&deps, d);
	}
	tix_strs_free(&seen);
	TIX_VEC(tix_assignment) as = {0};
	for (size_t a = 0; a < aadd->n; a++) {
		if (!tix_strs_add(&seen, aadd->v[a].key))
			continue;
		size_t s = or_survivor(c, aadd, arem, aadd->v[a].key);
		if (s == SIZE_MAX)
			continue;
		const tix_op *op = aadd->v[s].op;
		tix_assignment x = {op->u.assign.resource, op->u.assign.units, op->u.assign.effort};
		TIX_VEC_PUSH(&as, x);
	}
	tix_strs_free(&seen);
	if (deps.n)
		qsort(deps.v, deps.n, sizeof(tix_dep), cmp_dep);
	if (as.n)
		qsort(as.v, as.n, sizeof(tix_assignment), cmp_assignment);
	pl->deps = deps.v;
	pl->ndeps = deps.n;
	pl->assignments = as.v;
	pl->nassignments = as.n;
}

static int cmp_stage_def(const void *pa, const void *pb)
{
	const tix_stage_def *a = pa, *b = pb;
	if (a->phase != b->phase)
		return a->phase < b->phase ? -1 : 1;
	if (a->rank != b->rank)
		return a->rank < b->rank ? -1 : 1;
	return strcmp(a->name, b->name);
}

static void fold_stages(const ctx *c, const or_vec *sadd, const or_vec *srem, tix_ticket *t)
{
	tix_strs seen = {0};
	TIX_VEC(tix_stage_def) defs = {0};
	for (size_t a = 0; a < sadd->n; a++) {
		if (!tix_strs_add(&seen, sadd->v[a].key))
			continue;
		size_t s = or_survivor(c, sadd, srem, sadd->v[a].key);
		if (s == SIZE_MAX)
			continue;
		const tix_op *op = sadd->v[s].op;
		tix_stage_def d = {tix_xstrdup(op->u.stage_add.name), op->u.stage_add.phase, op->u.stage_add.human,
				   op->u.stage_add.rank};
		TIX_VEC_PUSH(&defs, d);
	}
	tix_strs_free(&seen);
	if (defs.n)
		qsort(defs.v, defs.n, sizeof(*defs.v), cmp_stage_def);
	t->stages = defs.v;
	t->nstages = defs.n;
}

static void set_phase(tix_ticket *t, tix_ticket_status s, const char *stage, int64_t time)
{
	t->status = s;
	free(t->stage);
	t->stage = stage ? tix_xstrdup(stage) : NULL;
	if (s == TIX_TICKET_IN_PROGRESS && t->plan.started_at < 0)
		t->plan.started_at = time;
	t->plan.done_at = s == TIX_TICKET_DONE ? time : -1;
}

static void fold_ticket(const ctx *c, const op_ref *ops, size_t n, tix_ticket *t)
{
	or_vec adds = {0}, rems = {0}, dadd = {0}, drem = {0}, aadd = {0}, arem = {0}, sadd = {0}, srem = {0};
	lww f[TIX_F_COUNT];
	memset(f, 0, sizeof(f));
	t->plan.started_at = -1;
	t->plan.done_at = -1;
	TIX_VEC(tix_comment) comments = {0};
	for (size_t i = 0; i < n; i++) {
		const op_ref *r = &ops[i];
		const tix_op *op = r->op;
		const tix_pack *p = &c->packs[r->node];
		switch (op->kind) {
		case TIX_OP_TICKET_CREATE:
			t->author = p->author;
			t->title = tix_xstrdup(op->u.ticket_create.title);
			t->body = tix_xstrdup(op->u.ticket_create.body);
			for (size_t k = 0; k < op->u.ticket_create.labels.n; k++)
				push_or(&adds, op->u.ticket_create.labels.v[k], r);
			break;
		case TIX_OP_SET_TITLE:
			free(t->title);
			t->title = tix_xstrdup(op->u.text.value);
			break;
		case TIX_OP_SET_BODY:
			free(t->body);
			t->body = tix_xstrdup(op->u.text.value);
			break;
		case TIX_OP_SET_STATUS: set_phase(t, op->u.set_status.status, NULL, p->time); break;
		case TIX_OP_SET_STATE:
			/* A writer puts set-status right before set-state for older readers; both
			 * land here in a row, so together they are one write (§15.1). */
			set_phase(t, op->u.set_state.status, op->u.set_state.stage, p->time);
			break;
		case TIX_OP_STAGE_ADD: push_or(&sadd, op->u.stage_add.name, r); break;
		case TIX_OP_STAGE_REMOVE: push_or(&srem, op->u.stage_remove.name, r); break;
		case TIX_OP_SET_FIELD: fold_field(c, r, f); break;
		case TIX_OP_DEP_ADD: push_or_id(&dadd, &op->u.dep_add.pred, r); break;
		case TIX_OP_DEP_REMOVE: push_or_id(&drem, &op->u.unref.target, r); break;
		case TIX_OP_ASSIGN: push_or_id(&aadd, &op->u.assign.resource, r); break;
		case TIX_OP_UNASSIGN: push_or_id(&arem, &op->u.unref.target, r); break;
		case TIX_OP_LABEL:
			for (size_t k = 0; k < op->u.label.add.n; k++)
				push_or(&adds, op->u.label.add.v[k], r);
			for (size_t k = 0; k < op->u.label.remove.n; k++)
				push_or(&rems, op->u.label.remove.v[k], r);
			break;
		case TIX_OP_COMMENT: {
			tix_comment cm = {{c->nodes[r->node].oid, r->line}, p->author, p->time,
					  tix_xstrdup(op->u.text.value), false};
			TIX_VEC_PUSH(&comments, cm);
			break;
		}
		case TIX_OP_EDIT_COMMENT:
			for (size_t k = 0; k < comments.n; k++) {
				tix_comment *cm = &comments.v[k];
				if (tix_opid_cmp(&cm->id, &op->u.edit_comment.target))
					continue;
				if (tix_oid_eq(&cm->author, &p->author)) {
					free(cm->body);
					cm->body = tix_xstrdup(op->u.edit_comment.body);
					cm->edited = true;
				}
				break;
			}
			break;
		default:
			break;
		}
	}
	for (size_t a = 0; a < adds.n; a++)
		if (!tix_strs_contains(&t->labels, adds.v[a].key) &&
		    or_survivor(c, &adds, &rems, adds.v[a].key) != SIZE_MAX)
			tix_strs_add(&t->labels, adds.v[a].key);
	t->comments = comments.v;
	t->ncomments = comments.n;
	fold_plan(c, f, &dadd, &drem, &aadd, &arem, &t->plan);
	fold_stages(c, &sadd, &srem, t);
	free(sadd.v);
	free(srem.v);
	free(adds.v);
	free(rems.v);
	free(dadd.v);
	free(drem.v);
	free(aadd.v);
	free(arem.v);
}

static void fold_resource(const ctx *c, const op_ref *ops, size_t n, tix_resource *res)
{
	lww f[TIX_F_COUNT];
	memset(f, 0, sizeof(f));
	tix_resource_kind kind = TIX_RES_PERSON;
	const char *name = "";
	for (size_t i = 0; i < n; i++) {
		const tix_op *op = ops[i].op;
		if (op->kind == TIX_OP_RESOURCE_CREATE) {
			name = op->u.resource_create.name;
			kind = op->u.resource_create.kind;
		} else if (op->kind == TIX_OP_SET_FIELD) {
			fold_field(c, &ops[i], f);
		}
	}
	res->name = f[TIX_F_RES_NAME].v ? lww_str(&f[TIX_F_RES_NAME]) : tix_xstrdup(name);
	res->kind = (tix_resource_kind)lww_int(&f[TIX_F_RES_KIND], kind);
	res->capacity = (int32_t)lww_int(&f[TIX_F_CAPACITY], 100);
	res->calendar = lww_id(&f[TIX_F_RES_CALENDAR]);
	res->identity = lww_id(&f[TIX_F_RES_IDENTITY]);
	res->rate = lww_int(&f[TIX_F_RATE], -1);
	res->currency = lww_str(&f[TIX_F_CURRENCY]);
	res->active = lww_int(&f[TIX_F_ACTIVE], 1) != 0;
}

static int cmp_exception(const void *pa, const void *pb)
{
	const tix_cal_exception *a = pa, *b = pb;
	if (a->from != b->from)
		return a->from < b->from ? -1 : 1;
	return (a->to > b->to) - (a->to < b->to);
}

static void fold_calendar(const ctx *c, const op_ref *ops, size_t n, tix_calendar *cal)
{
	lww f[TIX_F_COUNT];
	or_vec adds = {0}, rems = {0};
	memset(f, 0, sizeof(f));
	const char *name = "";
	for (size_t i = 0; i < n; i++) {
		const tix_op *op = ops[i].op;
		switch (op->kind) {
		case TIX_OP_CALENDAR_CREATE: name = op->u.calendar_create.name; break;
		case TIX_OP_SET_FIELD: fold_field(c, &ops[i], f); break;
		case TIX_OP_EXCEPTION_ADD:
			push_or_range(&adds, op->u.exception_add.from, op->u.exception_add.to, &ops[i]);
			break;
		case TIX_OP_EXCEPTION_REMOVE:
			push_or_range(&rems, op->u.exception_remove.from, op->u.exception_remove.to, &ops[i]);
			break;
		default: break;
		}
	}
	cal->name = f[TIX_F_CAL_NAME].v ? lww_str(&f[TIX_F_CAL_NAME]) : tix_xstrdup(name);
	cal->weekdays = (unsigned)lww_int(&f[TIX_F_WEEKDAYS], TIX_WEEKDAYS_MON_FRI);
	cal->hours = (int)lww_int(&f[TIX_F_HOURS], 8);
	tix_strs seen = {0};
	TIX_VEC(tix_cal_exception) ex = {0};
	for (size_t a = 0; a < adds.n; a++) {
		if (!tix_strs_add(&seen, adds.v[a].key))
			continue;
		size_t s = or_survivor(c, &adds, &rems, adds.v[a].key);
		if (s == SIZE_MAX)
			continue;
		const tix_op *op = adds.v[s].op;
		tix_cal_exception x = {op->u.exception_add.from, op->u.exception_add.to, op->u.exception_add.working,
				       tix_xstrdup(op->u.exception_add.note), order_of(c, adds.v[s].node, adds.v[s].line)};
		TIX_VEC_PUSH(&ex, x);
	}
	if (ex.n)
		qsort(ex.v, ex.n, sizeof(*ex.v), cmp_exception);
	cal->exceptions = ex.v;
	cal->nexceptions = ex.n;
	tix_strs_free(&seen);
	free(adds.v);
	free(rems.v);
}

static int cmp_key(const void *a, const void *b)
{
	return strcmp(((const tix_key *)a)->fingerprint, ((const tix_key *)b)->fingerprint);
}

static void fold_identity(const ctx *c, const op_ref *ops, size_t n, tix_identity *id)
{
	or_vec adds = {0}, rems = {0};
	lww f[TIX_F_COUNT];
	memset(f, 0, sizeof(f));
	for (size_t i = 0; i < n; i++) {
		const tix_op *op = ops[i].op;
		switch (op->kind) {
		case TIX_OP_SET_FIELD: fold_field(c, &ops[i], f); break;
		case TIX_OP_IDENTITY_CREATE:
			id->name = tix_xstrdup(op->u.identity_create.name);
			id->email = tix_xstrdup(op->u.identity_create.email);
			break;
		case TIX_OP_SET_NAME:
			free(id->name);
			id->name = tix_xstrdup(op->u.text.value);
			break;
		case TIX_OP_SET_EMAIL:
			free(id->email);
			id->email = tix_xstrdup(op->u.text.value);
			break;
		case TIX_OP_KEY_ADD:
			push_or(&adds, op->u.key_add.fingerprint, &ops[i]);
			break;
		case TIX_OP_KEY_REMOVE:
			push_or(&rems, op->u.key_remove.fingerprint, &ops[i]);
			break;
		default:
			break;
		}
	}
	id->kind = (tix_identity_kind)lww_int(&f[TIX_F_ID_KIND], TIX_ID_HUMAN);
	tix_strs seen = {0};
	TIX_VEC(tix_key) keys = {0};
	for (size_t a = 0; a < adds.n; a++) {
		if (!tix_strs_add(&seen, adds.v[a].key))
			continue;
		size_t s = or_survivor(c, &adds, &rems, adds.v[a].key);
		if (s == SIZE_MAX)
			continue;
		const tix_op *op = adds.v[s].op;
		tix_key k = {tix_xstrdup(op->u.key_add.kind), tix_xstrdup(op->u.key_add.fingerprint),
			     tix_xstrdup(op->u.key_add.pubkey)};
		TIX_VEC_PUSH(&keys, k);
	}
	if (keys.n)
		qsort(keys.v, keys.n, sizeof(tix_key), cmp_key);
	id->keys = keys.v;
	id->nkeys = keys.n;
	tix_strs_free(&seen);
	free(adds.v);
	free(rems.v);
}

static int cmp_baseline_task(const void *pa, const void *pb)
{
	return tix_oid_cmp(&((const tix_baseline_task *)pa)->ticket, &((const tix_baseline_task *)pb)->ticket);
}

static const tix_baseline_task *order_ctx; /* qsort context; single-threaded */

static int cmp_task_index(const void *pa, const void *pb)
{
	size_t a = *(const size_t *)pa, b = *(const size_t *)pb;
	int c = tix_oid_cmp(&order_ctx[a].ticket, &order_ctx[b].ticket);
	return c ? c : (a > b) - (a < b);
}

/* ops arrive in total order, so a later "task" op for a ticket replaces an earlier one. */
static void fold_baseline(const op_ref *ops, size_t n, tix_baseline *b)
{
	TIX_VEC(tix_baseline_task) v = {0};
	for (size_t i = 0; i < n; i++) {
		const tix_op *op = ops[i].op;
		switch (op->kind) {
		case TIX_OP_BASELINE_CREATE:
			b->name = tix_xstrdup(op->u.baseline_create.name);
			b->taken = op->u.baseline_create.taken;
			b->root = op->u.baseline_create.root;
			break;
		case TIX_OP_BASELINE_SET_NAME:
			free(b->name);
			b->name = tix_xstrdup(op->u.text.value);
			break;
		case TIX_OP_BASELINE_TASK: {
			const tix_baseline_task_op *t = &op->u.baseline_task;
			tix_baseline_task x = {t->ticket, t->start, t->finish, t->duration, t->effort, t->cost,
					       t->currency ? tix_xstrdup(t->currency) : NULL};
			TIX_VEC_PUSH(&v, x);
			break;
		}
		default:
			break;
		}
	}
	/* by ticket, then by position in total order (qsort isn't stable); keep the last */
	order_ctx = v.v;
	size_t *idx = tix_xcalloc(v.n ? v.n : 1, sizeof(*idx));
	for (size_t i = 0; i < v.n; i++)
		idx[i] = i;
	if (v.n)
		qsort(idx, v.n, sizeof(*idx), cmp_task_index);
	tix_baseline_task *out = tix_xcalloc(v.n ? v.n : 1, sizeof(*out));
	size_t keep = 0;
	for (size_t k = 0; k < v.n; k++) {
		tix_baseline_task *t = &v.v[idx[k]];
		if (keep && tix_oid_eq(&out[keep - 1].ticket, &t->ticket)) {
			free(out[keep - 1].currency);
			out[keep - 1] = *t;
		} else {
			out[keep++] = *t;
		}
	}
	free(idx);
	free(v.v);
	b->tasks = out;
	b->ntasks = keep;
}

const tix_baseline_task *tix_baseline_find(const tix_baseline *b, const tix_oid *ticket)
{
	tix_baseline_task key = {.ticket = *ticket};
	return b->ntasks ? bsearch(&key, b->tasks, b->ntasks, sizeof(*b->tasks), cmp_baseline_task) : NULL;
}

tix_status tix_replay(tix_entity_type type, const tix_dag_node *nodes, size_t n, tix_entity **out,
		      tix_error *err)
{
	TIX_REQUIRE(out && (nodes || !n));
	TIX_REQUIRE(type >= TIX_ENTITY_IDENTITY && type <= TIX_ENTITY_BASELINE);
	*out = NULL;
	if (n == 0 || n > TIX_ENTITY_MAX_COMMITS)
		return tix_error_set(err, TIX_ERR_INVALID, "entity has %zu commits", n);
	ctx c = {.nodes = nodes, .n = n, .algo = (tix_hash_algo)nodes[0].oid.algo};
	if (c.algo != TIX_HASH_SHA1 && c.algo != TIX_HASH_SHA256)
		return tix_error_set(err, TIX_ERR_INVALID, "bad hash algorithm");

	size_t root = 0, head = 0;
	tix_status st = check_graph(&c, &root, &head, err);
	if (!st)
		st = check_packs(&c, type, root, err);
	if (st) {
		ctx_free(&c);
		return st;
	}

	tix_entity *e = tix_xcalloc(1, sizeof(*e));
	e->type = type;
	e->id = nodes[root].oid;
	e->head = nodes[head].oid;
	e->created = c.packs[root].time;

	TIX_VEC(op_ref) ops = {0};
	bool need_anc = false;
	for (size_t i = 0; i < n; i++) {
		const tix_pack *p = &c.packs[i];
		if (p->lamport > e->max_lamport)
			e->max_lamport = p->lamport;
		if (p->nops && p->time > e->updated)
			e->updated = p->time;
		for (size_t k = 0; k < p->nops; k++) {
			const tix_op *op = &p->ops[k];
			if (op->kind == TIX_OP_UNKNOWN) {
				e->nunknown++;
				continue;
			}
			e->nops++;
			need_anc |= (op->kind == TIX_OP_LABEL && op->u.label.remove.n) || op->kind == TIX_OP_KEY_REMOVE ||
				    op->kind == TIX_OP_DEP_REMOVE || op->kind == TIX_OP_UNASSIGN ||
				    op->kind == TIX_OP_EXCEPTION_REMOVE || op->kind == TIX_OP_STAGE_REMOVE;
			op_ref r = {i, (uint32_t)(k + 2), op};
			TIX_VEC_PUSH(&ops, r);
		}
	}
	if (need_anc)
		build_ancestors(&c);
	sort_ctx = &c;
	if (ops.n)
		qsort(ops.v, ops.n, sizeof(op_ref), cmp_op_ref);
	switch (type) {
	case TIX_ENTITY_TICKET: fold_ticket(&c, ops.v, ops.n, &e->u.ticket); break;
	case TIX_ENTITY_IDENTITY: fold_identity(&c, ops.v, ops.n, &e->u.identity); break;
	case TIX_ENTITY_RESOURCE: fold_resource(&c, ops.v, ops.n, &e->u.resource); break;
	case TIX_ENTITY_CALENDAR: fold_calendar(&c, ops.v, ops.n, &e->u.calendar); break;
	case TIX_ENTITY_BASELINE: fold_baseline(ops.v, ops.n, &e->u.baseline); break;
	}
	free(ops.v);
	ctx_free(&c);
	*out = e;
	return TIX_OK;
}

void tix_entity_free(tix_entity *e)
{
	if (!e)
		return;
	if (e->type == TIX_ENTITY_TICKET) {
		tix_ticket *t = &e->u.ticket;
		free(t->title);
		free(t->body);
		free(t->stage);
		for (size_t i = 0; i < t->nstages; i++)
			free(t->stages[i].name);
		free(t->stages);
		tix_strs_free(&t->labels);
		for (size_t i = 0; i < t->ncomments; i++)
			free(t->comments[i].body);
		free(t->comments);
		free(t->plan.rank);
		free(t->plan.external);
		free(t->plan.deps);
		free(t->plan.assignments);
	} else if (e->type == TIX_ENTITY_RESOURCE) {
		free(e->u.resource.name);
		free(e->u.resource.currency);
	} else if (e->type == TIX_ENTITY_CALENDAR) {
		free(e->u.calendar.name);
		for (size_t i = 0; i < e->u.calendar.nexceptions; i++)
			free(e->u.calendar.exceptions[i].note);
		free(e->u.calendar.exceptions);
	} else if (e->type == TIX_ENTITY_BASELINE) {
		free(e->u.baseline.name);
		for (size_t i = 0; i < e->u.baseline.ntasks; i++)
			free(e->u.baseline.tasks[i].currency);
		free(e->u.baseline.tasks);
	} else {
		tix_identity *id = &e->u.identity;
		free(id->name);
		free(id->email);
		for (size_t i = 0; i < id->nkeys; i++) {
			free(id->keys[i].kind);
			free(id->keys[i].fingerprint);
			free(id->keys[i].pubkey);
		}
		free(id->keys);
	}
	free(e);
}

static void snap_id(tix_jw *w, const char *k, const tix_oid *id)
{
	char hex[TIX_OID_MAX_HEX + 1];
	tix_jw_key(w, k);
	if (tix_oid_is_null(id))
		tix_jw_null(w);
	else
		tix_jw_str(w, tix_oid_hex(id, hex));
}

static void snap_date(tix_jw *w, const char *k, tix_date d)
{
	char s[TIX_DATE_STR];
	tix_jw_key(w, k);
	if (d == TIX_DATE_NONE)
		tix_jw_null(w);
	else
		tix_jw_str(w, tix_date_str(d, s));
}

static void snap_int(tix_jw *w, const char *k, int64_t v)
{
	tix_jw_key(w, k);
	if (v < 0)
		tix_jw_null(w);
	else
		tix_jw_int(w, v);
}

static void snap_str(tix_jw *w, const char *k, const char *s)
{
	tix_jw_key(w, k);
	if (s)
		tix_jw_str(w, s);
	else
		tix_jw_null(w);
}

static void snap_plan(tix_jw *w, const tix_ticket_plan *pl)
{
	snap_id(w, "parent", &pl->parent);
	snap_str(w, "rank", pl->rank);
	snap_str(w, "external", pl->external);
	snap_str(w, "kind", tix_field_get(TIX_F_KIND)->enums[pl->kind]);
	snap_id(w, "calendar", &pl->calendar);
	snap_str(w, "mode", tix_field_get(TIX_F_MODE)->enums[pl->mode]);
	snap_date(w, "start", pl->start);
	snap_date(w, "finish", pl->finish);
	snap_int(w, "duration", pl->duration);
	snap_int(w, "effort", pl->effort);
	snap_date(w, "deadline", pl->deadline);
	snap_int(w, "progress", pl->progress);
	snap_date(w, "actual_start", pl->actual_start);
	snap_date(w, "actual_finish", pl->actual_finish);
	snap_int(w, "started_at", pl->started_at);
	snap_int(w, "done_at", pl->done_at);
	tix_jw_key(w, "deps");
	tix_jw_arr_begin(w);
	for (size_t i = 0; i < pl->ndeps; i++) {
		tix_jw_obj_begin(w);
		snap_id(w, "pred", &pl->deps[i].pred);
		snap_str(w, "kind", tix_dep_kind_name(pl->deps[i].kind));
		tix_jw_key(w, "lag");
		tix_jw_int(w, pl->deps[i].lag);
		tix_jw_obj_end(w);
	}
	tix_jw_arr_end(w);
	tix_jw_key(w, "assignments");
	tix_jw_arr_begin(w);
	for (size_t i = 0; i < pl->nassignments; i++) {
		tix_jw_obj_begin(w);
		snap_id(w, "resource", &pl->assignments[i].resource);
		snap_int(w, "units", pl->assignments[i].units);
		snap_int(w, "effort", pl->assignments[i].effort);
		tix_jw_obj_end(w);
	}
	tix_jw_arr_end(w);
}

void tix_entity_snapshot(const tix_entity *e, tix_buf *out)
{
	char hex[TIX_OID_MAX_HEX + 1], opid[TIX_OID_MAX_HEX + 12];
	tix_jw w;
	tix_jw_init(&w, out);
	tix_jw_obj_begin(&w);
	tix_jw_key(&w, "type");
	tix_jw_str(&w, tix_entity_type_name(e->type));
	tix_jw_key(&w, "id");
	tix_jw_str(&w, tix_oid_hex(&e->id, hex));
	tix_jw_key(&w, "created");
	tix_jw_int(&w, e->created);
	tix_jw_key(&w, "updated");
	tix_jw_int(&w, e->updated);
	if (e->type == TIX_ENTITY_TICKET) {
		const tix_ticket *t = &e->u.ticket;
		tix_jw_key(&w, "author");
		tix_jw_str(&w, tix_oid_hex(&t->author, hex));
		tix_jw_key(&w, "title");
		tix_jw_str(&w, t->title);
		tix_jw_key(&w, "body");
		tix_jw_str(&w, t->body);
		tix_jw_key(&w, "status");
		tix_jw_str(&w, tix_ticket_status_name(t->status));
		snap_str(&w, "stage", t->stage);
		tix_jw_key(&w, "stages");
		tix_jw_arr_begin(&w);
		for (size_t i = 0; i < t->nstages; i++) {
			tix_jw_obj_begin(&w);
			snap_str(&w, "name", t->stages[i].name);
			snap_str(&w, "phase", tix_ticket_status_name(t->stages[i].phase));
			tix_jw_key(&w, "human");
			tix_jw_bool(&w, t->stages[i].human);
			snap_int(&w, "rank", t->stages[i].rank);
			tix_jw_obj_end(&w);
		}
		tix_jw_arr_end(&w);
		tix_jw_key(&w, "labels");
		tix_jw_arr_begin(&w);
		for (size_t i = 0; i < t->labels.n; i++)
			tix_jw_str(&w, t->labels.v[i]);
		tix_jw_arr_end(&w);
		tix_jw_key(&w, "comments");
		tix_jw_arr_begin(&w);
		for (size_t i = 0; i < t->ncomments; i++) {
			const tix_comment *cm = &t->comments[i];
			tix_jw_obj_begin(&w);
			tix_jw_key(&w, "id");
			tix_jw_str(&w, tix_opid_str(&cm->id, opid));
			tix_jw_key(&w, "author");
			tix_jw_str(&w, tix_oid_hex(&cm->author, hex));
			tix_jw_key(&w, "time");
			tix_jw_int(&w, cm->time);
			tix_jw_key(&w, "body");
			tix_jw_str(&w, cm->body);
			tix_jw_key(&w, "edited");
			tix_jw_bool(&w, cm->edited);
			tix_jw_obj_end(&w);
		}
		tix_jw_arr_end(&w);
		snap_plan(&w, &t->plan);
	} else if (e->type == TIX_ENTITY_RESOURCE) {
		const tix_resource *r = &e->u.resource;
		snap_str(&w, "name", r->name);
		snap_str(&w, "kind", tix_field_get(TIX_F_RES_KIND)->enums[r->kind]);
		snap_int(&w, "capacity", r->capacity);
		snap_id(&w, "calendar", &r->calendar);
		snap_id(&w, "identity", &r->identity);
		snap_int(&w, "rate", r->rate);
		snap_str(&w, "currency", r->currency);
		tix_jw_key(&w, "active");
		tix_jw_bool(&w, r->active);
	} else if (e->type == TIX_ENTITY_CALENDAR) {
		const tix_calendar *cal = &e->u.calendar;
		snap_str(&w, "name", cal->name);
		snap_int(&w, "weekdays", cal->weekdays);
		snap_int(&w, "hours", cal->hours);
		tix_jw_key(&w, "exceptions");
		tix_jw_arr_begin(&w);
		for (size_t i = 0; i < cal->nexceptions; i++) {
			tix_jw_obj_begin(&w);
			snap_date(&w, "from", cal->exceptions[i].from);
			snap_date(&w, "to", cal->exceptions[i].to);
			tix_jw_key(&w, "working");
			tix_jw_bool(&w, cal->exceptions[i].working);
			snap_str(&w, "note", cal->exceptions[i].note);
			tix_jw_obj_end(&w);
		}
		tix_jw_arr_end(&w);
	} else if (e->type == TIX_ENTITY_BASELINE) {
		const tix_baseline *b = &e->u.baseline;
		snap_str(&w, "name", b->name);
		snap_date(&w, "taken", b->taken);
		snap_id(&w, "root", &b->root);
		tix_jw_key(&w, "tasks");
		tix_jw_arr_begin(&w);
		for (size_t i = 0; i < b->ntasks; i++) {
			const tix_baseline_task *t = &b->tasks[i];
			tix_jw_obj_begin(&w);
			snap_id(&w, "ticket", &t->ticket);
			snap_date(&w, "start", t->start);
			snap_date(&w, "finish", t->finish);
			snap_int(&w, "duration", t->duration);
			snap_int(&w, "effort", t->effort);
			snap_int(&w, "cost", t->cost);
			snap_str(&w, "currency", t->currency);
			tix_jw_obj_end(&w);
		}
		tix_jw_arr_end(&w);
	} else {
		const tix_identity *id = &e->u.identity;
		tix_jw_key(&w, "name");
		tix_jw_str(&w, id->name);
		tix_jw_key(&w, "email");
		tix_jw_str(&w, id->email);
		snap_str(&w, "kind", tix_field_get(TIX_F_ID_KIND)->enums[id->kind]);
		tix_jw_key(&w, "keys");
		tix_jw_arr_begin(&w);
		for (size_t i = 0; i < id->nkeys; i++) {
			tix_jw_obj_begin(&w);
			tix_jw_key(&w, "kind");
			tix_jw_str(&w, id->keys[i].kind);
			tix_jw_key(&w, "fingerprint");
			tix_jw_str(&w, id->keys[i].fingerprint);
			tix_jw_key(&w, "pubkey");
			tix_jw_str(&w, id->keys[i].pubkey);
			tix_jw_obj_end(&w);
		}
		tix_jw_arr_end(&w);
	}
	tix_jw_obj_end(&w);
}
