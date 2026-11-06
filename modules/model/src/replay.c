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

static bool is_create(tix_op_kind k)
{
	return k == TIX_OP_TICKET_CREATE || k == TIX_OP_IDENTITY_CREATE;
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
			if (is_create(p->ops[k].kind) != want_create)
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
	const char *key;
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
	or_ent e = {key, r->node, r->line, r->op};
	TIX_VEC_PUSH(v, e);
}

static void fold_ticket(const ctx *c, const op_ref *ops, size_t n, tix_ticket *t)
{
	or_vec adds = {0}, rems = {0};
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
		case TIX_OP_SET_STATUS:
			t->status = op->u.set_status.status;
			break;
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
	for (size_t i = 0; i < n; i++) {
		const tix_op *op = ops[i].op;
		switch (op->kind) {
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

tix_status tix_replay(tix_entity_type type, const tix_dag_node *nodes, size_t n, tix_entity **out,
		      tix_error *err)
{
	TIX_REQUIRE(out && (nodes || !n));
	TIX_REQUIRE(type == TIX_ENTITY_TICKET || type == TIX_ENTITY_IDENTITY);
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
			need_anc |= (op->kind == TIX_OP_LABEL && op->u.label.remove.n) ||
				    op->kind == TIX_OP_KEY_REMOVE;
			op_ref r = {i, (uint32_t)(k + 2), op};
			TIX_VEC_PUSH(&ops, r);
		}
	}
	if (need_anc)
		build_ancestors(&c);
	sort_ctx = &c;
	if (ops.n)
		qsort(ops.v, ops.n, sizeof(op_ref), cmp_op_ref);
	if (type == TIX_ENTITY_TICKET)
		fold_ticket(&c, ops.v, ops.n, &e->u.ticket);
	else
		fold_identity(&c, ops.v, ops.n, &e->u.identity);
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
		tix_strs_free(&t->labels);
		for (size_t i = 0; i < t->ncomments; i++)
			free(t->comments[i].body);
		free(t->comments);
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
	} else {
		const tix_identity *id = &e->u.identity;
		tix_jw_key(&w, "name");
		tix_jw_str(&w, id->name);
		tix_jw_key(&w, "email");
		tix_jw_str(&w, id->email);
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
